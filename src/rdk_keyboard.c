#include "rdk_keyboard.h"
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#include <stdio.h>

BOOL rdk_keyboard_configure(rdpSettings* settings)
{
	if (!freerdp_settings_set_bool(settings, FreeRDP_UnicodeInput, TRUE))
		return FALSE;
	if (freerdp_settings_get_uint32(settings, FreeRDP_KeyboardLayout) == 0)
	{
		WCHAR layout[KL_NAMELENGTH];
		if (!GetKeyboardLayoutNameW(layout) ||
		    !freerdp_settings_set_uint32(settings, FreeRDP_KeyboardLayout, wcstoul(layout, NULL, 16)))
			return FALSE;
	}
	return TRUE;
}

static BOOL rdk_input_ready(rdpInput* input)
{
	return input && input->context && freerdp_is_active_state(input->context) &&
	       !freerdp_settings_get_bool(input->context->settings, FreeRDP_SuspendInput);
}

static BOOL rdk_protocol_scan(rdpInput* input, UINT16 flags, UINT8 code)
{
	return rdk_input_ready(input) && freerdp_input_send_keyboard_event(input, flags, code);
}

static BOOL rdk_protocol_unicode(rdpInput* input, UINT16 flags, UINT16 code)
{
	if (!rdk_input_ready(input))
	{
		fprintf(stderr, "rdk: Unicode input rejected: session is inactive or input is suspended\n");
		return FALSE;
	}
	if (!freerdp_settings_get_bool(input->context->settings, FreeRDP_UnicodeInput))
	{
		fprintf(stderr, "rdk: Unicode input rejected: disabled in session settings\n");
		return FALSE;
	}
	if (!freerdp_input_send_unicode_keyboard_event(input, flags, code))
	{
		fprintf(stderr, "rdk: FreeRDP Unicode input send failed\n");
		return FALSE;
	}
	return TRUE;
}

static BOOL rdk_protocol_pause(rdpInput* input)
{
	return rdk_input_ready(input) && freerdp_input_send_keyboard_pause_event(input);
}

void rdk_keyboard_layout(rdkKeyboard* keyboard, HKL layout)
{
	const LCID locale = MAKELCID(LOWORD((ULONG_PTR)layout), SORT_DEFAULT);
	DWORD ansi = 0;
	DWORD oem = 0;
	if (!GetLocaleInfoW(locale, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
	                    (WCHAR*)&ansi, sizeof(ansi) / sizeof(WCHAR)) || ansi == 0)
		ansi = GetACP();
	if (!GetLocaleInfoW(locale, LOCALE_IDEFAULTCODEPAGE | LOCALE_RETURN_NUMBER,
	                    (WCHAR*)&oem, sizeof(oem) / sizeof(WCHAR)) || oem == 0)
		oem = GetOEMCP();
	keyboard->ansiCodePage = ansi;
	keyboard->oemCodePage = oem;
}

void rdk_keyboard_init(rdkKeyboard* keyboard, rdpInput* input)
{
	ZeroMemory(keyboard, sizeof(*keyboard));
	keyboard->input = input;
	keyboard->sendScan = rdk_protocol_scan;
	keyboard->sendUnicode = rdk_protocol_unicode;
	keyboard->sendPause = rdk_protocol_pause;
	rdk_keyboard_layout(keyboard, GetKeyboardLayout(0));
}

static BOOL rdk_scan(rdkKeyboard* keyboard, UINT16 code, BOOL down)
{
	if (!down && !keyboard->remoteDown[code])
		return TRUE;
	UINT16 flags = code & KBD_FLAGS_EXTENDED;
	flags |= down ? (keyboard->remoteDown[code] ? KBD_FLAGS_DOWN : 0) : KBD_FLAGS_RELEASE;
	if (!keyboard->sendScan(keyboard->input, flags, (UINT8)code))
		return FALSE;
	keyboard->remoteDown[code] = down;
	return TRUE;
}

BOOL rdk_keyboard_flush(rdkKeyboard* keyboard)
{
	const size_t count = keyboard->pendingCount;
	keyboard->altPending = FALSE;
	keyboard->pendingCount = 0;
	for (size_t index = 0; index < count; ++index)
	{
		const rdkKeyEvent event = keyboard->pending[index];
		if (!rdk_scan(keyboard, event.code, event.down))
			return FALSE;
	}
	return TRUE;
}

static BOOL rdk_unicode_unit(rdkKeyboard* keyboard, UINT16 unit)
{
	const BOOL down = keyboard->sendUnicode(keyboard->input, 0, unit);
	const BOOL up = keyboard->sendUnicode(keyboard->input, KBD_FLAGS_RELEASE, unit);
	return down && up;
}

