#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static char reportText[2048];
static unsigned reportCount;

static int record_report(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const int length = vsnprintf(reportText, sizeof(reportText), format, arguments);
	va_end(arguments);
	++reportCount;
	return length;
}

#define printf record_report
#include "../src/rdk_latency.c"
#undef printf

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

static DWORD WINAPI record_worker(LPVOID argument)
{
	(void)argument;
	for (UINT iteration = 0; iteration < 1000; ++iteration)
		record_duration(RDK_LATENCY_DECODE, 2.0);
	return 0;
}

int main(void)
{
	rdkLatencyStats snapshot[RDK_LATENCY_COUNT];
	UINT64 elapsed = 0;
	CHECK(rdk_latency_begin() == 0);
	record_duration(RDK_LATENCY_SEND, 90.0);
	CHECK(!take_snapshot(5000, TRUE, snapshot, &elapsed));
	CHECK(rdk_latency_enable(TRUE));
	CHECK(rdk_latency_begin() != 0);
	const UINT64 start = lastReport;
	record_duration(RDK_LATENCY_SEND, 1.0);
	record_duration(RDK_LATENCY_SEND, 50.0);
	record_duration(RDK_LATENCY_SEND, 99.0);
	record_duration(RDK_LATENCY_COUNT, 100.0);
	record_duration(RDK_LATENCY_SEND, -1.0);
	CHECK(!take_snapshot(start + 4999, FALSE, snapshot, &elapsed));
	CHECK(take_snapshot(start + 5000, FALSE, snapshot, &elapsed));
	CHECK(elapsed == 5000);
	CHECK(snapshot[RDK_LATENCY_SEND].count == 3 && snapshot[RDK_LATENCY_SEND].totalMs == 150.0);
	CHECK(snapshot[RDK_LATENCY_SEND].maxMs == 99.0 && snapshot[RDK_LATENCY_SEND].slow == 2);
	CHECK(snapshot[RDK_LATENCY_QUEUE].count == 0);
	CHECK(message_age(15, 0xFFFFFFF0u) == 31.0);
	CHECK(message_age(100, 95) == 5.0);
	CHECK(take_snapshot(start + 5001, TRUE, snapshot, &elapsed));
	CHECK(snapshot[RDK_LATENCY_SEND].count == 0 && elapsed == 1);
	HANDLE workers[4];
	for (size_t index = 0; index < ARRAYSIZE(workers); ++index)
	{
		workers[index] = CreateThread(NULL, 0, record_worker, NULL, 0, NULL);
		CHECK(workers[index]);
	}
	CHECK(WaitForMultipleObjects(ARRAYSIZE(workers), workers, TRUE, 5000) == WAIT_OBJECT_0);
	for (size_t index = 0; index < ARRAYSIZE(workers); ++index)
		CloseHandle(workers[index]);
	CHECK(take_snapshot(start + 10001, FALSE, snapshot, &elapsed));
	CHECK(snapshot[RDK_LATENCY_DECODE].count == 4000 && snapshot[RDK_LATENCY_DECODE].totalMs == 8000.0);
	const UINT64 timer = rdk_latency_begin();
	rdk_latency_end(RDK_LATENCY_PAINT, timer);
	CHECK(take_snapshot(start + 10002, TRUE, snapshot, &elapsed));
	CHECK(snapshot[RDK_LATENCY_PAINT].count == 1);
	record_duration(RDK_LATENCY_QUEUE, 60.0);
	rdk_latency_report(start + 10003, FALSE);
	CHECK(reportCount == 0);
	rdk_latency_report(start + 10004, TRUE);
	CHECK(reportCount == 1);
	CHECK(strstr(reportText, "rdk: latency interval=2ms"));
	CHECK(strstr(reportText, "queue[n=1 avg=60.00 max=60.00 slow=1]"));
	CHECK(strstr(reportText, "send[n=0 avg=0.00 max=0.00 slow=0]"));
	for (size_t index = 0; index < RDK_LATENCY_COUNT; ++index)
		CHECK(strstr(reportText, stageNames[index]));
	CHECK(rdk_latency_enable(FALSE));
	CHECK(rdk_latency_begin() == 0);
	rdk_latency_end(RDK_LATENCY_PAINT, timer);
	rdk_latency_report(start + 20000, TRUE);
	CHECK(reportCount == 1);
	CHECK(!take_snapshot(start + 20000, TRUE, snapshot, &elapsed));
	puts("Passed disabled timing, aggregation, reporting interval, timestamp wrap, reset, and concurrent writers");
	return 0;
}