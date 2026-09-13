#include "rdk_camera.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "camera.h"
UINT VCAPITYPE rdpecam_DVCPluginEntry(IDRDYNVC_ENTRY_POINTS* entry);
}

using Microsoft::WRL::ComPtr;

namespace {
class ComScope
{
	HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
public:
	~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
	bool ready() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};

class MediaRuntime
{
	HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
public:
	~MediaRuntime() { if (SUCCEEDED(result)) MFShutdown(); }
	bool ready() const { return SUCCEEDED(result); }
};

struct CameraInfo
{
	std::wstring link;
	std::wstring name;
};

bool enumerate(std::vector<CameraInfo>& cameras)
{
	ComPtr<IMFAttributes> attributes;
	if (FAILED(MFCreateAttributes(&attributes, 1)) ||
	    FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
		return false;
	IMFActivate** devices = nullptr;
	UINT32 count = 0;
	if (FAILED(MFEnumDeviceSources(attributes.Get(), &devices, &count)))
		return false;
	for (UINT32 index = 0; index < count; ++index)
	{
		WCHAR* link = nullptr;
		WCHAR* name = nullptr;
		UINT32 length = 0;
		const HRESULT linked = devices[index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &length);
		const HRESULT named = devices[index]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length);
		if (SUCCEEDED(linked) && SUCCEEDED(named))
			cameras.push_back({ link, name });
		CoTaskMemFree(link);
		CoTaskMemFree(name);
		devices[index]->Release();
	}
	CoTaskMemFree(devices);
	std::sort(cameras.begin(), cameras.end(), [](const CameraInfo& left, const CameraInfo& right) {
		return left.link < right.link;
	});
	return true;
}

std::string utf8(const std::wstring& input)
{
	const int length = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (length <= 0)
		return {};
	std::vector<char> buffer(static_cast<size_t>(length));
	WideCharToMultiByte(CP_UTF8, 0, input.c_str(), -1, buffer.data(), length, nullptr, nullptr);
	return buffer.data();
}

CAM_MEDIA_FORMAT formatOf(REFGUID subtype)
{
	if (subtype == MFVideoFormat_MJPG) return CAM_MEDIA_FORMAT_MJPG;
	if (subtype == MFVideoFormat_YUY2) return CAM_MEDIA_FORMAT_YUY2;
	if (subtype == MFVideoFormat_NV12) return CAM_MEDIA_FORMAT_NV12;
	if (subtype == MFVideoFormat_I420 || subtype == MFVideoFormat_IYUV) return CAM_MEDIA_FORMAT_I420;
	return CAM_MEDIA_FORMAT_INVALID;
}

bool describe(IMFMediaType* type, CAM_MEDIA_TYPE_DESCRIPTION& description)
{
	GUID subtype = GUID_NULL;
	if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)))
		return false;
	description = {};
	description.Format = formatOf(subtype);
	if (description.Format == CAM_MEDIA_FORMAT_INVALID ||
	    FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &description.Width, &description.Height)) ||
	    FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &description.FrameRateNumerator, &description.FrameRateDenominator)))
		return false;
	if (!description.Width || !description.Height || description.Width > 1920 || description.Height > 1080 ||
	    !description.FrameRateNumerator || !description.FrameRateDenominator ||
	    static_cast<UINT64>(description.FrameRateNumerator) > static_cast<UINT64>(description.FrameRateDenominator) * 30)
		return false;
	if (description.Format != CAM_MEDIA_FORMAT_MJPG)
	{
		if (description.Width % 2 || description.Height % 2)
			return false;
		UINT32 stride = 0;
		const UINT32 expected = description.Width * (description.Format == CAM_MEDIA_FORMAT_YUY2 ? 2 : 1);
		if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && stride != expected)
			return false;
	}
	UINT32 interlace = MFVideoInterlace_Progressive;
	if (SUCCEEDED(type->GetUINT32(MF_MT_INTERLACE_MODE, &interlace)) && interlace != MFVideoInterlace_Progressive)
		return false;
	if (FAILED(MFGetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO,
	                             &description.PixelAspectRatioNumerator, &description.PixelAspectRatioDenominator)))
	{
		description.PixelAspectRatioNumerator = 1;
		description.PixelAspectRatioDenominator = 1;
	}
	return description.PixelAspectRatioNumerator && description.PixelAspectRatioDenominator;
}

bool sameMode(const CAM_MEDIA_TYPE_DESCRIPTION& first, const CAM_MEDIA_TYPE_DESCRIPTION& second)
{
	return first.Format == second.Format && first.Width == second.Width && first.Height == second.Height &&
	       first.FrameRateNumerator == second.FrameRateNumerator && first.FrameRateDenominator == second.FrameRateDenominator &&
	       first.PixelAspectRatioNumerator == second.PixelAspectRatioNumerator &&
	       first.PixelAspectRatioDenominator == second.PixelAspectRatioDenominator;
}

