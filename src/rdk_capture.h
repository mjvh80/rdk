#ifndef RDK_CAPTURE_H
#define RDK_CAPTURE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#define RDK_WM_KEY (WM_APP + 1)

typedef struct
{
	HWND window;
	HANDLE thread;
	HANDLE ready;
	HANDLE stop;
	LONG state;
	LONG error;
	HWND (WINAPI *foreground)(void);
	BOOL (WINAPI *post)(HWND, UINT, WPARAM, LPARAM);
} rdkCapture;

void rdk_capture_init(rdkCapture* capture, HWND window);
BOOL rdk_capture_start(rdkCapture* capture, HWND window);
void rdk_capture_stop(rdkCapture* capture);
void rdk_capture_set_active(rdkCapture* capture, BOOL active);
DWORD rdk_capture_error(rdkCapture* capture);
BOOL rdk_capture_current(rdkCapture* capture, WPARAM key);
BOOL rdk_capture_route(rdkCapture* capture, int code, WPARAM message, const KBDLLHOOKSTRUCT* event);

#endif