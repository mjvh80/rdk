#include "rdk_session.h"

rdpContext* rdk_session_recreate(RDP_CLIENT_ENTRY_POINTS* entry, const rdpSettings* settings)
{
	rdpContext* context = freerdp_client_context_new(entry);
	if (context && !freerdp_settings_copy(context->settings, settings))
	{
		freerdp_client_context_free(context);
		context = NULL;
	}
	return context;
}