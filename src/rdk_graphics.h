#ifndef RDK_GRAPHICS_H
#define RDK_GRAPHICS_H

#include <freerdp/settings.h>

int rdk_graphics_parse(rdpSettings* settings, int argc, char** argv);
void rdk_graphics_log_settings(const rdpSettings* settings);

#endif