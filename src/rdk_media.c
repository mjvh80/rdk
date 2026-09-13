#define COBJMACROS
#include "rdk_media.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static const IID rdk_notification_iid = { 0x7991EEC9, 0x7E89, 0x4D85, { 0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0 } };
static const IID rdk_enumerator_iid = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const CLSID rdk_enumerator_clsid = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID rdk_volume_iid = { 0x5CDF2C82, 0x841E, 0x4546, { 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A } };
static const PROPERTYKEY rdk_friendly_name_key = { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };

static BOOL rdk_microphone_report(BOOL queryFormats)
{
	const HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
		return FALSE;
	IMMDeviceEnumerator* enumerator = NULL;
	IMMDeviceCollection* devices = NULL;
	IMMDevice* defaultDevice = NULL;
	WCHAR* defaultId = NULL;
	BOOL ok = FALSE;
	HRESULT result = CoCreateInstance(&rdk_enumerator_clsid, NULL, CLSCTX_INPROC_SERVER,
	                                  &rdk_enumerator_iid, (void**)&enumerator);
	if (FAILED(result))
		goto cleanup;
	result = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture, eCommunications, &defaultDevice);
	if (SUCCEEDED(result))
	{
		result = IMMDevice_GetId(defaultDevice, &defaultId);
		if (FAILED(result)) goto cleanup;
	}
	else if (result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
		goto cleanup;
	printf("rdk: local microphones (metadata only; capture not verified):\n");
	if (!defaultId)
		printf("  No default communications microphone is available.\n");
	result = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eCapture, DEVICE_STATEMASK_ALL, &devices);
	if (FAILED(result)) goto cleanup;
	UINT count = 0;
	result = IMMDeviceCollection_GetCount(devices, &count);
	if (FAILED(result)) goto cleanup;
	for (UINT index = 0; index < count; ++index)
	{
		IMMDevice* device = NULL;
		WCHAR* id = NULL;
		DWORD state = 0;
		result = IMMDeviceCollection_Item(devices, index, &device);
		if (FAILED(result)) goto cleanup;
		result = IMMDevice_GetId(device, &id);
		if (SUCCEEDED(result)) result = IMMDevice_GetState(device, &state);
		if (FAILED(result))
		{
			CoTaskMemFree(id);
			IMMDevice_Release(device);
			goto cleanup;
		}
		IPropertyStore* properties = NULL;
		PROPVARIANT name = { 0 };
		if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &properties)))
		{
			(void)IPropertyStore_GetValue(properties, &rdk_friendly_name_key, &name);
			IPropertyStore_Release(properties);
		}
		const BOOL selected = defaultId && wcscmp(defaultId, id) == 0;
		const WCHAR* label = name.vt == VT_LPWSTR && name.pwszVal ? name.pwszVal : id;
		char utf8[2048];
		if (!WideCharToMultiByte(CP_UTF8, 0, label, -1, utf8, sizeof(utf8), NULL, NULL))
			strcpy_s(utf8, sizeof(utf8), "(name unavailable)");
		printf("  %s [%s]%s\n", utf8,
		       state == DEVICE_STATE_ACTIVE ? "active" : state == DEVICE_STATE_DISABLED ? "disabled" :
		       state == DEVICE_STATE_UNPLUGGED ? "unplugged" : "not present",
		       selected ? " [default communications: selected when /microphone opens]" : "");
		if (selected)
		{
			IAudioEndpointVolume* volume = NULL;
			HRESULT queried = IMMDevice_Activate(device, &rdk_volume_iid, CLSCTX_INPROC_SERVER, NULL, (void**)&volume);
			if (SUCCEEDED(queried))
			{
				BOOL muted = FALSE;
				float level = 0;
				queried = IAudioEndpointVolume_GetMute(volume, &muted);
				if (SUCCEEDED(queried)) queried = IAudioEndpointVolume_GetMasterVolumeLevelScalar(volume, &level);
				if (SUCCEEDED(queried)) printf("    Mute: %s; volume: %.0f%%\n", muted ? "on" : "off", level * 100);
				IAudioEndpointVolume_Release(volume);
			}
			if (FAILED(queried)) printf("    Mute/volume query unavailable (0x%08lX)\n", (ULONG)queried);
		}
		PropVariantClear(&name);
		CoTaskMemFree(id);
		IMMDevice_Release(device);
	}
	if (queryFormats) printf("WinMM recording devices: %u\n", waveInGetNumDevs());
	if (queryFormats && defaultId)
	{
		printf("Default communications microphone, mono 16-bit PCM format queries:\n");
		const DWORD rates[] = { 8000, 16000, 22050, 44100, 48000 };
		for (size_t index = 0; index < ARRAYSIZE(rates); ++index)
		{
			WAVEFORMATEX format = { 0 };
			format.wFormatTag = WAVE_FORMAT_PCM;
			format.nChannels = 1;
			format.nSamplesPerSec = rates[index];
			format.wBitsPerSample = 16;
			format.nBlockAlign = 2;
			format.nAvgBytesPerSec = rates[index] * 2;
			const MMRESULT supported = waveInOpen(NULL, WAVE_MAPPER, &format, 0, 0,
			    WAVE_FORMAT_QUERY | WAVE_MAPPED_DEFAULT_COMMUNICATION_DEVICE);
			printf("  %lu Hz: %s (WinMM %u)\n", rates[index], supported == MMSYSERR_NOERROR ? "supported" : "unavailable", supported);
		}
	}
	if (queryFormats) printf("Format queries do not verify microphone privacy permission or live samples.\n");
	ok = TRUE;
