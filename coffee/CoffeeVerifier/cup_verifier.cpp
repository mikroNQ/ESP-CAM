// cup_verifier.cpp — see cup_verifier.h.
#include "cup_verifier.h"

#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "esp_camera.h"

#include "config.h"
#include "drink_model.h"

// Chirale_TensorFlowLite is the Arduino-compatible TFLite-Micro port.
#include <Chirale_TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {
// ---- TFLite-Micro objects (static; only the verifier task touches them) ----
const tflite::Model *g_model = nullptr;
tflite::MicroInterpreter *g_interpreter = nullptr;
TfLiteTensor *g_input = nullptr;
TfLiteTensor *g_output = nullptr;
alignas(16) uint8_t g_tensor_arena[CUP_TENSOR_ARENA_BYTES];
bool g_model_ready = false;

// ---- Runtime config (guarded by g_cfg_mux for cross-task access) ----------
portMUX_TYPE g_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
cup_roi_t g_roi = {CUP_DEFAULT_ROI_X, CUP_DEFAULT_ROI_Y, CUP_DEFAULT_ROI_W, CUP_DEFAULT_ROI_H};
volatile bool g_enabled = false;

uint32_t g_window_ms = CUP_DEFAULT_WINDOW_MS;
uint16_t g_fill_delta = CUP_DEFAULT_FILL_DELTA;
uint16_t g_settle_frames = CUP_DEFAULT_SETTLE_FRAMES;

volatile bool g_fixexp_enabled = false;
volatile int g_aec_value = CUP_DEFAULT_AEC_VALUE;
volatile int g_agc_gain = CUP_DEFAULT_AGC_GAIN;

// ---- Shared outputs (guarded by g_out_mux) --------------------------------
portMUX_TYPE g_out_mux = portMUX_INITIALIZER_UNLOCKED;
cup_metric_t g_live = {0, 0, 0, 0};
volatile int g_live_class = -1;
volatile float g_live_conf = 0.0f;
cup_verdict_t g_last = {0};
volatile uint32_t g_verdict_seq = 0;

// ---- Trigger handshake -----------------------------------------------------
volatile bool g_start_request = false;
volatile bool g_running = false;

QueueHandle_t g_verdict_queue = nullptr;
volatile uint32_t g_dropped = 0;
}  // namespace

const char *cup_result_name(cup_result_t r) {
  switch (r) {
    case CUP_RESULT_OK: return "ok";
    case CUP_RESULT_NOT_DISPENSED: return "not_dispensed";
    default: return "unknown";
  }
}

const char *cup_drink_name(int class_idx) {
  if (class_idx < 0) return "none";
  if (class_idx >= DRINK_MODEL_NUM_CLASSES) return "?";
  return g_drink_class_names[class_idx];
}

int cup_drink_num_classes(void) { return DRINK_MODEL_NUM_CLASSES; }

QueueHandle_t cupVerifierVerdictQueue(void) { return g_verdict_queue; }
bool cupVerifierModelReady(void) { return g_model_ready; }

uint32_t cupVerifierTakeDropped(void) {
  uint32_t n = g_dropped;
  g_dropped = 0;
  return n;
}

// ---- Config accessors ------------------------------------------------------
void cupVerifierSetEnabled(bool enabled) { g_enabled = enabled; }
bool cupVerifierIsEnabled(void) { return g_enabled; }

void cupVerifierSetRoi(const cup_roi_t *roi) {
  if (!roi) return;
  portENTER_CRITICAL(&g_cfg_mux);
  g_roi = *roi;
  portEXIT_CRITICAL(&g_cfg_mux);
}

cup_roi_t cupVerifierGetRoi(void) {
  portENTER_CRITICAL(&g_cfg_mux);
  cup_roi_t r = g_roi;
  portEXIT_CRITICAL(&g_cfg_mux);
  return r;
}

void cupVerifierSetParams(uint32_t window_ms, uint16_t fill_delta, uint16_t settle_frames) {
  portENTER_CRITICAL(&g_cfg_mux);
  if (window_ms) g_window_ms = window_ms;
  g_fill_delta = fill_delta;
  if (settle_frames) g_settle_frames = settle_frames;
  portEXIT_CRITICAL(&g_cfg_mux);
}

