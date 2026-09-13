#ifndef RDK_MEDIA_H
#define RDK_MEDIA_H

#include <winpr/wtypes.h>

typedef struct
{
	WCHAR* baseline;
	WCHAR* current;
	WCHAR* notified;
	UINT64 changedAt;
	BOOL invalidated;
	BOOL invalidationNotified;
} rdkMediaChange;

BOOL rdk_media_change_init(rdkMediaChange* change, const WCHAR* endpoint);
BOOL rdk_media_change_update(rdkMediaChange* change, const WCHAR* endpoint, UINT64 now);
BOOL rdk_media_change_ready(rdkMediaChange* change, UINT64 now);
void rdk_media_change_invalidate(rdkMediaChange* change, UINT64 now);
void rdk_media_change_free(rdkMediaChange* change);

typedef struct rdk_media_watch rdkMediaWatch;
rdkMediaWatch* rdk_media_watch_new(void);
BOOL rdk_media_watch_changed(rdkMediaWatch* watch, UINT64 now);
void rdk_media_watch_free(rdkMediaWatch* watch);
BOOL rdk_microphone_list(void);
BOOL rdk_microphone_status(void);

#endif