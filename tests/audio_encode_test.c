#include <freerdp/codec/dsp.h>
#include <stdio.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

static int encode_channels(UINT16 sourceChannels, UINT16 targetChannels)
{
	AUDIO_FORMAT source = { 0 };
	source.wFormatTag = WAVE_FORMAT_PCM;
	source.nChannels = sourceChannels;
	source.nSamplesPerSec = 44100;
	source.wBitsPerSample = 16;
	source.nBlockAlign = 2 * sourceChannels;
	source.nAvgBytesPerSec = source.nSamplesPerSec * source.nBlockAlign;
	AUDIO_FORMAT target = { 0 };
	target.wFormatTag = WAVE_FORMAT_AAC_MS;
	target.nChannels = targetChannels;
	target.nSamplesPerSec = 44100;
	target.nAvgBytesPerSec = 16000;
	target.nBlockAlign = 1;
	target.wBitsPerSample = 16;
	CHECK(freerdp_dsp_supports_format(&target, TRUE));
	FREERDP_DSP_CONTEXT* context = freerdp_dsp_context_new(TRUE);
	CHECK(context);
	CHECK(freerdp_dsp_context_reset(context, &target, 1024));
	wStream* output = Stream_New(NULL, 4096);
	CHECK(output);
	BYTE silence[4096] = { 0 };
	size_t totalBytes = 0;
	for (UINT packet = 0; packet < 8; ++packet)
	{
		Stream_ResetPosition(output);
		CHECK(freerdp_dsp_encode(context, &source, silence, 1024 * source.nBlockAlign, output));
		totalBytes += Stream_GetPosition(output);
	}
	CHECK(totalBytes > 0);
	Stream_Free(output, TRUE);
	freerdp_dsp_context_free(context);
	printf("Passed synthetic AAC encoding %u -> %u channels\n", sourceChannels, targetChannels);
	return 0;
}

int main(void)
{
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	__try
	{
		CHECK(encode_channels(1, 2) == 0);
		CHECK(encode_channels(2, 1) == 0);
		CHECK(encode_channels(1, 1) == 0);
		CHECK(encode_channels(2, 2) == 0);
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		fprintf(stderr, "Audio encoding raised native exception 0x%08lX\n", GetExceptionCode());
		return 1;
	}
}