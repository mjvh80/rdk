#ifndef RDK_SESSION_H
#define RDK_SESSION_H

#include <freerdp/client.h>

rdpContext* rdk_session_recreate(RDP_CLIENT_ENTRY_POINTS* entry, const rdpSettings* settings);

#endif