uint32_t cupVerifierWindowMs(void) { return g_window_ms; }
uint16_t cupVerifierFillDelta(void) { return g_fill_delta; }
uint16_t cupVerifierSettleFrames(void) { return g_settle_frames; }

bool cupVerifierBusy(void) { return g_running || g_start_request; }
uint32_t cupVerifierVerdictSeq(void) { return g_verdict_seq; }

cup_metric_t cupVerifierLiveMetric(void) {
  portENTER_CRITICAL(&g_out_mux);
  cup_metric_t m = g_live;
  portEXIT_CRITICAL(&g_out_mux);
  return m;
}

int cupVerifierLiveClass(void) { return g_live_class; }
float cupVerifierLiveClassConf(void) { return g_live_conf; }

bool cupVerifierLastVerdict(cup_verdict_t *out) {
  if (!out) return false;
  portENTER_CRITICAL(&g_out_mux);
  bool have = g_verdict_seq > 0;
  if (have) *out = g_last;
  portEXIT_CRITICAL(&g_out_mux);
  return have;
}

bool cupVerifierStartVerification(void) {
  if (!g_enabled) return false;
  if (cupVerifierBusy()) return false;
  bool accepted = false;
  portENTER_CRITICAL(&g_cfg_mux);
  if (!g_start_request && !g_running) {
    g_start_request = true;
    accepted = true;
  }
  portEXIT_CRITICAL(&g_cfg_mux);
  return accepted;
}

// ---- Fixed exposure lock ---------------------------------------------------
static void apply_fixed_exposure(void) {
  if (!g_fixexp_enabled) return;
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_exposure_ctrl(s, 0);
  s->set_aec2(s, 0);
  s->set_aec_value(s, g_aec_value);
  s->set_gain_ctrl(s, 0);
  s->set_agc_gain(s, g_agc_gain);
  s->set_whitebal(s, 0);
  s->set_awb_gain(s, 0);
}

void cupVerifierSetFixedExposure(bool enabled, int aec_value, int agc_gain) {
  g_aec_value = aec_value;
  g_agc_gain = agc_gain;
  g_fixexp_enabled = enabled;
  apply_fixed_exposure();
}

bool cupVerifierFixedExpEnabled(void) { return g_fixexp_enabled; }
int cupVerifierAecValue(void) { return g_aec_value; }
int cupVerifierAgcGain(void) { return g_agc_gain; }

// ---------------------------------------------------------------------------
// Mean R/G/B/luminance over the (clamped) ROI of an RGB565 frame.
// RGB565 is big-endian per pixel (esp32-camera to_bmp.c): pixel=(hi<<8)|lo,
// R bits 15..11, G 10..5, B 4..0.
// ---------------------------------------------------------------------------
static void clamp_roi(const camera_fb_t *fb, const cup_roi_t &roi,
                      int *rx, int *ry, int *rw, int *rh) {
  const int fw = fb->width, fh = fb->height;
  int x = roi.x, y = roi.y, w = roi.w, h = roi.h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= fw) x = fw - 1;
  if (y >= fh) y = fh - 1;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (x + w > fw) w = fw - x;
  if (y + h > fh) h = fh - y;
  *rx = x; *ry = y; *rw = w; *rh = h;
}

static bool roi_metric(const camera_fb_t *fb, const cup_roi_t &roi, cup_metric_t *out) {
  if (fb->format != PIXFORMAT_RGB565) return false;
  const uint8_t *buf = fb->buf;
  const int fw = fb->width;
  int rx, ry, rw, rh;
  clamp_roi(fb, roi, &rx, &ry, &rw, &rh);

  uint64_t accR = 0, accG = 0, accB = 0;
  uint32_t n = 0;
  for (int y = ry; y < ry + rh; y++) {
    const uint8_t *row = buf + (size_t)y * fw * 2;
    for (int x = rx; x < rx + rw; x++) {
      uint16_t px = ((uint16_t)row[x * 2] << 8) | row[x * 2 + 1];
      accR += ((px >> 11) & 0x1F) << 3;
      accG += ((px >> 5) & 0x3F) << 2;
      accB += (px & 0x1F) << 3;
      n++;
    }
  }
  if (n == 0) n = 1;
  out->r = (float)accR / n;
  out->g = (float)accG / n;
  out->b = (float)accB / n;
  out->y = 0.299f * out->r + 0.587f * out->g + 0.114f * out->b;
  return true;
}

