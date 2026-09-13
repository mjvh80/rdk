#include "rdk_startup.h"
#include <wchar.h>

typedef struct
{
	const WCHAR* text;
	UINT16 scan;
	BOOL delay;
} rdkStartupAction;

static BOOL rdk_startup_parse(const WCHAR** sequence, rdkStartupAction* action)
{
	static const struct { const WCHAR* name; UINT16 scan; } keys[] = {
		{ L"enter", RDP_SCANCODE_RETURN }, { L"return", RDP_SCANCODE_RETURN },
		{ L"tab", RDP_SCANCODE_TAB }, { L"esc", RDP_SCANCODE_ESCAPE },
		{ L"escape", RDP_SCANCODE_ESCAPE }, { L"backspace", RDP_SCANCODE_BACKSPACE },
		{ L"space", RDP_SCANCODE_SPACE }, { L"delete", RDP_SCANCODE_DELETE },
		{ L"insert", RDP_SCANCODE_INSERT }, { L"home", RDP_SCANCODE_HOME },
		{ L"end", RDP_SCANCODE_END }, { L"pageup", RDP_SCANCODE_PRIOR },
		{ L"pagedown", RDP_SCANCODE_NEXT }, { L"up", RDP_SCANCODE_UP },
		{ L"down", RDP_SCANCODE_DOWN }, { L"left", RDP_SCANCODE_LEFT },
		{ L"right", RDP_SCANCODE_RIGHT },
		{ L"F1", RDP_SCANCODE_F1 }, { L"F2", RDP_SCANCODE_F2 },
		{ L"F3", RDP_SCANCODE_F3 }, { L"F4", RDP_SCANCODE_F4 },
		{ L"F5", RDP_SCANCODE_F5 }, { L"F6", RDP_SCANCODE_F6 },
		{ L"F7", RDP_SCANCODE_F7 }, { L"F8", RDP_SCANCODE_F8 },
		{ L"F9", RDP_SCANCODE_F9 }, { L"F10", RDP_SCANCODE_F10 },
		{ L"F11", RDP_SCANCODE_F11 }, { L"F12", RDP_SCANCODE_F12 }
	};
	const WCHAR* next = *sequence;
	*action = (rdkStartupAction){ 0 };
	if (*next == L'<' && next[1] != L'<')
	{
		const WCHAR* end = wcschr(next + 1, L'>');
		if (!end) return FALSE;
		const size_t length = (size_t)(end - next - 1);
		if (length == 5 && _wcsnicmp(next + 1, L"delay", length) == 0)
			action->delay = TRUE;
		else
		{
			for (size_t index = 0; index < ARRAYSIZE(keys); ++index)
			{
				if (length == wcslen(keys[index].name) && _wcsnicmp(next + 1, keys[index].name, length) == 0)
				{
					action->scan = keys[index].scan;
					break;
				}
			}
			if (!action->scan) return FALSE;
		}
		*sequence = end + 1;
		return TRUE;
	}
	if (!*next) return FALSE;
	action->text = next;
	const WCHAR first = *next++;
	if ((first == L'<' && *next == L'<') || (first == L'\r' && *next == L'\n'))
		++next;
	else if (first >= 0xD800 && first <= 0xDBFF)
	{
		if (*next < 0xDC00 || *next > 0xDFFF) return FALSE;
		++next;
	}
	else if (first >= 0xDC00 && first <= 0xDFFF)
		return FALSE;
	*sequence = next;
	return TRUE;
}

void rdk_startup_init(rdkStartup* startup, const WCHAR* sequence, BOOL tokens, DWORD initialDelay, DWORD charDelay)
{
	*startup = (rdkStartup){ 0 };
	startup->next = sequence;
	startup->tokens = tokens;
	startup->initialDelay = initialDelay;
	startup->charDelay = charDelay;
}

BOOL rdk_startup_step(rdkStartup* startup, rdkKeyboard* keyboard, UINT64 now, BOOL ready)
{
	if (!startup->next || !*startup->next || !ready || !rdk_keyboard_idle(keyboard))
		return TRUE;
	if (!startup->armed)
	{
		startup->armed = TRUE;
		startup->nextAt = now + startup->initialDelay;
	}
	if (now < startup->nextAt)
		return TRUE;
	DWORD delay = startup->charDelay;
	const BOOL sent = startup->tokens ? rdk_startup_next(keyboard, &startup->next, &delay) :
	                                   rdk_keyboard_text_next(keyboard, &startup->next);
	if (!sent)
		return FALSE;
	startup->started = TRUE;
	startup->nextAt = now + delay;
	return TRUE;
}

DWORD rdk_startup_wait(const rdkStartup* startup, UINT64 now, BOOL ready)
{
	if (!startup->next || !*startup->next || !startup->armed || !ready)
		return 100;
	const UINT64 remaining = startup->nextAt > now ? startup->nextAt - now : 0;
	return remaining < 100 ? (DWORD)remaining : 100;
}

BOOL rdk_startup_keys_released(void)
{
	for (int key = VK_BACK; key <= VK_OEM_CLEAR; ++key)
		if (GetAsyncKeyState(key) & 0x8000)
			return FALSE;
	return TRUE;
}

BOOL rdk_startup_validate(const WCHAR* sequence, size_t* errorOffset)
{
	if (errorOffset) *errorOffset = 0;
	for (const WCHAR* next = sequence; next && *next; )
	{
		const WCHAR* before = next;
		rdkStartupAction action;
		if (!rdk_startup_parse(&next, &action))
		{
			if (errorOffset) *errorOffset = (size_t)(before - sequence);
			return FALSE;
		}
	}
	return TRUE;
}

BOOL rdk_startup_next(rdkKeyboard* keyboard, const WCHAR** sequence, DWORD* delay)
{
	if (!rdk_keyboard_idle(keyboard)) return FALSE;
	if (!*sequence || !**sequence) return TRUE;
	const WCHAR* next = *sequence;
	rdkStartupAction action;
	if (!rdk_startup_parse(&next, &action)) return FALSE;
	if (action.text && !rdk_keyboard_text_next(keyboard, &action.text)) return FALSE;
	if (action.scan && !rdk_keyboard_tap(keyboard, action.scan)) return FALSE;
	if (action.delay) *delay = 1000;
	*sequence = next;
	return TRUE;
}