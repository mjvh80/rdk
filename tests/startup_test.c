#include "rdk_startup.h"
#include <stdio.h>

typedef struct { BOOL unicode; UINT16 flags; UINT16 code; } RecordedEvent;
static RecordedEvent events[128];
static size_t eventCount;
static size_t failAt;

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

static BOOL record_scan(rdpInput* input, UINT16 flags, UINT8 code)
{
	(void)input;
	if (eventCount == ARRAYSIZE(events)) return FALSE;
	events[eventCount++] = (RecordedEvent){ FALSE, flags, code };
	return eventCount != failAt;
}

static BOOL record_unicode(rdpInput* input, UINT16 flags, UINT16 code)
{
	(void)input;
	if (eventCount == ARRAYSIZE(events)) return FALSE;
	events[eventCount++] = (RecordedEvent){ TRUE, flags, code };
	return eventCount != failAt;
}

int main(void)
{
	size_t offset;
	CHECK(rdk_startup_validate(L"<delay><enter><F1>hello<<world><DELAY><f12>", &offset));
	CHECK(rdk_startup_validate(L"", &offset));
	const WCHAR* invalid[] = { L"<", L"<enter", L"<>", L"<F0>", L"<F13>", L"<unknown>",
	    L"<delay:>", L"<delay:1000>", L"\xD800", L"\xDC00", L"\xD800!" };
	for (size_t index = 0; index < ARRAYSIZE(invalid); ++index)
		CHECK(!rdk_startup_validate(invalid[index], &offset) && offset == 0);
	CHECK(!rdk_startup_validate(L"abc<oops>", &offset) && offset == 3);
	rdkKeyboard keyboard;
	rdk_keyboard_init(&keyboard, NULL);
	keyboard.sendScan = record_scan;
	keyboard.sendUnicode = record_unicode;
	const WCHAR* sequence = L"<delay><enter><F1><delete>";
	DWORD delay = 10;
	CHECK(rdk_startup_next(&keyboard, &sequence, &delay) && delay == 1000 && eventCount == 0);
	delay = 10;
	CHECK(rdk_startup_next(&keyboard, &sequence, &delay) && delay == 10 && eventCount == 2);
	CHECK(!events[0].unicode && events[0].code == RDP_SCANCODE_RETURN && events[0].flags == 0);
	CHECK(events[1].flags == KBD_FLAGS_RELEASE);
	CHECK(rdk_startup_next(&keyboard, &sequence, &delay) && eventCount == 4);
	CHECK(events[2].code == RDP_SCANCODE_F1 && events[3].flags == KBD_FLAGS_RELEASE);
	CHECK(rdk_startup_next(&keyboard, &sequence, &delay) && eventCount == 6 && !*sequence);
	CHECK(events[4].flags == KBD_FLAGS_EXTENDED && events[4].code == (RDP_SCANCODE_DELETE & 0xFF));
	CHECK(events[5].flags == (KBD_FLAGS_EXTENDED | KBD_FLAGS_RELEASE));
	CHECK(!keyboard.remoteDown[RDP_SCANCODE_DELETE]);
	CHECK(rdk_startup_next(&keyboard, &sequence, &delay) && eventCount == 6);
	sequence = L"<<\xD83D\xDE00\r\n\t\b";
	while (*sequence) CHECK(rdk_startup_next(&keyboard, &sequence, &delay));
	CHECK(eventCount == 18 && events[6].code == L'<' && events[8].code == 0xD83D && events[10].code == 0xDE00);
	CHECK(events[12].code == RDP_SCANCODE_RETURN && events[14].code == RDP_SCANCODE_TAB && events[16].code == RDP_SCANCODE_BACKSPACE);
	sequence = L"<F1>";
	const WCHAR* before = sequence;
	keyboard.localDown[RDP_SCANCODE_LCONTROL] = TRUE;
	CHECK(!rdk_startup_next(&keyboard, &sequence, &delay) && sequence == before && eventCount == 18);
	keyboard.localDown[RDP_SCANCODE_LCONTROL] = FALSE;
	failAt = eventCount + 2;
	CHECK(!rdk_startup_next(&keyboard, &sequence, &delay) && sequence == before);
	CHECK(keyboard.remoteDown[RDP_SCANCODE_F1]);
	failAt = 0;
	CHECK(rdk_keyboard_release_all(&keyboard) && !keyboard.remoteDown[RDP_SCANCODE_F1]);
	eventCount = 0;
	sequence = L"<DELAY><delay><eSc><f12>";
	delay = 0;
	CHECK(rdk_startup_next(&keyboard, &sequence, &delay) && delay == 1000 && eventCount == 0);
	delay = 0;
	CHECK(rdk_startup_next(&keyboard, &sequence, &delay) && delay == 1000 && eventCount == 0);
	CHECK(rdk_startup_next(&keyboard, &sequence, &delay) && eventCount == 2);
	CHECK(events[0].code == RDP_SCANCODE_ESCAPE && events[1].flags == KBD_FLAGS_RELEASE);
	CHECK(rdk_startup_next(&keyboard, &sequence, &delay) && eventCount == 4);
	CHECK(events[2].code == RDP_SCANCODE_F12 && events[3].flags == KBD_FLAGS_RELEASE);
	sequence = L"<invalid>";
	before = sequence;
	CHECK(!rdk_startup_next(&keyboard, &sequence, &delay) && sequence == before && eventCount == 4);
	const WCHAR* configured = L"<delay><enter>ok";
	rdkStartup startup;
	for (UINT connection = 0; connection < 3; ++connection)
	{
		rdk_startup_init(&startup, configured, TRUE, 500, 20);
		rdk_keyboard_init(&keyboard, NULL);
		keyboard.sendScan = record_scan;
		keyboard.sendUnicode = record_unicode;
		eventCount = 0;
		CHECK(rdk_startup_step(&startup, &keyboard, 9000, FALSE));
		CHECK(!startup.armed && startup.next == configured && !eventCount);
		CHECK(rdk_startup_wait(&startup, 9000, FALSE) == 100);
		CHECK(rdk_startup_step(&startup, &keyboard, 10000, TRUE));
		CHECK(startup.armed && startup.nextAt == 10500 && !startup.started);
		CHECK(rdk_startup_step(&startup, &keyboard, 10499, TRUE) && !eventCount);
		CHECK(rdk_startup_wait(&startup, 10499, TRUE) == 1);
		CHECK(rdk_startup_step(&startup, &keyboard, 10500, TRUE));
		CHECK(startup.started && startup.nextAt == 11500 && !eventCount);
		CHECK(rdk_startup_step(&startup, &keyboard, 11499, TRUE) && !eventCount);
		CHECK(rdk_startup_step(&startup, &keyboard, 11500, TRUE) && eventCount == 2);
		CHECK(events[0].code == RDP_SCANCODE_RETURN && events[1].flags == KBD_FLAGS_RELEASE);
		CHECK(rdk_startup_step(&startup, &keyboard, 12000, FALSE) && eventCount == 2);
		CHECK(rdk_startup_step(&startup, &keyboard, 12000, TRUE) && eventCount == 4);
		CHECK(rdk_startup_step(&startup, &keyboard, 12020, TRUE) && eventCount == 6 && !*startup.next);
		CHECK(events[2].unicode && events[2].code == L'o' && events[4].code == L'k');
		CHECK(rdk_startup_step(&startup, &keyboard, 13000, TRUE) && eventCount == 6);
		CHECK(rdk_keyboard_key(&keyboard, WM_KEYDOWN, VK_CONTROL, ((LPARAM)RDP_SCANCODE_LCONTROL << 16) | 1));
		CHECK(rdk_keyboard_key(&keyboard, WM_KEYDOWN, VK_SHIFT, ((LPARAM)RDP_SCANCODE_LSHIFT << 16) | 1));
		CHECK(rdk_keyboard_key(&keyboard, WM_KEYDOWN, VK_F11, ((LPARAM)RDP_SCANCODE_F11 << 16) | 1));
		CHECK(keyboard.quit && keyboard.reconnect);
	}
	rdk_keyboard_init(&keyboard, NULL);
	keyboard.sendScan = record_scan;
	keyboard.sendUnicode = record_unicode;
	eventCount = 0;
	rdk_startup_init(&startup, L"<F1>", FALSE, 3000, 0);
	CHECK(rdk_startup_step(&startup, &keyboard, 9000, FALSE) && !startup.armed);
	CHECK(rdk_startup_step(&startup, &keyboard, 10000, TRUE) && !eventCount);
	CHECK(rdk_startup_step(&startup, &keyboard, 12999, TRUE) && !eventCount);
	CHECK(rdk_startup_step(&startup, &keyboard, 13000, TRUE) && eventCount == 2);
	CHECK(events[0].unicode && events[0].code == L'<');
	rdk_startup_init(&startup, configured, TRUE, 0, 0);
	CHECK(startup.next == configured && !startup.started && !startup.armed);
	CHECK(rdk_startup_step(&startup, &keyboard, 14000, TRUE) && startup.nextAt == 15000);
	CHECK(rdk_startup_step(&startup, &keyboard, 15000, TRUE) && eventCount == 4);
	rdk_startup_init(&startup, configured, TRUE, 0, 0);
	CHECK(startup.next == configured && !startup.started && !startup.armed);
	CHECK(rdk_startup_step(&startup, &keyboard, 16000, TRUE) && startup.nextAt == 17000 && eventCount == 4);
	puts("Passed startup tokens, fixed delays, Unicode, key pairs, held-key gating, and failure tests");
	return 0;
}