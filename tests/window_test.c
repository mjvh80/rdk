#include "../src/rdk_gdi.c"
#include <wchar.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

static UINT releases;
static UINT minimizeKeyEvents;
static UINT lockKeyEvents;
static BOOL record_scan(rdpInput* input, UINT16 flags, UINT8 code)
{
	(void)input;
	if (code == RDP_SCANCODE_NUMLOCK || code == RDP_SCANCODE_CAPSLOCK)
		++lockKeyEvents;
	if (code == RDP_SCANCODE_F10)
		++minimizeKeyEvents;
	if (flags == KBD_FLAGS_RELEASE && code == RDP_SCANCODE_LCONTROL)
		++releases;
	return TRUE;
}

static UINT surfaceCalls;
static UINT surfaceResult;
static const RDPGFX_SURFACE_COMMAND* lastSurfaceCommand;

static UINT record_surface(RdpgfxClientContext* gfx, const RDPGFX_SURFACE_COMMAND* command)
{
	(void)gfx;
	++surfaceCalls;
	lastSurfaceCommand = command;
	return surfaceResult;
}

static int check_graphics_logging(void)
{
	rdkContext rdk = { 0 };
	rdpGdi gdi = { 0 };
	RdpgfxClientContext gfx = { 0 };
	gdi.context = (rdpContext*)&rdk;
	gfx.custom = &gdi;
	rdk.origSurfaceCommand = record_surface;
	const UINT32 codecs[] = { RDPGFX_CODECID_AVC444v2, RDPGFX_CODECID_AVC444,
	    RDPGFX_CODECID_AVC420, RDPGFX_CODECID_PLANAR };
	for (size_t index = 0; index < ARRAYSIZE(codecs); ++index)
	{
		RDPGFX_SURFACE_COMMAND command = { 0 };
		command.codecId = codecs[index];
		const UINT32 previous = rdk.gfxCodecsSeen;
		surfaceResult = ERROR_INVALID_DATA;
		CHECK(rdk_gfx_surface_command(&gfx, &command) == ERROR_INVALID_DATA);
		CHECK(rdk.gfxCodecsSeen == previous);
		surfaceResult = CHANNEL_RC_OK;
		CHECK(rdk_gfx_surface_command(&gfx, &command) == CHANNEL_RC_OK);
		CHECK(rdk.gfxCodecsSeen == (previous | (1u << command.codecId)));
		CHECK(rdk_gfx_surface_command(&gfx, &command) == CHANNEL_RC_OK);
		CHECK(rdk.gfxCodecsSeen == (previous | (1u << command.codecId)));
		CHECK(lastSurfaceCommand == &command && surfaceCalls == (index + 1) * 3);
	}
	CHECK(!rdk.quit && !rdk.inputFailed);
	CHECK(strcmp(rdk_gfx_codec_name(RDPGFX_CODECID_AVC444v2), "AVC444v2") == 0);
	CHECK(strcmp(rdk_gfx_codec_name(RDPGFX_CODECID_AVC444), "AVC444") == 0);
	CHECK(strcmp(rdk_gfx_codec_name(RDPGFX_CODECID_AVC420), "AVC420") == 0);
	return 0;
}

static void pump_messages(void)
{
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
		DispatchMessageW(&message);
}

static HWND lockForeground;
static UINT16 localLocks;
static UINT lockInjections;

static HWND WINAPI lock_foreground(void)
{
	return lockForeground;
}

static SHORT WINAPI lock_key_state(int virtualKey)
{
	return (localLocks & (virtualKey == VK_NUMLOCK ? KBD_SYNC_NUM_LOCK : KBD_SYNC_CAPS_LOCK)) != 0;
}

static UINT WINAPI lock_inject(UINT count, LPINPUT inputs, int size)
{
	if (size != sizeof(INPUT))
		return 0;
	for (UINT index = 0; index < count; ++index)
	{
		if (!(inputs[index].ki.dwFlags & KEYEVENTF_KEYUP))
			localLocks ^= inputs[index].ki.wVk == VK_NUMLOCK ? KBD_SYNC_NUM_LOCK : KBD_SYNC_CAPS_LOCK;
	}
	lockInjections += count;
	return count;
}