cleanup:
	if (!ok) fprintf(stderr, "rdk: microphone diagnostic failed (0x%08lX)\n", (ULONG)result);
	CoTaskMemFree(defaultId);
	if (defaultDevice) IMMDevice_Release(defaultDevice);
	if (devices) IMMDeviceCollection_Release(devices);
	if (enumerator) IMMDeviceEnumerator_Release(enumerator);
	if (SUCCEEDED(initialized)) CoUninitialize();
	fflush(stdout);
	return ok;
}

BOOL rdk_microphone_list(void)
{
	return rdk_microphone_report(TRUE);
}

BOOL rdk_microphone_status(void)
{
	return rdk_microphone_report(FALSE);
}

BOOL rdk_media_change_init(rdkMediaChange* change, const WCHAR* endpoint)
{
	ZeroMemory(change, sizeof(*change));
	change->baseline = _wcsdup(endpoint);
	change->current = _wcsdup(endpoint);
	change->notified = _wcsdup(endpoint);
	if (change->baseline && change->current && change->notified)
		return TRUE;
	rdk_media_change_free(change);
	return FALSE;
}

BOOL rdk_media_change_update(rdkMediaChange* change, const WCHAR* endpoint, UINT64 now)
{
	if (wcscmp(change->current, endpoint) == 0)
		return TRUE;
	WCHAR* copy = _wcsdup(endpoint);
	if (!copy)
		return FALSE;
	free(change->current);
	change->current = copy;
	change->changedAt = now;
	return TRUE;
}

BOOL rdk_media_change_ready(rdkMediaChange* change, UINT64 now)
{
	if (now - change->changedAt < 1500 || (wcscmp(change->current, change->notified) == 0 &&
	    (!change->invalidated || change->invalidationNotified)))
		return FALSE;
	WCHAR* copy = _wcsdup(change->current);
	if (!copy)
		return FALSE;
	free(change->notified);
	change->notified = copy;
	change->invalidationNotified = change->invalidated;
	return change->invalidated || wcscmp(change->current, change->baseline) != 0;
}

void rdk_media_change_invalidate(rdkMediaChange* change, UINT64 now)
{
	change->invalidated = TRUE;
	change->changedAt = now;
}

void rdk_media_change_free(rdkMediaChange* change)
{
	free(change->baseline);
	free(change->current);
	free(change->notified);
	ZeroMemory(change, sizeof(*change));
}

struct rdk_media_watch
{
	IMMNotificationClient notification;
	LONG references;
	LONG generation;
	LONG observed;
	LONG removed;
	LONG reportGeneration;
	UINT64 reportAt;
	IMMDeviceEnumerator* enumerator;
	rdkMediaChange change;
	BOOL comInitialized;
	BOOL registered;
};

static HRESULT STDMETHODCALLTYPE rdk_notify_query(IMMNotificationClient* client, REFIID iid, void** result)
{
	if (!result)
		return E_POINTER;
	*result = NULL;
	if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &rdk_notification_iid))
		return E_NOINTERFACE;
	*result = client;
	IMMNotificationClient_AddRef(client);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE rdk_notify_addref(IMMNotificationClient* client)
{
	return (ULONG)InterlockedIncrement(&((rdkMediaWatch*)client)->references);
}

static ULONG STDMETHODCALLTYPE rdk_notify_release(IMMNotificationClient* client)
{
	rdkMediaWatch* watch = (rdkMediaWatch*)client;
	const LONG remaining = InterlockedDecrement(&watch->references);
	if (!remaining)
	{
		rdk_media_change_free(&watch->change);
		free(watch);
	}
	return (ULONG)remaining;
}

