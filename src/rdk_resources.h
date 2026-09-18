#ifndef RDK_RESOURCES_H
#define RDK_RESOURCES_H

#define IDI_RDK 101
#define IDD_SESSION_MENU 102
#define IDC_SESSION_MINIMIZE 1101
#define IDC_SESSION_SECURITY 1102
#define IDC_SESSION_RECONNECT 1103
#define IDC_SESSION_DISCONNECT 1104
#define IDC_SESSION_ICON 1105

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
