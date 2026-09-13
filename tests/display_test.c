#include "rdk_display.h"

#include <freerdp/client/cmdline.h>
#include <stdio.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); \
	return FALSE; \
} } while (0)

static BOOL selected_external_monitors(void)
{
	rdpSettings* settings = freerdp_settings_new(0);
	CHECK(settings);
	char* arguments[] = { "rdk", "/v:unused", "/monitors:1,2" };
	CHECK(freerdp_client_settings_parse_command_line(settings, ARRAYSIZE(arguments), arguments, FALSE) == 0);
	rdpMonitor monitors[3] = { 0 };
	monitors[0].x = 0;
	monitors[0].y = 0;
	monitors[0].width = 1920;
	monitors[0].height = 1080;
	monitors[0].is_primary = TRUE;
	monitors[0].orig_screen = 3;
	monitors[1].x = -2560;
	monitors[1].y = -1440;
	monitors[1].width = 2560;
	monitors[1].height = 1440;
	monitors[1].orig_screen = 1;
	monitors[2] = monitors[1];
	monitors[2].x = 0;
	monitors[2].orig_screen = 2;
	RECT bounds;
	CHECK(rdk_display_apply(settings, monitors, ARRAYSIZE(monitors), &bounds));
	CHECK(bounds.left == -2560 && bounds.top == -1440 && bounds.right == 2560 && bounds.bottom == 0);
	CHECK(freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount) == 2);
	CHECK(freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) == 5120);
	CHECK(freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) == 1440);
	const rdpMonitor* primary = freerdp_settings_get_pointer_array(settings, FreeRDP_MonitorDefArray, 0);
	CHECK(primary && primary->is_primary && primary->orig_screen == 1);
	CHECK(primary->x == 0 && primary->y == 0);
	CHECK(primary->width == 2560 && primary->height == 1440);
	UINT32 reversed[] = { 2, 1 };
	CHECK(freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, reversed, ARRAYSIZE(reversed)));
	CHECK(rdk_display_apply(settings, monitors, ARRAYSIZE(monitors), &bounds));
	CHECK(bounds.left == -2560 && bounds.top == -1440 && bounds.right == 2560 && bounds.bottom == 0);
	primary = freerdp_settings_get_pointer_array(settings, FreeRDP_MonitorDefArray, 0);
	CHECK(primary && primary->is_primary && primary->orig_screen == 2 && primary->x == 0 && primary->y == 0);
	const rdpMonitor* secondary = freerdp_settings_get_pointer_array(settings, FreeRDP_MonitorDefArray, 1);
	CHECK(secondary && secondary->orig_screen == 1 && secondary->x == -2560 && secondary->y == 0);
	CHECK(freerdp_settings_get_int32(settings, FreeRDP_MonitorLocalShiftX) == 0);
	CHECK(freerdp_settings_get_int32(settings, FreeRDP_MonitorLocalShiftY) == -1440);
	freerdp_settings_free(settings);
	return TRUE;
}

