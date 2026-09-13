#ifndef RDK_RESOURCES_H
#define RDK_RESOURCES_H

#define IDI_RDK 101

#ifndef RC_INVOKED
#include <windows.h>

static inline void rdk_window_icons(HWND window)
{
	const UINT dpi = GetDpiForWindow(window);
	const HINSTANCE instance = GetModuleHandleW(NULL);
	HICON largeIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_RDK), IMAGE_ICON,
	    GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), LR_SHARED);
	HICON smallIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_RDK), IMAGE_ICON,
	    GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_SHARED);
	SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)largeIcon);
	SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
}
#endif

#endif