// Crop ROI, box-downscale to the model input, unpack to RGB888, normalise to
// [0,1] and quantize to int8 into the input tensor. Must match ml preprocessing
// (Image.BOX resize + pixel/255).
static void preprocess_into_input(const camera_fb_t *fb, const cup_roi_t &roi) {
  const uint8_t *buf = fb->buf;
  const int fw = fb->width;
  int rx, ry, rw, rh;
  clamp_roi(fb, roi, &rx, &ry, &rw, &rh);

  const int outW = DRINK_MODEL_INPUT_W;
  const int outH = DRINK_MODEL_INPUT_H;
  const float in_scale = DRINK_MODEL_INPUT_SCALE;
  const int in_zp = DRINK_MODEL_INPUT_ZERO_POINT;
  int8_t *in = g_input->data.int8;

  for (int oy = 0; oy < outH; oy++) {
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
          uint16_t px = ((uint16_t)row[sx * 2] << 8) | row[sx * 2 + 1];
          accR += ((px >> 11) & 0x1F) << 3;
          accG += ((px >> 5) & 0x3F) << 2;
          accB += (px & 0x1F) << 3;
          n++;
        }
      }
      if (n == 0) n = 1;
      float rgb[3] = {(accR / n) / 255.0f, (accG / n) / 255.0f, (accB / n) / 255.0f};
      int idx = (oy * outW + ox) * DRINK_MODEL_INPUT_CH;
      for (int c = 0; c < DRINK_MODEL_INPUT_CH; c++) {
        int q = (int)lroundf(rgb[c] / in_scale) + in_zp;
        if (q < -128) q = -128;
        if (q > 127) q = 127;
        in[idx + c] = (int8_t)q;
      }
    }
  }
}

// Run one inference over the ROI; writes argmax class + softmax confidence.
static bool classify_drink(const camera_fb_t *fb, const cup_roi_t &roi, int *cls, float *conf) {
  if (!g_model_ready) return false;
  preprocess_into_input(fb, roi);
  if (g_interpreter->Invoke() != kTfLiteOk) return false;
  const float out_scale = DRINK_MODEL_OUTPUT_SCALE;
  const int out_zp = DRINK_MODEL_OUTPUT_ZERO_POINT;
  const int8_t *out = g_output->data.int8;
  int best = 0;
  float best_p = -1.0f;
  for (int i = 0; i < DRINK_MODEL_NUM_CLASSES; i++) {
    float p = (out[i] - out_zp) * out_scale;
    if (p > best_p) { best_p = p; best = i; }
  }
  *cls = best;
  *conf = best_p;
  return true;
}

static float fill_delta(const cup_metric_t &m, const cup_metric_t &base) {
  return (fabsf(m.r - base.r) + fabsf(m.g - base.g) + fabsf(m.b - base.b)) / 3.0f;
}

static void publish_live(const cup_metric_t &m) {
  portENTER_CRITICAL(&g_out_mux);
  if (g_live.y == 0 && g_live.r == 0) {
    g_live = m;
  } else {
    const float a = 0.4f;
    g_live.r = a * m.r + (1 - a) * g_live.r;
    g_live.g = a * m.g + (1 - a) * g_live.g;
    g_live.b = a * m.b + (1 - a) * g_live.b;
    g_live.y = a * m.y + (1 - a) * g_live.y;
  }
  portEXIT_CRITICAL(&g_out_mux);
}

static void publish_live_class(int cls, float conf) {
  g_live_class = cls;
  g_live_conf = conf;
}

static void commit_verdict(const cup_verdict_t &v) {
  portENTER_CRITICAL(&g_out_mux);
  g_last = v;
  g_verdict_seq = v.seq;
  portEXIT_CRITICAL(&g_out_mux);

  if (g_verdict_queue) {
    if (xQueueSend(g_verdict_queue, &v, 0) != pdTRUE) {
      cup_verdict_t discard;
      xQueueReceive(g_verdict_queue, &discard, 0);  // drop oldest
      xQueueSend(g_verdict_queue, &v, 0);
      g_dropped++;
    }
  }
  Serial.printf("[verify] verdict #%lu %s dispensed=%d drink=%s conf=%.2f fill_ms=%lu "
                "delta=%.1f base_y=%.1f final_y=%.1f\n",
                (unsigned long)v.seq, cup_result_name(v.result), v.dispensed,
                cup_drink_name(v.drink_class), v.drink_conf, (unsigned long)v.fill_ms,
                v.delta, v.baseline.y, v.final.y);
}

