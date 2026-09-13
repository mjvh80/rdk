#include "camera.h"
#include <stdio.h>

BOOL rdk_camera_conversion_supported(FREERDP_VIDEO_FORMAT source, FREERDP_VIDEO_FORMAT destination);

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

int main(void)
{
	CHECK(rdk_camera_conversion_supported(FREERDP_VIDEO_FORMAT_MJPEG, FREERDP_VIDEO_FORMAT_MJPEG));
	CHECK(!rdk_camera_conversion_supported(FREERDP_VIDEO_FORMAT_NONE, FREERDP_VIDEO_FORMAT_NONE));
	CHECK(!rdk_camera_conversion_supported(FREERDP_VIDEO_FORMAT_NV12, FREERDP_VIDEO_FORMAT_H264));
	CameraDeviceStream stream = { 0 };
	stream.formats.inputFormat = CAM_MEDIA_FORMAT_YUY2;
	stream.formats.outputFormat = CAM_MEDIA_FORMAT_YUY2;
	stream.currMediaType.Format = CAM_MEDIA_FORMAT_YUY2;
	stream.currMediaType.Width = 640;
	stream.currMediaType.Height = 480;
	stream.currMediaType.FrameRateNumerator = 30;
	stream.currMediaType.FrameRateDenominator = 1;
	CHECK(ecam_encoder_context_init(&stream));
	wStream* output = Stream_New(NULL, 2);
	BYTE sample[] = { 16, 128, 16, 128 };
	CHECK(output);
	CHECK(ecam_encoder_compress(&stream, sample, sizeof(sample), output));
	CHECK(Stream_GetPosition(output) == sizeof(sample));
	CHECK(memcmp(Stream_Buffer(output), sample, sizeof(sample)) == 0);
	stream.currMediaType.FrameRateDenominator = 0;
	CHECK(!ecam_encoder_context_init(&stream));
	stream.currMediaType.FrameRateDenominator = 1;
	stream.formats.outputFormat = CAM_MEDIA_FORMAT_H264;
	CHECK(!ecam_encoder_context_init(&stream));
	Stream_Free(output, TRUE);
	puts("Passed camera format and sample passthrough tests");
	return 0;
}