HRESULT openSource(const std::wstring& link, ComPtr<IMFMediaSource>& source, ComPtr<IMFSourceReader>& reader)
{
	ComPtr<IMFAttributes> attributes;
	HRESULT result = MFCreateAttributes(&attributes, 2);
	if (SUCCEEDED(result)) result = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	if (SUCCEEDED(result)) result = attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
	if (SUCCEEDED(result)) result = MFCreateDeviceSource(attributes.Get(), &source);
	if (SUCCEEDED(result)) result = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
	if (SUCCEEDED(result)) result = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
	if (SUCCEEDED(result)) result = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
	return result;
}

struct Capture
{
	std::atomic<bool> stopping{ false };
	std::mutex mutex;
	ComPtr<IMFMediaSource> source;
	std::thread worker;
	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HRESULT result = E_PENDING;
	~Capture() { stop(); if (ready) CloseHandle(ready); }
	void stop()
	{
		stopping = true;
		ComScope com;
		ComPtr<IMFMediaSource> shutdown;
		{
			std::lock_guard<std::mutex> guard(mutex);
			shutdown = source;
		}
		if (shutdown) shutdown->Shutdown();
		if (worker.joinable()) worker.join();
	}
	void run(std::wstring link, CAM_MEDIA_TYPE_DESCRIPTION wanted, CameraDevice* device,
	         ICamHalSampleCapturedCallback callback)
	{
		ComScope com;
		ComPtr<IMFMediaSource> localSource;
		ComPtr<IMFSourceReader> reader;
		result = com.ready() ? openSource(link, localSource, reader) : E_FAIL;
		if (SUCCEEDED(result))
		{
			result = MF_E_INVALIDMEDIATYPE;
			for (DWORD index = 0; index < 1024; ++index)
			{
				ComPtr<IMFMediaType> type;
				if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &type))) break;
				CAM_MEDIA_TYPE_DESCRIPTION available;
				if (describe(type.Get(), available) && sameMode(available, wanted))
				{
					result = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get());
					break;
				}
			}
		}
		{
			std::lock_guard<std::mutex> guard(mutex);
			source = localSource;
		}
		SetEvent(ready);
		if (FAILED(result))
			fprintf(stderr, "rdk: camera open/format failed (HRESULT=0x%08lX); check Windows camera privacy and device availability\n", result);
		while (SUCCEEDED(result) && !stopping)
		{
			DWORD flags = 0;
			ComPtr<IMFSample> sample;
			const HRESULT read = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, &sample);
			if (stopping) break;
			if (FAILED(read) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)))
			{
				fprintf(stderr, "rdk: camera stream ended or changed (HRESULT=0x%08lX); reopen video or reconnect\n", read);
				break;
			}
			if (!sample) continue;
			ComPtr<IMFMediaBuffer> buffer;
			if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) break;
			BYTE* data = nullptr;
			DWORD length = 0;
			if (FAILED(buffer->Lock(&data, nullptr, &length))) break;
			const size_t pixels = static_cast<size_t>(wanted.Width) * wanted.Height;
			const size_t expected = wanted.Format == CAM_MEDIA_FORMAT_YUY2 ? pixels * 2 : pixels * 3 / 2;
			const bool valid = length > 0 && length <= 4 * 1920 * 1080 &&
			                   (wanted.Format == CAM_MEDIA_FORMAT_MJPG || length == expected);
			const UINT sent = valid ? callback(device, 0, data, length) : ERROR_INVALID_DATA;
			buffer->Unlock();
			if (sent != CHANNEL_RC_OK) break;
		}
		if (localSource) localSource->Shutdown();
		{
			std::lock_guard<std::mutex> guard(mutex);
			source.Reset();
		}
	}
};

struct Device
{
	CameraInfo info;
	std::string id;
	std::vector<CAM_MEDIA_TYPE_DESCRIPTION> modes;
	std::unique_ptr<Capture> capture;
	bool activated = false;
};

struct CameraHal
{
	ICamHal iface{};
	MediaRuntime runtime;
	std::vector<std::unique_ptr<Device>> devices;
	Device* find(const char* id)
	{
		for (auto& device : devices)
			if (device->id == id) return device.get();
		return nullptr;
	}
};

