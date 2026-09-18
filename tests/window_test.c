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

int main(void)
{
	CHECK(check_graphics_logging() == 0);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	HWINSTA originalStation = GetProcessWindowStation();
	HDESK originalDesktop = GetThreadDesktop(GetCurrentThreadId());
	HWINSTA station = CreateWindowStationW(NULL, 0, WINSTA_ALL_ACCESS, NULL);
	CHECK(station && SetProcessWindowStation(station));
	HDESK desktop = CreateDesktopW(L"rdkWindowTest", NULL, NULL, 0, GENERIC_ALL, NULL);
	CHECK(desktop && SetThreadDesktop(desktop));
	CHECK(check_security_menu() == 0);
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
	puts("Passed per-window Ctrl+Alt+Delete, lock indicators, native minimize/restore, input release, owned notices, and windowless helper minimize/reconnect dispatch tests");
	return 0;
}