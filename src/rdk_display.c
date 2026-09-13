#include "rdk_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

BOOL rdk_display_number(const WCHAR* deviceName, UINT32* number)
{
	const WCHAR prefix[] = L"\\\\.\\DISPLAY";
	if (!deviceName || !number || wcsncmp(deviceName, prefix, ARRAYSIZE(prefix) - 1) != 0)
		return FALSE;
	const WCHAR* digits = deviceName + ARRAYSIZE(prefix) - 1;
	if (*digits < L'1' || *digits > L'9')
		return FALSE;
	WCHAR* end = NULL;
	const unsigned long long value = wcstoull(digits, &end, 10);
	if (*end || value > UINT32_MAX)
		return FALSE;
	*number = (UINT32)value;
	return TRUE;
}

typedef struct
{
	rdpMonitor* monitors;
	size_t count;
	size_t capacity;
	BOOL list;
} rdkMonitorEnumeration;

static BOOL CALLBACK rdk_monitor_proc(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM parameter)
{
	(void)dc;
	(void)rect;
	rdkMonitorEnumeration* enumeration = (rdkMonitorEnumeration*)parameter;
	if (enumeration->count >= enumeration->capacity)
		return FALSE;
	MONITORINFOEXW info = { 0 };
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(monitor, (MONITORINFO*)&info))
		return FALSE;
	rdpMonitor* entry = &enumeration->monitors[enumeration->count];
	entry->x = info.rcMonitor.left;
	entry->y = info.rcMonitor.top;
	entry->width = info.rcMonitor.right - info.rcMonitor.left;
	entry->height = info.rcMonitor.bottom - info.rcMonitor.top;
	entry->is_primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
	if (!rdk_display_number(info.szDevice, &entry->orig_screen))
	{
		fprintf(stderr, "rdk: cannot determine Windows display number for %ls\n", info.szDevice);
		return FALSE;
	}
	entry->attributes.desktopScaleFactor = 100;
	entry->attributes.deviceScaleFactor = 100;
	if (enumeration->list)
		printf("%u: %ux%u at (%d,%d)%s  %ls\n", entry->orig_screen, entry->width,
		       entry->height, entry->x, entry->y, entry->is_primary ? " primary" : "", info.szDevice);
	++enumeration->count;
	return TRUE;
}

