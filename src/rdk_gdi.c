/*
 * rdk — rendering + input for the fullscreen, multi-monitor RDP client.
 *
 * A single borderless (WS_POPUP) window spans the selected local displays.
 * We advertise a matching multi-monitor layout to the server, which makes
 * the FreeRDP framebuffer map 1:1 to our window client area — so blits and
 * mouse coordinates need no further translation.
 *
 * Alt-code composition is handled locally by the keyboard router; ordinary
 * keys and shortcuts retain the remote keyboard layout's scan-code behavior.
 */

#include "rdk_client.h"
#include "rdk_camera.h"
#include "rdk_resources.h"
#include "rdk_taskbar.h"
#include "rdk_latency.h"

#include <stdio.h>
#include <dbt.h>
#include <string.h> /* strcmp */
#include <windowsx.h> /* GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM */

#include <freerdp/settings.h>
#include <freerdp/codec/color.h>     /* PIXEL_FORMAT_BGRX32 */
#include <freerdp/gdi/gfx.h>         /* gdi_graphics_pipeline_init / _uninit */
#include <freerdp/client/rdpgfx.h>   /* RdpgfxClientContext */
#include <freerdp/channels/rdpgfx.h> /* RDPGFX_DVC_CHANNEL_NAME */
#include <freerdp/event.h>           /* ChannelConnected / ChannelDisconnected events */

static const wchar_t* const RDK_WINDOW_CLASS = L"rdkWindowClass";
static const UINT_PTR RDK_LOCK_TIMER = 1;
static const UINT RDK_SC_CTRL_ALT_DELETE = 0x1000;

/* ---- rendering + input ------------------------------------------------- */

static void rdk_blit(rdkContext* rdk, HDC hdc)
{
	rdpGdi* gdi = ((rdpContext*)rdk)->gdi;
	if (!gdi || !gdi->primary_buffer)
		return;
	const UINT64 started = rdk_latency_begin();
	(void)StretchDIBits(hdc, 0, 0, gdi->width, gdi->height, 0, 0, gdi->width, gdi->height,
	                    gdi->primary_buffer, &rdk->bmi, DIB_RGB_COLORS, SRCCOPY);
	rdk_latency_end(RDK_LATENCY_PAINT, started);
}

static void rdk_input_result(rdkContext* rdk, BOOL ok)
{
	if (!ok)
	{
		if (!rdk->inputFailed)
			fprintf(stderr, "rdk: failed to send input; closing the session\n");
		rdk->stopReason = "input forwarding failed";
		rdk->inputFailed = TRUE;
		rdk->quit = TRUE;
	}
}

static void rdk_cancel_lock_sync(rdkContext* rdk)
{
	if (rdk->lockIndicatorsPending)
	{
		KillTimer(rdk->hwnd, RDK_LOCK_TIMER);
		rdk->lockIndicatorsPending = FALSE;
	}
}

static BOOL rdk_set_keyboard_indicators(rdpContext* context, UINT16 flags)
{
	rdkContext* rdk = (rdkContext*)context;
	if (!rdk->focused || rdk->quit)
		return TRUE;
	const LONG state = InterlockedCompareExchange(&rdk->capture.state, 0, 0);
	const WPARAM indicators = MAKEWPARAM(flags, LOWORD(state));
	if (!rdk_capture_current(&rdk->capture, indicators))
		return TRUE;
	rdk->lockIndicators = indicators;
	rdk->lockIndicatorsPending = SetTimer(rdk->hwnd, RDK_LOCK_TIMER, USER_TIMER_MINIMUM, NULL) != 0;
	if (!rdk->lockIndicatorsPending)
		fprintf(stderr, "rdk: could not schedule local lock-key synchronization (%lu)\n", GetLastError());
	return TRUE;
}