UINT halEnumerate(ICamHal* iface, ICamHalEnumCallback callback, CameraPlugin* plugin, GENERIC_CHANNEL_CALLBACK* channel)
{
	ComScope com;
	auto* hal = reinterpret_cast<CameraHal*>(iface);
	if (hal->devices.empty())
	{
		std::vector<CameraInfo> cameras;
		if (!com.ready() || !enumerate(cameras)) return ERROR_NOT_READY;
		for (const auto& info : cameras)
		{
			auto device = std::make_unique<Device>();
			device->info = info;
			device->id = "rdk-camera-" + std::to_string(hal->devices.size());
			hal->devices.push_back(std::move(device));
		}
	}
	for (const auto& device : hal->devices)
	{
		const std::string name = utf8(device->info.name.substr(0, 64));
		const UINT result = callback(plugin, channel, device->id.c_str(), name.c_str());
		if (result != CHANNEL_RC_OK) return result;
	}
	return CHANNEL_RC_OK;
}

BOOL halActivate(ICamHal* iface, const char* id, CAM_ERROR_CODE* error)
{
	auto* device = reinterpret_cast<CameraHal*>(iface)->find(id);
	*error = device ? CAM_ERROR_CODE_None : CAM_ERROR_CODE_ItemNotFound;
	if (device) device->activated = true;
	return device != nullptr;
}

CAM_ERROR_CODE halStop(ICamHal* iface, const char* id, size_t stream)
{
	auto* device = reinterpret_cast<CameraHal*>(iface)->find(id);
	if (!device) return CAM_ERROR_CODE_ItemNotFound;
	if (stream != 0) return CAM_ERROR_CODE_InvalidStreamNumber;
	const bool wasCapturing = device->capture != nullptr;
	if (wasCapturing)
	{
		printf("rdk: stopping camera capture: %s\n", utf8(device->info.name).c_str());
		fflush(stdout);
	}
	device->capture.reset();
	if (wasCapturing)
	{
		printf("rdk: camera capture stopped: %s\n", utf8(device->info.name).c_str());
		fflush(stdout);
	}
	return CAM_ERROR_CODE_None;
}

BOOL halDeactivate(ICamHal* iface, const char* id, CAM_ERROR_CODE* error)
{
	*error = halStop(iface, id, 0);
	auto* device = reinterpret_cast<CameraHal*>(iface)->find(id);
	if (device) device->activated = false;
	return *error == CAM_ERROR_CODE_None;
}

INT16 halMediaTypes(ICamHal* iface, const char* id, size_t stream, const CAM_MEDIA_FORMAT_INFO* formats,
                   size_t formatCount, CAM_MEDIA_TYPE_DESCRIPTION* output, size_t* count)
{
	const size_t capacity = *count;
	*count = 0;
	auto* device = reinterpret_cast<CameraHal*>(iface)->find(id);
	if (!device || stream != 0) return -1;
	ComScope com;
	ComPtr<IMFMediaSource> source;
	ComPtr<IMFSourceReader> reader;
	const HRESULT opened = com.ready() ? openSource(device->info.link, source, reader) : E_FAIL;
	std::vector<CAM_MEDIA_TYPE_DESCRIPTION> available;
	if (SUCCEEDED(opened))
	{
		for (DWORD index = 0; index < 1024; ++index)
		{
			ComPtr<IMFMediaType> type;
			if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &type))) break;
			CAM_MEDIA_TYPE_DESCRIPTION description;
			if (describe(type.Get(), description)) available.push_back(description);
		}
	}
	if (source) source->Shutdown();
	device->modes.clear();
	for (size_t format = 0; format < formatCount; ++format)
	{
		if (formats[format].inputFormat != formats[format].outputFormat) continue;
		for (const auto& description : available)
		{
			if (description.Format == formats[format].inputFormat && device->modes.size() < capacity)
				device->modes.push_back(description);
		}
		if (!device->modes.empty())
		{
			std::copy(device->modes.begin(), device->modes.end(), output);
			*count = device->modes.size();
			return static_cast<INT16>(format);
		}
	}
	fprintf(stderr, "rdk: camera has no supported native MJPEG/YUV mode up to 1080p30 (HRESULT=0x%08lX)\n", opened);
	return -1;
}

CAM_ERROR_CODE halStart(ICamHal* iface, CameraDevice* camera, size_t stream,
                        const CAM_MEDIA_TYPE_DESCRIPTION* mode, ICamHalSampleCapturedCallback callback)
{
	auto* device = reinterpret_cast<CameraHal*>(iface)->find(camera->deviceId);
	if (!device || !device->activated) return CAM_ERROR_CODE_NotInitialized;
	if (stream != 0) return CAM_ERROR_CODE_InvalidStreamNumber;
	if (device->capture) return CAM_ERROR_CODE_InvalidRequest;
	const bool known = std::any_of(device->modes.begin(), device->modes.end(), [&](const CAM_MEDIA_TYPE_DESCRIPTION& available) {
		return sameMode(available, *mode);
	});
	if (!known || !callback) return CAM_ERROR_CODE_InvalidMediaType;
	try
	{
		auto capture = std::make_unique<Capture>();
		if (!capture->ready) return CAM_ERROR_CODE_OutOfMemory;
		Capture* running = capture.get();
		const auto link = device->info.link;
		const auto wanted = *mode;
		running->worker = std::thread([running, link, wanted, camera, callback] {
			running->run(link, wanted, camera, callback);
		});
		if (WaitForSingleObject(running->ready, INFINITE) != WAIT_OBJECT_0 || FAILED(running->result))
		{
			fprintf(stderr, "rdk: camera capture failed to start: %s (HRESULT=0x%08lX)\n",
			        utf8(device->info.name).c_str(), running->result);
			return CAM_ERROR_CODE_UnexpectedError;
		}
		device->capture = std::move(capture);
		printf("rdk: camera capture started on remote request: %s (%lux%lu)\n",
		       utf8(device->info.name).c_str(), (unsigned long)mode->Width, (unsigned long)mode->Height);
		fflush(stdout);
		return CAM_ERROR_CODE_None;
	}
	catch (...)
	{
		return CAM_ERROR_CODE_OutOfMemory;
	}
}

