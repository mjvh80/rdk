#include "camera.h"

BOOL rdk_camera_conversion_supported(FREERDP_VIDEO_FORMAT source, FREERDP_VIDEO_FORMAT destination)
{
	return source == destination && (source == FREERDP_VIDEO_FORMAT_MJPEG ||
	       source == FREERDP_VIDEO_FORMAT_YUYV422 || source == FREERDP_VIDEO_FORMAT_NV12 ||
	       source == FREERDP_VIDEO_FORMAT_YUV420P);
}

FREERDP_VIDEO_FORMAT ecamToVideoFormat(CAM_MEDIA_FORMAT format)
{
	switch (format)
	{
		case CAM_MEDIA_FORMAT_MJPG: return FREERDP_VIDEO_FORMAT_MJPEG;
		case CAM_MEDIA_FORMAT_YUY2: return FREERDP_VIDEO_FORMAT_YUYV422;
		case CAM_MEDIA_FORMAT_NV12: return FREERDP_VIDEO_FORMAT_NV12;
		case CAM_MEDIA_FORMAT_I420: return FREERDP_VIDEO_FORMAT_YUV420P;
		default: return FREERDP_VIDEO_FORMAT_NONE;
	}
}

BOOL ecam_encoder_context_init(CameraDeviceStream* stream)
{
	if (!stream || !stream->currMediaType.Width || !stream->currMediaType.Height ||
	    stream->currMediaType.Width > 1920 || stream->currMediaType.Height > 1080 ||
	    !stream->currMediaType.FrameRateNumerator || !stream->currMediaType.FrameRateDenominator ||
	    stream->currMediaType.Format != streamOutputFormat(stream))
		return FALSE;
	return rdk_camera_conversion_supported(ecamToVideoFormat(streamInputFormat(stream)),
	                                       ecamToVideoFormat(streamOutputFormat(stream)));
}

BOOL ecam_encoder_context_free(CameraDeviceStream* stream)
{
	(void)stream;
	return TRUE;
}

BOOL ecam_encoder_compress(CameraDeviceStream* stream, const BYTE* data, size_t size, wStream* output)
{
	if (!ecam_encoder_context_init(stream) || !data || !size || size > 4 * 1920 * 1080 ||
	    !output || !Stream_EnsureRemainingCapacity(output, size))
		return FALSE;
	Stream_Write(output, data, size);
	return TRUE;
}