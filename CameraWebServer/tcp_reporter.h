// tcp_reporter.h — persistent raw-TCP client that streams LED detector events
// as newline-delimited JSON to a configurable API endpoint (host:port).
//
// Drains the led_detector event queue in its own FreeRTOS task so network
// latency never stalls inference. Reconnects with exponential backoff and
// reports gaps caused by queue overflow.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the reporter with the runtime endpoint (loaded from NVS). An empty
// host leaves the reporter idle until tcpReporterSetEndpoint() is called.
void tcpReporterInit(const char *host, uint16_t port);

// Start the reporter FreeRTOS task (pinned to core 0, alongside WiFi/LWIP).
void tcpReporterStart(void);

// Update the endpoint at runtime (from /detcfg); forces a reconnect.
void tcpReporterSetEndpoint(const char *host, uint16_t port);

// Accessors for /status and /detcfg. The endpoint is copied out under the
// config lock so callers never observe a half-updated host string.
void tcpReporterGetEndpoint(char *host, size_t host_cap, uint16_t *port);
bool tcpReporterConnected(void);

#ifdef __cplusplus
}
#endif
