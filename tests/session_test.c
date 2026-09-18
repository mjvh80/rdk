#include "rdk_client.h"
#include "rdk_session.h"
#include "rdk_resources.h"
#include <freerdp/client/cmdline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

static int icon_check(HICON icon, int size)
{
	ICONINFO info = { 0 };
	CHECK(icon && GetIconInfo(icon, &info));
	BITMAP bitmap;
	CHECK(GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap));
	CHECK(bitmap.bmWidth == size && bitmap.bmHeight == size);
	BITMAPINFO format = { 0 };
	format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	format.bmiHeader.biWidth = size;
	format.bmiHeader.biHeight = -size;
	format.bmiHeader.biPlanes = 1;
	format.bmiHeader.biBitCount = 32;
	BYTE* pixels = calloc((size_t)size * size, 4);
	CHECK(pixels);
	HDC screen = GetDC(NULL);
	CHECK(GetDIBits(screen, info.hbmColor, 0, size, pixels, &format, DIB_RGB_COLORS) == size);
	size_t opaque = 0;
	for (size_t index = 0; index < (size_t)size * size; ++index)
		if (pixels[index * 4 + 3] > 127) ++opaque;
	CHECK(opaque * 100 > (size_t)size * size * 20);
	CHECK(opaque * 100 < (size_t)size * size * 55);
	CHECK(pixels[3] == 0);
	CHECK(pixels[((size_t)(size / 2) * size + size / 4) * 4 + 3] == 0);
	ReleaseDC(NULL, screen);
	free(pixels);
	DeleteObject(info.hbmColor);
	DeleteObject(info.hbmMask);
	return 0;
}

static int notice_layout_check(HWND window)
{
	RECT client;
	GetClientRect(window, &client);
	RECT previous = { 0 };
	for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
	{
		RECT bounds;
		GetWindowRect(child, &bounds);
		MapWindowPoints(NULL, window, (POINT*)&bounds, 2);
		CHECK(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom);
		RECT overlap;
		CHECK(!IntersectRect(&overlap, &previous, &bounds));
		previous = bounds;
		WCHAR text[256];
		if (!GetWindowTextW(child, text, ARRAYSIZE(text))) continue;
		HFONT font = (HFONT)SendMessageW(child, WM_GETFONT, 0, 0);
		LOGFONTW details;
		CHECK(font && GetObjectW(font, sizeof(details), &details));
		CHECK(wcscmp(details.lfFaceName, L"Segoe UI") == 0);
		HDC drawing = GetDC(child);
		HGDIOBJ old = SelectObject(drawing, font);
		RECT textBounds = { 0, 0, bounds.right - bounds.left, 0 };
		CHECK(DrawTextW(drawing, text, -1, &textBounds, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX));
		CHECK(textBounds.bottom <= bounds.bottom - bounds.top && textBounds.right <= bounds.right - bounds.left);
		SelectObject(drawing, old);
		ReleaseDC(child, drawing);
	}
	return 0;
}

static int notice_preview(HWND window, UINT dpi)
{
	RECT rect;
	GetClientRect(window, &rect);
	HDC screen = GetDC(window);
	HDC drawing = CreateCompatibleDC(screen);
	BITMAPINFO format = { 0 };
	format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	format.bmiHeader.biWidth = rect.right;
	format.bmiHeader.biHeight = -rect.bottom;
	format.bmiHeader.biPlanes = 1;
	format.bmiHeader.biBitCount = 32;
	void* pixels = NULL;
	HBITMAP bitmap = CreateDIBSection(drawing, &format, DIB_RGB_COLORS, &pixels, NULL, 0);
	CHECK(bitmap);
	HGDIOBJ previous = SelectObject(drawing, bitmap);
	CHECK(PrintWindow(window, drawing, PW_CLIENTONLY));
	GdiFlush();
	BITMAPFILEHEADER header = { 0 };
	header.bfType = 0x4D42;
	header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
	header.bfSize = header.bfOffBits + rect.right * rect.bottom * 4;
	char name[80];
	sprintf_s(name, sizeof(name), "rdk-notice-%u.bmp", dpi);
	FILE* file = NULL;
	CHECK(fopen_s(&file, name, "wb") == 0);
	CHECK(fwrite(&header, sizeof(header), 1, file) == 1);
	CHECK(fwrite(&format.bmiHeader, sizeof(BITMAPINFOHEADER), 1, file) == 1);
	CHECK(fwrite(pixels, header.bfSize - header.bfOffBits, 1, file) == 1);
	fclose(file);
	SelectObject(drawing, previous);
	DeleteObject(bitmap);
	DeleteDC(drawing);
	ReleaseDC(window, screen);
	return 0;
}