static void rdk_on_key(rdkContext* rdk, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (!rdk->focused || rdk->quit)
		return;
	if (wParam == VK_NUMLOCK || wParam == VK_CAPITAL)
		rdk_cancel_lock_sync(rdk);
	rdk_input_result(rdk, rdk_keyboard_key(&rdk->keyboard, msg, wParam, lParam));
	if (rdk->keyboard.reconnect && !rdk->inputFailed)
	{
		printf("rdk: reconnect shortcut selected; restarting the RDP connection\n");
		fflush(stdout);
		rdk->reconnectRequested = TRUE;
	}
	rdk->quit = rdk->quit || rdk->keyboard.quit;
	if (rdk->keyboard.quit && !rdk->inputFailed)
		rdk->stopReason = rdk->keyboard.reconnect ? "reconnect shortcut requested" : "quit shortcut requested";
	if (rdk->keyboard.minimize)
	{
		rdk->keyboard.minimize = FALSE;
		if (!rdk->quit)
		{
			printf("rdk: minimize shortcut selected; keeping the RDP connection open\n");
			fflush(stdout);
			SendMessageW(rdk->hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
		}
	}
}

static void rdk_release_mouse(rdkContext* rdk)
{
	if (rdk->recovering)
		return;
	rdpInput* input = ((rdpContext*)rdk)->input;
	const UINT16 buttons[] = { PTR_FLAGS_BUTTON1, PTR_FLAGS_BUTTON2, PTR_FLAGS_BUTTON3 };
	for (size_t index = 0; index < ARRAYSIZE(buttons); ++index)
	{
		if (rdk->mouseButtons & buttons[index])
			rdk_input_result(rdk, freerdp_input_send_mouse_event(input, buttons[index], 0, 0));
	}
	rdk->mouseButtons = 0;
}

/* Sync the remote session's lock-key state (Caps/Num/Scroll) to the local one
 * whenever we gain focus. Otherwise the server can disagree about Caps Lock and
 * letters come out with the wrong case. */
static void rdk_send_focus_in(rdkContext* rdk)
{
	rdpInput* input = ((rdpContext*)rdk)->input;
	if (!input)
		return;

	UINT16 sync = 0;
	if (GetKeyState(VK_NUMLOCK) & 1)
		sync |= KBD_SYNC_NUM_LOCK;
	if (GetKeyState(VK_CAPITAL) & 1)
		sync |= KBD_SYNC_CAPS_LOCK;
	if (GetKeyState(VK_SCROLL) & 1)
		sync |= KBD_SYNC_SCROLL_LOCK;
	rdk_input_result(rdk, freerdp_input_send_focus_in_event(input, sync));
}

static void rdk_on_mouse(rdkContext* rdk, UINT msg, WPARAM wParam, LPARAM lParam)
{
	rdpInput* input = ((rdpContext*)rdk)->input;
	if (rdk->quit || rdk->recovering)
		return;
	if (msg != WM_MOUSEMOVE)
	{
		rdk_input_result(rdk, rdk_keyboard_flush(&rdk->keyboard));
		if (rdk->quit)
			return;
	}

	if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
	{
		int delta = GET_WHEEL_DELTA_WPARAM(wParam);
		const UINT16 flags = (msg == WM_MOUSEWHEEL) ? PTR_FLAGS_WHEEL : PTR_FLAGS_HWHEEL;
		while (delta != 0)
		{
			const int step = (delta > 255) ? 255 : ((delta < -256) ? -256 : delta);
			rdk_input_result(rdk, freerdp_input_send_mouse_event(input,
			    flags | (UINT16)(step & WheelRotationMask), 0, 0));
			if (rdk->quit)
				break;
			delta -= step;
		}
		return;
	}

	int x = GET_X_LPARAM(lParam);
	int y = GET_Y_LPARAM(lParam);
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;

	UINT16 flags = 0;
	switch (msg)
	{
		case WM_MOUSEMOVE:
			flags = PTR_FLAGS_MOVE;
			break;
		case WM_LBUTTONDOWN:
			flags = PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON1;
			break;
		case WM_LBUTTONUP:
			flags = PTR_FLAGS_BUTTON1;
			break;
		case WM_RBUTTONDOWN:
			flags = PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON2;
			break;
		case WM_RBUTTONUP:
			flags = PTR_FLAGS_BUTTON2;
			break;
		case WM_MBUTTONDOWN:
			flags = PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON3;
			break;
		case WM_MBUTTONUP:
			flags = PTR_FLAGS_BUTTON3;
			break;
		default:
			return;
	}
	const UINT16 button = flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3);
	if (button)
	{
		if (flags & PTR_FLAGS_DOWN)
		{
			rdk->mouseButtons |= button;
			SetCapture(rdk->hwnd);
		}
		else
			rdk->mouseButtons &= ~button;
	}
	rdk_input_result(rdk, freerdp_input_send_mouse_event(input, flags, (UINT16)x, (UINT16)y));
	if (button && rdk->mouseButtons == 0 && GetCapture() == rdk->hwnd)
		ReleaseCapture();
}

