// tcp_reporter.h — persistent raw-TCP client that streams cup-verifier verdicts
// as newline-delimited JSON to a configurable API endpoint (host:port).
//
// Drains the verifier verdict queue in its own FreeRTOS task so network latency
// never stalls the verifier. Reconnects with exponential backoff, sends a hello
// on connect and a periodic status heartbeat while idle.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the reporter with the runtime endpoint (loaded from NVS). An empty
// host leaves the reporter idle until tcpReporterSetEndpoint() is called.
void tcpReporterInit(const char *host, uint16_t port);

// Start the reporter FreeRTOS task (pinned to core 0, alongside WiFi/LWIP).
void tcpReporterStart(void);

// Update the endpoint at runtime (from /cupcfg); forces a reconnect.
void tcpReporterSetEndpoint(const char *host, uint16_t port);

// Accessors for /status.
const char *tcpReporterHost(void);
uint16_t tcpReporterPort(void);
bool tcpReporterConnected(void);

#ifdef __cplusplus
}
#endif
