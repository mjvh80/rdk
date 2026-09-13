#include "rdk_keyboard.h"
#include <freerdp/client/cmdline.h>
#include <stdio.h>

typedef struct
{
	BOOL unicode;
	UINT16 flags;
	UINT16 code;
} RecordedEvent;

static RecordedEvent events[2048];
static size_t eventCount;
static size_t failAt;
static UINT pauseCount;
static rdkKeyboard keyboard;

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return FALSE; \
} } while (0)

static BOOL record(BOOL unicode, UINT16 flags, UINT16 code)
{
	if (eventCount == ARRAYSIZE(events))
		return FALSE;
	events[eventCount++] = (RecordedEvent){ unicode, flags, code };
	return eventCount != failAt;
}

static BOOL record_scan(rdpInput* input, UINT16 flags, UINT8 code)
{
	(void)input;
	return record(FALSE, flags, code);
}

static BOOL record_unicode(rdpInput* input, UINT16 flags, UINT16 code)
{
	(void)input;
	return record(TRUE, flags, code);
}

static BOOL record_pause(rdpInput* input)
{
	(void)input;
	++pauseCount;
	return TRUE;
}

static void reset(void)
{
	rdk_keyboard_init(&keyboard, NULL);
	keyboard.sendScan = record_scan;
	keyboard.sendUnicode = record_unicode;
	keyboard.sendPause = record_pause;
	keyboard.ansiCodePage = 1252;
	keyboard.oemCodePage = 437;
	eventCount = 0;
	failAt = 0;
	pauseCount = 0;
}

static BOOL key(UINT vk, UINT16 code, BOOL down)
{
	const LPARAM data = ((LPARAM)(code & 0xFF) << 16) |
	                    ((code & KBD_FLAGS_EXTENDED) ? ((LPARAM)1 << 24) : 0) | 1;
	return rdk_keyboard_key(&keyboard, down ? WM_SYSKEYDOWN : WM_SYSKEYUP, vk, data);
}

static BOOL tap(UINT16 code)
{
	return key(0, code, TRUE) && key(0, code, FALSE);
}

static BOOL digits(const char* text)
{
	const UINT16 codes[] = { RDP_SCANCODE_NUMPAD0, RDP_SCANCODE_NUMPAD1, RDP_SCANCODE_NUMPAD2,
		RDP_SCANCODE_NUMPAD3, RDP_SCANCODE_NUMPAD4, RDP_SCANCODE_NUMPAD5, RDP_SCANCODE_NUMPAD6,
		RDP_SCANCODE_NUMPAD7, RDP_SCANCODE_NUMPAD8, RDP_SCANCODE_NUMPAD9 };
	for (const char* digit = text; *digit; ++digit)
	{
		if (!tap(codes[*digit - '0']))
			return FALSE;
	}
	return TRUE;
}

static BOOL alt_code(const char* text)
{
	return key(VK_MENU, RDP_SCANCODE_LMENU, TRUE) && digits(text) &&
	       key(VK_MENU, RDP_SCANCODE_LMENU, FALSE);
}

static BOOL expect(size_t index, BOOL unicode, UINT16 flags, UINT16 code)
{
	CHECK(index < eventCount);
	CHECK(events[index].unicode == unicode);
	CHECK(events[index].flags == flags);
	CHECK(events[index].code == code);
	return TRUE;
}

static BOOL decimal_codes(void)
{
	CHECK(alt_code("0169"));
	CHECK(alt_code("130"));
	CHECK(alt_code("1"));
	CHECK(alt_code("0233"));
	CHECK(eventCount == 8);
	CHECK(expect(0, TRUE, 0, 0x00A9));
	CHECK(expect(1, TRUE, KBD_FLAGS_RELEASE, 0x00A9));
	CHECK(expect(2, TRUE, 0, 0x00E9));
	CHECK(expect(4, TRUE, 0, 0x263A));
	CHECK(expect(6, TRUE, 0, 0x00E9));
	CHECK(rdk_keyboard_idle(&keyboard));
	return TRUE;
}

