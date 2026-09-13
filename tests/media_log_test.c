#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static char output[65536];
static size_t outputLength;

static int record_printf(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const size_t available = sizeof(output) - outputLength;
	const int written = vsnprintf(output + outputLength, available, format, arguments);
	va_end(arguments);
	if (written > 0)
		outputLength += (size_t)written < available ? (size_t)written : available - 1;
	return written;
}

#define printf record_printf
#include "../src/rdk_media.c"
#undef printf

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

int main(void)
{
	rdkMediaWatch watch = { 0 };
	watch.notification.lpVtbl = &rdk_notification_vtable;
	CHECK(rdk_media_change_init(&watch.change, L"baseline"));
	CHECK(!rdk_media_watch_changed(&watch, 100) && outputLength == 0);
	CHECK(rdk_notify_device(&watch.notification, L"headset") == S_OK);
	watch.observed = watch.generation;
	CHECK(!rdk_media_watch_changed(&watch, 200) && outputLength == 0);
	CHECK(!rdk_media_watch_changed(&watch, 1699) && outputLength == 0);
	CHECK(rdk_notify_state(&watch.notification, L"headset", DEVICE_STATE_ACTIVE) == S_OK);
	watch.observed = watch.generation;
	CHECK(!rdk_media_watch_changed(&watch, 1700) && outputLength == 0);
	CHECK(!rdk_media_watch_changed(&watch, 3199) && outputLength == 0);
	CHECK(!rdk_media_watch_changed(&watch, 3200));
	CHECK(strstr(output, "local audio endpoint configuration changed"));
	CHECK(strstr(output, "local microphones (metadata only; capture not verified)"));
	CHECK(!strstr(output, "offering reconnect"));
	const size_t logged = outputLength;
	CHECK(!rdk_media_watch_changed(&watch, 9000) && outputLength == logged);
	CHECK(rdk_notify_removed(&watch.notification, L"baseline") == S_OK);
	watch.observed = watch.generation;
	CHECK(!rdk_media_watch_changed(&watch, 10000));
	CHECK(strstr(output, "session microphone was removed or became inactive"));
	CHECK(!rdk_media_watch_changed(&watch, 11499));
	CHECK(rdk_media_watch_changed(&watch, 11500));
	CHECK(strstr(output, "offering reconnect"));
	const size_t afterNotice = outputLength;
	CHECK(!rdk_media_watch_changed(&watch, 16000) && outputLength == afterNotice);
	rdk_media_change_free(&watch.change);
	puts("Passed debounced stdout device-change logging and duplicate suppression (no capture)");
	return 0;
}