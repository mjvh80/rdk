#include "rdk_graphics.h"

#include <freerdp/client/cmdline.h>
#include <stdio.h>
#include <string.h>

int rdk_graphics_parse(rdpSettings* settings, int argc, char** argv)
{
	const int status = freerdp_client_settings_parse_command_line(settings, argc, argv, FALSE);
	if (status != 0)
		return status;
	for (int index = 1; index < argc; ++index)
	{
		const char* option = argv[index];
		if (*option != '/' && *option != '+' && *option != '-')
			continue;
		++option;
		if (*option == '-')
			++option;
		if (_strnicmp(option, "gfx", 3) == 0 &&
		    (option[3] == '\0' || option[3] == ':' || option[3] == '-'))
			return 0;
	}
	if (!freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, FALSE))
		return COMMAND_LINE_ERROR;
	return 0;
}

void rdk_graphics_log_settings(const rdpSettings* settings)
{
	printf("rdk: graphics requested: GFX=%s, AVC420=%s, AVC444=%s, AVC444v2=%s; local display=GDI\n",
	    freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline) ? "on" : "off",
	    freerdp_settings_get_bool(settings, FreeRDP_GfxH264) ? "on" : "off",
	    freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444) ? "on" : "off",
	    freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444v2) ? "on" : "off");
	fflush(stdout);
}