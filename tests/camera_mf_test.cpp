#include "rdk_camera.h"
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <string>

extern "C" {
#include "camera.h"
BOOL rdk_camera_describe(IMFMediaType* type, CAM_MEDIA_TYPE_DESCRIPTION* description);
}
using Microsoft::WRL::ComPtr;

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return false; \
} } while (0)

static IWTSPlugin* plugin;
struct Listener { IWTSListener iface{}; IWTSListenerCallback* callback; std::string name; };
static std::vector<std::unique_ptr<Listener>> listeners;
static std::vector<BYTE> written;
static CameraDevice* streamingDevice;
static ICamHalSampleCapturedCallback sampleCallback;

static UINT registerPlugin(IDRDYNVC_ENTRY_POINTS*, const char*, IWTSPlugin* value) { plugin = value; return CHANNEL_RC_OK; }
static IWTSPlugin* getPlugin(IDRDYNVC_ENTRY_POINTS*, const char*) { return plugin; }
static UINT createListener(IWTSVirtualChannelManager*, const char* name, ULONG, IWTSListenerCallback* callback, IWTSListener** result)
{
	auto listener = std::make_unique<Listener>();
	listener->name = name;
	listener->callback = callback;
	*result = &listener->iface;
	listeners.push_back(std::move(listener));
	return CHANNEL_RC_OK;
}
static UINT destroyListener(IWTSVirtualChannelManager*, IWTSListener*) { return CHANNEL_RC_OK; }
static UINT32 channelId(IWTSVirtualChannel*) { return 1; }
static UINT channelWrite(IWTSVirtualChannel*, ULONG size, const BYTE* data, void*) { written.assign(data, data + size); return CHANNEL_RC_OK; }
static UINT enumerateFake(ICamHal*, ICamHalEnumCallback callback, CameraPlugin* ecam, GENERIC_CHANNEL_CALLBACK* channel)
{
	return callback(ecam, channel, "synthetic-camera", "Synthetic Camera");
}
static BOOL activateFake(ICamHal*, const char*, CAM_ERROR_CODE* error) { *error = CAM_ERROR_CODE_None; return TRUE; }
static CAM_ERROR_CODE stopFake(ICamHal*, const char*, size_t) { streamingDevice = nullptr; return CAM_ERROR_CODE_None; }
static CAM_ERROR_CODE freeFake(ICamHal*) { return CAM_ERROR_CODE_None; }
static INT16 formatsFake(ICamHal*, const char*, size_t, const CAM_MEDIA_FORMAT_INFO* formats,
                         size_t count, CAM_MEDIA_TYPE_DESCRIPTION* output, size_t* outputCount)
{
	for (size_t index = 0; index < count; ++index)
	{
		if (formats[index].inputFormat == CAM_MEDIA_FORMAT_YUY2 && formats[index].outputFormat == CAM_MEDIA_FORMAT_YUY2)
		{
			*output = { CAM_MEDIA_FORMAT_YUY2, 2, 2, 30, 1, 1, 1, AM_MEDIA_TYPE_DESCRIPTION_FLAG_Invalid };
			*outputCount = 1;
			return static_cast<INT16>(index);
		}
	}
	return -1;
}
static CAM_ERROR_CODE startFake(ICamHal*, CameraDevice* device, size_t, const CAM_MEDIA_TYPE_DESCRIPTION*, ICamHalSampleCapturedCallback callback)
{
	streamingDevice = device;
	sampleCallback = callback;
	return CAM_ERROR_CODE_None;
}
static UINT send(IWTSVirtualChannelCallback* channel, const std::vector<BYTE>& bytes)
{
	wStream* packet = Stream_New(nullptr, bytes.size());
	Stream_Write(packet, bytes.data(), bytes.size());
	Stream_SealLength(packet);
	Stream_SetPosition(packet, 0);
	const UINT result = channel->OnDataReceived(channel, packet);
	Stream_Free(packet, TRUE);
	return result;
}

