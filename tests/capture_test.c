#include "rdk_capture.h"
#include "rdk_keyboard.h"
#include "rdk_latency.h"
#include <stdio.h>

typedef struct
{
	WPARAM key;
	LPARAM data;
} KeyMessage;

typedef struct
{
	BOOL unicode;
	UINT16 flags;
	UINT16 code;
} SentEvent;

static rdkCapture capture;
static rdkKeyboard keyboard;
static HWND foreground;
static KeyMessage messages[1024];
static size_t messageCount;
static SentEvent sent[1024];
static size_t sentCount;
static BOOL queueOk;
static BOOL localNumLock;
static BOOL localCapsLock;
static UINT injectedCount;
static UINT injectLimit;
static INPUT injected[8];

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return FALSE; \
} } while (0)

static HWND WINAPI get_foreground(void)
{
	return foreground;
}

static BOOL WINAPI post_key(HWND window, UINT message, WPARAM key, LPARAM data)
{
	if (!queueOk || messageCount == ARRAYSIZE(messages))
	{
		SetLastError(ERROR_NOT_ENOUGH_QUOTA);
		return FALSE;
	}
	CHECK(window == capture.window && message == RDK_WM_KEY);
	messages[messageCount++] = (KeyMessage){ key, data };
	return TRUE;
}

static BOOL send_scan(rdpInput* input, UINT16 flags, UINT8 code)
{
	(void)input;
	CHECK(sentCount < ARRAYSIZE(sent));
	sent[sentCount++] = (SentEvent){ FALSE, flags, code };
	return TRUE;
}

static BOOL send_unicode(rdpInput* input, UINT16 flags, UINT16 code)
{
	(void)input;
	CHECK(sentCount < ARRAYSIZE(sent));
	sent[sentCount++] = (SentEvent){ TRUE, flags, code };
	return TRUE;
}

static SHORT WINAPI get_key_state(int virtualKey)
{
	return (SHORT)(virtualKey == VK_NUMLOCK ? localNumLock : localCapsLock);
}

static UINT WINAPI inject_keys(UINT count, LPINPUT inputs, int size)
{
	CHECK(size == sizeof(INPUT));
	if (count > injectLimit)
		count = injectLimit;
	for (UINT index = 0; index < count; ++index)
	{
		CHECK(injectedCount < ARRAYSIZE(injected));
		injected[injectedCount++] = inputs[index];
		CHECK(inputs[index].type == INPUT_KEYBOARD);
		const KEYBDINPUT input = inputs[index].ki;
		CHECK(input.wVk == VK_NUMLOCK || input.wVk == VK_CAPITAL);
		const BOOL up = (input.dwFlags & KEYEVENTF_KEYUP) != 0;
		KBDLLHOOKSTRUCT event = { 0 };
		event.vkCode = input.wVk;
		event.scanCode = input.wVk == VK_NUMLOCK ? 0x45 : 0x3A;
		event.dwExtraInfo = input.dwExtraInfo;
		event.flags = LLKHF_INJECTED | (up ? LLKHF_UP : 0) |
		              ((input.dwFlags & KEYEVENTF_EXTENDEDKEY) ? LLKHF_EXTENDED : 0);
		CHECK(!rdk_capture_route(&capture, HC_ACTION, up ? WM_KEYUP : WM_KEYDOWN, &event));
		CHECK(messageCount == 0);
		if (!up)
		{
			if (input.wVk == VK_NUMLOCK)
				localNumLock = !localNumLock;
			else
				localCapsLock = !localCapsLock;
		}
	}
	return count;
}

static void reset(void)
{
	rdk_capture_init(&capture, (HWND)(UINT_PTR)1);
	capture.foreground = get_foreground;
	capture.post = post_key;
	capture.keyState = get_key_state;
	capture.inject = inject_keys;
	foreground = capture.window;
	rdk_capture_set_active(&capture, TRUE);
	rdk_keyboard_init(&keyboard, NULL);
	keyboard.sendScan = send_scan;
	keyboard.sendUnicode = send_unicode;
	keyboard.ansiCodePage = 1252;
	keyboard.oemCodePage = 437;
	messageCount = sentCount = 0;
	queueOk = TRUE;
	localNumLock = localCapsLock = FALSE;
	injectedCount = 0;
	injectLimit = UINT_MAX;
}