BOOL rdk_display_apply(rdpSettings* settings, const rdpMonitor* monitors, size_t count,
                       RECT* bounds)
{
	if (!settings || !monitors || !count || !bounds)
		return FALSE;
	const UINT32 requested = freerdp_settings_get_uint32(settings, FreeRDP_NumMonitorIds);
	const UINT32* ids = freerdp_settings_get_pointer(settings, FreeRDP_MonitorIds);
	const size_t selectedCount = requested ? requested : count;
	if (selectedCount > 16 || (requested && !ids))
	{
		fprintf(stderr, "rdk: select between 1 and 16 monitors\n");
		return FALSE;
	}
	rdpMonitor selected[16] = { 0 };
	RECT selectedBounds = { 0 };
	BOOL hasPrimary = FALSE;
	for (size_t index = 0; index < selectedCount; ++index)
	{
		size_t source = index;
		if (requested)
		{
			for (source = 0; source < count; ++source)
			{
				if (monitors[source].orig_screen == ids[index])
					break;
			}
		}
		if (source == count)
		{
			fprintf(stderr, "rdk: Windows display %u is not active; use /list:monitor for numbers\n",
			        ids[index]);
			return FALSE;
		}
		for (size_t previous = 0; previous < index; ++previous)
		{
			if (selected[previous].orig_screen == monitors[source].orig_screen)
			{
				fprintf(stderr, "rdk: Windows display %u was selected more than once\n",
				        monitors[source].orig_screen);
				return FALSE;
			}
		}
		selected[index] = monitors[source];
		if (selected[index].width <= 0 || selected[index].height <= 0)
			return FALSE;
		if (requested)
			selected[index].is_primary = index == 0;
		hasPrimary = hasPrimary || selected[index].is_primary;
		RECT monitorBounds = { selected[index].x, selected[index].y,
		                       selected[index].x + selected[index].width,
		                       selected[index].y + selected[index].height };
		if (index == 0)
			selectedBounds = monitorBounds;
		else
			UnionRect(&selectedBounds, &selectedBounds, &monitorBounds);
	}
	if (!hasPrimary)
		selected[0].is_primary = TRUE;
	BOOL connected[16] = { TRUE };
	for (size_t pass = 0; pass < selectedCount; ++pass)
	{
		for (size_t index = 0; index < selectedCount; ++index)
		{
			if (!connected[index])
				continue;
			const rdpMonitor* current = &selected[index];
			for (size_t neighbor = 0; neighbor < selectedCount; ++neighbor)
			{
				const rdpMonitor* other = &selected[neighbor];
				const BOOL horizontal = (current->x + current->width == other->x ||
				                         other->x + other->width == current->x) &&
				                        current->y < other->y + other->height &&
				                        other->y < current->y + current->height;
				const BOOL vertical = (current->y + current->height == other->y ||
				                       other->y + other->height == current->y) &&
				                      current->x < other->x + other->width &&
				                      other->x < current->x + current->width;
				connected[neighbor] = connected[neighbor] || horizontal || vertical;
			}
		}
	}
	for (size_t index = 0; index < selectedCount; ++index)
	{
		if (!connected[index])
		{
			fprintf(stderr, "rdk: selected displays must form a connected layout with shared edges; "
			                "rearrange them in Windows display settings\n");
			return FALSE;
		}
	}
	for (size_t index = 0; index < count; ++index)
	{
		BOOL included = FALSE;
		for (size_t selection = 0; selection < selectedCount; ++selection)
			included = included || selected[selection].orig_screen == monitors[index].orig_screen;
		RECT monitorBounds = { monitors[index].x, monitors[index].y,
		                       monitors[index].x + monitors[index].width,
		                       monitors[index].y + monitors[index].height };
		RECT overlap;
		if (!included && IntersectRect(&overlap, &selectedBounds, &monitorBounds))
		{
			fprintf(stderr, "rdk: selected desktop would cover unselected Windows display %u; "
			                "select neighboring monitors or rearrange them in Windows\n",
			        monitors[index].orig_screen);
			return FALSE;
		}
	}
	BOOL ok = freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, selectedCount > 1);
	ok = ok && freerdp_settings_set_bool(settings, FreeRDP_Fullscreen, TRUE);
	ok = ok && freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
	ok = ok && freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
	                                     (UINT32)(selectedBounds.right - selectedBounds.left));
	ok = ok && freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
	                                     (UINT32)(selectedBounds.bottom - selectedBounds.top));
	ok = ok && freerdp_settings_set_monitor_def_array_sorted(settings, selected, selectedCount);
	if (ok)
		*bounds = selectedBounds;
	return ok;
}

static BOOL rdk_enumerate_display(rdpSettings* settings, RECT* bounds, BOOL list)
{
	const int capacity = GetSystemMetrics(SM_CMONITORS);
	if (capacity < 1)
		return FALSE;
	rdpMonitor* monitors = calloc((size_t)capacity, sizeof(rdpMonitor));
	if (!monitors)
		return FALSE;
	rdkMonitorEnumeration enumeration = { monitors, 0, (size_t)capacity, list };
	BOOL ok = EnumDisplayMonitors(NULL, NULL, rdk_monitor_proc, (LPARAM)&enumeration);
	ok = ok && enumeration.count > 0;
	if (ok && !list)
		ok = rdk_display_apply(settings, monitors, enumeration.count, bounds);
	free(monitors);
	return ok;
}

BOOL rdk_configure_display(rdpSettings* settings, RECT* bounds)
{
	return rdk_enumerate_display(settings, bounds, FALSE);
}

BOOL rdk_list_monitors(void)
{
	return rdk_enumerate_display(NULL, NULL, TRUE);
}