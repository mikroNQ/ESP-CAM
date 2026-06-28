// cup_verifier.h — on-device coffee-dispense verifier.
//
// Watches an ROI over a cup under the coffee-machine spout. A FreeRTOS task
// continuously samples the ROI's mean colour/luminance (cheap) AND runs a tiny
// on-device CNN (TFLite-Micro, drink_model.h) that classifies the drink in the
// cup — so the live metric AND the live drink class are always inspectable via
// /metrics. On an external trigger (HTTP /verify, emulating the payment terminal)
// it:
//   1. snapshots a BASELINE of the (expected-empty) cup,
//   2. opens a verification window and watches for a sustained change from that
//      baseline -> "a drink was poured" (the money axis: robust baseline delta,
//      NOT the CNN — a misclassification must never flip the financial verdict),
//   3. waits for the content to SETTLE, then reads the CNN class of the settled
//      frame as the drink type,
//   4. emits a verdict onto a queue the TCP reporter drains, and stores it as
//      the "last verdict" for /status and /metrics.
//
// The window ALWAYS terminates: a timeout emits NOT_DISPENSED. Only the verifier
// task ever calls the interpreter, so no lock is needed around inference.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Final verdict of one verification (money axis only; drink type is reported
// separately as drink_class — expected-vs-observed matching is a later pass).
typedef enum {
  CUP_RESULT_OK = 0,             // a drink was dispensed (see drink_class)
  CUP_RESULT_NOT_DISPENSED = 1,  // window elapsed, nothing was poured -> dispute
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
  bool dispensed;        // was a sustained change from baseline observed
  int drink_class;       // CNN class index of the settled drink (-1 if none)
  float drink_conf;      // softmax confidence of that class
  uint32_t fill_ms;      // time from trigger to "filled" (0 if never)
  cup_metric_t baseline; // ROI metric snapshot at trigger
  cup_metric_t final;    // ROI metric at the moment of decision
  float delta;           // |final.y - baseline.y| at decision (evidence)
} cup_verdict_t;

const char *cup_result_name(cup_result_t r);
// Human-readable class name (from drink_model.h) or "none"/"?" out of range.
const char *cup_drink_name(int class_idx);
int cup_drink_num_classes(void);

// Initialise verifier state + load the TFLite-Micro model. Call after the camera
// is initialised. `roi`/`enabled` are the runtime config from NVS. Returns true
// even if the model fails to load (verifier still does the dispensed/not axis);
// check cupVerifierModelReady() for the CNN.
bool cupVerifierInit(const cup_roi_t *roi, bool enabled);
bool cupVerifierModelReady(void);

// Start the verifier FreeRTOS task (pinned to core 1).
void cupVerifierStart(void);

// Kick off a verification. Returns false if one is already running (caller
// should reply HTTP 409) or the verifier is disabled. The verdict arrives
// asynchronously (TCP + last verdict); poll cupVerifierVerdictSeq() to wait.
bool cupVerifierStartVerification(void);

bool cupVerifierBusy(void);
uint32_t cupVerifierVerdictSeq(void);
bool cupVerifierLastVerdict(cup_verdict_t *out);

// Latest live ROI metric and live CNN class (smoothed / most-recent), for the
// /metrics live-tuning + on-device sanity readout.
cup_metric_t cupVerifierLiveMetric(void);
int cupVerifierLiveClass(void);      // -1 until the first inference
float cupVerifierLiveClassConf(void);

// Runtime config updates (from /cupcfg).
void cupVerifierSetEnabled(bool enabled);
bool cupVerifierIsEnabled(void);
void cupVerifierSetRoi(const cup_roi_t *roi);
cup_roi_t cupVerifierGetRoi(void);

// Decision thresholds (saved to NVS by the handler; applied live here).
void cupVerifierSetParams(uint32_t window_ms, uint16_t fill_delta, uint16_t settle_frames);
uint32_t cupVerifierWindowMs(void);
uint16_t cupVerifierFillDelta(void);
uint16_t cupVerifierSettleFrames(void);

// Fixed-exposure lock so metrics + inference stay comparable frame-to-frame.
// Re-asserted by the verifier task (the sensor reverts to auto after a stall).
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
