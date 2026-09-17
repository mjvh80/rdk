#include "rdk_latency.h"

#include <stdio.h>

typedef struct
{
	UINT64 count;
	double totalMs;
	double maxMs;
	UINT64 slow;
} rdkLatencyStats;

static SRWLOCK statsLock = SRWLOCK_INIT;
static volatile LONG timingEnabled;
static LARGE_INTEGER frequency;
static UINT64 lastReport;
static rdkLatencyStats measurements[RDK_LATENCY_COUNT];
static const char* stageNames[RDK_LATENCY_COUNT] = {
	"hook", "queue", "send", "events", "decode", "paint", "devices"
};

static BOOL timing_enabled(void)
{
	return InterlockedCompareExchange(&timingEnabled, 0, 0) != 0;
}

BOOL rdk_latency_enable(BOOL enabled)
{
	InterlockedExchange(&timingEnabled, 0);
	if (enabled && (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0))
		return FALSE;
	AcquireSRWLockExclusive(&statsLock);
	ZeroMemory(measurements, sizeof(measurements));
	lastReport = GetTickCount64();
	ReleaseSRWLockExclusive(&statsLock);
	InterlockedExchange(&timingEnabled, enabled ? 1 : 0);
	return TRUE;
}

static void record_duration(rdkLatencyStage stage, double milliseconds)
{
	if (!timing_enabled() || stage < 0 || stage >= RDK_LATENCY_COUNT || milliseconds < 0)
		return;
	AcquireSRWLockExclusive(&statsLock);
	rdkLatencyStats* stats = &measurements[stage];
	++stats->count;
	stats->totalMs += milliseconds;
	if (milliseconds > stats->maxMs)
		stats->maxMs = milliseconds;
	if (milliseconds >= 50.0)
		++stats->slow;
	ReleaseSRWLockExclusive(&statsLock);
}

UINT64 rdk_latency_begin(void)
{
	LARGE_INTEGER now;
	return timing_enabled() && QueryPerformanceCounter(&now) ? (UINT64)now.QuadPart : 0;
}

void rdk_latency_end(rdkLatencyStage stage, UINT64 started)
{
	if (!started)
		return;
	const UINT64 now = rdk_latency_begin();
	if (now >= started)
		record_duration(stage, (double)(now - started) * 1000.0 / (double)frequency.QuadPart);
}

static double message_age(DWORD now, DWORD postedAt)
{
	return (double)(DWORD)(now - postedAt);
}

void rdk_latency_message(rdkLatencyStage stage, DWORD postedAt)
{
	if (timing_enabled())
		record_duration(stage, message_age(GetTickCount(), postedAt));
}

static BOOL take_snapshot(UINT64 now, BOOL force, rdkLatencyStats* snapshot, UINT64* elapsed)
{
	if (!timing_enabled())
		return FALSE;
	AcquireSRWLockExclusive(&statsLock);
	*elapsed = now - lastReport;
	if (!force && *elapsed < 5000)
	{
		ReleaseSRWLockExclusive(&statsLock);
		return FALSE;
	}
	CopyMemory(snapshot, measurements, sizeof(measurements));
	ZeroMemory(measurements, sizeof(measurements));
	lastReport = now;
	ReleaseSRWLockExclusive(&statsLock);
	return TRUE;
}

void rdk_latency_report(UINT64 now, BOOL force)
{
	rdkLatencyStats snapshot[RDK_LATENCY_COUNT];
	UINT64 elapsed = 0;
	if (!take_snapshot(now, force, snapshot, &elapsed))
		return;
	char output[2048];
	int length = snprintf(output, sizeof(output), "rdk: latency interval=%llums", (unsigned long long)elapsed);
	for (size_t index = 0; index < RDK_LATENCY_COUNT; ++index)
	{
		const rdkLatencyStats* stats = &snapshot[index];
		const int written = snprintf(output + length, sizeof(output) - (size_t)length,
		    " %s[n=%llu avg=%.2f max=%.2f slow=%llu]", stageNames[index],
		    (unsigned long long)stats->count, stats->count ? stats->totalMs / (double)stats->count : 0.0,
		    stats->maxMs, (unsigned long long)stats->slow);
		if (written < 0 || (size_t)written >= sizeof(output) - (size_t)length)
			return;
		length += written;
	}
	printf("%s\n", output);
	fflush(stdout);
}