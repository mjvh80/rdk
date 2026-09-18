#ifndef RDK_CLIENT_H
#define RDK_CLIENT_H

/* Include winsock2 before windows.h to avoid the legacy winsock.h clash, and
 * keep windows.h lean so it doesn't drag in winsock.h itself. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/update.h>
#include <freerdp/client/rdpgfx.h>

#include "rdk_keyboard.h"
#include "rdk_capture.h"
#include "rdk_display.h"
#include "rdk_media.h"
#include "rdk_clipboard.h"

/* Custom FreeRDP client context. rdpClientContext (which embeds rdpContext as
 * its first member) must stay first so FreeRDP can treat this as an rdpContext. */
typedef struct
{
	rdpClientContext common;

	HWND hwnd;         /* fullscreen window spanning the selected displays */
	HWND notice;
	rdkMediaWatch* mediaWatch;
	rdkClipboard* clipboard;
	BOOL reconnectRequested;
	BOOL cameraEnabled;
	BOOL cameraWatching;
	HDEVNOTIFY cameraNotification;
	UINT64 cameraCheckAt;
	rdkMediaChange cameraChange;
	HCURSOR cursor;
	BITMAPINFO bmi;    /* describes the FreeRDP framebuffer for GDI blits */
	RECT displayBounds;
	int winX, winY;    /* window origin in virtual-screen coordinates */
	int winW, winH;    /* window size */

	pEndPaint origEndPaint; /* chained GDI end-paint callback */
	pcRdpgfxSurfaceCommand origSurfaceCommand;
	UINT32 gfxCodecsSeen;

	rdkKeyboard keyboard;
	rdkCapture capture;
	WPARAM lockIndicators;
	BOOL lockIndicatorsPending;
	BOOL recovering;
	BOOL focused;
	BOOL desktopReady;
	BOOL inputFailed;
	const char* stopReason;
	UINT16 mouseButtons;
	BOOL quit; /* set from the window thread when the user asks to exit */
} rdkContext;

BOOL rdk_pointer_register(rdkContext* rdk);
BOOL rdk_pointer_setcursor(rdkContext* rdk, HWND window, UINT hitTest);
void rdk_media_notice(rdkContext* rdk);

/* PostConnect/PostDisconnect: framebuffer + window lifecycle. */
BOOL rdk_gdi_post_connect(freerdp* instance);
void rdk_gdi_activate(rdkContext* rdk);
BOOL rdk_gdi_recovery(rdkContext* rdk, BOOL active);
void rdk_gdi_post_disconnect(freerdp* instance);

#endif /* RDK_CLIENT_H */