BOOL rdk_keyboard_unicode(rdkKeyboard* keyboard, UINT32 codepoint)
{
	if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
		return FALSE;
	if (codepoint <= 0xFFFF)
		return rdk_unicode_unit(keyboard, (UINT16)codepoint);
	codepoint -= 0x10000;
	return rdk_unicode_unit(keyboard, (UINT16)(0xD800 + (codepoint >> 10))) &&
	       rdk_unicode_unit(keyboard, (UINT16)(0xDC00 + (codepoint & 0x3FF)));
}

BOOL rdk_keyboard_idle(const rdkKeyboard* keyboard)
{
	for (size_t index = 0; index < RDK_KEY_COUNT; ++index)
	{
		if (keyboard->localDown[index])
			return FALSE;
	}
	return !keyboard->altPending;
}

BOOL rdk_keyboard_text_next(rdkKeyboard* keyboard, const WCHAR** text)
{
	const WCHAR* next = *text;
	UINT32 codepoint = *next++;
	if (codepoint == 0)
		return TRUE;
	UINT16 scan = 0;
	if (codepoint == L'\r' || codepoint == L'\n')
	{
		scan = RDP_SCANCODE_RETURN;
		if (codepoint == L'\r' && *next == L'\n')
			++next;
	}
	else if (codepoint == L'\t')
		scan = RDP_SCANCODE_TAB;
	else if (codepoint == L'\b')
		scan = RDP_SCANCODE_BACKSPACE;
	else if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
	{
		if (*next < 0xDC00 || *next > 0xDFFF)
			return FALSE;
		codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (*next++ - 0xDC00);
	}
	BOOL ok;
	if (scan)
	{
		const BOOL down = rdk_scan(keyboard, scan, TRUE);
		const BOOL up = rdk_scan(keyboard, scan, FALSE);
		ok = down && up;
	}
	else
		ok = rdk_keyboard_unicode(keyboard, codepoint);
	if (ok)
		*text = next;
	return ok;
}

BOOL rdk_keyboard_tap(rdkKeyboard* keyboard, UINT16 scan)
{
	if (!scan || scan >= RDK_KEY_COUNT || !rdk_keyboard_idle(keyboard))
		return FALSE;
	const BOOL down = rdk_scan(keyboard, scan, TRUE);
	const BOOL up = rdk_scan(keyboard, scan, FALSE);
	return down && up;
}

BOOL rdk_keyboard_release_all(rdkKeyboard* keyboard)
{
	keyboard->altPending = FALSE;
	keyboard->pendingCount = 0;
	ZeroMemory(keyboard->localDown, sizeof(keyboard->localDown));
	ZeroMemory(keyboard->suppressed, sizeof(keyboard->suppressed));
	BOOL ok = TRUE;
	for (UINT16 code = 1; code < RDK_KEY_COUNT; ++code)
	{
		if (keyboard->remoteDown[code])
			ok = rdk_scan(keyboard, code, FALSE) && ok;
	}
	return ok;
}

static int rdk_numpad_digit(UINT16 code)
{
	switch (code)
	{
		case RDP_SCANCODE_NUMPAD0: return 0;
		case RDP_SCANCODE_NUMPAD1: return 1;
		case RDP_SCANCODE_NUMPAD2: return 2;
		case RDP_SCANCODE_NUMPAD3: return 3;
		case RDP_SCANCODE_NUMPAD4: return 4;
		case RDP_SCANCODE_NUMPAD5: return 5;
		case RDP_SCANCODE_NUMPAD6: return 6;
		case RDP_SCANCODE_NUMPAD7: return 7;
		case RDP_SCANCODE_NUMPAD8: return 8;
		case RDP_SCANCODE_NUMPAD9: return 9;
		default: return -1;
	}
}

static int rdk_hex_digit(UINT16 code)
{
	if (code >= RDP_SCANCODE_KEY_1 && code <= RDP_SCANCODE_KEY_9)
		return code - RDP_SCANCODE_KEY_1 + 1;
	switch (code)
	{
		case RDP_SCANCODE_KEY_0: return 0;
		case RDP_SCANCODE_KEY_A: return 10;
		case RDP_SCANCODE_KEY_B: return 11;
		case RDP_SCANCODE_KEY_C: return 12;
		case RDP_SCANCODE_KEY_D: return 13;
		case RDP_SCANCODE_KEY_E: return 14;
		case RDP_SCANCODE_KEY_F: return 15;
		default: return -1;
	}
}

