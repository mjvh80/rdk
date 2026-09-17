#include "../src/rdk_gdi.c"
#include <wchar.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

static UINT releases;
static UINT minimizeKeyEvents;
static BOOL record_scan(rdpInput* input, UINT16 flags, UINT8 code)
{
	(void)input;
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
	puts("Passed native minimize/restore, input release, owned notices, and windowless helper minimize/reconnect dispatch tests");
	return 0;
}