// Build and commit one verdict. The money axis (dispensed) is the baseline
// delta; the drink class comes from the CNN on the settled frame (caller passes
// the settled class, never the early dispense frame).
static void emit_verdict(bool dispensed, const cup_metric_t &base, const cup_metric_t &final_m,
                         int drink_class, float drink_conf, uint32_t fill_ms, uint32_t now) {
  cup_verdict_t v = {0};
  v.seq = g_verdict_seq + 1;
  v.ts_ms = now;
  v.dispensed = dispensed;
  v.baseline = base;
  v.final = final_m;
  v.delta = fill_delta(final_m, base);
  if (!dispensed) {
    v.drink_class = -1;
    v.drink_conf = 0.0f;
    v.fill_ms = 0;
    v.result = CUP_RESULT_NOT_DISPENSED;
  } else {
    v.drink_class = drink_class;
    v.drink_conf = drink_conf;
    v.fill_ms = fill_ms;
    v.result = CUP_RESULT_OK;
  }
  commit_verdict(v);
}

// ---------------------------------------------------------------------------
// Verifier task. Per loop: grab a frame, measure the ROI metric AND run the CNN,
// return the framebuffer ONCE (before any early-continue, so it never leaks),
// then advance the trigger-driven state machine on the cached metric+class.
// BASELINE averages frames into the baseline; WATCHING latches `dispensed` on the
// first sustained change (money axis) then waits for the content to settle and
// reads the CNN class. The window deadline is a wall-clock check that runs even
// when no frame arrives, so a camera stall can never wedge the verifier busy.
// ---------------------------------------------------------------------------
enum sub_t { SUB_IDLE, SUB_BASELINE, SUB_WATCHING };

static void verifier_task(void *arg) {
  (void)arg;
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(CUP_SAMPLE_INTERVAL_MS);
  const uint32_t reassert_every = 2000 / CUP_SAMPLE_INTERVAL_MS;
  uint32_t reassert_ctr = 0;

  sub_t sub = SUB_IDLE;
  cup_metric_t base = {0, 0, 0, 0};
  cup_metric_t base_acc = {0, 0, 0, 0};
  cup_metric_t last_m = {0, 0, 0, 0};
  cup_metric_t prev_m = {0, 0, 0, 0};
  bool have_last = false;
  int base_count = 0;
  uint32_t trigger_ms = 0;
  uint32_t watch_start_ms = 0;
  int settle_run = 0;
  int plateau_run = 0;
  bool dispensed = false;
  uint32_t fill_ms = 0;
  int last_class = -1;
  float last_conf = 0.0f;

  for (;;) {
    vTaskDelayUntil(&last_wake, period);

    if (g_fixexp_enabled && (++reassert_ctr % reassert_every == 0)) {
      apply_fixed_exposure();
    }

    if (sub == SUB_IDLE && g_start_request) {
      portENTER_CRITICAL(&g_cfg_mux);
      g_start_request = false;
      g_running = true;
      portEXIT_CRITICAL(&g_cfg_mux);
      sub = SUB_BASELINE;
      base_acc = (cup_metric_t){0, 0, 0, 0};
      base_count = 0;
      settle_run = 0;
      plateau_run = 0;
      dispensed = false;
      fill_ms = 0;
      have_last = false;
      trigger_ms = millis();
      Serial.println("[verify] start");
    }

    uint32_t now = millis();

    portENTER_CRITICAL(&g_cfg_mux);
    uint32_t window_ms = g_window_ms;
    uint16_t fill_thr = g_fill_delta;
    uint16_t settle_n = g_settle_frames;
    portEXIT_CRITICAL(&g_cfg_mux);

    // Wall-clock window guard (frame-independent — a stall can't wedge busy).
    if (sub != SUB_IDLE && (now - trigger_ms >= window_ms)) {
      cup_metric_t fm = have_last ? last_m : base;
      emit_verdict(dispensed, base, fm, dispensed ? last_class : -1,
                   dispensed ? last_conf : 0.0f, fill_ms, now);
      g_running = false;
      sub = SUB_IDLE;
      continue;
    }

    cup_roi_t roi = cupVerifierGetRoi();

    // --- frame work: grab, measure + classify, RETURN ONCE ---
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) continue;
    cup_metric_t m;
    bool okm = roi_metric(fb, roi, &m);
    int cls = -1;
    float conf = 0.0f;
    bool okc = okm && classify_drink(fb, roi, &cls, &conf);
    esp_camera_fb_return(fb);
    if (!okm) continue;

    publish_live(m);
    if (okc) publish_live_class(cls, conf);
    if (sub == SUB_IDLE) continue;

    last_m = m;
    have_last = true;
    if (okc) { last_class = cls; last_conf = conf; }

    if (sub == SUB_BASELINE) {
      base_acc.r += m.r; base_acc.g += m.g; base_acc.b += m.b; base_acc.y += m.y;
      if (++base_count >= CUP_BASELINE_FRAMES) {
        base.r = base_acc.r / base_count;
        base.g = base_acc.g / base_count;
        base.b = base_acc.b / base_count;
        base.y = base_acc.y / base_count;
        watch_start_ms = now;
        settle_run = 0;
        prev_m = m;
        sub = SUB_WATCHING;
        Serial.printf("[verify] baseline r=%.1f g=%.1f b=%.1f y=%.1f\n",
                      base.r, base.g, base.b, base.y);
      }
      continue;
    }

    // SUB_WATCHING ----------------------------------------------------------
    if (!dispensed) {
      // Phase A (money): wait for the first sustained change from baseline.
      if (fill_delta(m, base) >= fill_thr) {
        if (++settle_run >= settle_n) {
          dispensed = true;
          fill_ms = now - watch_start_ms;
          plateau_run = 0;
          prev_m = m;
          Serial.printf("[verify] dispensed at %lums (delta=%.1f)\n",
                        (unsigned long)fill_ms, fill_delta(m, base));
        }
      } else {
        settle_run = 0;
      }
    } else {
      // Phase B (drink): wait for the content to settle, then the latest CNN
      // class (last_class) is the drink type.
      float ft = fill_delta(m, prev_m);
      prev_m = m;
      if (ft < CUP_PLATEAU_EPS) {
        if (++plateau_run >= CUP_PLATEAU_FRAMES) {
          emit_verdict(true, base, m, last_class, last_conf, fill_ms, now);
          g_running = false;
          sub = SUB_IDLE;
          continue;
        }
      } else {
        plateau_run = 0;
      }
    }
  }
}