static BOOL hex_codes(void)
{
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, TRUE));
	CHECK(tap(RDP_SCANCODE_ADD));
	CHECK(digits("20"));
	CHECK(tap(RDP_SCANCODE_KEY_A));
	CHECK(tap(RDP_SCANCODE_KEY_C));
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, FALSE));
	CHECK(eventCount == 2);
	CHECK(expect(0, TRUE, 0, 0x20AC));
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, TRUE));
	CHECK(tap(RDP_SCANCODE_ADD));
	CHECK(digits("1"));
	CHECK(tap(RDP_SCANCODE_KEY_F));
	CHECK(digits("600"));
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, FALSE));
	CHECK(eventCount == 6);
	CHECK(expect(2, TRUE, 0, 0xD83D));
	CHECK(expect(3, TRUE, KBD_FLAGS_RELEASE, 0xD83D));
	CHECK(expect(4, TRUE, 0, 0xDE00));
	CHECK(expect(5, TRUE, KBD_FLAGS_RELEASE, 0xDE00));
	return TRUE;
}

static BOOL shortcuts(void)
{
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, TRUE));
	CHECK(eventCount == 0);
	CHECK(tap(RDP_SCANCODE_KEY_F));
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, FALSE));
	CHECK(eventCount == 4);
	CHECK(expect(0, FALSE, 0, 0x38));
	CHECK(expect(1, FALSE, 0, 0x21));
	CHECK(expect(2, FALSE, KBD_FLAGS_RELEASE, 0x21));
	CHECK(expect(3, FALSE, KBD_FLAGS_RELEASE, 0x38));
	CHECK(tap(RDP_SCANCODE_LMENU));
	CHECK(eventCount == 6);
	CHECK(key(VK_CONTROL, RDP_SCANCODE_LCONTROL, TRUE));
	CHECK(alt_code("1"));
	CHECK(key(VK_CONTROL, RDP_SCANCODE_LCONTROL, FALSE));
	CHECK(eventCount == 12);
	CHECK(key(VK_RMENU, RDP_SCANCODE_RMENU, TRUE));
	CHECK(tap(RDP_SCANCODE_NUMPAD1));
	CHECK(key(VK_RMENU, RDP_SCANCODE_RMENU, FALSE));
	CHECK(expect(12, FALSE, KBD_FLAGS_EXTENDED, 0x38));
	CHECK(expect(15, FALSE, KBD_FLAGS_EXTENDED | KBD_FLAGS_RELEASE, 0x38));
	return TRUE;
}

static BOOL focus_loss(void)
{
	CHECK(key(VK_CONTROL, RDP_SCANCODE_LCONTROL, TRUE));
	CHECK(key('A', RDP_SCANCODE_KEY_A, TRUE));
	CHECK(rdk_keyboard_release_all(&keyboard));
	CHECK(eventCount == 4);
	CHECK(expect(2, FALSE, KBD_FLAGS_RELEASE, 0x1D));
	CHECK(expect(3, FALSE, KBD_FLAGS_RELEASE, 0x1E));
	CHECK(key('A', RDP_SCANCODE_KEY_A, FALSE));
	CHECK(eventCount == 4);
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, TRUE));
	CHECK(digits("016"));
	CHECK(rdk_keyboard_release_all(&keyboard));
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, FALSE));
	CHECK(eventCount == 4);
	CHECK(rdk_keyboard_idle(&keyboard));
	return TRUE;
}

static BOOL repeats_and_fixups(void)
{
	CHECK(rdk_keyboard_key(&keyboard, WM_KEYDOWN, 'A', ((LPARAM)0x1E << 16) | 3));
	CHECK(key('A', RDP_SCANCODE_KEY_A, FALSE));
	CHECK(eventCount == 4);
	CHECK(expect(0, FALSE, 0, 0x1E));
	CHECK(expect(1, FALSE, KBD_FLAGS_DOWN, 0x1E));
	CHECK(expect(2, FALSE, KBD_FLAGS_DOWN, 0x1E));
	CHECK(key(VK_NUMLOCK, RDP_SCANCODE_NUMLOCK_EXTENDED, TRUE));
	CHECK(key(VK_NUMLOCK, RDP_SCANCODE_NUMLOCK_EXTENDED, FALSE));
	CHECK(expect(4, FALSE, 0, 0x45));
	CHECK(key(VK_RSHIFT, RDP_SCANCODE_RSHIFT_EXTENDED, TRUE));
	CHECK(key(VK_RSHIFT, RDP_SCANCODE_RSHIFT_EXTENDED, FALSE));
	CHECK(expect(6, FALSE, 0, 0x36));
	CHECK(tap(RDP_SCANCODE_UP));
	CHECK(expect(8, FALSE, KBD_FLAGS_EXTENDED, 0x48));
	CHECK(key(VK_PAUSE, 0x45, TRUE));
	CHECK(key(VK_PAUSE, 0x45, FALSE));
	CHECK(pauseCount == 1);
	return TRUE;
}

