#ifndef RDK_KEYBOARD_H
#define RDK_KEYBOARD_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>

#define RDK_KEY_COUNT 512
#define RDK_PENDING_KEYS 128

typedef struct
{
	UINT16 code;
	BOOL down;
} rdkKeyEvent;

typedef struct
{
	rdpInput* input;
	pKeyboardEvent sendScan;
	pUnicodeKeyboardEvent sendUnicode;
	pKeyboardPauseEvent sendPause;
	BOOL localDown[RDK_KEY_COUNT];
	BOOL remoteDown[RDK_KEY_COUNT];
	BOOL suppressed[RDK_KEY_COUNT];
	rdkKeyEvent pending[RDK_PENDING_KEYS];
	size_t pendingCount;
	BOOL altPending;
	BOOL altDigits;
	BOOL altHex;
	BOOL altLeadingZero;
	UINT32 altValue;
	UINT ansiCodePage;
	UINT oemCodePage;
	BOOL quit;
	BOOL reconnect;
	BOOL minimize;
	BOOL menu;
} rdkKeyboard;

BOOL rdk_keyboard_configure(rdpSettings* settings);
void rdk_keyboard_init(rdkKeyboard* keyboard, rdpInput* input);
void rdk_keyboard_layout(rdkKeyboard* keyboard, HKL layout);
BOOL rdk_keyboard_key(rdkKeyboard* keyboard, UINT message, WPARAM vk, LPARAM data);
BOOL rdk_keyboard_flush(rdkKeyboard* keyboard);
BOOL rdk_keyboard_release_all(rdkKeyboard* keyboard);
BOOL rdk_keyboard_ctrl_alt_delete(rdkKeyboard* keyboard);
BOOL rdk_keyboard_unicode(rdkKeyboard* keyboard, UINT32 codepoint);
BOOL rdk_keyboard_tap(rdkKeyboard* keyboard, UINT16 scan);
BOOL rdk_keyboard_text_next(rdkKeyboard* keyboard, const WCHAR** text);
BOOL rdk_keyboard_idle(const rdkKeyboard* keyboard);

#endif