bool cupVerifierInit(const cup_roi_t *roi, bool enabled) {
  if (roi) cupVerifierSetRoi(roi);
  g_enabled = enabled;

  g_verdict_queue = xQueueCreate(CUP_EVENT_QUEUE_LEN, sizeof(cup_verdict_t));
  if (!g_verdict_queue) {
    Serial.println("[verify] failed to create verdict queue");
    return false;
  }

  // Load the drink classifier. Failure is non-fatal: the dispensed/not money
  // axis still works; only the drink type is unavailable.
  tflite::InitializeTarget();
  g_model = tflite::GetModel(g_drink_model);
  if (g_model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("[verify] model schema %lu != %d; CNN disabled\n",
                  (unsigned long)g_model->version(), TFLITE_SCHEMA_VERSION);
    return true;
  }

  static tflite::MicroMutableOpResolver<7> resolver;
  resolver.AddConv2D();
  resolver.AddDepthwiseConv2D();
  resolver.AddFullyConnected();
  resolver.AddSoftmax();
  resolver.AddReshape();
  resolver.AddMaxPool2D();
  resolver.AddMean();  // global average pooling lowers to Mean

  static tflite::MicroInterpreter static_interpreter(
      g_model, resolver, g_tensor_arena, CUP_TENSOR_ARENA_BYTES);
  g_interpreter = &static_interpreter;

  if (g_interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("[verify] AllocateTensors failed (arena too small?); CNN disabled");
    return true;
  }
  g_input = g_interpreter->input(0);
  g_output = g_interpreter->output(0);
  g_model_ready = true;
  Serial.printf("[verify] model ready, %d classes, arena used %u / %u bytes\n",
                DRINK_MODEL_NUM_CLASSES, (unsigned)g_interpreter->arena_used_bytes(),
                (unsigned)CUP_TENSOR_ARENA_BYTES);
  return true;
}

void cupVerifierStart(void) {
  xTaskCreatePinnedToCore(verifier_task, "cup_verify", 8192, nullptr, 4, nullptr, 1);
}