static void rdk_focus_out(rdkContext* rdk)
{
	if (!rdk)
		return;
	rdk_cancel_lock_sync(rdk);
	rdk_capture_set_active(&rdk->capture, FALSE);
	if (!rdk->focused)
		return;
	rdk->focused = FALSE;
	rdk_input_result(rdk, rdk_keyboard_release_all(&rdk->keyboard));
	rdk_release_mouse(rdk);
	if (GetCapture() == rdk->hwnd)
		ReleaseCapture();
}

static LRESULT CALLBACK rdk_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	rdkContext* rdk = (rdkContext*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

	switch (msg)
	{
		case WM_INITMENUPOPUP:
			EnableMenuItem(GetSystemMenu(hwnd, FALSE), RDK_SC_CTRL_ALT_DELETE,
			    MF_BYCOMMAND | ((rdk && !rdk->quit && !rdk->inputFailed && !rdk->recovering) ? MF_ENABLED : MF_GRAYED));
			return DefWindowProcW(hwnd, msg, wParam, lParam);
		case WM_SYSCOMMAND:
			if ((wParam & 0xFFF0) == RDK_SC_CTRL_ALT_DELETE)
			{
				if (rdk && !rdk->quit && !rdk->inputFailed && !rdk->recovering)
					rdk_input_result(rdk, rdk_keyboard_ctrl_alt_delete(&rdk->keyboard));
				return 0;
			}
			return DefWindowProcW(hwnd, msg, wParam, lParam);
		case WM_TIMER:
			if (wParam != RDK_LOCK_TIMER)
				return DefWindowProcW(hwnd, msg, wParam, lParam);
			if (rdk && rdk->lockIndicatorsPending)
			{
				if (!rdk->focused || rdk->quit ||
				    !rdk_capture_current(&rdk->capture, rdk->lockIndicators))
					rdk_cancel_lock_sync(rdk);
				else if (rdk_keyboard_idle(&rdk->keyboard))
				{
					rdk_cancel_lock_sync(rdk);
					const UINT16 flags = LOWORD(rdk->lockIndicators);
					if (!rdk_capture_sync_locks(&rdk->capture, flags & KBD_SYNC_NUM_LOCK, flags & KBD_SYNC_CAPS_LOCK))
						fprintf(stderr, "rdk: could not update local Num Lock/Caps Lock state\n");
				}
			}
			return 0;
		case RDK_WM_TASKBAR_RECONNECT:
			if (rdk && !rdk->quit)
			{
				printf("rdk: taskbar Reconnect selected; restarting the RDP connection\n");
				fflush(stdout);
				rdk->reconnectRequested = TRUE;
				rdk->stopReason = "taskbar reconnect requested";
				rdk->quit = TRUE;
			}
			return 0;
		case WM_DPICHANGED:
			rdk_window_icons(hwnd);
			return 0;
		case WM_DEVICECHANGE:
			if (rdk && rdk->cameraWatching && (wParam == DBT_DEVICEARRIVAL ||
			    wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED))
			{
				rdk->cameraCheckAt = GetTickCount64() + 1500;
				const DEV_BROADCAST_HDR* header = (const DEV_BROADCAST_HDR*)lParam;
				if (wParam == DBT_DEVICEREMOVECOMPLETE && header &&
				    header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
				{
					const DEV_BROADCAST_DEVICEINTERFACE_W* device = (const DEV_BROADCAST_DEVICEINTERFACE_W*)header;
					if (rdk_camera_snapshot_contains(rdk->cameraChange.baseline, device->dbcc_name))
						rdk_media_change_invalidate(&rdk->cameraChange, GetTickCount64());
				}
			}
			return DefWindowProcW(hwnd, msg, wParam, lParam);
		case WM_NCCREATE:
		{
			CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
			return DefWindowProcW(hwnd, msg, wParam, lParam);
		}
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			if (rdk && hdc)
				rdk_blit(rdk, hdc);
			EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_SETFOCUS:
			if (rdk && !rdk->recovering && !IsIconic(hwnd))
			{
				rdk->focused = TRUE;
				rdk_capture_set_active(&rdk->capture, TRUE);
				rdk_send_focus_in(rdk);
			}
			return 0;
		case WM_SETCURSOR:
			if (rdk_pointer_setcursor(rdk, (HWND)wParam, LOWORD(lParam)))
				return TRUE;
			return DefWindowProcW(hwnd, msg, wParam, lParam);
		case WM_KILLFOCUS:
			rdk_focus_out(rdk);
			return 0;
		case WM_SIZE:
			if (wParam == SIZE_MINIMIZED)
				rdk_focus_out(rdk);
			return DefWindowProcW(hwnd, msg, wParam, lParam);
		case WM_CAPTURECHANGED:
			if (rdk)
				rdk_release_mouse(rdk);
			return 0;
		case WM_INPUTLANGCHANGE:
			if (rdk)
			{
				rdk_input_result(rdk, rdk_keyboard_flush(&rdk->keyboard));
				rdk_keyboard_layout(&rdk->keyboard, (HKL)lParam);
			}
			return 1;
		case WM_CHAR:
		case WM_SYSCHAR:
		case WM_DEADCHAR:
		case WM_SYSDEADCHAR:
			return 0;
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
			return 0;
		case RDK_WM_KEY:
			if (rdk && rdk_capture_current(&rdk->capture, wParam))
				rdk_on_key(rdk, (lParam & ((LPARAM)1 << 31)) ? WM_KEYUP : WM_KEYDOWN,
				           LOWORD(wParam), lParam);
			return 0;
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_MOUSEWHEEL:
		case WM_MOUSEHWHEEL:
			if (rdk)
				rdk_on_mouse(rdk, msg, wParam, lParam);
			return 0;
		case WM_CLOSE:
			if (rdk)
			{
				rdk->stopReason = "window close requested";
				rdk->quit = TRUE;
			}
			return 0;
		default:
			return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

/* ---- update callbacks -------------------------------------------------- */

static BOOL rdk_end_paint(rdpContext* context)
{
	rdkContext* rdk = (rdkContext*)context;
	if (rdk->origEndPaint && !rdk->origEndPaint(context))
		return FALSE;
	rdk->desktopReady = TRUE;
	if (rdk->hwnd)
		InvalidateRect(rdk->hwnd, NULL, FALSE);
	return TRUE;
}

static BOOL rdk_desktop_resize(rdpContext* context)
{
	rdkContext* rdk = (rdkContext*)context;
	rdpGdi* gdi = context->gdi;
	const UINT32 w = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
	const UINT32 h = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);

	if (!gdi_resize(gdi, w, h))
		return FALSE;

	rdk->bmi.bmiHeader.biWidth = gdi->width;
	rdk->bmi.bmiHeader.biHeight = -gdi->height;
	if (rdk->hwnd)
		InvalidateRect(rdk->hwnd, NULL, FALSE);
	return TRUE;
}

/* ---- graphics pipeline (RDPGFX) --------------------------------------- */

/* Modern Windows servers push the desktop through the RDPGFX dynamic channel
 * (H.264 / progressive / planar) instead of legacy bitmap updates. Without
 * binding that channel to the GDI framebuffer the primary buffer is never
 * filled and the window stays blank. gdi_graphics_pipeline_init() makes the
 * GFX decoder render into gdi->primary_buffer and drive the EndPaint path. */
static const char* rdk_gfx_codec_name(UINT32 codec)
{
	switch (codec)
	{
		case RDPGFX_CODECID_AVC444v2: return "AVC444v2";
		case RDPGFX_CODECID_AVC444: return "AVC444";
		case RDPGFX_CODECID_AVC420: return "AVC420";
		case RDPGFX_CODECID_UNCOMPRESSED: return "Uncompressed";
		case RDPGFX_CODECID_CAVIDEO: return "RemoteFX";
		case RDPGFX_CODECID_CLEARCODEC: return "ClearCodec";
		case RDPGFX_CODECID_PLANAR: return "Planar";
		case RDPGFX_CODECID_CAPROGRESSIVE: return "Progressive";
		case RDPGFX_CODECID_CAPROGRESSIVE_V2: return "Progressive v2";
		case RDPGFX_CODECID_ALPHA: return "Alpha";
		default: return "Other";
	}
}

static UINT rdk_gfx_surface_command(RdpgfxClientContext* gfx, const RDPGFX_SURFACE_COMMAND* command)
{
	if (!gfx || !gfx->custom || !command)
		return ERROR_INVALID_DATA;
	rdpGdi* gdi = (rdpGdi*)gfx->custom;
	rdkContext* rdk = (rdkContext*)gdi->context;
	if (!rdk || !rdk->origSurfaceCommand)
		return ERROR_INVALID_DATA;
	const UINT64 started = rdk_latency_begin();
	const UINT status = rdk->origSurfaceCommand(gfx, command);
	rdk_latency_end(RDK_LATENCY_DECODE, started);
	const UINT32 mask = 1u << (command->codecId < 31 ? command->codecId : 31);
	if (status == CHANNEL_RC_OK && !(rdk->gfxCodecsSeen & mask))
	{
		rdk->gfxCodecsSeen |= mask;
		printf("rdk: graphics received: %s (codec=0x%04lX); decoded to GDI framebuffer\n",
		    rdk_gfx_codec_name(command->codecId), (unsigned long)command->codecId);
		fflush(stdout);
	}
	return status;
}

static void rdk_on_channel_connected(void* context, const ChannelConnectedEventArgs* e)
{
	rdpContext* ctx = (rdpContext*)context;
	if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
	{
		rdkContext* rdk = (rdkContext*)context;
		RdpgfxClientContext* gfx = (RdpgfxClientContext*)e->pInterface;
		if (!gdi_graphics_pipeline_init(ctx->gdi, gfx))
		{
			fprintf(stderr, "rdk: GDI graphics pipeline initialization failed\n");
			rdk->stopReason = "GDI graphics pipeline initialization failed";
			rdk->inputFailed = TRUE;
			rdk->quit = TRUE;
			return;
		}
		rdk->origSurfaceCommand = gfx->SurfaceCommand;
		rdk->gfxCodecsSeen = 0;
		gfx->SurfaceCommand = rdk_gfx_surface_command;
		printf("rdk: GFX channel connected; waiting for server graphics updates\n");
		fflush(stdout);
	}
	else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0 &&
	         freerdp_settings_get_bool(ctx->settings, FreeRDP_RedirectClipboard))
	{
		rdkContext* rdk = (rdkContext*)context;
		rdk->clipboard = rdk_clipboard_new((CliprdrClientContext*)e->pInterface, TRUE);
		if (!rdk->clipboard)
			fprintf(stderr, "rdk: Windows clipboard redirection could not start\n");
	}
}

static void rdk_on_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e)
{
	rdpContext* ctx = (rdpContext*)context;
	if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
	{
		gdi_graphics_pipeline_uninit(ctx->gdi, (RdpgfxClientContext*)e->pInterface);
		((rdkContext*)context)->origSurfaceCommand = NULL;
		((rdkContext*)context)->gfxCodecsSeen = 0;
	}
	else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
	{
		rdkContext* rdk = (rdkContext*)context;
		rdk_clipboard_free(rdk->clipboard);
		rdk->clipboard = NULL;
	}
}

