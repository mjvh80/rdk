#include "rdk_media.h"
#include <stdio.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

int main(void)
{
	rdkMediaChange change;
	CHECK(rdk_media_change_init(&change, L"built-in"));
	CHECK(!rdk_media_change_ready(&change, 5000));
	CHECK(rdk_media_change_update(&change, L"headset", 6000));
	CHECK(!rdk_media_change_ready(&change, 7499));
	CHECK(rdk_media_change_ready(&change, 7500));
	CHECK(!rdk_media_change_ready(&change, 9000));
	CHECK(rdk_media_change_update(&change, L"headset", 10000));
	CHECK(!rdk_media_change_ready(&change, 12000));
	CHECK(rdk_media_change_update(&change, L"", 13000));
	CHECK(rdk_media_change_update(&change, L"headset", 13500));
	CHECK(!rdk_media_change_ready(&change, 16000));
	CHECK(rdk_media_change_update(&change, L"built-in", 17000));
	CHECK(!rdk_media_change_ready(&change, 19000));
	CHECK(rdk_media_change_update(&change, L"headset", 20000));
	CHECK(rdk_media_change_ready(&change, 22000));
	CHECK(rdk_media_change_update(&change, L"", 23000));
	CHECK(rdk_media_change_ready(&change, 25000));
	rdk_media_change_free(&change);
	CHECK(rdk_media_change_init(&change, L""));
	CHECK(rdk_media_change_update(&change, L"new microphone", 100));
	CHECK(rdk_media_change_ready(&change, 2000));
	rdk_media_change_free(&change);
	CHECK(rdk_media_change_init(&change, L"same headset"));
	rdk_media_change_invalidate(&change, 1000);
	CHECK(!rdk_media_change_ready(&change, 2499));
	CHECK(rdk_media_change_ready(&change, 2500));
	CHECK(!rdk_media_change_ready(&change, 4000));
	rdk_media_change_free(&change);
	rdkMediaWatch* watch = rdk_media_watch_new();
	CHECK(watch);
	CHECK(!rdk_media_watch_changed(watch, GetTickCount64()));
	rdk_media_watch_free(watch);
	CHECK(rdk_microphone_list());
	puts("Passed microphone debounce and notification lifecycle tests (no capture)");
	return 0;
}