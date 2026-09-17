#ifndef RDK_CRASH_H
#define RDK_CRASH_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

BOOL rdk_crash_init(const WCHAR* directory);
void rdk_crash_enable_dump(void);
void rdk_crash_message(const char* message);
void rdk_crash_stage(const char* stage);
LONG rdk_crash_filter(EXCEPTION_POINTERS* exception);
void rdk_crash_finish(int exitCode);

#endif