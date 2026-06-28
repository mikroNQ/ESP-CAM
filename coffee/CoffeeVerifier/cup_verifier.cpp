// cup_verifier.cpp — see cup_verifier.h.
#include "cup_verifier.h"

#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "esp_camera.h"

#include "config.h"

namespace {
// ---- Runtime config (guarded by g_cfg_mux for cross-task access) ----------
portMUX_TYPE g_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
cup_roi_t g_roi = {CUP_DEFAULT_ROI_X, CUP_DEFAULT_ROI_Y, CUP_DEFAULT_ROI_W, CUP_DEFAULT_ROI_H};
volatile bool g_enabled = false;

uint32_t g_window_ms = CUP_DEFAULT_WINDOW_MS;
uint16_t g_fill_delta = CUP_DEFAULT_FILL_DELTA;
uint16_t g_settle_frames = CUP_DEFAULT_SETTLE_FRAMES;
uint16_t g_milk_luma = CUP_DEFAULT_MILK_LUMA;

// Fixed exposure/gain/white-balance lock; re-asserted by the task (the sensor
// reverts to auto after a camera stall).
volatile bool g_fixexp_enabled = false;
volatile int g_aec_value = CUP_DEFAULT_AEC_VALUE;
volatile int g_agc_gain = CUP_DEFAULT_AGC_GAIN;

// ---- Shared outputs (guarded by g_out_mux) --------------------------------
portMUX_TYPE g_out_mux = portMUX_INITIALIZER_UNLOCKED;
cup_metric_t g_live = {0, 0, 0, 0};   // latest smoothed ROI metric, for /metrics
cup_verdict_t g_last = {0};           // last completed verdict (seq 0 => none)
volatile uint32_t g_verdict_seq = 0;  // bumps once per completed verdict

// ---- Trigger handshake (set by HTTP task, consumed by verifier task) ------
volatile bool g_start_request = false;
cup_expect_t g_pending_expect = CUP_EXPECT_ANY;
volatile bool g_running = false;      // a verification window is open

QueueHandle_t g_verdict_queue = nullptr;
volatile uint32_t g_dropped = 0;
}  // namespace

const char *cup_result_name(cup_result_t r) {
  switch (r) {
    case CUP_RESULT_OK: return "ok";
    case CUP_RESULT_NOT_DISPENSED: return "not_dispensed";
    case CUP_RESULT_WRONG_DRINK: return "wrong_drink";
    default: return "unknown";
  }
}

const char *cup_expect_name(cup_expect_t e) {
  switch (e) {
    case CUP_EXPECT_ANY: return "any";
    case CUP_EXPECT_MILK: return "milk";
    case CUP_EXPECT_NOMILK: return "nomilk";
    default: return "unknown";
  }
}

QueueHandle_t cupVerifierVerdictQueue(void) { return g_verdict_queue; }

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

void cupVerifierSetParams(uint32_t window_ms, uint16_t fill_delta,
                          uint16_t settle_frames, uint16_t milk_luma) {
  portENTER_CRITICAL(&g_cfg_mux);
  if (window_ms) g_window_ms = window_ms;
  g_fill_delta = fill_delta;
  if (settle_frames) g_settle_frames = settle_frames;
  g_milk_luma = milk_luma;
  portEXIT_CRITICAL(&g_cfg_mux);
}

uint32_t cupVerifierWindowMs(void) { return g_window_ms; }
uint16_t cupVerifierFillDelta(void) { return g_fill_delta; }
uint16_t cupVerifierSettleFrames(void) { return g_settle_frames; }
uint16_t cupVerifierMilkLuma(void) { return g_milk_luma; }

bool cupVerifierBusy(void) { return g_running || g_start_request; }
uint32_t cupVerifierVerdictSeq(void) { return g_verdict_seq; }

cup_metric_t cupVerifierLiveMetric(void) {
  portENTER_CRITICAL(&g_out_mux);
  cup_metric_t m = g_live;
  portEXIT_CRITICAL(&g_out_mux);
  return m;
}

bool cupVerifierLastVerdict(cup_verdict_t *out) {
  if (!out) return false;
  portENTER_CRITICAL(&g_out_mux);
  bool have = g_verdict_seq > 0;
  if (have) *out = g_last;
  portEXIT_CRITICAL(&g_out_mux);
  return have;
}