static BOOL rdk_finish_alt(rdkKeyboard* keyboard)
{
	UINT32 codepoint = keyboard->altValue;
	if (!keyboard->altDigits)
		return rdk_keyboard_flush(keyboard);
	if (!keyboard->altHex)
	{
		const char byte = (char)codepoint;
		WCHAR character;
		const UINT page = keyboard->altLeadingZero ? keyboard->ansiCodePage : keyboard->oemCodePage;
		const DWORD flags = (page == CP_UTF8) ? MB_ERR_INVALID_CHARS : MB_USEGLYPHCHARS;
		if (MultiByteToWideChar(page, flags, &byte, 1, &character, 1) != 1)
			return rdk_keyboard_flush(keyboard);
		codepoint = character;
	}
	if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
		return rdk_keyboard_flush(keyboard);
	keyboard->altPending = FALSE;
	keyboard->pendingCount = 0;
	for (size_t index = 0; index < RDK_KEY_COUNT; ++index)
		keyboard->suppressed[index] = keyboard->localDown[index];
	return rdk_keyboard_unicode(keyboard, codepoint);
}

static BOOL rdk_key_event(rdkKeyboard* keyboard, UINT16 code, BOOL down)
{
	const BOOL wasDown = keyboard->localDown[code];
	const BOOL wasIdle = rdk_keyboard_idle(keyboard);
	keyboard->localDown[code] = down;
	if (keyboard->suppressed[code])
	{
		if (!down)
			keyboard->suppressed[code] = FALSE;
		return TRUE;
	}
	if (down && (code == RDP_SCANCODE_F12 || code == RDP_SCANCODE_F11 || code == RDP_SCANCODE_F10) &&
	    (keyboard->localDown[RDP_SCANCODE_LCONTROL] || keyboard->localDown[RDP_SCANCODE_RCONTROL]) &&
	    (keyboard->localDown[RDP_SCANCODE_LSHIFT] || keyboard->localDown[RDP_SCANCODE_RSHIFT]))
	{
		keyboard->quit = code != RDP_SCANCODE_F10;
		keyboard->reconnect = code == RDP_SCANCODE_F11;
		keyboard->minimize = code == RDP_SCANCODE_F10;
		return rdk_keyboard_release_all(keyboard);
	}
	if (down && !wasDown && code == RDP_SCANCODE_LMENU && wasIdle)
	{
		keyboard->altPending = TRUE;
		keyboard->altDigits = FALSE;
		keyboard->altHex = FALSE;
		keyboard->altLeadingZero = FALSE;
		keyboard->altValue = 0;
	}
	if (!keyboard->altPending)
		return rdk_scan(keyboard, code, down);
	if (keyboard->pendingCount == RDK_PENDING_KEYS)
		return rdk_keyboard_flush(keyboard) && rdk_scan(keyboard, code, down);
	keyboard->pending[keyboard->pendingCount++] = (rdkKeyEvent){ code, down };
	if (code == RDP_SCANCODE_LMENU)
		return down ? TRUE : rdk_finish_alt(keyboard);
	if (code == RDP_SCANCODE_ADD && !keyboard->altDigits)
	{
		keyboard->altHex = TRUE;
		return TRUE;
	}
	int digit = rdk_numpad_digit(code);
	if (digit < 0 && keyboard->altHex)
		digit = rdk_hex_digit(code);
	if (digit < 0)
		return rdk_keyboard_flush(keyboard);
	if (down)
	{
		if (!keyboard->altDigits)
			keyboard->altLeadingZero = (digit == 0);
		keyboard->altDigits = TRUE;
		keyboard->altValue = keyboard->altValue * (keyboard->altHex ? 16u : 10u) + (UINT32)digit;
		if (keyboard->altHex && keyboard->altValue > 0x10FFFF)
			return rdk_keyboard_flush(keyboard);
		if (!keyboard->altHex)
			keyboard->altValue &= 0xFF;
	}
	return TRUE;
}

BOOL rdk_keyboard_key(rdkKeyboard* keyboard, UINT message, WPARAM vk, LPARAM data)
{
	const BOOL down = (message == WM_KEYDOWN) || (message == WM_SYSKEYDOWN);
	UINT16 code = (UINT16)((data >> 16) & 0xFF);
	if (vk == VK_PAUSE)
	{
		if (!down || (data & ((LPARAM)1 << 30)))
			return TRUE;
		return rdk_keyboard_flush(keyboard) && keyboard->sendPause(keyboard->input);
	}
	if (code == 0)
		return TRUE;
	if ((data & (1u << 24)) && vk != VK_NUMLOCK && vk != VK_RSHIFT)
		code |= KBD_FLAGS_EXTENDED;
	UINT repeats = down ? (UINT)(data & 0xFFFF) : 1;
	if (repeats == 0)
		repeats = 1;
	for (UINT index = 0; index < repeats; ++index)
	{
		if (!rdk_key_event(keyboard, code, down))
			return FALSE;
		if (keyboard->quit || keyboard->minimize)
			break;
	}
	return TRUE;
}