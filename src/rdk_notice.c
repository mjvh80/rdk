#include "rdk_client.h"
#include "rdk_resources.h"
#include <stdio.h>
#include <stdlib.h>

enum { RDK_NOTICE_ICON = 100, RDK_NOTICE_TITLE, RDK_NOTICE_BODY };

typedef struct
{
	rdkContext* context;
	HFONT body;
	HFONT title;
	UINT dpi;
} rdkNotice;

static void rdk_notice_layout(HWND window, UINT dpi)
{
	rdkNotice* notice = (rdkNotice*)GetWindowLongPtrW(window, GWLP_USERDATA);
	notice->dpi = dpi;
	HFONT body = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
	    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
	    DEFAULT_PITCH, L"Segoe UI");
	HFONT title = CreateFontW(-MulDiv(20, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
	    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
	    DEFAULT_PITCH, L"Segoe UI");
	const struct { int id; int left; int top; int width; int height; } controls[] = {
		{ RDK_NOTICE_ICON, 24, 26, 40, 40 },
		{ RDK_NOTICE_TITLE, 80, 24, 344, 32 },
		{ RDK_NOTICE_BODY, 80, 66, 344, 76 },
		{ IDYES, 196, 180, 124, 36 },
		{ IDCANCEL, 332, 180, 92, 36 }
	};
	for (size_t index = 0; index < ARRAYSIZE(controls); ++index)
	{
		HWND control = GetDlgItem(window, controls[index].id);
		SetWindowPos(control, NULL, MulDiv(controls[index].left, dpi, 96),
		    MulDiv(controls[index].top, dpi, 96), MulDiv(controls[index].width, dpi, 96),
		    MulDiv(controls[index].height, dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);
		SendMessageW(control, WM_SETFONT, (WPARAM)(controls[index].id == RDK_NOTICE_TITLE ? title : body), TRUE);
	}
	if (notice->body) DeleteObject(notice->body);
	if (notice->title) DeleteObject(notice->title);
	notice->body = body;
	notice->title = title;
	HICON icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_RDK), IMAGE_ICON,
	    MulDiv(40, dpi, 96), MulDiv(40, dpi, 96), LR_SHARED);
	SendDlgItemMessageW(window, RDK_NOTICE_ICON, STM_SETICON, (WPARAM)icon, 0);
	rdk_window_icons(window);
	InvalidateRect(window, NULL, TRUE);
}

static LRESULT CALLBACK rdk_notice_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	rdkNotice* notice = (rdkNotice*)GetWindowLongPtrW(window, GWLP_USERDATA);
	if (message == WM_NCCREATE)
	{
		notice = calloc(1, sizeof(*notice));
		if (!notice) return FALSE;
		notice->context = (rdkContext*)((CREATESTRUCTW*)lParam)->lpCreateParams;
		notice->dpi = GetDpiForWindow(window);
		SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)notice);
	}
	rdkContext* rdk = notice ? notice->context : NULL;
	if (message == WM_DPICHANGED && notice)
	{
		const RECT* rect = (const RECT*)lParam;
		SetWindowPos(window, NULL, rect->left, rect->top, rect->right - rect->left,
		    rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
		rdk_notice_layout(window, HIWORD(wParam));
		return 0;
	}
	if (message == WM_ERASEBKGND && notice)
	{
		RECT rect;
		GetClientRect(window, &rect);
		FillRect((HDC)wParam, &rect, GetSysColorBrush(COLOR_WINDOW));
		rect.top = MulDiv(160, notice->dpi, 96);
		FillRect((HDC)wParam, &rect, GetSysColorBrush(COLOR_3DFACE));
		return 1;
	}
	if (message == WM_CTLCOLORSTATIC)
	{
		SetTextColor((HDC)wParam, GetSysColor(COLOR_WINDOWTEXT));
		SetBkColor((HDC)wParam, GetSysColor(COLOR_WINDOW));
		return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
	}
	if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED && rdk)
	{
		if (LOWORD(wParam) == IDYES)
		{
			printf("rdk: device-change notice: Reconnect selected; restarting the RDP connection\n");
			fflush(stdout);
			rdk->reconnectRequested = TRUE;
			rdk->stopReason = "device-change reconnect requested";
			rdk->quit = TRUE;
		}
		else if (LOWORD(wParam) == IDCANCEL)
		{
			printf("rdk: device-change notice: Later selected; keeping the current connection\n");
			fflush(stdout);
		}
		if (LOWORD(wParam) == IDYES || LOWORD(wParam) == IDCANCEL)
		{
			DestroyWindow(window);
			return 0;
		}
	}
	if (message == WM_CLOSE && rdk)
	{
		printf("rdk: device-change notice closed; keeping the current connection\n");
		fflush(stdout);
	}
	if (message == WM_DESTROY && rdk)
		rdk->notice = NULL;
	if (message == WM_NCDESTROY && notice)
	{
		if (notice->body) DeleteObject(notice->body);
		if (notice->title) DeleteObject(notice->title);
		free(notice);
		SetWindowLongPtrW(window, GWLP_USERDATA, 0);
	}
	if (message == DM_GETDEFID)
		return MAKELRESULT(IDCANCEL, DC_HASDEFID);
	return DefWindowProcW(window, message, wParam, lParam);
}

