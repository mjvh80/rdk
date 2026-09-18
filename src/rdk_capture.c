#include "rdk_capture.h"
#include "rdk_latency.h"

#define RDK_LOCK_SYNC_TAG ((ULONG_PTR)0x52444B4C)

static __declspec(thread) rdkCapture* g_capture;

void rdk_capture_init(rdkCapture* capture, HWND window)
{
	ZeroMemory(capture, sizeof(*capture));
	capture->window = window;
	capture->foreground = GetForegroundWindow;
	capture->post = PostMessageW;
	capture->keyState = GetKeyState;
	capture->inject = SendInput;
}

void rdk_capture_set_active(rdkCapture* capture, BOOL active)
{
	const LONG state = InterlockedCompareExchange(&capture->state, 0, 0);
	if ((state & 1) != (active ? 1 : 0))
		InterlockedIncrement(&capture->state);
}

DWORD rdk_capture_error(rdkCapture* capture)
{
	return (DWORD)InterlockedCompareExchange(&capture->error, 0, 0);
}

BOOL rdk_capture_current(rdkCapture* capture, WPARAM key)
{
	const LONG state = InterlockedCompareExchange(&capture->state, 0, 0);
	return (state & 1) && HIWORD(key) == LOWORD(state) &&
	       capture->foreground() == capture->window;
}

BOOL rdk_capture_route(rdkCapture* capture, int code, WPARAM message, const KBDLLHOOKSTRUCT* event)
{
	if (code != HC_ACTION || !event || event->vkCode == VK_PACKET ||
	    (message != WM_KEYDOWN && message != WM_KEYUP &&
	     message != WM_SYSKEYDOWN && message != WM_SYSKEYUP))
		return FALSE;
	if ((event->flags & LLKHF_INJECTED) && event->dwExtraInfo == RDK_LOCK_SYNC_TAG &&
	    (event->vkCode == VK_NUMLOCK || event->vkCode == VK_CAPITAL))
		return FALSE;
	const LONG state = InterlockedCompareExchange(&capture->state, 0, 0);
	if (!(state & 1) || capture->foreground() != capture->window)
		return FALSE;
	UINT scan = event->scanCode;
	if (scan == 0)
		scan = MapVirtualKeyW(event->vkCode, MAPVK_VK_TO_VSC_EX);
	if (scan == 0)
		return FALSE;
	LPARAM data = ((LPARAM)(scan & 0xFF) << 16) | 1;
	if ((event->flags & LLKHF_EXTENDED) || (scan & 0xFF00) == 0xE000)
		data |= (LPARAM)1 << 24;
	if (message == WM_KEYUP || message == WM_SYSKEYUP)
		data |= (LPARAM)1 << 31;
	rdk_latency_message(RDK_LATENCY_HOOK, event->time);
	if (!capture->post(capture->window, RDK_WM_KEY, MAKEWPARAM(event->vkCode, LOWORD(state)), data))
	{
		const DWORD error = GetLastError();
		InterlockedExchange(&capture->error, error ? (LONG)error : ERROR_GEN_FAILURE);
	}
	return event->vkCode != VK_NUMLOCK && event->vkCode != VK_CAPITAL;
}

BOOL rdk_capture_sync_locks(rdkCapture* capture, BOOL numLock, BOOL capsLock)
{
	const LONG state = InterlockedCompareExchange(&capture->state, 0, 0);
	if (!(state & 1) || capture->foreground() != capture->window)
		return TRUE;
	const WORD virtualKeys[] = { VK_NUMLOCK, VK_CAPITAL };
	const BOOL enabled[] = { numLock, capsLock };
	INPUT inputs[4] = { 0 };
	UINT count = 0;
	for (size_t index = 0; index < ARRAYSIZE(virtualKeys); ++index)
	{
		if (((capture->keyState(virtualKeys[index]) & 1) != 0) == (enabled[index] != FALSE))
			continue;
		INPUT* down = &inputs[count++];
		down->type = INPUT_KEYBOARD;
		down->ki.wVk = virtualKeys[index];
		down->ki.dwFlags = virtualKeys[index] == VK_NUMLOCK ? KEYEVENTF_EXTENDEDKEY : 0;
		down->ki.dwExtraInfo = RDK_LOCK_SYNC_TAG;
		inputs[count] = *down;
		inputs[count++].ki.dwFlags |= KEYEVENTF_KEYUP;
	}
	if (!count)
		return TRUE;
	const UINT sent = capture->inject(count, inputs, sizeof(INPUT));
	if (sent < count && (sent & 1))
		(void)capture->inject(1, &inputs[sent], sizeof(INPUT));
	return sent == count;
}

static LRESULT CALLBACK rdk_keyboard_hook(int code, WPARAM message, LPARAM data)
{
	if (g_capture && rdk_capture_route(g_capture, code, message, (const KBDLLHOOKSTRUCT*)data))
		return 1;
	return CallNextHookEx(NULL, code, message, data);
}

static DWORD WINAPI rdk_capture_thread(LPVOID parameter)
{
	rdkCapture* capture = parameter;
	g_capture = capture;
	HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, rdk_keyboard_hook, GetModuleHandleW(NULL), 0);
	if (!hook)
		InterlockedExchange(&capture->error, (LONG)GetLastError());
	SetEvent(capture->ready);
	while (hook)
	{
		const DWORD status = MsgWaitForMultipleObjectsEx(1, &capture->stop, INFINITE,
		                                               QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		if (status == WAIT_OBJECT_0)
			break;
		if (status == WAIT_FAILED)
		{
			InterlockedExchange(&capture->error, (LONG)GetLastError());
			break;
		}
		MSG message;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&message);
	}
	if (hook)
		UnhookWindowsHookEx(hook);
	g_capture = NULL;
	return 0;
}

void rdk_capture_stop(rdkCapture* capture)
{
	rdk_capture_set_active(capture, FALSE);
	if (capture->thread)
	{
		SetEvent(capture->stop);
		WaitForSingleObject(capture->thread, INFINITE);
		CloseHandle(capture->thread);
		capture->thread = NULL;
	}
	if (capture->ready)
		CloseHandle(capture->ready);
	if (capture->stop)
		CloseHandle(capture->stop);
	capture->ready = NULL;
	capture->stop = NULL;
}

BOOL rdk_capture_start(rdkCapture* capture, HWND window)
{
	rdk_capture_init(capture, window);
	capture->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	capture->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (capture->ready && capture->stop)
		capture->thread = CreateThread(NULL, 0, rdk_capture_thread, capture, 0, NULL);
	if (!capture->thread)
		InterlockedExchange(&capture->error, (LONG)GetLastError());
	else if (WaitForSingleObject(capture->ready, INFINITE) != WAIT_OBJECT_0)
		InterlockedExchange(&capture->error, (LONG)GetLastError());
	else if (rdk_capture_error(capture) == 0)
		return TRUE;
	rdk_capture_stop(capture);
	return FALSE;
}