static int check_lock_indicators(rdkContext* rdk)
{
	rdk_capture_init(&rdk->capture, rdk->hwnd);
	rdk->capture.foreground = lock_foreground;
	rdk->capture.keyState = lock_key_state;
	rdk->capture.inject = lock_inject;
	lockForeground = rdk->hwnd;
	rdk->focused = TRUE;
	rdk_capture_set_active(&rdk->capture, TRUE);
	const UINT16 both = KBD_SYNC_NUM_LOCK | KBD_SYNC_CAPS_LOCK;
	CHECK(rdk_set_keyboard_indicators((rdpContext*)rdk, KBD_SYNC_NUM_LOCK));
	CHECK(rdk_set_keyboard_indicators((rdpContext*)rdk, both));
	CHECK(rdk->lockIndicatorsPending && !lockInjections);
	rdk->keyboard.localDown[RDP_SCANCODE_KEY_A] = TRUE;
	SendMessageW(rdk->hwnd, WM_TIMER, RDK_LOCK_TIMER, 0);
	CHECK(rdk->lockIndicatorsPending && !lockInjections);
	rdk->keyboard.localDown[RDP_SCANCODE_KEY_A] = FALSE;
	SendMessageW(rdk->hwnd, WM_TIMER, RDK_LOCK_TIMER, 0);
	CHECK(!rdk->lockIndicatorsPending && lockInjections == 4 && localLocks == both);
	CHECK(rdk_set_keyboard_indicators((rdpContext*)rdk, both));
	SendMessageW(rdk->hwnd, WM_TIMER, RDK_LOCK_TIMER, 0);
	CHECK(lockInjections == 4);
	CHECK(rdk_set_keyboard_indicators((rdpContext*)rdk, 0));
	SendMessageW(rdk->hwnd, WM_KILLFOCUS, 0, 0);
	CHECK(!rdk->lockIndicatorsPending);
	SendMessageW(rdk->hwnd, WM_TIMER, RDK_LOCK_TIMER, 0);
	CHECK(rdk_set_keyboard_indicators((rdpContext*)rdk, 0));
	CHECK(!rdk->lockIndicatorsPending && lockInjections == 4);
	SendMessageW(rdk->hwnd, WM_SETFOCUS, 0, 0);
	CHECK(rdk_set_keyboard_indicators((rdpContext*)rdk, 0));
	rdk_capture_set_active(&rdk->capture, FALSE);
	rdk_capture_set_active(&rdk->capture, TRUE);
	SendMessageW(rdk->hwnd, WM_TIMER, RDK_LOCK_TIMER, 0);
	CHECK(!rdk->lockIndicatorsPending && lockInjections == 4);
	lockForeground = NULL;
	CHECK(rdk_set_keyboard_indicators((rdpContext*)rdk, 0));
	CHECK(!rdk->lockIndicatorsPending);
	lockForeground = rdk->hwnd;
	const UINT virtualKeys[] = { VK_NUMLOCK, VK_CAPITAL };
	const UINT scanCodes[] = { RDP_SCANCODE_NUMLOCK_EXTENDED, RDP_SCANCODE_CAPSLOCK };
	for (size_t index = 0; index < ARRAYSIZE(virtualKeys); ++index)
	{
		CHECK(rdk_set_keyboard_indicators((rdpContext*)rdk, 0));
		const WPARAM key = MAKEWPARAM(virtualKeys[index], LOWORD(rdk->capture.state));
		const LPARAM data = ((LPARAM)(scanCodes[index] & 0xFF) << 16) | 1 |
		                    ((scanCodes[index] & KBD_FLAGS_EXTENDED) ? ((LPARAM)1 << 24) : 0);
		SendMessageW(rdk->hwnd, RDK_WM_KEY, key, data);
		SendMessageW(rdk->hwnd, WM_KEYDOWN, virtualKeys[index], data);
		SendMessageW(rdk->hwnd, RDK_WM_KEY, key, data | ((LPARAM)1 << 31));
		SendMessageW(rdk->hwnd, WM_KEYUP, virtualKeys[index], data | ((LPARAM)1 << 31));
		CHECK(lockKeyEvents == (index + 1) * 2);
		CHECK(!rdk->lockIndicatorsPending);
		SendMessageW(rdk->hwnd, WM_TIMER, RDK_LOCK_TIMER, 0);
		CHECK(lockInjections == 4);
	}
	CHECK(rdk_set_keyboard_indicators((rdpContext*)rdk, 0));
	rdk->quit = TRUE;
	SendMessageW(rdk->hwnd, WM_TIMER, RDK_LOCK_TIMER, 0);
	CHECK(!rdk->lockIndicatorsPending && lockInjections == 4);
	rdk->quit = FALSE;
	rdk->focused = FALSE;
	rdk_capture_init(&rdk->capture, rdk->hwnd);
	return 0;
}

typedef struct
{
	rdpInput* input;
	UINT16 flags;
	UINT8 code;
} SecurityEvent;

static SecurityEvent securityEvents[32];
static size_t securityCount;
static size_t securityFailAt;

static BOOL record_security(rdpInput* input, UINT16 flags, UINT8 code)
{
	if (securityCount == ARRAYSIZE(securityEvents))
		return FALSE;
	securityEvents[securityCount++] = (SecurityEvent){ input, flags, code };
	return securityCount != securityFailAt;
}