/* ---- connect / disconnect --------------------------------------------- */

/* Reliably move keyboard focus to our window. Launched from a terminal, the
 * console keeps the foreground and a plain SetForegroundWindow() is refused by
 * the foreground-lock rules -- so mouse input (delivered by cursor position)
 * works while typed keys leak to the terminal behind us. Briefly attaching to
 * the current foreground thread's input queue lets us take focus for real. */
static void rdk_force_foreground(HWND hwnd)
{
	const HWND fg = GetForegroundWindow();
	const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
	const DWORD myThread = GetCurrentThreadId();
	const BOOL attach = (fgThread != 0) && (fgThread != myThread);

	if (attach)
		(void)AttachThreadInput(myThread, fgThread, TRUE);

	BringWindowToTop(hwnd);
	(void)SetForegroundWindow(hwnd);
	(void)SetActiveWindow(hwnd);
	(void)SetFocus(hwnd);

	if (attach)
		(void)AttachThreadInput(myThread, fgThread, FALSE);
}

static HWND rdk_create_window(rdkContext* rdk)
{
	WNDCLASSEXW wc = { 0 };
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = rdk_wndproc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszClassName = RDK_WINDOW_CLASS;
	RegisterClassExW(&wc); /* harmless if already registered */

	HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, RDK_WINDOW_CLASS, L"rdk",
	    WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, rdk->winX, rdk->winY, rdk->winW, rdk->winH,
	    NULL, NULL, wc.hInstance, rdk);
	if (!window)
		return NULL;
	HMENU menu = GetSystemMenu(window, FALSE);
	if (!menu || !InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING, RDK_SC_CTRL_ALT_DELETE,
	                         L"Send Ctrl+Alt+Delete\tCtrl+Alt+End") ||
	    !InsertMenuW(menu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL))
	{
		DestroyWindow(window);
		return NULL;
	}
	return window;
}

