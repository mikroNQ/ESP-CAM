#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"
#include "config.h"
#include "led_detector.h"
#include "tcp_reporter.h"

void startCameraServer();
void setupLedFlash();

// Detector/reporter runtime config, loaded from NVS in setup().
static led_roi_t det_roi = {DET_DEFAULT_ROI_X, DET_DEFAULT_ROI_Y, DET_DEFAULT_ROI_W, DET_DEFAULT_ROI_H};
static bool det_enabled = DET_DEFAULT_ENABLED;
static char det_host[64] = DET_DEFAULT_API_HOST;
static uint16_t det_port = DET_DEFAULT_API_PORT;
static bool det_fixexp = DET_DEFAULT_FIXEXP;
static uint16_t det_aecval = DET_DEFAULT_AEC_VALUE;
static uint8_t det_agcgain = DET_DEFAULT_AGC_GAIN;

static void loadDetectorConfig() {
  Preferences p;
  if (!p.begin(DET_NVS_NS, true)) {  // read-only; absent namespace => defaults
    return;
  }
  String h = p.getString(DET_NVS_HOST, det_host);
  strncpy(det_host, h.c_str(), sizeof(det_host) - 1);
  det_host[sizeof(det_host) - 1] = '\0';
  det_port = p.getUShort(DET_NVS_PORT, det_port);
  det_roi.x = p.getUShort(DET_NVS_ROI_X, det_roi.x);
  det_roi.y = p.getUShort(DET_NVS_ROI_Y, det_roi.y);
  det_roi.w = p.getUShort(DET_NVS_ROI_W, det_roi.w);
  det_roi.h = p.getUShort(DET_NVS_ROI_H, det_roi.h);
  det_enabled = p.getBool(DET_NVS_EN, det_enabled);
  det_fixexp = p.getBool(DET_NVS_FIXEXP, det_fixexp);
  det_aecval = p.getUShort(DET_NVS_AECVAL, det_aecval);
  det_agcgain = p.getUChar(DET_NVS_AGCGAIN, det_agcgain);
  p.end();
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  // reset_reason 9=BROWNOUT (слабое питание!), 6/7=WDT, 4=PANIC, 1=POWERON.
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
  // RGB565 @ QQVGA in DRAM: the on-device ML detector needs raw pixels, and on
  // this no-PSRAM board a small RGB565 frame is the only thing that fits. The
  // MJPEG /stream and /capture handlers JPEG-encode this buffer on the fly.
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QQVGA;  // 160x120, matches DET_FRAME_W/H
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_DRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // Keep the sensor at QQVGA to match the detector's expected frame geometry.
  s->set_framesize(s, FRAMESIZE_QQVGA);

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
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

  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  // On-device LED detector + TCP reporter.
  loadDetectorConfig();
  // Lock exposure/gain/white-balance to the dataset's capture conditions before
  // the detector starts classifying (independent of detector init success).
  ledDetectorSetFixedExposure(det_fixexp, det_aecval, det_agcgain);
  Serial.printf("Fixed exposure %s (aec_value=%u, agc_gain=%u, awb off)\n",
                det_fixexp ? "ON" : "off", det_aecval, det_agcgain);
  if (ledDetectorInit(&det_roi, det_enabled)) {
    ledDetectorStart();
    tcpReporterInit(det_host, det_port);
    tcpReporterStart();
    Serial.printf("LED detector started (enabled=%d, endpoint=%s:%u)\n",
                  det_enabled, det_host[0] ? det_host : "<unset>", det_port);
  } else {
    Serial.println("LED detector init failed; detection disabled");
  }
}

void loop() {
  // Do nothing. Everything is done in another task by the web server
  delay(10000);
}
