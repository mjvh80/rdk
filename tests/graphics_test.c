#include "rdk_graphics.h"

#include <freerdp/codec/h264.h>
#include <freerdp/freerdp.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

int main(void)
{
	CHECK(strstr(freerdp_get_build_config(), "WITH_GFX_H264=ON"));
	CHECK(strstr(freerdp_get_build_config(), "WITH_FFMPEG=ON"));
	const struct { const char* option; BOOL avc420; BOOL avc444; } cases[] = {
		{ NULL, FALSE, FALSE },
		{ "/network:auto", FALSE, FALSE },
		{ "/gfx:avc444", FALSE, TRUE },
		{ "/gfx:AVC444", FALSE, TRUE },
		{ "/gfx:avc420", TRUE, FALSE },
		{ "/gfx:avc444:off,avc420:off", FALSE, FALSE },
		{ "/gfx:rfx", FALSE, FALSE },
		{ "/gfx:progressive", FALSE, FALSE }
	};
	for (size_t index = 0; index < ARRAYSIZE(cases); ++index)
	{
		rdpSettings* settings = freerdp_settings_new(0);
		CHECK(settings);
		char* arguments[] = { "rdk", "/v:graphics-test.invalid", (char*)cases[index].option };
		CHECK(rdk_graphics_parse(settings, cases[index].option ? 3 : 2, arguments) == 0);
		CHECK(freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline));
		CHECK(freerdp_settings_get_bool(settings, FreeRDP_GfxH264) == cases[index].avc420);
		CHECK(freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444) == cases[index].avc444);
		CHECK(freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444v2) == cases[index].avc444);
		rdpSettings* copy = freerdp_settings_clone(settings);
		CHECK(copy);
		CHECK(freerdp_settings_get_bool(copy, FreeRDP_GfxH264) == cases[index].avc420);
		CHECK(freerdp_settings_get_bool(copy, FreeRDP_GfxAVC444) == cases[index].avc444);
		CHECK(freerdp_settings_get_bool(copy, FreeRDP_GfxAVC444v2) == cases[index].avc444);
		freerdp_settings_free(copy);
		freerdp_settings_free(settings);
	}
	H264_CONTEXT* decoder = h264_context_new(FALSE);
	CHECK(decoder);
	CHECK(h264_context_reset(decoder, 64, 64));
	h264_context_free(decoder);
	puts("Passed AVC build/decoder availability, native graphics options, non-AVC default, and settings clone checks");
	return 0;
}