static BOOL key(UINT vk, UINT16 scan, BOOL down, DWORD extraFlags)
{
	KBDLLHOOKSTRUCT event = { 0 };
	event.vkCode = vk;
	event.scanCode = scan & 0xFF;
	event.time = GetTickCount();
	event.flags = extraFlags | (down ? 0 : LLKHF_UP) |
	              ((scan & KBD_FLAGS_EXTENDED) ? LLKHF_EXTENDED : 0);
	return rdk_capture_route(&capture, HC_ACTION,
	                        down ? WM_KEYDOWN : WM_KEYUP, &event);
}

static BOOL drain(void)
{
	for (size_t index = 0; index < messageCount; ++index)
	{
		const KeyMessage message = messages[index];
		if (!rdk_capture_current(&capture, message.key))
			continue;
		const UINT type = (message.data & ((LPARAM)1 << 31)) ? WM_KEYUP : WM_KEYDOWN;
		CHECK(rdk_keyboard_key(&keyboard, type, LOWORD(message.key), message.data));
	}
	messageCount = 0;
	return TRUE;
}

static BOOL expect(size_t index, BOOL unicode, UINT16 flags, UINT16 code)
{
	CHECK(index < sentCount);
	CHECK(sent[index].unicode == unicode);
	CHECK(sent[index].flags == flags);
	CHECK(sent[index].code == code);
	return TRUE;
}

static BOOL windows_run(void)
{
	CHECK(key(VK_LWIN, RDP_SCANCODE_LWIN, TRUE, 0));
	CHECK(key('R', RDP_SCANCODE_KEY_R, TRUE, 0));
	CHECK(key('R', RDP_SCANCODE_KEY_R, FALSE, 0));
	CHECK(key(VK_LWIN, RDP_SCANCODE_LWIN, FALSE, 0));
	CHECK(messageCount == 4 && sentCount == 0);
	CHECK(drain());
	CHECK(sentCount == 4);
	CHECK(expect(0, FALSE, KBD_FLAGS_EXTENDED, 0x5B));
	CHECK(expect(1, FALSE, 0, 0x13));
	CHECK(expect(2, FALSE, KBD_FLAGS_RELEASE, 0x13));
	CHECK(expect(3, FALSE, KBD_FLAGS_EXTENDED | KBD_FLAGS_RELEASE, 0x5B));
	return TRUE;
}

static BOOL lock_passthrough(void)
{
	const UINT virtualKeys[] = { VK_NUMLOCK, VK_CAPITAL };
	const UINT16 scanCodes[] = { RDP_SCANCODE_NUMLOCK_EXTENDED, RDP_SCANCODE_CAPSLOCK };
	for (size_t index = 0; index < ARRAYSIZE(virtualKeys); ++index)
	{
		reset();
		for (UINT press = 0; press < 2; ++press)
		{
			CHECK(!key(virtualKeys[index], scanCodes[index], TRUE, 0));
			CHECK(!key(virtualKeys[index], scanCodes[index], FALSE, 0));
			CHECK(messageCount == 2);
			CHECK(drain());
			CHECK(sentCount == (press + 1) * 2);
			CHECK(expect(press * 2, FALSE, 0, scanCodes[index] & 0xFF));
			CHECK(expect(press * 2 + 1, FALSE, KBD_FLAGS_RELEASE, scanCodes[index] & 0xFF));
		}
		CHECK(!key(virtualKeys[index], scanCodes[index], TRUE, LLKHF_INJECTED));
		CHECK(!key(virtualKeys[index], scanCodes[index], TRUE, LLKHF_INJECTED));
		CHECK(!key(virtualKeys[index], scanCodes[index], FALSE, LLKHF_INJECTED));
		CHECK(drain());
		CHECK(sentCount == 7);
		CHECK(expect(4, FALSE, 0, scanCodes[index] & 0xFF));
		CHECK(expect(5, FALSE, KBD_FLAGS_DOWN, scanCodes[index] & 0xFF));
		CHECK(expect(6, FALSE, KBD_FLAGS_RELEASE, scanCodes[index] & 0xFF));
		foreground = (HWND)(UINT_PTR)2;
		CHECK(!key(virtualKeys[index], scanCodes[index], TRUE, 0));
		CHECK(!key(virtualKeys[index], scanCodes[index], FALSE, 0));
		CHECK(messageCount == 0);
		CHECK(sentCount == 7);
		foreground = capture.window;
		queueOk = FALSE;
		CHECK(!key(virtualKeys[index], scanCodes[index], TRUE, 0));
		CHECK(rdk_capture_error(&capture) == ERROR_NOT_ENOUGH_QUOTA);
	}
	return TRUE;
}