static bool protocol()
{
	CHECK(rdk_camera_register());
	CHECK(rdk_camera_register());
	const auto provider = freerdp_get_current_addin_provider();
	const auto entry = reinterpret_cast<PDVC_PLUGIN_ENTRY>(provider("rdpecam", nullptr, nullptr, FREERDP_ADDIN_CHANNEL_DYNAMIC));
	CHECK(entry);
	IDRDYNVC_ENTRY_POINTS points{};
	points.GetPlugin = getPlugin;
	points.RegisterPlugin = registerPlugin;
	CHECK(entry(&points) == CHANNEL_RC_OK && plugin);
	auto* ecam = reinterpret_cast<CameraPlugin*>(plugin);
	ICamHal* native = ecam->ihal;
	CHECK(native && native->Enumerate && native->StartStream && native->StopStream);
	CAM_ERROR_CODE error;
	CHECK(!native->Activate(native, "missing-device", &error) && error == CAM_ERROR_CODE_ItemNotFound);
	ICamHal fake{};
	fake.Enumerate = enumerateFake;
	fake.Activate = activateFake;
	fake.Deactivate = activateFake;
	fake.GetMediaTypeDescriptions = formatsFake;
	fake.StartStream = startFake;
	fake.StopStream = stopFake;
	fake.Free = freeFake;
	ecam->ihal = &fake;
	IWTSVirtualChannelManager manager{};
	manager.CreateListener = createListener;
	manager.DestroyListener = destroyListener;
	manager.GetChannelId = channelId;
	CHECK(plugin->Initialize(plugin, &manager) == CHANNEL_RC_OK);
	CHECK(listeners.size() == 1 && listeners[0]->name == "RDCamera_Device_Enumerator");
	IWTSVirtualChannel channel{};
	channel.Write = channelWrite;
	IWTSVirtualChannelCallback* enumerator = nullptr;
	BOOL accepted = TRUE;
	CHECK(listeners[0]->callback->OnNewChannelConnection(listeners[0]->callback, &channel, nullptr, &accepted, &enumerator) == CHANNEL_RC_OK);
	CHECK(enumerator->OnOpen(enumerator) == CHANNEL_RC_OK);
	CHECK(written.size() == 2 && written[1] == CAM_MSG_ID_SelectVersionRequest);
	CHECK(send(enumerator, { 2, CAM_MSG_ID_SelectVersionResponse }) == CHANNEL_RC_OK);
	CHECK(listeners.size() == 2 && written[1] == CAM_MSG_ID_DeviceAddedNotification);
	IWTSVirtualChannelCallback* device = nullptr;
	CHECK(listeners[1]->callback->OnNewChannelConnection(listeners[1]->callback, &channel, nullptr, &accepted, &device) == CHANNEL_RC_OK);
	CHECK(send(device, { 2, CAM_MSG_ID_ActivateDeviceRequest }) == CHANNEL_RC_OK);
	CHECK(written[1] == CAM_MSG_ID_SuccessResponse);
	CHECK(send(device, { 2, CAM_MSG_ID_MediaTypeListRequest, 0 }) == CHANNEL_RC_OK);
	CHECK(written[1] == CAM_MSG_ID_MediaTypeListResponse);
	std::vector<BYTE> start = { 2, CAM_MSG_ID_StartStreamsRequest, 0, CAM_MEDIA_FORMAT_YUY2 };
	for (UINT32 value : { 2u, 2u, 30u, 1u, 1u, 1u })
		for (UINT32 shift = 0; shift < 32; shift += 8) start.push_back(static_cast<BYTE>(value >> shift));
	start.push_back(CAM_MEDIA_TYPE_DESCRIPTION_FLAG_DecodingRequired);
	CHECK(send(device, start) == CHANNEL_RC_OK);
	CHECK(written[1] == CAM_MSG_ID_SuccessResponse && streamingDevice && sampleCallback);
	CHECK(send(device, { 2, CAM_MSG_ID_SampleRequest, 0 }) == CHANNEL_RC_OK);
	BYTE pixels[] = { 16, 128, 20, 128, 24, 128, 28, 128 };
	CHECK(sampleCallback(streamingDevice, 0, pixels, sizeof(pixels)) == CHANNEL_RC_OK);
	CHECK(written[1] == CAM_MSG_ID_SampleResponse);
	CHECK(written.size() >= sizeof(pixels) && memcmp(written.data() + written.size() - sizeof(pixels), pixels, sizeof(pixels)) == 0);
	CHECK(send(device, { 2, CAM_MSG_ID_StopStreamsRequest }) == CHANNEL_RC_OK);
	CHECK(!streamingDevice);
	CHECK(device->OnClose(device) == CHANNEL_RC_OK);
	CHECK(enumerator->OnClose(enumerator) == CHANNEL_RC_OK);
	CHECK(plugin->Terminated(plugin) == CHANNEL_RC_OK);
	plugin = nullptr;
	CHECK(native->Free(native) == CAM_ERROR_CODE_None);
	listeners.clear();
	return true;
}

static bool formats()
{
	CHECK(rdk_camera_snapshot_contains(L"camera-one\ncamera-two\n", L"CAMERA-TWO"));
	CHECK(!rdk_camera_snapshot_contains(L"camera-one\ncamera-two\n", L"camera"));
	CHECK(!rdk_camera_snapshot_contains(L"", L"camera-one"));
	ComPtr<IMFMediaType> type;
	CHECK(SUCCEEDED(MFCreateMediaType(&type)));
	CHECK(SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)));
	CHECK(SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_MJPG)));
	CHECK(SUCCEEDED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, 1280, 720)));
	CHECK(SUCCEEDED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 30, 1)));
	CAM_MEDIA_TYPE_DESCRIPTION description;
	CHECK(rdk_camera_describe(type.Get(), &description));
	CHECK(description.Format == CAM_MEDIA_FORMAT_MJPG && description.Width == 1280 && description.Height == 720);
	CHECK(SUCCEEDED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 60, 1)));
	CHECK(!rdk_camera_describe(type.Get(), &description));
	CHECK(SUCCEEDED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 30, 1)));
	CHECK(SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2)));
	CHECK(SUCCEEDED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, 2560)));
	CHECK(rdk_camera_describe(type.Get(), &description));
	CHECK(SUCCEEDED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, 2600)));
	CHECK(!rdk_camera_describe(type.Get(), &description));
	CHECK(SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)));
	CHECK(!rdk_camera_describe(type.Get(), &description));
	return true;
}

int main()
{
	const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(com)) return 1;
	const bool ok = formats() && protocol();
	CoUninitialize();
	if (ok) puts("Passed Media Foundation format and RDPECAM handshake/frame tests (synthetic camera)");
	return ok ? 0 : 1;
}