static int check_security_menu(void)
{
	rdkContext clients[2] = { 0 };
	rdpInput inputs[2] = { 0 };
	for (size_t index = 0; index < ARRAYSIZE(clients); ++index)
	{
		rdkContext* client = &clients[index];
		client->winW = 300;
		client->winH = 200;
		rdk_keyboard_init(&client->keyboard, &inputs[index]);
		client->keyboard.sendScan = record_security;
		client->hwnd = rdk_create_window(client);
		CHECK(client->hwnd);
		rdk_capture_init(&client->capture, client->hwnd);
		HMENU menu = GetSystemMenu(client->hwnd, FALSE);
		WCHAR label[128];
		CHECK(GetMenuStringW(menu, RDK_SC_CTRL_ALT_DELETE, label, ARRAYSIZE(label), MF_BYCOMMAND));
		CHECK(wcscmp(label, L"Send Ctrl+Alt+Delete\tCtrl+Alt+End") == 0);
		CHECK(!(GetMenuState(menu, RDK_SC_CTRL_ALT_DELETE, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)));
	}
	for (size_t index = 0; index < ARRAYSIZE(clients); ++index)
	{
		rdkContext* client = &clients[index];
		CHECK(!client->focused);
		if (index == 1)
		{
			SendMessageW(client->hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
			CHECK(IsIconic(client->hwnd));
		}
		SendMessageW(client->hwnd, WM_SYSCOMMAND, RDK_SC_CTRL_ALT_DELETE | 3, 0);
		CHECK(securityCount == (index + 1) * 6);
		const UINT8 codes[] = { 0x1D, 0x38, 0x53, 0x53, 0x38, 0x1D };
		const UINT16 flags[] = { 0, 0, KBD_FLAGS_EXTENDED, KBD_FLAGS_EXTENDED | KBD_FLAGS_RELEASE,
		                        KBD_FLAGS_RELEASE, KBD_FLAGS_RELEASE };
		for (size_t event = 0; event < ARRAYSIZE(codes); ++event)
		{
			const SecurityEvent recorded = securityEvents[index * 6 + event];
			CHECK(recorded.input == &inputs[index]);
			CHECK(recorded.code == codes[event] && recorded.flags == flags[event]);
		}
		CHECK(!client->quit && !client->inputFailed && !client->reconnectRequested);
		CHECK(rdk_keyboard_idle(&client->keyboard));
		for (size_t code = 0; code < RDK_KEY_COUNT; ++code)
			CHECK(!client->keyboard.remoteDown[code]);
	}
	clients[0].quit = TRUE;
	CHECK(rdk_gdi_recovery(&clients[1], TRUE));
	CHECK(clients[1].recovering && !clients[1].focused && !(clients[1].capture.state & 1));
	SendMessageW(clients[1].hwnd, WM_SETFOCUS, 0, 0);
	CHECK(!clients[1].focused && !(clients[1].capture.state & 1));
	SendMessageW(clients[1].hwnd, WM_SYSCOMMAND, RDK_SC_CTRL_ALT_DELETE, 0);
	SendMessageW(clients[1].hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
	CHECK(securityCount == 12 && clients[1].mouseButtons == 0);
	CHECK(rdk_gdi_recovery(&clients[1], FALSE));
	CHECK(!clients[1].recovering);
	SendMessageW(clients[0].hwnd, WM_SYSCOMMAND, RDK_SC_CTRL_ALT_DELETE, 0);
	CHECK(securityCount == 12);
	HMENU menu = GetSystemMenu(clients[0].hwnd, FALSE);
	SendMessageW(clients[0].hwnd, WM_INITMENUPOPUP, (WPARAM)menu, MAKELPARAM(0, TRUE));
	CHECK(GetMenuState(menu, RDK_SC_CTRL_ALT_DELETE, MF_BYCOMMAND) & MF_GRAYED);
	securityFailAt = securityCount + 3;
	SendMessageW(clients[1].hwnd, WM_SYSCOMMAND, RDK_SC_CTRL_ALT_DELETE, 0);
	CHECK(clients[1].quit && clients[1].inputFailed);
	CHECK(strcmp(clients[1].stopReason, "input forwarding failed") == 0);
	CHECK(!clients[0].inputFailed);
	const size_t afterFailure = securityCount;
	SendMessageW(clients[1].hwnd, WM_SYSCOMMAND, RDK_SC_CTRL_ALT_DELETE, 0);
	CHECK(securityCount == afterFailure);
	for (size_t index = 0; index < ARRAYSIZE(clients); ++index)
		DestroyWindow(clients[index].hwnd);
	return 0;
}

static UINT menuScanCount;
static UINT menuF9Count;
static UINT menuDeleteCount;
static BOOL menuPreview;

static int save_menu_preview(HWND window, UINT dpi, BOOL narrow)
{
	RECT bounds;
	CHECK(GetWindowRect(window, &bounds));
	const int width = bounds.right - bounds.left;
	const int height = bounds.bottom - bounds.top;
	HDC screen = GetDC(window);
	HDC drawing = CreateCompatibleDC(screen);
	BITMAPINFO format = { 0 };
	format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	format.bmiHeader.biWidth = width;
	format.bmiHeader.biHeight = -height;
	format.bmiHeader.biPlanes = 1;
	format.bmiHeader.biBitCount = 32;
	void* pixels = NULL;
	HBITMAP bitmap = CreateDIBSection(drawing, &format, DIB_RGB_COLORS, &pixels, NULL, 0);
	CHECK(bitmap);
	HGDIOBJ previous = SelectObject(drawing, bitmap);
	SendMessageW(window, WM_PRINT, (WPARAM)drawing, PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
	GdiFlush();
	size_t marks = 0;
	for (size_t index = 0; index < (size_t)width * height; ++index)
	{
		const BYTE* pixel = (const BYTE*)pixels + index * 4;
		if (pixel[0] < 180 || pixel[1] < 180 || pixel[2] < 180)
			++marks;
	}
	CHECK(marks > (size_t)width * height / 100);
	CHECK(marks < (size_t)width * height * 3 / 4);
	BITMAPFILEHEADER header = { 0 };
	header.bfType = 0x4D42;
	header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
	header.bfSize = header.bfOffBits + width * height * 4;
	char name[80];
	sprintf_s(name, sizeof(name), "rdk-session-menu-%u%s.bmp", dpi, narrow ? "-narrow" : "");
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

static int check_menu_scaling(HWND menu)
{
	const UINT scales[] = { 96, 144, 192 };
	for (size_t scale = 0; scale < ARRAYSIZE(scales); ++scale)
	{
		const UINT dpi = scales[scale];
		for (UINT narrow = 0; narrow < 2; ++narrow)
		{
			RECT area = { 20, 30, 20 + MulDiv(narrow ? 340 : 800, dpi, 96), 30 + MulDiv(600, dpi, 96) };
			rdk_layout_session_menu(menu, &area, dpi);
			RECT outer;
			CHECK(GetWindowRect(menu, &outer));
			CHECK(outer.top == area.top + MulDiv(10, dpi, 96));
			CHECK(outer.left >= area.left && outer.right <= area.right && outer.bottom <= area.bottom);
			CHECK(abs((outer.left - area.left) - (area.right - outer.right)) <= 1);
			RECT client;
			CHECK(GetClientRect(menu, &client));
			RECT previous = { 0 };
			for (size_t index = 0; index < ARRAYSIZE(rdk_menu_commands); ++index)
			{
				HWND button = GetDlgItem(menu, rdk_menu_commands[index]);
				CHECK((GetWindowLongW(button, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW);
				RECT bounds;
				CHECK(GetWindowRect(button, &bounds));
				MapWindowPoints(NULL, menu, (POINT*)&bounds, 2);
				CHECK(bounds.left >= 0 && bounds.right <= client.right && bounds.top >= 0 && bounds.bottom <= client.bottom);
				CHECK(bounds.bottom - bounds.top == MulDiv(34, dpi, 96));
				if (index)
				{
					RECT overlap;
					CHECK(!IntersectRect(&overlap, &previous, &bounds));
					if (!narrow) CHECK(bounds.top == previous.top && bounds.bottom == previous.bottom);
				}
				rdkSessionMenu* state = (rdkSessionMenu*)GetWindowLongPtrW(menu, DWLP_USER);
				const BOOL symbol = rdk_menu_commands[index] == IDCANCEL || rdk_menu_commands[index] == IDC_SESSION_MINIMIZE;
				HDC drawing = GetDC(button);
				CHECK(drawing);
				HGDIOBJ original = SelectObject(drawing, symbol ? state->symbols : state->font);
				WCHAR label[80];
				CHECK(GetWindowTextW(button, label, ARRAYSIZE(label)));
				const WCHAR* text = symbol ? (rdk_menu_commands[index] == IDCANCEL ? L"\xE8BB" : L"\xE921") : label;
				WORD glyphs[80];
				CHECK(GetGlyphIndicesW(drawing, text, (int)wcslen(text), glyphs, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR);
				for (size_t character = 0; character < wcslen(text); ++character)
					CHECK(glyphs[character] != 0xFFFF);
				RECT textBounds = { 0 };
				CHECK(DrawTextW(drawing, text, -1, &textBounds, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX));
				CHECK(textBounds.right + MulDiv(12, dpi, 96) <= bounds.right - bounds.left);
				CHECK(textBounds.bottom + MulDiv(4, dpi, 96) <= bounds.bottom - bounds.top);
				SelectObject(drawing, original);
				ReleaseDC(button, drawing);
				previous = bounds;
			}
			if (menuPreview) CHECK(save_menu_preview(menu, dpi, narrow != 0) == 0);
		}
	}
	return 0;
}

static BOOL record_menu_scan(rdpInput* input, UINT16 flags, UINT8 code)
{
	(void)input;
	++menuScanCount;
	if (code == RDP_SCANCODE_F9)
		++menuF9Count;
	if (code == (RDP_SCANCODE_DELETE & 0xFF) && (flags & KBD_FLAGS_EXTENDED))
		++menuDeleteCount;
	return TRUE;
}

static int check_session_menu(void)
{
	rdkContext client = { 0 };
	client.winW = 800;
	client.winH = 600;
	rdk_keyboard_init(&client.keyboard, NULL);
	client.keyboard.sendScan = record_menu_scan;
	client.hwnd = rdk_create_window(&client);
	CHECK(client.hwnd);
	rdk_capture_init(&client.capture, client.hwnd);
	CHECK(!(GetWindowLongW(client.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST));
	CHECK((GetWindowLongW(client.hwnd, GWL_STYLE) & WS_CAPTION) == 0);
	ShowWindow(client.hwnd, SW_SHOWNOACTIVATE);
	HWND local = CreateWindowExW(0, L"STATIC", L"Local application", WS_OVERLAPPEDWINDOW,
	    50, 50, 300, 250, NULL, NULL, GetModuleHandleW(NULL), NULL);
	CHECK(local);
	ShowWindow(local, SW_SHOWNOACTIVATE);
	CHECK(SetWindowPos(local, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
	BOOL localAbove = FALSE;
	for (HWND window = GetWindow(client.hwnd, GW_HWNDPREV); window; window = GetWindow(window, GW_HWNDPREV))
		localAbove = localAbove || window == local;
	CHECK(localAbove && !IsIconic(client.hwnd));
	DestroyWindow(local);
	CHECK(GetMenuState(GetSystemMenu(client.hwnd, FALSE), RDK_SC_SESSION_MENU, MF_BYCOMMAND) != (UINT)-1);
	SendMessageW(client.hwnd, WM_SETFOCUS, 0, 0);
	CHECK(client.focused && (client.capture.state & 1));
	rdk_on_key(&client, WM_KEYDOWN, VK_CONTROL, ((LPARAM)RDP_SCANCODE_LCONTROL << 16) | 1);
	rdk_on_key(&client, WM_KEYDOWN, VK_SHIFT, ((LPARAM)RDP_SCANCODE_LSHIFT << 16) | 1);
	rdk_on_key(&client, WM_KEYDOWN, VK_F9, ((LPARAM)RDP_SCANCODE_F9 << 16) | 3);
	HWND menu = client.sessionMenu;
	CHECK(menu && IsWindow(menu));
	CHECK(!client.focused && !(client.capture.state & 1));
	CHECK(!client.keyboard.menu && !client.quit && !client.reconnectRequested);
	CHECK(menuScanCount == 4 && menuF9Count == 0);
	CHECK(rdk_keyboard_idle(&client.keyboard));
	CHECK(GetWindow(menu, GW_OWNER) == client.hwnd);
	CHECK(!(GetWindowLongW(menu, GWL_EXSTYLE) & (WS_EX_TOPMOST | WS_EX_APPWINDOW)));
	CHECK((GetWindowLongW(menu, GWL_STYLE) & WS_CAPTION) != WS_CAPTION);
	CHECK(LOWORD(SendMessageW(menu, DM_GETDEFID, 0, 0)) == IDCANCEL);
	rdkSessionMenu* menuState = (rdkSessionMenu*)GetWindowLongPtrW(menu, DWLP_USER);
	CHECK(menuState && menuState->tooltip);
	CHECK(SendMessageW(menuState->tooltip, TTM_GETTOOLCOUNT, 0, 0) == ARRAYSIZE(rdk_menu_commands));
	HWND closeButton = GetDlgItem(menu, IDCANCEL);
	SendMessageW(closeButton, WM_MOUSEMOVE, 0, MAKELPARAM(5, 5));
	CHECK(menuState->hovered == IDCANCEL);
	SendMessageW(closeButton, WM_MOUSELEAVE, 0, 0);
	CHECK(menuState->hovered == 0);
	const int commands[] = { IDC_SESSION_MINIMIZE, IDC_SESSION_SECURITY, IDC_SESSION_RECONNECT,
	                         IDC_SESSION_DISCONNECT, IDCANCEL };
	RECT previous = { 0 };
	RECT clientBounds;
	CHECK(GetClientRect(menu, &clientBounds));
	for (size_t index = 0; index < ARRAYSIZE(commands); ++index)
	{
		HWND button = GetDlgItem(menu, commands[index]);
		CHECK(button && IsWindowEnabled(button));
		RECT bounds;
		CHECK(GetWindowRect(button, &bounds));
		MapWindowPoints(NULL, menu, (POINT*)&bounds, 2);
		CHECK(bounds.left >= 0 && bounds.right <= clientBounds.right);
		CHECK(bounds.top >= 0 && bounds.bottom <= clientBounds.bottom);
		if (index)
			CHECK(bounds.top == previous.top && bounds.bottom == previous.bottom && bounds.left > previous.right);
		WCHAR label[128];
		CHECK(GetWindowTextW(button, label, ARRAYSIZE(label)));
		HDC drawing = GetDC(button);
		CHECK(drawing);
		HGDIOBJ original = SelectObject(drawing, (HFONT)SendMessageW(button, WM_GETFONT, 0, 0));
		RECT text = { 0 };
		CHECK(DrawTextW(drawing, label, -1, &text, DT_CALCRECT | DT_SINGLELINE));
		if (commands[index] != IDCANCEL && commands[index] != IDC_SESSION_MINIMIZE)
			CHECK(text.right + 12 <= bounds.right - bounds.left);
		CHECK(text.bottom + 4 <= bounds.bottom - bounds.top);
		SelectObject(drawing, original);
		ReleaseDC(button, drawing);
		previous = bounds;
	}
	rdk_show_session_menu(&client);
	CHECK(client.sessionMenu == menu);
	SendMessageW(client.hwnd, WM_SETFOCUS, 0, 0);
	CHECK(!client.focused && !(client.capture.state & 1));
	rdk_on_key(&client, WM_KEYDOWN, 'A', ((LPARAM)RDP_SCANCODE_KEY_A << 16) | 1);
	SendMessageW(client.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
	CHECK(menuScanCount == 4 && !client.mouseButtons);
	CHECK(check_menu_scaling(menu) == 0);
	MSG escape = { 0 };
	escape.hwnd = GetDlgItem(menu, IDCANCEL);
	escape.message = WM_KEYDOWN;
	escape.wParam = VK_ESCAPE;
	CHECK(IsDialogMessageW(menu, &escape));
	CHECK(!client.sessionMenu && !IsWindow(menu) && !client.quit);
	SendMessageW(client.hwnd, WM_SETFOCUS, 0, 0);
	CHECK(client.focused && (client.capture.state & 1));
	rdk_on_key(&client, WM_KEYDOWN, 'A', ((LPARAM)RDP_SCANCODE_KEY_A << 16) | 1);
	rdk_on_key(&client, WM_KEYUP, 'A', ((LPARAM)RDP_SCANCODE_KEY_A << 16) | 1);
	CHECK(menuScanCount == 6);
	SendMessageW(client.hwnd, WM_SYSCOMMAND, RDK_SC_SESSION_MENU, 0);
	CHECK(client.sessionMenu);
	MSG navigation = { 0 };
	navigation.hwnd = GetDlgItem(client.sessionMenu, IDCANCEL);
	navigation.message = WM_KEYDOWN;
	navigation.wParam = VK_TAB;
	CHECK(IsDialogMessageW(client.sessionMenu, &navigation));
	CHECK(GetFocus() == GetDlgItem(client.sessionMenu, IDC_SESSION_MINIMIZE));
	for (size_t index = 0; index < ARRAYSIZE(rdk_menu_commands); ++index)
		CHECK((GetWindowLongW(GetDlgItem(client.sessionMenu, rdk_menu_commands[index]), GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW);
	SetFocus(GetDlgItem(client.sessionMenu, IDCANCEL));
	navigation.hwnd = GetDlgItem(client.sessionMenu, IDCANCEL);
	navigation.wParam = VK_RETURN;
	CHECK(IsDialogMessageW(client.sessionMenu, &navigation));
	CHECK(!client.sessionMenu && !client.quit);
	SendMessageW(client.hwnd, WM_SYSCOMMAND, RDK_SC_SESSION_MENU, 0);
	CHECK(client.sessionMenu);
	SendMessageW(client.sessionMenu, WM_COMMAND, IDC_SESSION_SECURITY, 0);
	CHECK(!client.sessionMenu && menuDeleteCount == 2 && !client.quit);
	SendMessageW(client.hwnd, WM_SYSCOMMAND, RDK_SC_SESSION_MENU, 0);
	CHECK(client.sessionMenu);
	SendMessageW(client.sessionMenu, WM_COMMAND, IDC_SESSION_MINIMIZE, 0);
	CHECK(!client.sessionMenu && IsIconic(client.hwnd) && !client.quit);
	SendMessageW(client.hwnd, WM_SYSCOMMAND, RDK_SC_SESSION_MENU, 0);
	CHECK(client.sessionMenu && !IsIconic(client.hwnd));
	SendMessageW(client.sessionMenu, WM_ACTIVATE, WA_INACTIVE, 0);
	CHECK(!client.sessionMenu && !client.quit);
	rdk_show_session_menu(&client);
	CHECK(client.sessionMenu);
	CHECK(rdk_gdi_recovery(&client, TRUE));
	CHECK(!client.sessionMenu);
	rdk_show_session_menu(&client);
	CHECK(!client.sessionMenu);
	CHECK(rdk_gdi_recovery(&client, FALSE));
	rdk_show_session_menu(&client);
	CHECK(client.sessionMenu);
	SendMessageW(client.sessionMenu, WM_COMMAND, IDC_SESSION_RECONNECT, 0);
	CHECK(!client.sessionMenu && client.quit && client.reconnectRequested);
	CHECK(strcmp(client.stopReason, "session menu reconnect requested") == 0);
	client.quit = client.reconnectRequested = FALSE;
	rdk_show_session_menu(&client);
	CHECK(client.sessionMenu);
	SendMessageW(client.sessionMenu, WM_COMMAND, IDC_SESSION_DISCONNECT, 0);
	CHECK(!client.sessionMenu && client.quit && !client.reconnectRequested);
	CHECK(strcmp(client.stopReason, "session menu disconnect requested") == 0);
	rdk_show_session_menu(&client);
	CHECK(!client.sessionMenu);
	client.quit = FALSE;
	rdk_show_session_menu(&client);
	CHECK(client.sessionMenu);
	SendMessageW(client.hwnd, WM_CLOSE, 0, 0);
	CHECK(!client.sessionMenu && client.quit);
	DestroyWindow(client.hwnd);
	return 0;
}

static int run_taskbar_helper(const WCHAR* action, const WCHAR* executable, DWORD expectedExit)
{
	WCHAR helper[32768];
	const DWORD length = GetModuleFileNameW(NULL, helper, ARRAYSIZE(helper));
	CHECK(length && length < ARRAYSIZE(helper));
	WCHAR* separator = wcsrchr(helper, L'\\');
	CHECK(separator);
	CHECK(wcscpy_s(separator + 1, ARRAYSIZE(helper) - (separator + 1 - helper), L"rdk-taskbar.exe") == 0);
	WCHAR command[32768];
	CHECK(swprintf_s(command, ARRAYSIZE(command), L"\"%ls\" %ls \"%ls\"", helper, action, executable) > 0);
	WCHAR stationName[256];
	DWORD needed = 0;
	CHECK(GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, stationName, sizeof(stationName), &needed));
	WCHAR desktopName[512];
	CHECK(swprintf_s(desktopName, ARRAYSIZE(desktopName), L"%ls\\rdkWindowTest", stationName) > 0);
	STARTUPINFOW startup = { 0 };
	startup.cb = sizeof(startup);
	startup.lpDesktop = desktopName;
	PROCESS_INFORMATION process = { 0 };
	CHECK(CreateProcessW(helper, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));
	CloseHandle(process.hThread);
	const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
	DWORD exitCode = STILL_ACTIVE;
	const BOOL queried = GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hProcess);
	CHECK(wait == WAIT_OBJECT_0 && queried && exitCode == expectedExit);
	pump_messages();
	return 0;
}

int main(int argc, char** argv)
{
	menuPreview = argc == 2 && strcmp(argv[1], "--preview-menu") == 0;
	CHECK(check_graphics_logging() == 0);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	HWINSTA originalStation = GetProcessWindowStation();
	HDESK originalDesktop = GetThreadDesktop(GetCurrentThreadId());
	HWINSTA station = CreateWindowStationW(NULL, 0, WINSTA_ALL_ACCESS, NULL);
	CHECK(station && SetProcessWindowStation(station));
	HDESK desktop = CreateDesktopW(L"rdkWindowTest", NULL, NULL, 0, GENERIC_ALL, NULL);
	CHECK(desktop && SetThreadDesktop(desktop));
	CHECK(check_security_menu() == 0);
	CHECK(check_session_menu() == 0);
	rdkContext rdk = { 0 };
	rdk.winX = 20;
	rdk.winY = 30;
	rdk.winW = 800;
	rdk.winH = 500;
	rdk_keyboard_init(&rdk.keyboard, NULL);
	rdk.keyboard.sendScan = record_scan;
	rdk.hwnd = rdk_create_window(&rdk);
	CHECK(rdk.hwnd);
	rdk_capture_init(&rdk.capture, rdk.hwnd);
	ShowWindow(rdk.hwnd, SW_SHOWNOACTIVATE);
	CHECK(check_lock_indicators(&rdk) == 0);
	HMENU menu = GetSystemMenu(rdk.hwnd, FALSE);
	CHECK(menu);
	const UINT minimize = GetMenuState(menu, SC_MINIMIZE, MF_BYCOMMAND);
	CHECK(minimize != (UINT)-1 && !(minimize & (MF_DISABLED | MF_GRAYED)));
	CHECK(GetMenuState(menu, SC_RESTORE, MF_BYCOMMAND) != (UINT)-1);
	CHECK(GetMenuState(menu, SC_CLOSE, MF_BYCOMMAND) != (UINT)-1);
	RECT original;
	GetWindowRect(rdk.hwnd, &original);
	RECT client;
	GetClientRect(rdk.hwnd, &client);
	CHECK(client.right == rdk.winW && client.bottom == rdk.winH);
	POINT origin = { 0 };
	ClientToScreen(rdk.hwnd, &origin);
	CHECK(origin.x == original.left && origin.y == original.top);
	rdk_media_notice(&rdk);
	CHECK(rdk.notice && (GetWindowLongW(rdk.notice, GWL_STYLE) & WS_VISIBLE));
	for (UINT iteration = 0; iteration < 3; ++iteration)
	{
		rdk.focused = TRUE;
		rdk_capture_set_active(&rdk.capture, TRUE);
		rdk.keyboard.localDown[RDP_SCANCODE_LCONTROL] = TRUE;
		rdk.keyboard.remoteDown[RDP_SCANCODE_LCONTROL] = TRUE;
		SendMessageW(rdk.hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
		CHECK(IsIconic(rdk.hwnd));
		CHECK(!rdk.focused && !(rdk.capture.state & 1));
		CHECK(!rdk.keyboard.remoteDown[RDP_SCANCODE_LCONTROL] && releases == iteration * 2 + 1);
		CHECK(!rdk.quit && !rdk.reconnectRequested && !rdk.inputFailed);
		CHECK(!(GetWindowLongW(rdk.notice, GWL_STYLE) & WS_VISIBLE));
		SendMessageW(rdk.hwnd, WM_SETFOCUS, 0, 0);
		CHECK(!rdk.focused && !(rdk.capture.state & 1));
		SendMessageW(rdk.hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
		CHECK(!IsIconic(rdk.hwnd) && (GetWindowLongW(rdk.hwnd, GWL_STYLE) & WS_VISIBLE));
		RECT restored;
		GetWindowRect(rdk.hwnd, &restored);
		CHECK(EqualRect(&original, &restored));
		CHECK(GetWindowLongW(rdk.notice, GWL_STYLE) & WS_VISIBLE);
		SendMessageW(rdk.hwnd, WM_SETFOCUS, 0, 0);
		CHECK(rdk.focused && (rdk.capture.state & 1));
		rdk_on_key(&rdk, WM_KEYDOWN, VK_CONTROL, ((LPARAM)RDP_SCANCODE_LCONTROL << 16) | 1);
		rdk_on_key(&rdk, WM_KEYDOWN, VK_SHIFT, ((LPARAM)RDP_SCANCODE_LSHIFT << 16) | 1);
		rdk_on_key(&rdk, WM_KEYDOWN, VK_F10, ((LPARAM)RDP_SCANCODE_F10 << 16) | 3);
		CHECK(IsIconic(rdk.hwnd) && !rdk.focused && !(rdk.capture.state & 1));
		CHECK(!rdk.quit && !rdk.reconnectRequested && !rdk.inputFailed);
		CHECK(!rdk.keyboard.minimize && !rdk.keyboard.quit && !rdk.keyboard.reconnect);
		CHECK(!minimizeKeyEvents && releases == (iteration + 1) * 2);
		CHECK(!(GetWindowLongW(rdk.notice, GWL_STYLE) & WS_VISIBLE));
		SendMessageW(rdk.hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
		CHECK(!IsIconic(rdk.hwnd));
		GetWindowRect(rdk.hwnd, &restored);
		CHECK(EqualRect(&original, &restored));
		SendMessageW(rdk.hwnd, WM_SETFOCUS, 0, 0);
		rdk_on_key(&rdk, WM_KEYUP, VK_F10, ((LPARAM)RDP_SCANCODE_F10 << 16) | 1);
		CHECK(!minimizeKeyEvents && !IsIconic(rdk.hwnd) && !rdk.keyboard.minimize);
	}
	CHECK(rdk_taskbar_minimize() == 1);
	pump_messages();
	CHECK(IsIconic(rdk.hwnd) && !rdk.quit && !rdk.reconnectRequested);
	CHECK(!rdk.focused && !(rdk.capture.state & 1));
	SendMessageW(rdk.hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
	CHECK(!IsIconic(rdk.hwnd));
	WCHAR executable[32768];
	CHECK(GetModuleFileNameW(NULL, executable, ARRAYSIZE(executable)));
	CHECK(run_taskbar_helper(L"/minimize", L"C:\\rdk-test-other\\rdk.exe", 0) == 0);
	CHECK(!IsIconic(rdk.hwnd) && !rdk.quit && !rdk.reconnectRequested);
	CHECK(run_taskbar_helper(L"/reconnect", L"C:\\rdk-test-other\\rdk.exe", 0) == 0);
	CHECK(!rdk.quit && !rdk.reconnectRequested);
	CHECK(run_taskbar_helper(L"/invalid", executable, 1) == 0);
	CHECK(!IsIconic(rdk.hwnd) && !rdk.quit && !rdk.reconnectRequested);
	CHECK(run_taskbar_helper(L"/minimize", executable, 0) == 0);
	CHECK(IsIconic(rdk.hwnd) && !rdk.quit && !rdk.reconnectRequested);
	CHECK(!rdk.focused && !(rdk.capture.state & 1));
	CHECK(run_taskbar_helper(L"/reconnect", executable, 0) == 0);
	CHECK(IsIconic(rdk.hwnd) && rdk.quit && rdk.reconnectRequested && !rdk.inputFailed);
	CHECK(rdk.stopReason && strcmp(rdk.stopReason, "taskbar reconnect requested") == 0);
	CHECK(!rdk.focused && !(rdk.capture.state & 1));
	CHECK(run_taskbar_helper(L"/reconnect", executable, 0) == 0);
	CHECK(rdk.quit && rdk.reconnectRequested);
	rdk.reconnectRequested = FALSE;
	CHECK(run_taskbar_helper(L"/reconnect", executable, 0) == 0);
	CHECK(rdk.quit && !rdk.reconnectRequested);
	SendMessageW(rdk.hwnd, WM_CLOSE, 0, 0);
	CHECK(rdk.stopReason && strcmp(rdk.stopReason, "window close requested") == 0);
	DestroyWindow(rdk.hwnd);
	CHECK(SetThreadDesktop(originalDesktop));
	CHECK(SetProcessWindowStation(originalStation));
	CloseDesktop(desktop);
	CloseWindowStation(station);
	puts("Passed session menu, normal window stacking, per-window Ctrl+Alt+Delete, lock indicators, native minimize/restore, input release, owned notices, and windowless helper dispatch tests");
	return 0;
}