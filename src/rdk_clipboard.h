#ifndef RDK_CLIPBOARD_H
#define RDK_CLIPBOARD_H

#include <freerdp/client/cliprdr.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct rdk_clipboard rdkClipboard;
rdkClipboard* rdk_clipboard_new(CliprdrClientContext* channel, BOOL systemClipboard);
void rdk_clipboard_free(rdkClipboard* clipboard);
#ifdef __cplusplus
}
struct IDataObject;
HRESULT rdk_clipboard_remote_object(rdkClipboard* clipboard, IDataObject** object);
#endif

#endif