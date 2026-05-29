// led_detector.h — on-device TFLite-Micro classifier for the scanner LED line.
//
// Runs a tiny RGB CNN over a configurable ROI of each RGB565 camera frame and
// classifies the LED line state (OFF / RED_ON / WHITE_ON). A debounced state
// machine times how long each state lasts and emits transition events (with
// millis() timestamps and durations) onto a FreeRTOS queue that the TCP
// reporter drains.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Classifier output classes. Index order MUST match the trained model
// (see ml/train.py CLASS_NAMES and led_model.h).
typedef enum {
  LED_STATE_OFF = 0,
  LED_STATE_RED_ON = 1,
  LED_STATE_WHITE_ON = 2,
  LED_STATE_COUNT = 3,
} led_state_t;

// Region of interest over the LED line, in source-frame (DET_FRAME_W/H) pixels.
typedef struct {
  uint16_t x;
  uint16_t y;
  uint16_t w;
  uint16_t h;
} led_roi_t;

// One classifier transition, enqueued for the TCP reporter.
typedef struct {
  uint32_t ts_ms;     // millis() at the moment the new state was committed
  led_state_t from;   // previous committed state
  led_state_t to;     // newly committed state
  uint32_t dur_ms;    // how long `from` lasted before this transition
  float conf;         // softmax confidence of the new state
} led_event_t;

// Returns the human-readable name of a state ("off"/"red_on"/"white_on").
const char *led_state_name(led_state_t s);

// Initialise the TFLite-Micro interpreter and detector state. Must be called
// after the camera is initialised. `roi` and `enabled` are the runtime config
// loaded from NVS. Returns true on success (model loaded, arena allocated).
bool ledDetectorInit(const led_roi_t *roi, bool enabled);

// Start the detector FreeRTOS task (pinned to core 1).
void ledDetectorStart(void);

// Runtime config updates (called from the /detcfg HTTP handler).
void ledDetectorSetEnabled(bool enabled);
void ledDetectorSetRoi(const led_roi_t *roi);
led_roi_t ledDetectorGetRoi(void);
bool ledDetectorIsEnabled(void);

// Current observed state and confidence (for /status).
led_state_t ledDetectorCurrentState(void);
float ledDetectorCurrentConfidence(void);

// Queue of led_event_t consumed by the TCP reporter. Valid after init.
QueueHandle_t ledDetectorEventQueue(void);

// Returns and clears the count of events dropped due to queue overflow (used by
// the TCP reporter to emit a "dropped" gap notice on reconnect).
uint32_t ledDetectorTakeDropped(void);

#ifdef __cplusplus
}
#endif
