#ifndef RDK_CAMERA_H
#define RDK_CAMERA_H

#include <freerdp/freerdp.h>

#ifdef __cplusplus
extern "C" {
#endif
BOOL rdk_camera_register(void);
BOOL rdk_camera_list(void);
WCHAR* rdk_camera_snapshot(void);
BOOL rdk_camera_snapshot_contains(const WCHAR* snapshot, const WCHAR* link);
#ifdef __cplusplus
}
#endif

#endif