static BOOL early_alt_release(void)
{
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, TRUE));
	CHECK(digits("016"));
	CHECK(key(VK_NUMPAD9, RDP_SCANCODE_NUMPAD9, TRUE));
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, FALSE));
	CHECK(key(VK_NUMPAD9, RDP_SCANCODE_NUMPAD9, TRUE));
	CHECK(key(VK_NUMPAD9, RDP_SCANCODE_NUMPAD9, FALSE));
	CHECK(eventCount == 2);
	CHECK(expect(0, TRUE, 0, 0x00A9));
	return TRUE;
}

static BOOL fallback_and_mouse(void)
{
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, TRUE));
	CHECK(rdk_keyboard_flush(&keyboard));
	CHECK(expect(0, FALSE, 0, 0x38));
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, FALSE));
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, TRUE));
	CHECK(tap(RDP_SCANCODE_ADD));
	CHECK(digits("110000"));
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, FALSE));
	CHECK(eventCount == 18);
	for (size_t index = 0; index < eventCount; ++index)
		CHECK(!events[index].unicode);
	reset();
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, TRUE));
	for (size_t index = 0; index < RDK_PENDING_KEYS; ++index)
		CHECK(tap(RDP_SCANCODE_NUMPAD1));
	CHECK(key(VK_MENU, RDP_SCANCODE_LMENU, FALSE));
	CHECK(eventCount == 2 * RDK_PENDING_KEYS + 2);
	for (size_t index = 0; index < eventCount; ++index)
		CHECK(!events[index].unicode);
	return TRUE;
}

static BOOL quit_and_failures(void)
{
	for (UINT side = 0; side < 2; ++side)
	{
		for (UINT modifiers = 0; modifiers < 8; ++modifiers)
		{
			reset();
			if (modifiers & 1) CHECK(key(VK_CONTROL, side ? RDP_SCANCODE_RCONTROL : RDP_SCANCODE_LCONTROL, TRUE));
			if (modifiers & 2) CHECK(key(VK_SHIFT, side ? RDP_SCANCODE_RSHIFT : RDP_SCANCODE_LSHIFT, TRUE));
			if (modifiers & 4) CHECK(key(VK_MENU, side ? RDP_SCANCODE_RMENU : RDP_SCANCODE_LMENU, TRUE));
			CHECK(key(VK_F11, RDP_SCANCODE_F11, TRUE));
			const BOOL reconnect = (modifiers & 3) == 3;
			CHECK(keyboard.reconnect == reconnect && keyboard.quit == reconnect);
			if (reconnect)
			{
				for (size_t index = 0; index < eventCount; ++index)
					CHECK(events[index].code != RDP_SCANCODE_F11);
				for (size_t index = 0; index < RDK_KEY_COUNT; ++index)
					CHECK(!keyboard.remoteDown[index]);
			}
			else
				CHECK(keyboard.remoteDown[RDP_SCANCODE_F11]);
			reset();
			if (modifiers & 1) CHECK(key(VK_CONTROL, side ? RDP_SCANCODE_RCONTROL : RDP_SCANCODE_LCONTROL, TRUE));
			if (modifiers & 2) CHECK(key(VK_SHIFT, side ? RDP_SCANCODE_RSHIFT : RDP_SCANCODE_LSHIFT, TRUE));
			if (modifiers & 4) CHECK(key(VK_MENU, side ? RDP_SCANCODE_RMENU : RDP_SCANCODE_LMENU, TRUE));
			CHECK(rdk_keyboard_key(&keyboard, WM_KEYDOWN, VK_F10, ((LPARAM)RDP_SCANCODE_F10 << 16) | 3));
			CHECK(keyboard.minimize == ((modifiers & 3) == 3));
			CHECK(!keyboard.quit && !keyboard.reconnect);
			if (keyboard.minimize)
			{
				for (size_t index = 0; index < eventCount; ++index)
					CHECK(events[index].code != RDP_SCANCODE_F10);
				for (size_t index = 0; index < RDK_KEY_COUNT; ++index)
					CHECK(!keyboard.remoteDown[index]);
				keyboard.minimize = FALSE;
				CHECK(key(VK_F10, RDP_SCANCODE_F10, FALSE));
				CHECK(tap(RDP_SCANCODE_KEY_A));
				CHECK(!keyboard.minimize);
			}
			else
				CHECK(keyboard.remoteDown[RDP_SCANCODE_F10]);
		}
	}
	reset();
	CHECK(key(VK_CONTROL, RDP_SCANCODE_LCONTROL, TRUE));
	CHECK(key(VK_SHIFT, RDP_SCANCODE_LSHIFT, TRUE));
	CHECK(key(VK_F12, RDP_SCANCODE_F12, TRUE));
	CHECK(keyboard.quit);
	CHECK(!keyboard.reconnect);
	CHECK(eventCount == 4);
	CHECK(rdk_keyboard_idle(&keyboard));
	for (size_t index = 0; index < eventCount; ++index)
		CHECK(events[index].code != 0x58);
	reset();
	failAt = 1;
	CHECK(!alt_code("0169"));
	CHECK(eventCount == 2);
	CHECK(expect(1, TRUE, KBD_FLAGS_RELEASE, 0x00A9));
	reset();
	CHECK(key(VK_CONTROL, RDP_SCANCODE_LCONTROL, TRUE));
	CHECK(key(VK_SHIFT, RDP_SCANCODE_LSHIFT, TRUE));
	failAt = 3;
	CHECK(!rdk_keyboard_release_all(&keyboard));
	CHECK(eventCount == 4);
	CHECK(rdk_keyboard_release_all(&keyboard));
	CHECK(eventCount == 5);
	return TRUE;
}