BOOL rdk_gdi_post_connect(freerdp* instance)
{
	rdpContext* context = instance->context;
	rdkContext* rdk = (rdkContext*)context;
	rdk->desktopReady = FALSE;
	rdk_keyboard_init(&rdk->keyboard, context->input);

	if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
		return FALSE;
	if (!rdk_pointer_register(rdk))
		return FALSE;

	rdpGdi* gdi = context->gdi;

	rdk->winX = rdk->displayBounds.left;
	rdk->winY = rdk->displayBounds.top;
	rdk->winW = rdk->displayBounds.right - rdk->displayBounds.left;
	rdk->winH = rdk->displayBounds.bottom - rdk->displayBounds.top;

	rdk->hwnd = rdk_create_window(rdk);
	if (!rdk->hwnd)
		return FALSE;
	rdk_window_icons(rdk->hwnd);
	(void)rdk_taskbar_attach(rdk->hwnd);
	if (!rdk_capture_start(&rdk->capture, rdk->hwnd))
	{
		fprintf(stderr, "rdk: keyboard capture could not start (%lu)\n", rdk_capture_error(&rdk->capture));
		return FALSE;
	}
	if (freerdp_settings_get_bool(context->settings, FreeRDP_AudioCapture))
	{
		printf("rdk: microphone redirection requested; remote capture is not yet verified\n");
		(void)rdk_microphone_status();
		rdk->mediaWatch = rdk_media_watch_new();
		if (!rdk->mediaWatch)
			fprintf(stderr, "rdk: microphone change notifications are unavailable\n");
		else
			printf("rdk: microphone change monitoring active\n");
	}
	else
		printf("rdk: microphone redirection disabled\n");
	if (rdk->cameraEnabled)
	{
		printf("rdk: camera redirection requested; local camera inventory follows (not capturing)\n");
		if (!rdk_camera_list())
			fprintf(stderr, "rdk: local camera inventory is unavailable\n");
		WCHAR* snapshot = rdk_camera_snapshot();
		rdk->cameraWatching = snapshot && rdk_media_change_init(&rdk->cameraChange, snapshot);
		free(snapshot);
		DEV_BROADCAST_DEVICEINTERFACE_W filter = { 0 };
		filter.dbcc_size = sizeof(filter);
		filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
		if (rdk->cameraWatching)
			rdk->cameraNotification = RegisterDeviceNotificationW(rdk->hwnd, &filter,
			    DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
		if (!rdk->cameraNotification)
			fprintf(stderr, "rdk: camera change notifications are unavailable; reconnect after device changes\n");
		else
			printf("rdk: camera change monitoring active\n");
	}
	else
		printf("rdk: camera redirection disabled\n");
	fflush(stdout);

	ZeroMemory(&rdk->bmi, sizeof(rdk->bmi));
	rdk->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	rdk->bmi.bmiHeader.biWidth = gdi->width;
	rdk->bmi.bmiHeader.biHeight = -gdi->height; /* top-down */
	rdk->bmi.bmiHeader.biPlanes = 1;
	rdk->bmi.bmiHeader.biBitCount = 32;
	rdk->bmi.bmiHeader.biCompression = BI_RGB;

	rdpUpdate* update = context->update;
	rdk->origEndPaint = update->EndPaint;
	update->EndPaint = rdk_end_paint;
	update->DesktopResize = rdk_desktop_resize;
	update->SetKeyboardIndicators = rdk_set_keyboard_indicators;

	/* Bind the RDPGFX channel to the GDI framebuffer once it connects. Must be
	 * subscribed before the dynamic channels come up (i.e. here in PostConnect,
	 * which runs before the session activates the DVCs). */
	(void)PubSub_SubscribeChannelConnected(context->pubSub, rdk_on_channel_connected);
	(void)PubSub_SubscribeChannelDisconnected(context->pubSub, rdk_on_channel_disconnected);

	return TRUE;
}

void rdk_gdi_activate(rdkContext* rdk)
{
	ShowWindow(rdk->hwnd, SW_SHOW);
	rdk_force_foreground(rdk->hwnd);
}

BOOL rdk_gdi_recovery(rdkContext* rdk, BOOL active)
{
	rdk->recovering = active;
	if (active)
	{
		rdk_cancel_lock_sync(rdk);
		rdk->focused = FALSE;
		rdk_capture_set_active(&rdk->capture, FALSE);
		rdk->keyboard.altPending = FALSE;
		rdk->keyboard.pendingCount = 0;
		if (GetCapture() == rdk->hwnd)
			ReleaseCapture();
		if (rdk->notice)
			ShowWindow(rdk->notice, SW_HIDE);
		SetWindowTextW(rdk->hwnd, L"rdk - Reconnecting");
	}
	else
	{
		rdk_input_result(rdk, rdk_keyboard_release_all(&rdk->keyboard));
		rdk_release_mouse(rdk);
		SetWindowTextW(rdk->hwnd, L"rdk");
		if (!rdk->quit && GetForegroundWindow() == rdk->hwnd && GetFocus() == rdk->hwnd)
			SendMessageW(rdk->hwnd, WM_SETFOCUS, 0, 0);
	}
	return !rdk->inputFailed;
}

void rdk_gdi_post_disconnect(freerdp* instance)
{
	rdpContext* context = instance->context;
	rdkContext* rdk = (rdkContext*)context;

	printf("rdk: shutdown: cleaning up local session\n");
	fflush(stdout);
	rdk_clipboard_free(rdk->clipboard);
	rdk->clipboard = NULL;
	rdk_cancel_lock_sync(rdk);
	rdk_capture_stop(&rdk->capture);
	printf("rdk: shutdown: stopping microphone change monitoring\n");
	fflush(stdout);
	rdk_media_watch_free(rdk->mediaWatch);
	rdk->mediaWatch = NULL;
	printf("rdk: shutdown: releasing window and graphics\n");
	fflush(stdout);
	if (rdk->cameraNotification)
		UnregisterDeviceNotification(rdk->cameraNotification);
	rdk->cameraNotification = NULL;
	rdk->cameraWatching = FALSE;
	rdk_media_change_free(&rdk->cameraChange);
	if (rdk->notice)
		DestroyWindow(rdk->notice);
	(void)PubSub_UnsubscribeChannelConnected(context->pubSub, rdk_on_channel_connected);
	(void)PubSub_UnsubscribeChannelDisconnected(context->pubSub, rdk_on_channel_disconnected);
	rdk->focused = FALSE;
	if (rdk->hwnd)
	{
		DestroyWindow(rdk->hwnd);
		rdk->hwnd = NULL;
	}
	gdi_free(instance);
	printf("rdk: shutdown: local session cleanup complete\n");
	fflush(stdout);
}