static HRESULT STDMETHODCALLTYPE rdk_notify_device(IMMNotificationClient* client, LPCWSTR id)
{
	(void)id;
	InterlockedIncrement(&((rdkMediaWatch*)client)->generation);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE rdk_notify_state(IMMNotificationClient* client, LPCWSTR id, DWORD state)
{
	rdkMediaWatch* watch = (rdkMediaWatch*)client;
	if (state != DEVICE_STATE_ACTIVE && id && wcscmp(id, watch->change.baseline) == 0)
		InterlockedExchange(&watch->removed, 1);
	return rdk_notify_device(client, id);
}

static HRESULT STDMETHODCALLTYPE rdk_notify_removed(IMMNotificationClient* client, LPCWSTR id)
{
	return rdk_notify_state(client, id, DEVICE_STATE_NOTPRESENT);
}

static HRESULT STDMETHODCALLTYPE rdk_notify_default(IMMNotificationClient* client, EDataFlow flow,
                                                     ERole role, LPCWSTR id)
{
	if (flow == eCapture && role == eCommunications)
		return rdk_notify_device(client, id);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE rdk_notify_property(IMMNotificationClient* client, LPCWSTR id, const PROPERTYKEY key)
{
	(void)client;
	(void)id;
	(void)key;
	return S_OK;
}

static IMMNotificationClientVtbl rdk_notification_vtable = {
	rdk_notify_query, rdk_notify_addref, rdk_notify_release, rdk_notify_state,
	rdk_notify_device, rdk_notify_removed, rdk_notify_default, rdk_notify_property
};

static HRESULT rdk_default_microphone(rdkMediaWatch* watch, WCHAR** id)
{
	IMMDevice* device = NULL;
	*id = NULL;
	HRESULT result = IMMDeviceEnumerator_GetDefaultAudioEndpoint(watch->enumerator, eCapture,
	                                                            eCommunications, &device);
	if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
		return S_OK;
	if (FAILED(result))
		return result;
	DWORD state = 0;
	result = IMMDevice_GetState(device, &state);
	if (SUCCEEDED(result) && state == DEVICE_STATE_ACTIVE)
		result = IMMDevice_GetId(device, id);
	IMMDevice_Release(device);
	return result;
}

rdkMediaWatch* rdk_media_watch_new(void)
{
	rdkMediaWatch* watch = calloc(1, sizeof(*watch));
	if (!watch)
		return NULL;
	watch->notification.lpVtbl = &rdk_notification_vtable;
	watch->references = 1;
	const HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	watch->comInitialized = SUCCEEDED(initialized);
	if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
		goto fail;
	if (FAILED(CoCreateInstance(&rdk_enumerator_clsid, NULL, CLSCTX_INPROC_SERVER,
	                            &rdk_enumerator_iid, (void**)&watch->enumerator)))
		goto fail;
	WCHAR* id = NULL;
	const HRESULT queried = rdk_default_microphone(watch, &id);
	const BOOL ready = SUCCEEDED(queried) && rdk_media_change_init(&watch->change, id ? id : L"");
	CoTaskMemFree(id);
	if (!ready)
		goto fail;
	if (FAILED(IMMDeviceEnumerator_RegisterEndpointNotificationCallback(watch->enumerator,
	                                                                   &watch->notification)))
		goto fail;
	watch->registered = TRUE;
	watch->reportGeneration = InterlockedIncrement(&watch->generation);
	return watch;
fail:
	rdk_media_watch_free(watch);
	return NULL;
}

BOOL rdk_media_watch_changed(rdkMediaWatch* watch, UINT64 now)
{
	if (!watch)
		return FALSE;
	if (InterlockedExchange(&watch->removed, 0))
	{
		rdk_media_change_invalidate(&watch->change, now);
		printf("rdk: session microphone was removed or became inactive; reconnect may be needed\n");
		fflush(stdout);
	}
	const LONG generation = InterlockedCompareExchange(&watch->generation, 0, 0);
	if (generation != watch->reportGeneration)
	{
		watch->reportGeneration = generation;
		watch->reportAt = now + 1500;
	}
	if (generation != watch->observed)
	{
		WCHAR* id = NULL;
		const HRESULT queried = rdk_default_microphone(watch, &id);
		if (SUCCEEDED(queried) && rdk_media_change_update(&watch->change, id ? id : L"", now))
			watch->observed = generation;
		CoTaskMemFree(id);
	}
	if (watch->reportAt && now >= watch->reportAt)
	{
		watch->reportAt = 0;
		printf("rdk: local audio endpoint configuration changed; refreshing microphone status\n");
		(void)rdk_microphone_status();
	}
	const BOOL changed = rdk_media_change_ready(&watch->change, now);
	if (changed)
	{
		printf("rdk: default communications microphone changed or session device was interrupted; offering reconnect\n");
		fflush(stdout);
	}
	return changed;
}

void rdk_media_watch_free(rdkMediaWatch* watch)
{
	if (!watch)
		return;
	if (watch->registered)
		IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(watch->enumerator, &watch->notification);
	if (watch->enumerator)
		IMMDeviceEnumerator_Release(watch->enumerator);
	if (watch->comInitialized)
		CoUninitialize();
	IMMNotificationClient_Release(&watch->notification);
}