void rdk_media_notice(rdkContext* rdk)
{
	if (rdk->notice || !rdk->hwnd)
		return;
	WNDCLASSEXW definition = { 0 };
	definition.cbSize = sizeof(definition);
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpfnWndProc = rdk_notice_proc;
	definition.hCursor = LoadCursorW(NULL, IDC_ARROW);
	definition.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	definition.lpszClassName = L"rdkMediaNotice";
	RegisterClassExW(&definition);
	const UINT dpi = GetDpiForWindow(rdk->hwnd);
	const int width = MulDiv(448, dpi, 96);
	const int height = MulDiv(240, dpi, 96);
	MONITORINFO monitor = { sizeof(monitor) };
	GetMonitorInfoW(MonitorFromWindow(rdk->hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
	RECT rect = { 0, 0, width, height };
	const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
	const DWORD extended = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT;
	AdjustWindowRectExForDpi(&rect, style, FALSE, extended, dpi);
	rdk->notice = CreateWindowExW(extended, definition.lpszClassName, L"rdk", style,
	    monitor.rcWork.right - (rect.right - rect.left) - MulDiv(20, dpi, 96),
	    monitor.rcWork.bottom - (rect.bottom - rect.top) - MulDiv(20, dpi, 96),
	    rect.right - rect.left, rect.bottom - rect.top, rdk->hwnd, NULL, definition.hInstance, rdk);
	if (!rdk->notice)
	{
		fprintf(stderr, "rdk: could not show device-change notice; reconnect manually to refresh devices\n");
		return;
	}
	CreateWindowExW(0, L"STATIC", NULL, WS_CHILD | WS_VISIBLE | SS_ICON,
	    0, 0, 0, 0, rdk->notice, (HMENU)(INT_PTR)RDK_NOTICE_ICON, definition.hInstance, NULL);
	CreateWindowExW(0, L"STATIC", L"Update remote devices", WS_CHILD | WS_VISIBLE,
	    0, 0, 0, 0, rdk->notice, (HMENU)(INT_PTR)RDK_NOTICE_TITLE, definition.hInstance, NULL);
	CreateWindowExW(0, L"STATIC", L"A microphone or camera changed.\r\n"
	    L"Reconnect to use the updated devices.\r\nAn active call may be interrupted.", WS_CHILD | WS_VISIBLE,
	    0, 0, 0, 0, rdk->notice, (HMENU)(INT_PTR)RDK_NOTICE_BODY, definition.hInstance, NULL);
	CreateWindowExW(0, L"BUTTON", L"Reconnect", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
	    0, 0, 0, 0,
	    rdk->notice, (HMENU)(INT_PTR)IDYES, definition.hInstance, NULL);
	CreateWindowExW(0, L"BUTTON", L"Later", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
	    0, 0, 0, 0,
	    rdk->notice, (HMENU)(INT_PTR)IDCANCEL, definition.hInstance, NULL);
	rdk_notice_layout(rdk->notice, GetDpiForWindow(rdk->notice));
	ShowWindow(rdk->notice, SW_SHOWNOACTIVATE);
	printf("rdk: device-change notice shown; waiting for Reconnect or Later\n");
	fflush(stdout);
}