static BOOL windows_display_numbers(void)
{
	UINT32 number = 0;
	CHECK(rdk_display_number(L"\\\\.\\DISPLAY1", &number) && number == 1);
	CHECK(rdk_display_number(L"\\\\.\\DISPLAY12", &number) && number == 12);
	CHECK(!rdk_display_number(L"DISPLAY1", &number));
	CHECK(!rdk_display_number(L"\\\\.\\DISPLAY0", &number));
	CHECK(!rdk_display_number(L"\\\\.\\DISPLAY-1", &number));
	CHECK(!rdk_display_number(L"\\\\.\\DISPLAY2bad", &number));
	CHECK(!rdk_display_number(L"\\\\.\\DISPLAY4294967296", &number));
	rdpSettings* settings = freerdp_settings_new(0);
	CHECK(settings);
	rdpMonitor monitors[3] = { 0 };
	for (size_t index = 0; index < ARRAYSIZE(monitors); ++index)
	{
		monitors[index].x = (INT32)index * 1920 - 1920;
		monitors[index].width = 1920;
		monitors[index].height = 1080;
	}
	monitors[0].orig_screen = 7;
	monitors[1].orig_screen = 3;
	monitors[1].is_primary = TRUE;
	monitors[2].orig_screen = 1;
	UINT32 ids[] = { 7 };
	CHECK(freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, ids, ARRAYSIZE(ids)));
	RECT bounds;
	CHECK(rdk_display_apply(settings, monitors, ARRAYSIZE(monitors), &bounds));
	CHECK(bounds.left == -1920 && bounds.right == 0);
	CHECK(freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount) == 1);
	CHECK(!freerdp_settings_get_bool(settings, FreeRDP_UseMultimon));
	ids[0] = 2;
	CHECK(freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, ids, ARRAYSIZE(ids)));
	CHECK(!rdk_display_apply(settings, monitors, ARRAYSIZE(monitors), &bounds));
	CHECK(freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, NULL, 0));
	CHECK(rdk_display_apply(settings, monitors, ARRAYSIZE(monitors), &bounds));
	CHECK(bounds.left == -1920 && bounds.right == 3840);
	CHECK(freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount) == 3);
	const rdpMonitor* primary = freerdp_settings_get_pointer_array(settings, FreeRDP_MonitorDefArray, 0);
	CHECK(primary && primary->orig_screen == 3 && primary->is_primary);
	freerdp_settings_free(settings);
	return TRUE;
}

static BOOL invalid_layouts(void)
{
	rdpSettings* settings = freerdp_settings_new(0);
	CHECK(settings);
	rdpMonitor monitors[4] = { 0 };
	for (size_t index = 0; index < ARRAYSIZE(monitors); ++index)
	{
		monitors[index].orig_screen = (UINT32)index + 1;
		monitors[index].x = (INT32)(index % 2) * 1920;
		monitors[index].y = (INT32)(index / 2) * 1080;
		monitors[index].width = 1920;
		monitors[index].height = 1080;
	}
	RECT bounds = { 10, 20, 30, 40 };
	UINT32 duplicate[] = { 1, 1 };
	CHECK(freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, duplicate, ARRAYSIZE(duplicate)));
	CHECK(!rdk_display_apply(settings, monitors, ARRAYSIZE(monitors), &bounds));
	UINT32 zero[] = { 0 };
	CHECK(freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, zero, ARRAYSIZE(zero)));
	CHECK(!rdk_display_apply(settings, monitors, ARRAYSIZE(monitors), &bounds));
	UINT32 diagonal[] = { 1, 4 };
	CHECK(freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, diagonal, ARRAYSIZE(diagonal)));
	CHECK(!rdk_display_apply(settings, monitors, ARRAYSIZE(monitors), &bounds));
	UINT32 surrounding[] = { 1, 2, 3 };
	CHECK(freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, surrounding, ARRAYSIZE(surrounding)));
	CHECK(!rdk_display_apply(settings, monitors, ARRAYSIZE(monitors), &bounds));
	CHECK(bounds.left == 10 && bounds.top == 20 && bounds.right == 30 && bounds.bottom == 40);
	UINT32 adjacent[] = { 1, 2 };
	CHECK(freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorIds, adjacent, ARRAYSIZE(adjacent)));
	monitors[1].height = 1440;
	CHECK(rdk_display_apply(settings, monitors, 2, &bounds));
	CHECK(bounds.left == 0 && bounds.top == 0 && bounds.right == 3840 && bounds.bottom == 1440);
	CHECK(freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) == 1440);
	freerdp_settings_free(settings);
	return TRUE;
}

int main(void)
{
	if (!selected_external_monitors() || !windows_display_numbers() || !invalid_layouts())
		return 1;
	printf("Passed 3 display selection tests\n");
	return 0;
}