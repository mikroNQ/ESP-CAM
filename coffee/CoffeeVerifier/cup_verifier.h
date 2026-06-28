// cup_verifier.h — on-device coffee-dispense verifier.
//
// Watches an ROI over a cup under the coffee-machine spout. A FreeRTOS task
// continuously samples the ROI's mean colour/luminance (cheap, no ML) so the
// live values are always inspectable via /metrics. On an external trigger
// (HTTP /verify, emulating the payment terminal) it:
//   1. snapshots a BASELINE of the (expected-empty) cup,
//   2. opens a verification window and watches for a sustained change from that
//      baseline -> "a drink was poured" (robust to lighting; the verdict is
//      "contents changed", not "cup is full now"),
//   3. classifies milk vs no-milk from the settled surface luminance (heuristic;
//      an optional CNN can replace classify_drink() in stage 2),
//   4. emits a verdict onto a queue the TCP reporter drains, and stores it as
//      the "last verdict" for /status and /metrics.
//
// The window ALWAYS terminates: a timeout emits NOT_DISPENSED. The verifier
// never blocks the camera or the web server.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the trigger said the customer ordered, on the milk axis.
typedef enum {
  CUP_EXPECT_ANY = 0,     // no expectation given: only verify dispensed/not
  CUP_EXPECT_MILK = 1,    // latte / cappuccino / flat white ...
  CUP_EXPECT_NOMILK = 2,  // espresso / americano / black ...
} cup_expect_t;

// Final verdict of one verification.
typedef enum {
  CUP_RESULT_OK = 0,             // dispensed and matches the ordered drink
  CUP_RESULT_NOT_DISPENSED = 1,  // window elapsed, nothing was poured -> dispute
  CUP_RESULT_WRONG_DRINK = 2,    // poured, but milk axis mismatches the order
} cup_result_t;

// Region of interest over the cup mouth, in source-frame (CUP_FRAME_W/H) pixels.
typedef struct {
  uint16_t x;
  uint16_t y;
  uint16_t w;
  uint16_t h;
} cup_roi_t;

// Mean ROI colour for one sample. y is luminance (0..255).
typedef struct {
  float r;
  float g;
  float b;
  float y;
} cup_metric_t;

// One verdict, enqueued for the TCP reporter and kept as the "last verdict".
typedef struct {
  uint32_t seq;          // monotonic verification id (also bumps on each verdict)
  uint32_t ts_ms;        // millis() when the verdict was committed
  cup_result_t result;
  cup_expect_t expected;
  bool dispensed;        // was a sustained change from baseline observed
  bool observed_milk;    // milk-axis classification of the settled drink
  uint32_t fill_ms;      // time from trigger to "filled" (0 if never)
  cup_metric_t baseline; // ROI metric snapshot at trigger
  cup_metric_t final;    // ROI metric at the moment of decision
  float delta;           // |final.y - baseline.y| at decision (evidence)
} cup_verdict_t;

const char *cup_result_name(cup_result_t r);
const char *cup_expect_name(cup_expect_t e);

// Initialise verifier state. Call after the camera is initialised. `roi` and
// `enabled` are the runtime config loaded from NVS. Returns true on success.
bool cupVerifierInit(const cup_roi_t *roi, bool enabled);

// Start the verifier FreeRTOS task (pinned to core 1).
void cupVerifierStart(void);

// Kick off a verification. Returns false if one is already running (caller
// should reply HTTP 409) or the verifier is disabled. `expected` is the milk
// axis from the trigger. The verdict arrives asynchronously (TCP + last verdict);
// poll cupVerifierVerdictSeq() to wait for completion.
bool cupVerifierStartVerification(cup_expect_t expected);

// True while a verification window is open.
bool cupVerifierBusy(void);

// Monotonic counter, incremented once per completed verdict. A /verify handler
// can snapshot it, start a verification, then poll until it changes to block for
// the result (with its own timeout).
uint32_t cupVerifierVerdictSeq(void);

// Copy the most recent verdict (valid once seq > 0). Returns false if none yet.
bool cupVerifierLastVerdict(cup_verdict_t *out);

// Latest live ROI metric (smoothed), for /metrics live tuning.
cup_metric_t cupVerifierLiveMetric(void);

// Runtime config updates (from /cupcfg).
void cupVerifierSetEnabled(bool enabled);
bool cupVerifierIsEnabled(void);
void cupVerifierSetRoi(const cup_roi_t *roi);
cup_roi_t cupVerifierGetRoi(void);

// Decision thresholds (saved to NVS by the handler; applied live here).
void cupVerifierSetParams(uint32_t window_ms, uint16_t fill_delta,
                          uint16_t settle_frames, uint16_t milk_luma);
uint32_t cupVerifierWindowMs(void);
uint16_t cupVerifierFillDelta(void);
uint16_t cupVerifierSettleFrames(void);
uint16_t cupVerifierMilkLuma(void);

// Fixed-exposure lock so metrics stay comparable frame-to-frame (see config.h).
// Re-asserted by the verifier task because the sensor reverts to auto after a
// camera stall. Applies immediately.
void cupVerifierSetFixedExposure(bool enabled, int aec_value, int agc_gain);
bool cupVerifierFixedExpEnabled(void);
int cupVerifierAecValue(void);
int cupVerifierAgcGain(void);

// Queue of cup_verdict_t consumed by the TCP reporter. Valid after init.
QueueHandle_t cupVerifierVerdictQueue(void);

// Returns and clears the count of verdicts dropped due to queue overflow.
uint32_t cupVerifierTakeDropped(void);

#ifdef __cplusplus
}
#endif