static BOOL remote_lock_indicators(void)
{
	CHECK(rdk_capture_sync_locks(&capture, FALSE, FALSE));
	CHECK(injectedCount == 0);
	CHECK(rdk_capture_sync_locks(&capture, TRUE, TRUE));
	CHECK(localNumLock && localCapsLock && injectedCount == 4);
	CHECK(injected[0].ki.dwFlags == KEYEVENTF_EXTENDEDKEY);
	CHECK(injected[1].ki.dwFlags == (KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP));
	CHECK(injected[2].ki.dwFlags == 0 && injected[3].ki.dwFlags == KEYEVENTF_KEYUP);
	CHECK(rdk_capture_sync_locks(&capture, TRUE, TRUE));
	CHECK(injectedCount == 4);
	CHECK(rdk_capture_sync_locks(&capture, FALSE, TRUE));
	CHECK(!localNumLock && localCapsLock && injectedCount == 6);
	CHECK(rdk_capture_sync_locks(&capture, FALSE, FALSE));
	CHECK(!localNumLock && !localCapsLock && injectedCount == 8);
	CHECK(messageCount == 0 && sentCount == 0);
	foreground = (HWND)(UINT_PTR)2;
	CHECK(rdk_capture_sync_locks(&capture, TRUE, TRUE));
	foreground = capture.window;
	rdk_capture_set_active(&capture, FALSE);
	CHECK(rdk_capture_sync_locks(&capture, TRUE, TRUE));
	CHECK(injectedCount == 8 && !localNumLock && !localCapsLock);
	reset();
	injectLimit = 0;
	CHECK(!rdk_capture_sync_locks(&capture, TRUE, TRUE));
	CHECK(!localNumLock && !localCapsLock && injectedCount == 0);
	injectLimit = 1;
	CHECK(!rdk_capture_sync_locks(&capture, TRUE, TRUE));
	CHECK(injectedCount == 2);
	CHECK(injected[1].ki.dwFlags & KEYEVENTF_KEYUP);
	CHECK(messageCount == 0 && sentCount == 0);
	return TRUE;
}

static BOOL only_foreground(void)
{
	foreground = (HWND)(UINT_PTR)2;
	CHECK(!key(VK_LWIN, RDP_SCANCODE_LWIN, TRUE, 0));
	foreground = capture.window;
	rdk_capture_set_active(&capture, FALSE);
	CHECK(!key('R', RDP_SCANCODE_KEY_R, TRUE, 0));
	CHECK(messageCount == 0);
	CHECK(!rdk_capture_route(&capture, -1, WM_KEYDOWN, NULL));
	return TRUE;
}

static BOOL focus_generation(void)
{
	CHECK(key(VK_LWIN, RDP_SCANCODE_LWIN, TRUE, 0));
	CHECK(drain());
	CHECK(key('R', RDP_SCANCODE_KEY_R, TRUE, 0));
	rdk_capture_set_active(&capture, FALSE);
	CHECK(rdk_keyboard_release_all(&keyboard));
	rdk_capture_set_active(&capture, TRUE);
	CHECK(drain());
	CHECK(sentCount == 2);
	CHECK(expect(1, FALSE, KBD_FLAGS_EXTENDED | KBD_FLAGS_RELEASE, 0x5B));
	CHECK(key('R', RDP_SCANCODE_KEY_R, FALSE, 0));
	CHECK(drain());
	CHECK(sentCount == 2);
	return TRUE;
}

