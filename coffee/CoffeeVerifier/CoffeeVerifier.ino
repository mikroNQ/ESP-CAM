#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <Preferences.h>

// mDNS hostname: reach the camera at http://<MDNS_HOSTNAME>.local/ regardless
// of the DHCP-assigned IP.
#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "coffeecam"
#endif

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"
#include "config.h"
#include "cup_verifier.h"
#include "tcp_reporter.h"

void startCameraServer();
void setupLedFlash();

// Verifier/reporter runtime config, loaded from NVS in setup().
static cup_roi_t cup_roi = {CUP_DEFAULT_ROI_X, CUP_DEFAULT_ROI_Y, CUP_DEFAULT_ROI_W, CUP_DEFAULT_ROI_H};
static bool cup_enabled = CUP_DEFAULT_ENABLED;
static char cup_host[64] = CUP_DEFAULT_API_HOST;
static uint16_t cup_port = CUP_DEFAULT_API_PORT;
static bool cup_fixexp = CUP_DEFAULT_FIXEXP;
static uint16_t cup_aecval = CUP_DEFAULT_AEC_VALUE;
static uint8_t cup_agcgain = CUP_DEFAULT_AGC_GAIN;
static uint32_t cup_window = CUP_DEFAULT_WINDOW_MS;
static uint16_t cup_filldelta = CUP_DEFAULT_FILL_DELTA;
static uint16_t cup_settle = CUP_DEFAULT_SETTLE_FRAMES;
static uint16_t cup_milkluma = CUP_DEFAULT_MILK_LUMA;

static void loadVerifierConfig() {
  Preferences p;
  if (!p.begin(CUP_NVS_NS, true)) {  // read-only; absent namespace => defaults
    return;
  }
  String h = p.getString(CUP_NVS_HOST, cup_host);
  strncpy(cup_host, h.c_str(), sizeof(cup_host) - 1);
  cup_host[sizeof(cup_host) - 1] = '\0';
  cup_port = p.getUShort(CUP_NVS_PORT, cup_port);
  cup_roi.x = p.getUShort(CUP_NVS_ROI_X, cup_roi.x);
  cup_roi.y = p.getUShort(CUP_NVS_ROI_Y, cup_roi.y);
  cup_roi.w = p.getUShort(CUP_NVS_ROI_W, cup_roi.w);
  cup_roi.h = p.getUShort(CUP_NVS_ROI_H, cup_roi.h);
  cup_enabled = p.getBool(CUP_NVS_EN, cup_enabled);
  cup_fixexp = p.getBool(CUP_NVS_FIXEXP, cup_fixexp);
  cup_aecval = p.getUShort(CUP_NVS_AECVAL, cup_aecval);
  cup_agcgain = p.getUChar(CUP_NVS_AGCGAIN, cup_agcgain);
  cup_window = p.getULong(CUP_NVS_WINDOW, cup_window);
  cup_filldelta = p.getUShort(CUP_NVS_FILLDELTA, cup_filldelta);
  cup_settle = p.getUShort(CUP_NVS_SETTLE, cup_settle);
  cup_milkluma = p.getUShort(CUP_NVS_MILKLUMA, cup_milkluma);
  p.end();
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  // reset_reason 9=BROWNOUT (weak supply!), 6/7=WDT, 4=PANIC, 1=POWERON.
  Serial.printf("[boot] reset_reason=%d, free heap=%u\n",
                (int)esp_reset_reason(), (unsigned)esp_get_free_heap_size());

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  // RGB565 @ QQVGA: the verifier needs raw pixels; /stream and /capture
  // JPEG-encode this buffer on the fly.
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QQVGA;  // 160x120, matches CUP_FRAME_W/H
  config.jpeg_quality = 12;
  if (psramFound()) {
    // Double-buffer in PSRAM: the verifier task and the HTTP handlers stop
    // fighting over a single framebuffer; GRAB_LATEST hands the verifier the
    // freshest frame without blocking.
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  // Keep the sensor at QQVGA to match the verifier's expected frame geometry.
  s->set_framesize(s, FRAMESIZE_QQVGA);

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  bool ok = wm.autoConnect("ESP32-CAM-Setup");
  if (!ok) {
    Serial.println("WiFiManager: portal timeout, restarting");
    delay(1000);
    ESP.restart();
  }
  WiFi.setSleep(false);
  Serial.println("");
  Serial.print("WiFi connected: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin(MDNS_HOSTNAME)) {
    Serial.printf("mDNS started: http://%s.local/\n", MDNS_HOSTNAME);
  } else {
    Serial.println("mDNS start failed");
  }

  startCameraServer();
  MDNS.addService("http", "tcp", 80);

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  // On-device cup verifier + TCP reporter.
  loadVerifierConfig();
  // Lock exposure/gain/white-balance so the ROI metrics stay comparable
  // frame-to-frame (independent of verifier init success).
  cupVerifierSetFixedExposure(cup_fixexp, cup_aecval, cup_agcgain);
  cupVerifierSetParams(cup_window, cup_filldelta, cup_settle, cup_milkluma);
  Serial.printf("Fixed exposure %s (aec_value=%u, agc_gain=%u, awb off)\n",
                cup_fixexp ? "ON" : "off", cup_aecval, cup_agcgain);
  if (cupVerifierInit(&cup_roi, cup_enabled)) {
    cupVerifierStart();
    tcpReporterInit(cup_host, cup_port);
    tcpReporterStart();
    Serial.printf("Cup verifier started (enabled=%d, endpoint=%s:%u, window=%lums)\n",
                  cup_enabled, cup_host[0] ? cup_host : "<unset>", cup_port,
                  (unsigned long)cup_window);
  } else {
    Serial.println("Cup verifier init failed; verification disabled");
  }
}

void loop() {
  // Everything runs in FreeRTOS tasks (web server, verifier, reporter).
  delay(10000);
}
