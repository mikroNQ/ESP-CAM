// config.h — compile-time defaults for the coffee-cup verifier + TCP reporter.
//
// The verifier watches a cup under the coffee-machine spout. On an external
// trigger (HTTP /verify, emulating the payment terminal) it opens a time window
// and decides whether the machine actually dispensed a drink, by detecting a
// change in the ROI relative to a baseline frame captured at trigger time.
//
// Runtime values (TCP endpoint, ROI, thresholds, exposure) are overridden from
// NVS (Preferences namespace "cupcfg") and via the /cupcfg HTTP endpoint. These
// defines are only the fallback used on first boot / factory state.
#pragma once

// ---------------------------------------------------------------------------
// TCP reporter — where to send the newline-delimited JSON verdicts.
// ---------------------------------------------------------------------------
#define CUP_DEFAULT_API_HOST ""    // empty => reporter idle until /cupcfg sets it
#define CUP_DEFAULT_API_PORT 9000

// ---------------------------------------------------------------------------
// Camera frame geometry the ROI is expressed against.
// RGB565 @ QQVGA (160x120): the verifier needs raw pixels; /stream and /capture
// JPEG-encode this buffer on the fly.
// ---------------------------------------------------------------------------
#define CUP_FRAME_W 160
#define CUP_FRAME_H 120

// ---------------------------------------------------------------------------
// Region Of Interest over the cup mouth (top-down view), in source-frame pixels.
// Default is a square in the frame centre — tune via
// /cupcfg?roi_x=..&roi_y=..&roi_w=..&roi_h=.. while watching /capture?roi=1.
// ---------------------------------------------------------------------------
#define CUP_DEFAULT_ROI_X 50
#define CUP_DEFAULT_ROI_Y 30
#define CUP_DEFAULT_ROI_W 60
#define CUP_DEFAULT_ROI_H 60

// ---------------------------------------------------------------------------
// Verifier behaviour.
// ---------------------------------------------------------------------------
#define CUP_SAMPLE_INTERVAL_MS 50   // ~20 Hz metric sampling cadence
#define CUP_DEFAULT_ENABLED    1    // verifier task active by default

// Verification window: how long after a trigger we wait for the machine to pour.
// Brewing + pouring typically takes 20-40 s; the window MUST always end (a
// timeout emits a NOT_DISPENSED verdict — the verifier never hangs).
#define CUP_DEFAULT_WINDOW_MS  30000

// Baseline: number of frames averaged at trigger time to snapshot the empty cup.
#define CUP_BASELINE_FRAMES    4

// "Something was poured" = the ROI luminance changed from baseline by at least
// this much (0..255), and stayed changed for CUP_DEFAULT_SETTLE_FRAMES samples
// (so a hand placing the cup or a transient splash doesn't false-trigger). The
// verdict is "contents CHANGED during the window", not "cup looks full now" — a
// pre-existing full cup at trigger time therefore does NOT count as dispensed.
#define CUP_DEFAULT_FILL_DELTA 25
#define CUP_DEFAULT_SETTLE_FRAMES 6

// After a drink is detected (sustained change), the verifier keeps sampling
// until the content SETTLES before reading the milk axis — so a latte that pours
// dark espresso first and milk seconds later is classified from the final, not
// the early, frame. Settled = frame-to-frame mean channel change stays below
// CUP_PLATEAU_EPS for CUP_PLATEAU_FRAMES samples (~1 s), or the window ends.
#define CUP_PLATEAU_EPS    4.0f
#define CUP_PLATEAU_FRAMES 20

// Milk vs no-milk heuristic (stage 1, no ML): a settled milky drink (latte,
// cappuccino) is lighter than black coffee. If the filled-surface luminance is
// above this threshold the drink is classified "with milk". Scene-dependent —
// tune against /metrics; the optional CNN (stage 2) supersedes this.
#define CUP_DEFAULT_MILK_LUMA  110

// ---------------------------------------------------------------------------
// Fixed exposure / gain / white-balance.
// Stable metrics require a LOCKED sensor: in auto mode the AEC re-brightens a
// dark coffee surface back toward the empty-cup level (killing the delta) and
// auto-white-balance drifts the colour. Applied at boot and re-asserted by the
// verifier task (~2 s), because the sensor reverts to auto after a camera stall.
// Tunable at runtime via /cupcfg?fixexp=..&aec_value=..&agc_gain=.. (saved to
// NVS) so values can change without reflashing.
// ---------------------------------------------------------------------------
#define CUP_DEFAULT_FIXEXP    1     // 1 => hold the fixed exposure below
#define CUP_DEFAULT_AEC_VALUE 600   // manual exposure register; coffee scenes are
                                    // usually brighter than the strobed scanner,
                                    // so a shorter default than the LED project.
#define CUP_DEFAULT_AGC_GAIN  0     // manual gain (0 = lowest)

// Event queue length (verdicts buffered between verifier and TCP reporter).
#define CUP_EVENT_QUEUE_LEN 8

// ---------------------------------------------------------------------------
// NVS (Preferences) persistence — namespace + keys shared by the sketch (load)
// and the /cupcfg HTTP handler (save). Keep keys <= 15 chars (NVS limit).
// ---------------------------------------------------------------------------
#define CUP_NVS_NS       "cupcfg"
#define CUP_NVS_HOST     "host"
#define CUP_NVS_PORT     "port"
#define CUP_NVS_ROI_X    "roi_x"
#define CUP_NVS_ROI_Y    "roi_y"
#define CUP_NVS_ROI_W    "roi_w"
#define CUP_NVS_ROI_H    "roi_h"
#define CUP_NVS_EN       "enabled"
#define CUP_NVS_FIXEXP   "fixexp"
#define CUP_NVS_AECVAL   "aecval"
#define CUP_NVS_AGCGAIN  "agcgain"
#define CUP_NVS_WINDOW   "window"
#define CUP_NVS_FILLDELTA "filldelta"
#define CUP_NVS_SETTLE   "settle"
#define CUP_NVS_MILKLUMA "milkluma"