static BOOL alt_tab_and_exit(void)
{
	CHECK(key(VK_LMENU, RDP_SCANCODE_LMENU, TRUE, LLKHF_ALTDOWN));
	CHECK(key(VK_TAB, RDP_SCANCODE_TAB, TRUE, LLKHF_ALTDOWN));
	CHECK(key(VK_TAB, RDP_SCANCODE_TAB, FALSE, LLKHF_ALTDOWN));
	CHECK(key(VK_LMENU, RDP_SCANCODE_LMENU, FALSE, 0));
	CHECK(drain());
	CHECK(sentCount == 4);
	CHECK(expect(0, FALSE, 0, 0x38));
	CHECK(expect(1, FALSE, 0, 0x0F));
	CHECK(expect(3, FALSE, KBD_FLAGS_RELEASE, 0x38));
	CHECK(key(VK_LCONTROL, RDP_SCANCODE_LCONTROL, TRUE, 0));
	CHECK(key(VK_LSHIFT, RDP_SCANCODE_LSHIFT, TRUE, 0));
	CHECK(key(VK_F12, RDP_SCANCODE_F12, TRUE, 0));
	CHECK(drain());
	CHECK(keyboard.quit && sentCount == 8);
	for (size_t index = 0; index < sentCount; ++index)
		CHECK(sent[index].code != 0x58);
	return TRUE;
}

static BOOL session_menu_shortcut(void)
{
	CHECK(key(VK_LCONTROL, RDP_SCANCODE_LCONTROL, TRUE, 0));
	CHECK(key(VK_RSHIFT, RDP_SCANCODE_RSHIFT, TRUE, 0));
	CHECK(key(VK_F9, RDP_SCANCODE_F9, TRUE, 0));
	CHECK(key(VK_F9, RDP_SCANCODE_F9, TRUE, 0));
	CHECK(drain());
	CHECK(keyboard.menu && !keyboard.quit && sentCount == 4);
	for (size_t index = 0; index < sentCount; ++index)
		CHECK(sent[index].code != RDP_SCANCODE_F9);
	rdk_capture_set_active(&capture, FALSE);
	CHECK(!key(VK_DOWN, RDP_SCANCODE_DOWN, TRUE, 0));
	CHECK(!key(VK_ESCAPE, RDP_SCANCODE_ESCAPE, TRUE, 0));
	CHECK(messageCount == 0);
	keyboard.menu = FALSE;
	CHECK(rdk_keyboard_release_all(&keyboard));
	rdk_capture_set_active(&capture, TRUE);
	CHECK(key('A', RDP_SCANCODE_KEY_A, TRUE, 0));
	CHECK(key('A', RDP_SCANCODE_KEY_A, FALSE, 0));
	CHECK(drain());
	CHECK(sentCount == 6);
	return TRUE;
}

static BOOL ctrl_alt_end(void)
{
	CHECK(key(VK_LCONTROL, RDP_SCANCODE_LCONTROL, TRUE, 0));
	CHECK(key(VK_LMENU, RDP_SCANCODE_LMENU, TRUE, LLKHF_ALTDOWN));
	CHECK(key(VK_END, RDP_SCANCODE_END, TRUE, LLKHF_ALTDOWN));
	CHECK(key(VK_END, RDP_SCANCODE_END, TRUE, LLKHF_ALTDOWN));
	CHECK(key(VK_END, RDP_SCANCODE_END, FALSE, LLKHF_ALTDOWN));
	CHECK(key(VK_LMENU, RDP_SCANCODE_LMENU, FALSE, 0));
	CHECK(key(VK_LCONTROL, RDP_SCANCODE_LCONTROL, FALSE, 0));
	CHECK(messageCount == 7);
	CHECK(drain());
	CHECK(sentCount == 6);
	CHECK(expect(0, FALSE, 0, 0x1D));
	CHECK(expect(1, FALSE, 0, 0x38));
	CHECK(expect(2, FALSE, KBD_FLAGS_EXTENDED, 0x53));
	CHECK(expect(3, FALSE, KBD_FLAGS_EXTENDED | KBD_FLAGS_RELEASE, 0x53));
	CHECK(expect(4, FALSE, KBD_FLAGS_RELEASE, 0x38));
	CHECK(expect(5, FALSE, KBD_FLAGS_RELEASE, 0x1D));
	CHECK(rdk_keyboard_idle(&keyboard));
	CHECK(!keyboard.quit && !keyboard.reconnect && !keyboard.minimize);
	foreground = (HWND)(UINT_PTR)2;
	CHECK(!key(VK_LCONTROL, RDP_SCANCODE_LCONTROL, TRUE, 0));
	CHECK(!key(VK_LMENU, RDP_SCANCODE_LMENU, TRUE, LLKHF_ALTDOWN));
	CHECK(!key(VK_END, RDP_SCANCODE_END, TRUE, LLKHF_ALTDOWN));
	CHECK(messageCount == 0 && sentCount == 6);
	foreground = capture.window;
	CHECK(key(VK_LCONTROL, RDP_SCANCODE_LCONTROL, TRUE, 0));
	CHECK(key(VK_LMENU, RDP_SCANCODE_LMENU, TRUE, LLKHF_ALTDOWN));
	CHECK(key(VK_END, RDP_SCANCODE_END, TRUE, LLKHF_ALTDOWN));
	rdk_capture_set_active(&capture, FALSE);
	rdk_capture_set_active(&capture, TRUE);
	CHECK(drain());
	CHECK(sentCount == 6);
	return TRUE;
}

