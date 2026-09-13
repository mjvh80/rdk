#ifndef RDK_STARTUP_H
#define RDK_STARTUP_H

#include "rdk_keyboard.h"

typedef struct
{
	const WCHAR* next;
	BOOL tokens;
	BOOL armed;
	BOOL started;
	UINT64 nextAt;
	DWORD initialDelay;
	DWORD charDelay;
} rdkStartup;

BOOL rdk_startup_validate(const WCHAR* sequence, size_t* errorOffset);
BOOL rdk_startup_next(rdkKeyboard* keyboard, const WCHAR** sequence, DWORD* delay);
void rdk_startup_init(rdkStartup* startup, const WCHAR* sequence, BOOL tokens, DWORD initialDelay, DWORD charDelay);
BOOL rdk_startup_step(rdkStartup* startup, rdkKeyboard* keyboard, UINT64 now, BOOL ready);
DWORD rdk_startup_wait(const rdkStartup* startup, UINT64 now, BOOL ready);
BOOL rdk_startup_keys_released(void);

#endif