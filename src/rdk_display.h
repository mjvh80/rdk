#ifndef RDK_DISPLAY_H
#define RDK_DISPLAY_H

#include <freerdp/freerdp.h>

BOOL rdk_display_number(const WCHAR* deviceName, UINT32* number);
BOOL rdk_display_apply(rdpSettings* settings, const rdpMonitor* monitors, size_t count,
                       RECT* bounds);
BOOL rdk_configure_display(rdpSettings* settings, RECT* bounds);
BOOL rdk_list_monitors(void);

#endif