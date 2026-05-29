// led_detector.cpp — see led_detector.h.
#include "led_detector.h"

#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "esp_camera.h"

#include "config.h"
#include "led_model.h"

// Chirale_TensorFlowLite is the Arduino-compatible TFLite-Micro port.
#include <Chirale_TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ---------------------------------------------------------------------------
// TFLite-Micro objects (static, no heap fragmentation against camera DMA).
// ---------------------------------------------------------------------------
namespace {
const tflite::Model *g_model = nullptr;
tflite::MicroInterpreter *g_interpreter = nullptr;
TfLiteTensor *g_input = nullptr;
TfLiteTensor *g_output = nullptr;

// Arena must be 16-byte aligned and live in BSS.
alignas(16) uint8_t g_tensor_arena[DET_TENSOR_ARENA_BYTES];

// Runtime config, guarded by a spinlock for cross-task access.
portMUX_TYPE g_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
led_roi_t g_roi = {DET_DEFAULT_ROI_X, DET_DEFAULT_ROI_Y, DET_DEFAULT_ROI_W, DET_DEFAULT_ROI_H};
volatile bool g_enabled = false;

// Debounced state machine.
led_state_t g_committed_state = LED_STATE_OFF;
uint32_t g_committed_since_ms = 0;
led_state_t g_last_raw = LED_STATE_OFF;
int g_raw_run = 0;

// Exposed snapshot for /status.
volatile led_state_t g_cur_state = LED_STATE_OFF;
volatile float g_cur_conf = 0.0f;

QueueHandle_t g_event_queue = nullptr;
bool g_model_ready = false;
volatile uint32_t g_dropped = 0;  // events lost to queue overflow
}  // namespace

uint32_t ledDetectorTakeDropped(void) {
  uint32_t n = g_dropped;
  g_dropped = 0;
  return n;
}

const char *led_state_name(led_state_t s) {
  switch (s) {
    case LED_STATE_OFF: return "off";
    case LED_STATE_RED_ON: return "red_on";
    case LED_STATE_WHITE_ON: return "white_on";
    default: return "unknown";
  }
}

QueueHandle_t ledDetectorEventQueue(void) { return g_event_queue; }

void ledDetectorSetEnabled(bool enabled) { g_enabled = enabled; }
bool ledDetectorIsEnabled(void) { return g_enabled; }

void ledDetectorSetRoi(const led_roi_t *roi) {
  if (!roi) return;
  portENTER_CRITICAL(&g_cfg_mux);
  g_roi = *roi;
  portEXIT_CRITICAL(&g_cfg_mux);
}

led_roi_t ledDetectorGetRoi(void) {
  portENTER_CRITICAL(&g_cfg_mux);
  led_roi_t r = g_roi;
  portEXIT_CRITICAL(&g_cfg_mux);
  return r;
}

led_state_t ledDetectorCurrentState(void) { return g_cur_state; }
float ledDetectorCurrentConfidence(void) { return g_cur_conf; }

// ---------------------------------------------------------------------------
// Preprocess: crop ROI from the RGB565 frame, box-downscale to the model input
// size, unpack to RGB888, normalise to [0,1] and quantize to int8 in-place into
// the input tensor.
//
// RGB565 byte order matches esp32-camera (to_bmp.c): big-endian per pixel, so
// pixel = (hi<<8)|lo with R in bits 15..11, G 10..5, B 4..0.
// ---------------------------------------------------------------------------
static void preprocess_into_input(const camera_fb_t *fb, const led_roi_t &roi) {
  const uint8_t *buf = fb->buf;
  const int fw = fb->width;
  const int fh = fb->height;

  // Clamp ROI to frame bounds.
  int rx = roi.x, ry = roi.y, rw = roi.w, rh = roi.h;
  if (rx < 0) rx = 0;
  if (ry < 0) ry = 0;
  if (rx >= fw) rx = fw - 1;
  if (ry >= fh) ry = fh - 1;
  if (rw < 1) rw = 1;
  if (rh < 1) rh = 1;
  if (rx + rw > fw) rw = fw - rx;
  if (ry + rh > fh) rh = fh - ry;

  const int outW = LED_MODEL_INPUT_W;
  const int outH = LED_MODEL_INPUT_H;
  const float in_scale = LED_MODEL_INPUT_SCALE;
  const int in_zp = LED_MODEL_INPUT_ZERO_POINT;
  int8_t *in = g_input->data.int8;

  for (int oy = 0; oy < outH; oy++) {
    // Source row span for this output row.
    int sy0 = ry + (oy * rh) / outH;
    int sy1 = ry + ((oy + 1) * rh) / outH;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    for (int ox = 0; ox < outW; ox++) {
      int sx0 = rx + (ox * rw) / outW;
      int sx1 = rx + ((ox + 1) * rw) / outW;
      if (sx1 <= sx0) sx1 = sx0 + 1;

      uint32_t accR = 0, accG = 0, accB = 0, n = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        const uint8_t *row = buf + (size_t)sy * fw * 2;
        for (int sx = sx0; sx < sx1; sx++) {
          uint8_t hi = row[sx * 2];
          uint8_t lo = row[sx * 2 + 1];
          uint16_t px = ((uint16_t)hi << 8) | lo;
          accR += ((px >> 11) & 0x1F) << 3;
          accG += ((px >> 5) & 0x3F) << 2;
          accB += (px & 0x1F) << 3;
          n++;
        }
      }
      if (n == 0) n = 1;
      uint8_t r8 = accR / n;
      uint8_t g8 = accG / n;
      uint8_t b8 = accB / n;

      // Normalise to [0,1] (must match ml/train.py) then quantize to int8.
      int idx = (oy * outW + ox) * LED_MODEL_INPUT_CH;
      float rgb[3] = {r8 / 255.0f, g8 / 255.0f, b8 / 255.0f};
      for (int c = 0; c < LED_MODEL_INPUT_CH; c++) {
        int q = (int)lroundf(rgb[c] / in_scale) + in_zp;
        if (q < -128) q = -128;
        if (q > 127) q = 127;
        in[idx + c] = (int8_t)q;
      }
    }
  }
}

