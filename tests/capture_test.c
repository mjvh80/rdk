#include "rdk_capture.h"
#include "rdk_keyboard.h"
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

static void reset(void)
{
	rdk_capture_init(&capture, (HWND)(UINT_PTR)1);
	capture.foreground = get_foreground;
	capture.post = post_key;
	foreground = capture.window;
	rdk_capture_set_active(&capture, TRUE);
	rdk_keyboard_init(&keyboard, NULL);
	keyboard.sendScan = send_scan;
	keyboard.sendUnicode = send_unicode;
	keyboard.ansiCodePage = 1252;
	keyboard.oemCodePage = 437;
	messageCount = sentCount = 0;
	queueOk = TRUE;
}

static BOOL key(UINT vk, UINT16 scan, BOOL down, DWORD extraFlags)
{
	KBDLLHOOKSTRUCT event = { 0 };
	event.vkCode = vk;
	event.scanCode = scan & 0xFF;
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
	BOOL (*tests[])(void) = { windows_run, only_foreground, focus_generation,
		alt_tab_and_exit, injected_alt_codes, zero_scan_and_repeats, queue_failure,
		inactive_hook_lifecycle };
	for (size_t index = 0; index < ARRAYSIZE(tests); ++index)
	{
		reset();
		if (!tests[index]())
			return 1;
	}
	printf("Passed %zu keyboard capture tests\n", ARRAYSIZE(tests));
	return 0;
}