#ifndef RDK_LATENCY_H
#define RDK_LATENCY_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef enum
{
	RDK_LATENCY_HOOK,
	RDK_LATENCY_QUEUE,
	RDK_LATENCY_SEND,
	RDK_LATENCY_NETWORK,
	RDK_LATENCY_DECODE,
	RDK_LATENCY_PAINT,
	RDK_LATENCY_DEVICES,
	RDK_LATENCY_COUNT
} rdkLatencyStage;

BOOL rdk_latency_enable(BOOL enabled);
UINT64 rdk_latency_begin(void);
void rdk_latency_end(rdkLatencyStage stage, UINT64 started);
void rdk_latency_message(rdkLatencyStage stage, DWORD postedAt);
void rdk_latency_report(UINT64 now, BOOL force);

#endif