static BOOL injected_alt_codes(void)
{
	const UINT16 codes[] = { RDP_SCANCODE_NUMPAD0, RDP_SCANCODE_NUMPAD1,
	                        RDP_SCANCODE_NUMPAD6, RDP_SCANCODE_NUMPAD9 };
	CHECK(key(VK_LMENU, RDP_SCANCODE_LMENU, TRUE, LLKHF_INJECTED));
	for (size_t index = 0; index < ARRAYSIZE(codes); ++index)
	{
		CHECK(key(0, codes[index], TRUE, LLKHF_INJECTED));
		CHECK(key(0, codes[index], FALSE, LLKHF_INJECTED));
	}
	CHECK(key(VK_LMENU, RDP_SCANCODE_LMENU, FALSE, LLKHF_INJECTED));
	CHECK(drain());
	CHECK(sentCount == 2);
	CHECK(expect(0, TRUE, 0, 0x00A9));
	CHECK(expect(1, TRUE, KBD_FLAGS_RELEASE, 0x00A9));
	return TRUE;
}

static BOOL zero_scan_and_repeats(void)
{
	CHECK(key('R', 0, TRUE, LLKHF_INJECTED));
	CHECK(key('R', 0, TRUE, LLKHF_INJECTED));
	CHECK(key('R', 0, FALSE, LLKHF_INJECTED));
	CHECK(drain());
	CHECK(sentCount == 3);
	CHECK(expect(0, FALSE, 0, 0x13));
	CHECK(expect(1, FALSE, KBD_FLAGS_DOWN, 0x13));
	CHECK(expect(2, FALSE, KBD_FLAGS_RELEASE, 0x13));
	return TRUE;
}

static BOOL queue_failure(void)
{
	queueOk = FALSE;
	CHECK(key(VK_LWIN, RDP_SCANCODE_LWIN, TRUE, 0));
	CHECK(rdk_capture_error(&capture) == ERROR_NOT_ENOUGH_QUOTA);
	CHECK(messageCount == 0 && sentCount == 0);
	return TRUE;
}

static BOOL inactive_hook_lifecycle(void)
{
	rdkCapture realCapture;
	CHECK(rdk_capture_start(&realCapture, GetDesktopWindow()));
	CHECK(rdk_capture_error(&realCapture) == 0);
	rdk_capture_stop(&realCapture);
	CHECK(!realCapture.thread && !realCapture.ready && !realCapture.stop);
	rdk_capture_stop(&realCapture);
	return TRUE;
}

int main(void)
{
	BOOL (*tests[])(void) = { windows_run, lock_passthrough, only_foreground, focus_generation,
		alt_tab_and_exit, ctrl_alt_end, injected_alt_codes, zero_scan_and_repeats, queue_failure,
		remote_lock_indicators, session_menu_shortcut, inactive_hook_lifecycle };
	for (UINT mode = 0; mode < 2; ++mode)
	{
		if (!rdk_latency_enable(mode != 0))
			return 1;
		for (size_t index = 0; index < ARRAYSIZE(tests); ++index)
		{
			reset();
			if (!tests[index]())
				return 1;
		}
	}
	printf("Passed %zu keyboard capture tests with timing disabled and enabled\n", ARRAYSIZE(tests));
	return 0;
}