int main(int argc, char** argv)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	ACTCTX_SECTION_KEYED_DATA theme = { sizeof(theme) };
	CHECK(FindActCtxSectionStringW(0, NULL, ACTIVATION_CONTEXT_SECTION_DLL_REDIRECTION, L"comctl32.dll", &theme));
	const int sizes[] = { 16, 20, 24, 32, 40, 48, 60, 64, 80, 96, 128, 256 };
	for (size_t index = 0; index < ARRAYSIZE(sizes); ++index)
	{
		HICON icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_RDK), IMAGE_ICON,
		    sizes[index], sizes[index], 0);
		CHECK(icon_check(icon, sizes[index]) == 0);
		DestroyIcon(icon);
	}
	RDP_CLIENT_ENTRY_POINTS entry = { 0 };
	entry.Size = sizeof(entry);
	entry.Version = RDP_CLIENT_INTERFACE_VERSION;
	entry.ContextSize = sizeof(rdkContext);
	rdpContext* context = freerdp_client_context_new(&entry);
	CHECK(context);
	char* args[] = { "rdk", "/v:unused", "/u:remote-user", "/microphone", "/sound", "/monitors:1,2" };
	CHECK(freerdp_client_settings_parse_command_line(context->settings, ARRAYSIZE(args), args, FALSE) == 0);
	const char* camera[] = { "rdpecam" };
	CHECK(freerdp_client_add_dynamic_channel(context->settings, ARRAYSIZE(camera), camera));
	rdpSettings* saved = freerdp_settings_clone(context->settings);
	CHECK(saved);
	for (UINT iteration = 0; iteration < 5; ++iteration)
	{
		((rdkContext*)context)->quit = TRUE;
		((rdkContext*)context)->reconnectRequested = TRUE;
		((rdkContext*)context)->desktopReady = TRUE;
		CHECK(freerdp_settings_set_string(context->settings, FreeRDP_Username, "changed-in-session"));
		freerdp_client_context_free(context);
		context = rdk_session_recreate(&entry, saved);
		CHECK(context);
		CHECK(!((rdkContext*)context)->quit && !((rdkContext*)context)->reconnectRequested);
		CHECK(!((rdkContext*)context)->desktopReady);
		CHECK(!((rdkContext*)context)->hwnd && !((rdkContext*)context)->mediaWatch);
		CHECK(strcmp(freerdp_settings_get_string(context->settings, FreeRDP_Username), "remote-user") == 0);
		CHECK(freerdp_dynamic_channel_collection_find(context->settings, "audin"));
		CHECK(freerdp_dynamic_channel_collection_find(context->settings, "rdpsnd") ||
		      freerdp_static_channel_collection_find(context->settings, "rdpsnd"));
		CHECK(freerdp_settings_get_uint32(context->settings, FreeRDP_NumMonitorIds) == 2);
		CHECK(freerdp_dynamic_channel_collection_find(context->settings, "rdpecam"));
	}
	freerdp_client_context_free(context);
	freerdp_settings_free(saved);

	rdkContext notice = { 0 };
	notice.hwnd = CreateWindowExW(0, L"STATIC", L"rdk notice test", WS_POPUP, 0, 0, 640, 480,
	                              NULL, NULL, GetModuleHandleW(NULL), NULL);
	CHECK(notice.hwnd);
	const HWND foreground = GetForegroundWindow();
	rdk_media_notice(&notice);
	CHECK(notice.notice && GetForegroundWindow() == foreground);
	const UINT windowDpi = GetDpiForWindow(notice.notice);
	CHECK(icon_check((HICON)SendMessageW(notice.notice, WM_GETICON, ICON_BIG, 0), GetSystemMetricsForDpi(SM_CXICON, windowDpi)) == 0);
	CHECK(icon_check((HICON)SendMessageW(notice.notice, WM_GETICON, ICON_SMALL, 0), GetSystemMetricsForDpi(SM_CXSMICON, windowDpi)) == 0);
	const UINT scales[] = { 96, 144, 192 };
	for (size_t index = 0; index < ARRAYSIZE(scales); ++index)
	{
		const UINT dpi = scales[index];
		RECT bounds = { 0, 0, MulDiv(448, dpi, 96), MulDiv(240, dpi, 96) };
		CHECK(AdjustWindowRectExForDpi(&bounds, (DWORD)GetWindowLongW(notice.notice, GWL_STYLE), FALSE,
		    (DWORD)GetWindowLongW(notice.notice, GWL_EXSTYLE), windowDpi));
		SendMessageW(notice.notice, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&bounds);
		CHECK(notice_layout_check(notice.notice) == 0);
		CHECK(icon_check((HICON)SendDlgItemMessageW(notice.notice, 100, STM_GETICON, 0, 0), MulDiv(40, dpi, 96)) == 0);
		if (argc == 2 && strcmp(argv[1], "--preview") == 0) CHECK(notice_preview(notice.notice, dpi) == 0);
	}
	CHECK(LOWORD(SendMessageW(notice.notice, DM_GETDEFID, 0, 0)) == IDCANCEL);
	SendMessageW(notice.notice, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
	CHECK(!notice.notice && !notice.quit && !notice.reconnectRequested);
	rdk_media_notice(&notice);
	CHECK(notice.notice);
	SendMessageW(notice.notice, WM_CLOSE, 0, 0);
	CHECK(!notice.notice && !notice.quit && !notice.reconnectRequested);
	rdk_media_notice(&notice);
	CHECK(notice.notice);
	SendMessageW(notice.notice, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), 0);
	CHECK(!notice.notice && notice.quit && notice.reconnectRequested);
	DestroyWindow(notice.hwnd);
	puts("Passed reconnect settings/lifecycle and non-activating notice tests");
	return 0;
}