bool cupVerifierStartVerification(cup_expect_t expected) {
  bool accepted = false;
  portENTER_CRITICAL(&g_cfg_mux);
  bool enabled = g_enabled;
  portEXIT_CRITICAL(&g_cfg_mux);
  if (!enabled) return false;
  if (cupVerifierBusy()) return false;
  portENTER_CRITICAL(&g_cfg_mux);
  if (!g_start_request && !g_running) {
    g_pending_expect = expected;
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
  s->set_exposure_ctrl(s, 0);  // AEC off => manual exposure
  s->set_aec2(s, 0);
  s->set_aec_value(s, g_aec_value);
  s->set_gain_ctrl(s, 0);      // AGC off => manual gain
  s->set_agc_gain(s, g_agc_gain);
  s->set_whitebal(s, 0);       // AWB off => fixed white balance
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
// Compute the mean R/G/B/luminance over the (clamped) ROI of an RGB565 frame.
// RGB565 byte order matches esp32-camera (to_bmp.c): big-endian per pixel, so
// pixel = (hi<<8)|lo with R in bits 15..11, G 10..5, B 4..0.
// ---------------------------------------------------------------------------
static bool roi_metric(const camera_fb_t *fb, const cup_roi_t &roi, cup_metric_t *out) {
  if (fb->format != PIXFORMAT_RGB565) return false;
  const uint8_t *buf = fb->buf;
  const int fw = fb->width;
  const int fh = fb->height;

  int rx = roi.x, ry = roi.y, rw = roi.w, rh = roi.h;
  if (rx < 0) rx = 0;
  if (ry < 0) ry = 0;
  if (rx >= fw) rx = fw - 1;
  if (ry >= fh) ry = fh - 1;
  if (rw < 1) rw = 1;
  if (rh < 1) rh = 1;
  if (rx + rw > fw) rw = fw - rx;
  if (ry + rh > fh) rh = fh - ry;

  uint64_t accR = 0, accG = 0, accB = 0;
  uint32_t n = 0;
  for (int y = ry; y < ry + rh; y++) {
    const uint8_t *row = buf + (size_t)y * fw * 2;
    for (int x = rx; x < rx + rw; x++) {
      uint8_t hi = row[x * 2];
      uint8_t lo = row[x * 2 + 1];
      uint16_t px = ((uint16_t)hi << 8) | lo;
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

// Milk-axis classification of the settled drink (STAGE 1: heuristic). A milky
// drink (latte, cappuccino) is lighter than black coffee, so a luminance
// threshold separates them. This is the seam for STAGE 2: replace the body with
// a TFLite-Micro CNN inference over the ROI (see coffee/ml/) — the verdict logic
// upstream is unchanged.
static bool classify_milk(const cup_metric_t &settled, uint16_t milk_luma) {
  return settled.y >= milk_luma;
}

// Mean absolute per-channel change of `m` from `base` — the fill signal. Using
// all channels (not just luminance) catches a colour shift even when brightness
// barely moves (e.g. beige latte poured into a white cup).
static float fill_delta(const cup_metric_t &m, const cup_metric_t &base) {
  return (fabsf(m.r - base.r) + fabsf(m.g - base.g) + fabsf(m.b - base.b)) / 3.0f;
}

static void publish_live(const cup_metric_t &m) {
  portENTER_CRITICAL(&g_out_mux);
  // Light EMA so /metrics is readable but still responsive.
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
  Serial.printf("[verify] verdict #%lu %s dispensed=%d milk=%d fill_ms=%lu "
                "delta=%.1f base_y=%.1f final_y=%.1f expect=%s\n",
                (unsigned long)v.seq, cup_result_name(v.result), v.dispensed,
                v.observed_milk, (unsigned long)v.fill_ms, v.delta,
                v.baseline.y, v.final.y, cup_expect_name(v.expected));
}

// Build and commit one verdict. The money axis (dispensed) and the drink axis
// (milk) are independent: `final_m` is the frame the milk classification reads —
// the caller passes the SETTLED frame, never the early dispense frame.
static void emit_verdict(cup_expect_t expect, bool dispensed, const cup_metric_t &base,
                         const cup_metric_t &final_m, uint32_t fill_ms,
                         uint16_t milk_luma, uint32_t now) {
  cup_verdict_t v = {0};
  v.seq = g_verdict_seq + 1;
  v.ts_ms = now;
  v.expected = expect;
  v.dispensed = dispensed;
  v.baseline = base;
  v.final = final_m;
  v.delta = fill_delta(final_m, base);
  if (!dispensed) {
    v.observed_milk = false;
    v.fill_ms = 0;
    v.result = CUP_RESULT_NOT_DISPENSED;
  } else {
    v.observed_milk = classify_milk(final_m, milk_luma);
    v.fill_ms = fill_ms;
    if (expect == CUP_EXPECT_ANY || ((expect == CUP_EXPECT_MILK) == v.observed_milk)) {
      v.result = CUP_RESULT_OK;
    } else {
      v.result = CUP_RESULT_WRONG_DRINK;
    }
  }
  commit_verdict(v);
}

// ---------------------------------------------------------------------------
// Verifier task. One verification runs as:
//   BASELINE  — average CUP_BASELINE_FRAMES frames into the baseline snapshot
//   WATCHING  — two independent jobs over one window:
//     (money) latch `dispensed` on the FIRST sustained change from baseline,
//     (drink) then keep sampling until the content SETTLES (plateau) and read
//             the milk axis from that settled frame; commit on settle or window end.
// The window deadline is checked on WALL-CLOCK time every loop — independent of
// whether a frame arrived — so a camera stall can never wedge the verifier busy.
// Outside a verification it just samples the ROI to keep the live metric fresh.
// ---------------------------------------------------------------------------
enum sub_t { SUB_IDLE, SUB_BASELINE, SUB_WATCHING };

static void verifier_task(void *arg) {
  (void)arg;
  TickType_t last_wake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(CUP_SAMPLE_INTERVAL_MS);
  const uint32_t reassert_every = 2000 / CUP_SAMPLE_INTERVAL_MS;
  uint32_t reassert_ctr = 0;

  sub_t sub = SUB_IDLE;
  cup_expect_t expect = CUP_EXPECT_ANY;
  cup_metric_t base = {0, 0, 0, 0};
  cup_metric_t base_acc = {0, 0, 0, 0};
  cup_metric_t last_m = {0, 0, 0, 0};  // most recent good sample (verdict's final)
  cup_metric_t prev_m = {0, 0, 0, 0};  // previous good sample (for plateau)
  bool have_last = false;
  int base_count = 0;
  uint32_t trigger_ms = 0;             // when the trigger was picked up
  uint32_t watch_start_ms = 0;         // when baseline completed (window origin)
  int settle_run = 0;                  // consecutive over-threshold frames (Phase A)
  int plateau_run = 0;                 // consecutive settled frames (Phase B)
  bool dispensed = false;
  uint32_t fill_ms = 0;

  for (;;) {
    vTaskDelayUntil(&last_wake, period);

    if (g_fixexp_enabled && (++reassert_ctr % reassert_every == 0)) {
      apply_fixed_exposure();
    }

    // Pick up a pending trigger only when idle.
    if (sub == SUB_IDLE && g_start_request) {
      portENTER_CRITICAL(&g_cfg_mux);
      expect = g_pending_expect;
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
      Serial.printf("[verify] start, expect=%s\n", cup_expect_name(expect));
    }

    uint32_t now = millis();

    // Snapshot decision thresholds once per loop.
    portENTER_CRITICAL(&g_cfg_mux);
    uint32_t window_ms = g_window_ms;
    uint16_t fill_thr = g_fill_delta;
    uint16_t settle_n = g_settle_frames;
    uint16_t milk_luma = g_milk_luma;
    portEXIT_CRITICAL(&g_cfg_mux);

    // ---- Wall-clock window guard (runs even when no frame is available) ----
    // A stalled / wrong-format camera must not keep the window open forever.
    if (sub != SUB_IDLE && (now - trigger_ms >= window_ms)) {
      cup_metric_t final_m = have_last ? last_m : base;
      emit_verdict(expect, dispensed, base, final_m, fill_ms, milk_luma, now);
      g_running = false;
      sub = SUB_IDLE;
      continue;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) continue;
    cup_metric_t m;
    bool ok = roi_metric(fb, cupVerifierGetRoi(), &m);
    esp_camera_fb_return(fb);  // return promptly; never held across work
    if (!ok) continue;

    publish_live(m);
    if (sub == SUB_IDLE) continue;

    last_m = m;
    have_last = true;

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
      // Phase B (drink): wait for the content to settle, then classify milk.
      float ft = fill_delta(m, prev_m);
      prev_m = m;
      if (ft < CUP_PLATEAU_EPS) {
        if (++plateau_run >= CUP_PLATEAU_FRAMES) {
          emit_verdict(expect, true, base, m, fill_ms, milk_luma, now);
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
  Serial.println("[verify] init ok");
  return true;
}

void cupVerifierStart(void) {
  xTaskCreatePinnedToCore(verifier_task, "cup_verify", 8192, nullptr, 4, nullptr, 1);
}
