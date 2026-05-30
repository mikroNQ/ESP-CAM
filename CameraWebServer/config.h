// config.h — compile-time defaults for the on-device LED detector + TCP reporter.
//
// Runtime values (API host/port, ROI, enable flag) are overridden from NVS
// (Preferences namespace "detcfg") and via the /detcfg HTTP endpoint. These
// defines are only the fallback used on first boot / factory state.
#pragma once

// ---------------------------------------------------------------------------
// TCP reporter — where to send the newline-delimited JSON events.
// ---------------------------------------------------------------------------
#define DET_DEFAULT_API_HOST ""    // empty => reporter idle until /detcfg sets it
#define DET_DEFAULT_API_PORT 9000

// ---------------------------------------------------------------------------
// Camera frame geometry the ROI is expressed against.
// We run the sensor in RGB565 @ QQVGA (160x120) on no-PSRAM boards.
// ---------------------------------------------------------------------------
#define DET_FRAME_W 160
#define DET_FRAME_H 120

// ---------------------------------------------------------------------------
// Region Of Interest over the scanner's LED line, in source-frame pixels.
// Default is a wide horizontal band across the vertical centre — tune via
// /detcfg?roi_x=..&roi_y=..&roi_w=..&roi_h=.. while watching /capture.
// ---------------------------------------------------------------------------
#define DET_DEFAULT_ROI_X 16
#define DET_DEFAULT_ROI_Y 40
#define DET_DEFAULT_ROI_W 128
#define DET_DEFAULT_ROI_H 40

// ---------------------------------------------------------------------------
// Detector behaviour.
// ---------------------------------------------------------------------------
#define DET_SAMPLE_INTERVAL_MS 25  // ~40 Hz inference cadence
#define DET_DEBOUNCE_COUNT     1   // identical classifications before committing
                                   // (1 = commit on first frame, ~25ms latency)
#define DET_CONF_THRESHOLD     0.60f  // min softmax confidence to accept a class
#define DET_DEFAULT_ENABLED    1   // detector task active by default

// ---------------------------------------------------------------------------
// Fixed exposure / gain / white-balance.
// The classifier is trained on frames captured with these LOCKED. If the sensor
// runs in auto, it brightens 'off' (merging it with the on-states) and its
// auto-white-balance neutralises the red/white colour difference — so deploy
// MUST reproduce the capture conditions. Applied at boot and re-asserted by the
// detector task (~2 s), because the sensor reverts to auto after a camera stall.
// Tunable at runtime via /detcfg?fixexp=..&aec_value=..&agc_gain=.. (saved to
// NVS) so values can change without reflashing.
// ---------------------------------------------------------------------------
#define DET_DEFAULT_FIXEXP    1     // 1 => hold the fixed exposure below
#define DET_DEFAULT_AEC_VALUE 1300  // manual exposure register (dataset value):
                                    // long enough to integrate the red scanner's
                                    // slow LED strobe into a steady reading.
#define DET_DEFAULT_AGC_GAIN  0     // manual gain (0 = lowest)

// ---------------------------------------------------------------------------
// TFLite-Micro tensor arena. Start generous, trim using arena_used_bytes()
// reported at boot. No PSRAM => this lives in DRAM (BSS), keep it small.
// ---------------------------------------------------------------------------
#define DET_TENSOR_ARENA_BYTES (40 * 1024)

// Event queue length (events buffered between detector and TCP reporter).
#define DET_EVENT_QUEUE_LEN 32

// ---------------------------------------------------------------------------
// NVS (Preferences) persistence — namespace + keys shared by the sketch (load)
// and the /detcfg HTTP handler (save). Keep keys <= 15 chars (NVS limit).
// ---------------------------------------------------------------------------
#define DET_NVS_NS    "detcfg"
#define DET_NVS_HOST  "host"
#define DET_NVS_PORT  "port"
#define DET_NVS_ROI_X "roi_x"
#define DET_NVS_ROI_Y "roi_y"
#define DET_NVS_ROI_W "roi_w"
#define DET_NVS_ROI_H "roi_h"
#define DET_NVS_EN    "enabled"
#define DET_NVS_FIXEXP  "fixexp"
#define DET_NVS_AECVAL  "aecval"
#define DET_NVS_AGCGAIN "agcgain"