CAM_ERROR_CODE halFree(ICamHal* iface)
{
	ComScope com;
	delete reinterpret_cast<CameraHal*>(iface);
	return CAM_ERROR_CODE_None;
}

UINT VCAPITYPE cameraEntry(IDRDYNVC_ENTRY_POINTS* entry)
{
	try
	{
		ComScope com;
		if (!com.ready()) return ERROR_NOT_READY;
		auto hal = std::make_unique<CameraHal>();
		if (!hal->runtime.ready()) return ERROR_NOT_SUPPORTED;
		hal->iface.Enumerate = halEnumerate;
		hal->iface.Activate = halActivate;
		hal->iface.Deactivate = halDeactivate;
		hal->iface.GetMediaTypeDescriptions = halMediaTypes;
		hal->iface.StartStream = halStart;
		hal->iface.StopStream = halStop;
		hal->iface.Free = halFree;
		const UINT result = rdpecam_DVCPluginEntry(entry);
		if (result != CHANNEL_RC_OK) return result;
		auto* plugin = reinterpret_cast<CameraPlugin*>(entry->GetPlugin(entry, RDPECAM_CHANNEL_NAME));
		plugin->ihal = &hal.release()->iface;
		return CHANNEL_RC_OK;
	}
	catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}

FREERDP_LOAD_CHANNEL_ADDIN_ENTRY_FN previousProvider = nullptr;
PVIRTUALCHANNELENTRY cameraProvider(LPCSTR name, LPCSTR subsystem, LPCSTR type, DWORD flags)
{
	if (strcmp(name, RDPECAM_CHANNEL_NAME) == 0 && !subsystem)
		return reinterpret_cast<PVIRTUALCHANNELENTRY>(cameraEntry);
	return previousProvider ? previousProvider(name, subsystem, type, flags) : nullptr;
}
}

extern "C" BOOL rdk_camera_register(void)
{
	const auto provider = freerdp_get_current_addin_provider();
	if (provider == cameraProvider) return TRUE;
	previousProvider = provider;
	return freerdp_register_addin_provider(cameraProvider, 0) == 0;
}

extern "C" BOOL rdk_camera_list(void)
{
	try
	{
		ComScope com;
		MediaRuntime runtime;
		std::vector<CameraInfo> cameras;
		if (!com.ready() || !runtime.ready() || !enumerate(cameras)) return FALSE;
		if (cameras.empty()) puts("No local cameras found.");
		for (const auto& camera : cameras) printf("Camera: %s\n", utf8(camera.name).c_str());
		fflush(stdout);
		return TRUE;
	}
	catch (...) { return FALSE; }
}

extern "C" WCHAR* rdk_camera_snapshot(void)
{
	try
	{
		ComScope com;
		MediaRuntime runtime;
		std::vector<CameraInfo> cameras;
		if (!com.ready() || !runtime.ready() || !enumerate(cameras)) return nullptr;
		std::wstring snapshot;
		for (const auto& camera : cameras) snapshot += camera.link + L"\n";
		return _wcsdup(snapshot.c_str());
	}
	catch (...) { return nullptr; }
}

extern "C" BOOL rdk_camera_describe(IMFMediaType* type, CAM_MEDIA_TYPE_DESCRIPTION* description)
{
	return describe(type, *description);
}

extern "C" BOOL rdk_camera_snapshot_contains(const WCHAR* snapshot, const WCHAR* link)
{
	if (!snapshot || !link || !*link) return FALSE;
	const size_t length = wcslen(link);
	for (const WCHAR* current = snapshot; *current; )
	{
		const WCHAR* end = wcschr(current, L'\n');
		const size_t available = end ? static_cast<size_t>(end - current) : wcslen(current);
		if (available == length && _wcsnicmp(current, link, length) == 0) return TRUE;
		if (!end) break;
		current = end + 1;
	}
	return FALSE;
}