// Run one inference; writes argmax class and confidence to out params.
static bool classify(const camera_fb_t *fb, const led_roi_t &roi, led_state_t *cls, float *conf) {
  preprocess_into_input(fb, roi);
  if (g_interpreter->Invoke() != kTfLiteOk) {
    return false;
  }
  const float out_scale = LED_MODEL_OUTPUT_SCALE;
  const int out_zp = LED_MODEL_OUTPUT_ZERO_POINT;
  const int8_t *out = g_output->data.int8;
  int best = 0;
  float best_p = -1.0f;
  for (int i = 0; i < LED_STATE_COUNT; i++) {
    float p = (out[i] - out_zp) * out_scale;
    if (p > best_p) {
      best_p = p;
      best = i;
    }
  }
  *cls = (led_state_t)best;
  *conf = best_p;
  return true;
}

// Feed one raw classification through the debounce + transition logic.
static void update_state_machine(led_state_t raw, float conf, uint32_t now) {
  g_cur_state = raw;
  g_cur_conf = conf;

  // Low-confidence frames carry no evidence; do not advance the debounce run.
  if (conf < DET_CONF_THRESHOLD) {
    return;
  }
  if (raw == g_last_raw) {
    if (g_raw_run < 1000000) g_raw_run++;
  } else {
    g_last_raw = raw;
    g_raw_run = 1;
  }

  if (raw != g_committed_state && g_raw_run >= DET_DEBOUNCE_COUNT) {
    led_event_t ev;
    ev.ts_ms = now;
    ev.from = g_committed_state;
    ev.to = raw;
    ev.dur_ms = now - g_committed_since_ms;
    ev.conf = conf;

    g_committed_state = raw;
    g_committed_since_ms = now;

    if (g_event_queue) {
      // Drop-oldest on overflow so the newest event always lands; the reporter
      // notices the gap via queue spaces and emits a "dropped" line.
      if (xQueueSend(g_event_queue, &ev, 0) != pdTRUE) {
        led_event_t discard;
        xQueueReceive(g_event_queue, &discard, 0);
        xQueueSend(g_event_queue, &ev, 0);
        g_dropped++;
      }
    }
  }
}

static void detector_task(void *arg) {
  (void)arg;
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(DET_SAMPLE_INTERVAL_MS);

  for (;;) {
    vTaskDelayUntil(&last_wake, period);
    if (!g_enabled || !g_model_ready) continue;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) continue;
    if (fb->format != PIXFORMAT_RGB565) {
      // Detector requires raw RGB565; skip if the sensor is in another mode.
      esp_camera_fb_return(fb);
      continue;
    }

    led_roi_t roi = ledDetectorGetRoi();
    led_state_t cls;
    float conf;
    bool ok = classify(fb, roi, &cls, &conf);
    esp_camera_fb_return(fb);  // return promptly; never held across Invoke()

    if (ok) {
      update_state_machine(cls, conf, millis());
    }
  }
}

bool ledDetectorInit(const led_roi_t *roi, bool enabled) {
  if (roi) ledDetectorSetRoi(roi);
  g_enabled = enabled;

  g_event_queue = xQueueCreate(DET_EVENT_QUEUE_LEN, sizeof(led_event_t));
  if (!g_event_queue) {
    Serial.println("[det] failed to create event queue");
    return false;
  }

  tflite::InitializeTarget();

  g_model = tflite::GetModel(g_led_model);
  if (g_model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("[det] model schema %lu != supported %d\n",
                  (unsigned long)g_model->version(), TFLITE_SCHEMA_VERSION);
    return false;
  }

  // Register only the ops this model uses.
  static tflite::MicroMutableOpResolver<7> resolver;
  resolver.AddConv2D();
  resolver.AddDepthwiseConv2D();
  resolver.AddFullyConnected();
  resolver.AddSoftmax();
  resolver.AddReshape();
  resolver.AddMaxPool2D();
  resolver.AddMean();  // global average pooling lowers to Mean

  static tflite::MicroInterpreter static_interpreter(
      g_model, resolver, g_tensor_arena, DET_TENSOR_ARENA_BYTES);
  g_interpreter = &static_interpreter;

  if (g_interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("[det] AllocateTensors failed (arena too small?)");
    return false;
  }

  g_input = g_interpreter->input(0);
  g_output = g_interpreter->output(0);

  Serial.printf("[det] model ready, arena used %u / %u bytes\n",
                (unsigned)g_interpreter->arena_used_bytes(),
                (unsigned)DET_TENSOR_ARENA_BYTES);

  g_committed_since_ms = millis();
  g_model_ready = true;
  return true;
}

void ledDetectorStart(void) {
  if (!g_model_ready) {
    Serial.println("[det] not started: model not ready");
    return;
  }
  xTaskCreatePinnedToCore(detector_task, "led_det", 8192, nullptr, 4, nullptr, 1);
}
