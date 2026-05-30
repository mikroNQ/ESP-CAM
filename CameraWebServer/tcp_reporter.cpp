// tcp_reporter.cpp — see tcp_reporter.h.
#include "tcp_reporter.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "config.h"
#include "led_detector.h"

namespace {
WiFiClient g_client;

portMUX_TYPE g_ep_mux = portMUX_INITIALIZER_UNLOCKED;
char g_host[64] = {0};
volatile uint16_t g_port = DET_DEFAULT_API_PORT;
volatile bool g_endpoint_dirty = false;  // forces reconnect when set at runtime

volatile bool g_connected = false;
uint32_t g_dropped_since_send = 0;  // events lost to queue overflow / no link
}  // namespace

const char *tcpReporterHost(void) { return g_host; }
uint16_t tcpReporterPort(void) { return g_port; }
bool tcpReporterConnected(void) { return g_connected; }

void tcpReporterInit(const char *host, uint16_t port) {
  portENTER_CRITICAL(&g_ep_mux);
  if (host) {
    strncpy(g_host, host, sizeof(g_host) - 1);
    g_host[sizeof(g_host) - 1] = '\0';
  }
  if (port) g_port = port;
  portEXIT_CRITICAL(&g_ep_mux);
}

void tcpReporterSetEndpoint(const char *host, uint16_t port) {
  tcpReporterInit(host, port);
  g_endpoint_dirty = true;
}

// Build a JSON line into buf. Returns length written (excluding NUL).
static int format_event(char *buf, size_t cap, const led_event_t &ev) {
  return snprintf(buf, cap,
                  "{\"ts_ms\":%lu,\"event\":\"transition\",\"from\":\"%s\",\"to\":\"%s\","
                  "\"dur_ms\":%lu,\"conf\":%.2f}\n",
                  (unsigned long)ev.ts_ms, led_state_name(ev.from),
                  led_state_name(ev.to), (unsigned long)ev.dur_ms, ev.conf);
}

// Returns true on full write.
static bool write_line(const char *line, int len) {
  if (len <= 0) return false;
  return g_client.write((const uint8_t *)line, (size_t)len) == (size_t)len;
}

static void reporter_task(void *arg) {
  (void)arg;
  QueueHandle_t q = ledDetectorEventQueue();
  uint32_t backoff_ms = 500;
  const uint32_t backoff_max = 30000;
  uint32_t idle_ticks = 0;
  char line[192];

  for (;;) {
    // Endpoint changed at runtime → drop the old connection.
    if (g_endpoint_dirty) {
      g_endpoint_dirty = false;
      if (g_client.connected()) g_client.stop();
      g_connected = false;
    }

    // Ensure connection (only if WiFi up and a host is configured).
    if (!g_client.connected()) {
      g_connected = false;
      char host[64];
      uint16_t port;
      portENTER_CRITICAL(&g_ep_mux);
      strncpy(host, g_host, sizeof(host));
      host[sizeof(host) - 1] = '\0';
      port = g_port;
      portEXIT_CRITICAL(&g_ep_mux);

      if (WiFi.status() != WL_CONNECTED || host[0] == '\0') {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      if (g_client.connect(host, port)) {
        g_connected = true;
        backoff_ms = 500;
        // Hello line so the consumer can identify the device + a gap notice.
        int n = snprintf(line, sizeof(line),
                         "{\"type\":\"hello\",\"dev\":\"esp32cam\",\"ip\":\"%s\"}\n",
                         WiFi.localIP().toString().c_str());
        write_line(line, n);
        g_dropped_since_send += ledDetectorTakeDropped();
        if (g_dropped_since_send > 0) {
          n = snprintf(line, sizeof(line), "{\"type\":\"dropped\",\"count\":%lu}\n",
                       (unsigned long)g_dropped_since_send);
          write_line(line, n);
          g_dropped_since_send = 0;
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms += backoff_ms / 2 + (esp_random() % 250);  // 1.5x + jitter
        if (backoff_ms > backoff_max) backoff_ms = backoff_max;
        continue;
      }
    }

    // Drain events. Block up to 1s waiting for the next one.
    led_event_t ev;
    if (q && xQueueReceive(q, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
      int n = format_event(line, sizeof(line), ev);
      if (!write_line(line, n)) {
        // Peer gone; count this event as dropped and reconnect next loop.
        g_dropped_since_send++;
        g_client.stop();
        g_connected = false;
      }
    } else if (g_client.connected()) {
      // Idle: every ~3s send a status heartbeat (RSSI / uptime / heap) so the
      // dashboard can show link quality; in between, a bare newline cheaply
      // detects a dead peer. Both double as keepalive.
      bool ok;
      if (++idle_ticks % 3 == 0) {
        int n = snprintf(line, sizeof(line),
                         "{\"type\":\"status\",\"rssi\":%d,\"uptime_s\":%lu,\"heap\":%u}\n",
                         (int)WiFi.RSSI(),
                         (unsigned long)(esp_timer_get_time() / 1000000ULL),
                         (unsigned)esp_get_free_heap_size());
        ok = write_line(line, n);
      } else {
        ok = g_client.write((const uint8_t *)"\n", 1) == 1;
      }
      if (!ok) {
        g_client.stop();
        g_connected = false;
      }
    }
  }
}

void tcpReporterStart(void) {
  xTaskCreatePinnedToCore(reporter_task, "tcp_rep", 4096, nullptr, 3, nullptr, 0);
}
