#ifndef RDK_TASKBAR_H
#define RDK_TASKBAR_H

#include <windows.h>

#define RDK_WM_TASKBAR_RECONNECT (WM_APP + 90)

typedef enum
{
	RDK_TASKBAR_MINIMIZE = 1,
	RDK_TASKBAR_RECONNECT = 2
} rdkTaskbarAction;

#ifdef __cplusplus
extern "C" {
#endif

BOOL rdk_taskbar_attach(HWND window);
int rdk_taskbar_minimize(void);
int rdk_taskbar_minimize_executable(const WCHAR* executable);
int rdk_taskbar_dispatch(const WCHAR* executable, rdkTaskbarAction action);

#ifdef __cplusplus
}
#endif

#endif