static BOOL rapid_sequences(void)
{
	for (size_t index = 0; index < 250; ++index)
	{
		CHECK(alt_code("0169"));
		CHECK(tap(RDP_SCANCODE_KEY_A));
		CHECK(expect(index * 4, TRUE, 0, 0x00A9));
		CHECK(expect(index * 4 + 2, FALSE, 0, 0x1E));
	}
	CHECK(eventCount == 1000);
	return TRUE;
}

static BOOL startup_text(void)
{
	const WCHAR* text = L"A\r\n\t\b\xD83D\xDE00";
	for (UINT index = 0; index < 5; ++index)
		CHECK(rdk_keyboard_text_next(&keyboard, &text));
	CHECK(*text == 0);
	CHECK(eventCount == 12);
	CHECK(expect(0, TRUE, 0, 'A'));
	CHECK(expect(2, FALSE, 0, 0x1C));
	CHECK(expect(3, FALSE, KBD_FLAGS_RELEASE, 0x1C));
	CHECK(expect(4, FALSE, 0, 0x0F));
	CHECK(expect(6, FALSE, 0, 0x0E));
	CHECK(expect(8, TRUE, 0, 0xD83D));
	CHECK(expect(10, TRUE, 0, 0xDE00));
	CHECK(rdk_keyboard_text_next(&keyboard, &text));
	CHECK(eventCount == 12);
	text = L"\xD800";
	CHECK(!rdk_keyboard_text_next(&keyboard, &text));
	CHECK(*text == 0xD800);
	text = L"\xDC00";
	CHECK(!rdk_keyboard_text_next(&keyboard, &text));
	CHECK(eventCount == 12);
	CHECK(!rdk_keyboard_unicode(&keyboard, 0x110000));
	return TRUE;
}

static BOOL configure_after_command_line(void)
{
	rdpSettings* settings = freerdp_settings_new(0);
	CHECK(settings);
	char program[] = "rdk";
	char server[] = "/v:example.invalid";
	char* args[] = { program, server };
	CHECK(freerdp_client_settings_parse_command_line(settings, ARRAYSIZE(args), args, FALSE) == 0);
	CHECK(!freerdp_settings_get_bool(settings, FreeRDP_UnicodeInput));
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_KeyboardLayout, 0));
	CHECK(rdk_keyboard_configure(settings));
	CHECK(freerdp_settings_get_bool(settings, FreeRDP_UnicodeInput));
	CHECK(freerdp_settings_get_uint32(settings, FreeRDP_KeyboardLayout) != 0);
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_KeyboardLayout, 0x0000041D));
	CHECK(rdk_keyboard_configure(settings));
	CHECK(freerdp_settings_get_uint32(settings, FreeRDP_KeyboardLayout) == 0x0000041D);
	CHECK(freerdp_settings_get_bool(settings, FreeRDP_UnicodeInput));
	freerdp_settings_free(settings);
	return TRUE;
}

int main(void)
{
	BOOL (*tests[])(void) = { decimal_codes, hex_codes, shortcuts, focus_loss,
		repeats_and_fixups, early_alt_release, fallback_and_mouse, quit_and_failures, rapid_sequences,
		startup_text, configure_after_command_line };
	for (size_t index = 0; index < ARRAYSIZE(tests); ++index)
	{
		reset();
		if (!tests[index]())
			return 1;
	}
	printf("Passed %zu keyboard tests\n", ARRAYSIZE(tests));
	return 0;
}