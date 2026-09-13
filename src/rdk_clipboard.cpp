#include "rdk_clipboard.h"
#include "rdk_clipboard_files.h"
#include <freerdp/freerdp.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {
using namespace rdkClipboardFiles;
constexpr UINT workMessage = WM_APP + 80;
constexpr UINT stopMessage = WM_APP + 81;

struct Format { UINT local; UINT32 remote; };

UINT descriptorFormat() { return RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW); }
UINT contentsFormat() { return RegisterClipboardFormatW(CFSTR_FILECONTENTS); }
UINT effectFormat() { return RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT); }

bool supportedFormat(UINT format)
{
	if (format == CF_UNICODETEXT || format == CF_TEXT || format == CF_DIB || format == CF_DIBV5) return true;
	for (const auto* name : { L"HTML Format", L"Rich Text Format", L"PNG" })
		if (format == RegisterClipboardFormatW(name)) return true;
	return false;
}

struct State
{
	std::recursive_mutex channelMutex;
	CliprdrClientContext* channel = nullptr;
	std::atomic<UINT64> epoch{ 0 };
	std::atomic<bool> stopped{ false };
	std::atomic<bool> synced{ false };
	std::atomic<bool> filesEnabled{ false };
	UINT32 features = CLIPRDR_FLAG_DEFAULT_MASK;
	std::mutex requestMutex;
	std::mutex responseMutex;
	std::condition_variable responseReady;
	UINT pending = 0;
	UINT32 streamId = 0;
	UINT32 nextStream = 0;
	size_t limit = 0;
	bool completed = false;
	bool success = false;
	bool desynchronized = false;
	std::vector<BYTE> response;
	std::vector<Format> formats;
	HWND window = nullptr;

	template<class Action> UINT send(Action action)
	{
		std::lock_guard<std::recursive_mutex> guard(channelMutex);
		return channel && !stopped ? action(channel) : ERROR_CANCELLED;
	}

	HRESULT request(UINT64 version, UINT32 format, UINT32 index, UINT32 flags, UINT64 offset,
	                ULONG count, std::vector<BYTE>& result)
	{
		std::unique_lock<std::mutex> serial(requestMutex);
		result.clear();
		if (stopped || epoch != version || desynchronized) return E_ABORT;
		const bool file = flags != 0;
		UINT32 id = ++nextStream;
		{
			std::lock_guard<std::mutex> guard(responseMutex);
			pending = file ? 2 : 1;
			streamId = id;
			limit = file ? count : maxData;
			completed = false;
			success = false;
			response.clear();
		}
		const UINT sent = send([&](CliprdrClientContext* channel) {
			if (file)
			{
				CLIPRDR_FILE_CONTENTS_REQUEST request{};
				request.streamId = id;
				request.listIndex = index;
				request.dwFlags = flags;
				request.nPositionLow = static_cast<UINT32>(offset);
				request.nPositionHigh = static_cast<UINT32>(offset >> 32);
				request.cbRequested = count;
				return channel->ClientFileContentsRequest ? channel->ClientFileContentsRequest(channel, &request) : ERROR_NOT_SUPPORTED;
			}
			CLIPRDR_FORMAT_DATA_REQUEST request{};
			request.requestedFormatId = format;
			return channel->ClientFormatDataRequest ? channel->ClientFormatDataRequest(channel, &request) : ERROR_NOT_SUPPORTED;
		});
		std::unique_lock<std::mutex> guard(responseMutex);
		bool received = false;
		if (sent == CHANNEL_RC_OK)
			received = responseReady.wait_for(guard, std::chrono::seconds(15), [&] { return completed || stopped; });
		pending = 0;
		if (!received && !file) desynchronized = true;
		if (stopped || epoch != version) return E_ABORT;
		if (sent != CHANNEL_RC_OK || !received || !success) return STG_E_READFAULT;
		result = std::move(response);
		return S_OK;
	}

	void reply(UINT kind, UINT32 id, bool ok, const BYTE* data, size_t size)
	{
		std::lock_guard<std::mutex> guard(responseMutex);
		if (pending != kind || completed || (kind == 2 && streamId != id)) return;
		success = ok && size <= limit && (!size || data);
		try { if (success && size) response.assign(data, data + size); }
		catch (...) { success = false; }
		completed = true;
		responseReady.notify_all();
	}
};

class RemoteStream final : public IStream
{
	std::atomic<ULONG> references{ 1 };
	std::shared_ptr<State> state;
	UINT64 version;
	UINT32 index;
	FILEDESCRIPTORW file;
	UINT64 position = 0;
	UINT64 size = 0;
	bool sizeKnown;
	HRESULT getSize()
	{
		if (sizeKnown) return S_OK;
		std::vector<BYTE> data;
		const HRESULT result = state->request(version, 0, index, FILECONTENTS_SIZE, 0, 8, data);
		if (FAILED(result)) return result;
		if (data.size() != 8) return STG_E_READFAULT;
		memcpy(&size, data.data(), 8);
		sizeKnown = true;
		return S_OK;
	}
public:
	RemoteStream(std::shared_ptr<State> shared, UINT64 epoch, UINT32 number, const FILEDESCRIPTORW& descriptor)
	    : state(std::move(shared)), version(epoch), index(number), file(descriptor),
	      size((static_cast<UINT64>(file.nFileSizeHigh) << 32) | file.nFileSizeLow),
	      sizeKnown((file.dwFlags & FD_FILESIZE) != 0) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
	{
		if (!object) return E_POINTER;
		*object = nullptr;
		if (iid != IID_IUnknown && iid != IID_ISequentialStream && iid != IID_IStream) return E_NOINTERFACE;
		*object = static_cast<IStream*>(this); AddRef(); return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
	ULONG STDMETHODCALLTYPE Release() override { ULONG left = --references; if (!left) delete this; return left; }
	HRESULT STDMETHODCALLTYPE Read(void* data, ULONG count, ULONG* read) override
	{
		if (read) *read = 0;
		if (!data && count) return STG_E_INVALIDPOINTER;
		if (!count) return S_OK;
		if (state->stopped || state->epoch != version) return E_ABORT;
		HRESULT result = getSize();
		if (FAILED(result)) return result;
		ULONG total = 0;
		while (total < count && position < size)
		{
			const ULONG request = static_cast<ULONG>(std::min<UINT64>({ count - total, blockSize, size - position }));
			std::vector<BYTE> bytes;
			result = state->request(version, 0, index, FILECONTENTS_RANGE, position, request, bytes);
			if (FAILED(result) || bytes.size() != request) return FAILED(result) ? result : STG_E_READFAULT;
			memcpy(static_cast<BYTE*>(data) + total, bytes.data(), bytes.size());
			position += bytes.size();
			total += static_cast<ULONG>(bytes.size());
			if (read) *read = total;
		}
		return total == count ? S_OK : S_FALSE;
	}
	HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
	HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER distance, DWORD origin, ULARGE_INTEGER* result) override
	{
		if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
		if (origin == STREAM_SEEK_END) { HRESULT checked = getSize(); if (FAILED(checked)) return checked; }
		const UINT64 base = origin == STREAM_SEEK_SET ? 0 : (origin == STREAM_SEEK_CUR ? position : size);
		if (distance.QuadPart < 0)
		{
			const UINT64 amount = static_cast<UINT64>(-(distance.QuadPart + 1)) + 1;
			if (amount > base) return STG_E_INVALIDFUNCTION;
			position = base - amount;
		}
		else
		{
			if (base > UINT64_MAX - static_cast<UINT64>(distance.QuadPart)) return STG_E_INVALIDFUNCTION;
			position = base + distance.QuadPart;
		}
		if (result) result->QuadPart = position;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
	HRESULT STDMETHODCALLTYPE CopyTo(IStream* destination, ULARGE_INTEGER count, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override
	{
		if (read) read->QuadPart = 0;
		if (written) written->QuadPart = 0;
		if (!destination) return STG_E_INVALIDPOINTER;
		std::vector<BYTE> buffer(blockSize);
		UINT64 remaining = count.QuadPart;
		while (remaining)
		{
			ULONG received = 0, saved = 0;
			HRESULT result = Read(buffer.data(), static_cast<ULONG>(std::min<UINT64>(remaining, buffer.size())), &received);
			if (read) read->QuadPart += received;
			if (FAILED(result)) return result;
			HRESULT output = destination->Write(buffer.data(), received, &saved);
			if (written) written->QuadPart += saved;
			if (FAILED(output) || saved != received) return FAILED(output) ? output : STG_E_WRITEFAULT;
			remaining -= received;
			if (result == S_FALSE) return S_FALSE;
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
	HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
	HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
	HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override
	{
		if (!stat) return E_POINTER;
		ZeroMemory(stat, sizeof(*stat));
		HRESULT result = getSize(); if (FAILED(result)) return result;
		stat->type = STGTY_STREAM;
		stat->cbSize.QuadPart = size;
		stat->grfMode = STGM_READ;
		stat->mtime = file.ftLastWriteTime;
		if (!(flags & STATFLAG_NONAME))
		{
			const size_t length = (wcslen(file.cFileName) + 1) * sizeof(WCHAR);
			stat->pwcsName = static_cast<WCHAR*>(CoTaskMemAlloc(length));
			if (!stat->pwcsName) return E_OUTOFMEMORY;
			memcpy(stat->pwcsName, file.cFileName, length);
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Clone(IStream** object) override
	{
		if (!object) return E_POINTER;
		auto* clone = new (std::nothrow) RemoteStream(state, version, index, file);
		if (!clone) return E_OUTOFMEMORY;
		clone->position = position; clone->size = size; clone->sizeKnown = sizeKnown;
		*object = clone; return S_OK;
	}
};

HRESULT globalData(const std::vector<BYTE>& data, STGMEDIUM* medium)
{
	HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, std::max<size_t>(data.size(), 1));
	if (!handle) return E_OUTOFMEMORY;
	void* destination = GlobalLock(handle);
	if (!destination) { GlobalFree(handle); return E_OUTOFMEMORY; }
	if (!data.empty()) memcpy(destination, data.data(), data.size());
	GlobalUnlock(handle);
	medium->tymed = TYMED_HGLOBAL; medium->hGlobal = handle; medium->pUnkForRelease = nullptr;
	return S_OK;
}

class RemoteObject final : public IDataObject
{
	std::atomic<ULONG> references{ 1 };
	std::shared_ptr<State> state;
	UINT64 version;
	std::vector<Format> formats;
	std::vector<FILEDESCRIPTORW> files;
	HRESULT descriptors()
	{
		if (!files.empty()) return S_OK;
		for (const auto& format : formats)
		{
			if (format.local != descriptorFormat()) continue;
			std::vector<BYTE> bytes;
			HRESULT result = state->request(version, format.remote, 0, 0, 0, 0, bytes);
			if (FAILED(result)) return result;
			return parseDescriptors(bytes, files) ? S_OK : DV_E_FORMATETC;
		}
		return DV_E_FORMATETC;
	}
public:
	RemoteObject(std::shared_ptr<State> shared, UINT64 epoch, std::vector<Format> supported)
	    : state(std::move(shared)), version(epoch), formats(std::move(supported)) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
	{
		if (!object) return E_POINTER;
		*object = nullptr;
		if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
		*object = static_cast<IDataObject*>(this); AddRef(); return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
	ULONG STDMETHODCALLTYPE Release() override { ULONG left = --references; if (!left) delete this; return left; }
	HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* requested) override
	{
		if (!requested) return E_POINTER;
		if (requested->dwAspect != DVASPECT_CONTENT || requested->ptd) return DV_E_DVASPECT;
		bool filesAvailable = false;
		for (const auto& format : formats)
		{
			filesAvailable = filesAvailable || format.local == descriptorFormat();
			if (format.local == requested->cfFormat && (requested->tymed & TYMED_HGLOBAL) && requested->lindex == -1) return S_OK;
		}
		if (filesAvailable && requested->cfFormat == contentsFormat() && (requested->tymed & TYMED_ISTREAM) && requested->lindex >= -1) return S_OK;
		if (filesAvailable && requested->cfFormat == effectFormat() && (requested->tymed & TYMED_HGLOBAL)) return S_OK;
		return DV_E_FORMATETC;
	}
	HRESULT STDMETHODCALLTYPE GetData(FORMATETC* requested, STGMEDIUM* medium) override
	{
		if (!medium) return E_POINTER;
		ZeroMemory(medium, sizeof(*medium));
		if (state->stopped || state->epoch != version) return E_ABORT;
		HRESULT checked = QueryGetData(requested); if (FAILED(checked)) return checked;
		try
		{
			if (requested->cfFormat == descriptorFormat() || requested->cfFormat == contentsFormat())
			{
				checked = descriptors(); if (FAILED(checked)) return checked;
				if (requested->cfFormat == descriptorFormat()) return globalData(packDescriptors(files), medium);
				if (static_cast<size_t>(requested->lindex) >= files.size() || (files[requested->lindex].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return DV_E_LINDEX;
				medium->pstm = new RemoteStream(state, version, requested->lindex, files[requested->lindex]);
				medium->tymed = TYMED_ISTREAM;
				return S_OK;
			}
			if (requested->cfFormat == effectFormat())
			{
				std::vector<BYTE> data(sizeof(DWORD)); DWORD effect = DROPEFFECT_COPY;
				memcpy(data.data(), &effect, sizeof(effect)); return globalData(data, medium);
			}
			for (const auto& format : formats)
			{
				if (format.local != requested->cfFormat) continue;
				std::vector<BYTE> bytes;
				checked = state->request(version, format.remote, 0, 0, 0, 0, bytes);
				if (FAILED(checked)) return checked;
				if (format.local == CF_UNICODETEXT)
				{
					if (bytes.size() % 2) return DV_E_FORMATETC;
					bytes.push_back(0); bytes.push_back(0);
				}
				else if (format.local == CF_TEXT) bytes.push_back(0);
				return globalData(bytes, medium);
			}
		}
		catch (...) { return E_OUTOFMEMORY; }
		return DV_E_FORMATETC;
	}
	HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }
	HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override { if (output) output->ptd = nullptr; return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** enumerator) override
	{
		if (direction != DATADIR_GET) return E_NOTIMPL;
		std::vector<FORMATETC> entries;
		for (const auto& format : formats)
		{
			entries.push_back({ static_cast<CLIPFORMAT>(format.local), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL });
			if (format.local == descriptorFormat())
			{
				entries.push_back({ static_cast<CLIPFORMAT>(contentsFormat()), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM });
				entries.push_back({ static_cast<CLIPFORMAT>(effectFormat()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL });
			}
		}
		return SHCreateStdEnumFmtEtc(static_cast<UINT>(entries.size()), entries.data(), enumerator);
	}
	HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
	HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
	HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};
}

struct rdk_clipboard
{
	std::shared_ptr<State> state = std::make_shared<State>();
	std::thread worker;
	HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	bool systemClipboard;
	IDataObject* current = nullptr;
	DWORD localSequence = 0;
	std::vector<LocalFile> localFiles;
	std::vector<Format> localFormats;
	std::mutex queueMutex;
	std::vector<std::function<void()>> jobs;

	explicit rdk_clipboard(bool system) : systemClipboard(system) {}
	~rdk_clipboard() { if (ready) CloseHandle(ready); }
	void post(std::function<void()> action)
	{
		std::lock_guard<std::mutex> guard(queueMutex);
		if (state->stopped) return;
		jobs.push_back(std::move(action));
		PostMessageW(state->window, workMessage, 0, 0);
	}
	void drain()
	{
		std::vector<std::function<void()>> pending;
		{ std::lock_guard<std::mutex> guard(queueMutex); pending.swap(jobs); }
		for (auto& job : pending) { if (state->stopped) break; try { job(); } catch (...) {} }
	}
	void announceLocal(bool initial = false)
	{
		if (!systemClipboard || !state->synced ||
		    (current && OleIsCurrentClipboard(current) == S_OK)) return;
		const DWORD sequence = GetClipboardSequenceNumber();
		if ((!initial && sequence == localSequence) || !OpenClipboard(state->window)) return;
		std::vector<UINT> available;
		for (UINT format = EnumClipboardFormats(0); format && available.size() < maxFormats; format = EnumClipboardFormats(format))
		{
			if (!(state->features & CLIPRDR_FLAG_LOCAL_TO_REMOTE)) break;
			if (supportedFormat(format)) available.push_back(format);
			else if (format == CF_HDROP && state->filesEnabled && (state->features & CLIPRDR_FLAG_LOCAL_TO_REMOTE_FILES))
			{
				available.push_back(descriptorFormat());
				available.push_back(contentsFormat());
			}
		}
		CloseClipboard();
		if (sequence != localSequence) ++state->epoch;
		localSequence = sequence;
		localFormats.clear(); localFiles.clear();
		if (current) { current->Release(); current = nullptr; }
		std::vector<CLIPRDR_FORMAT> formats;
		std::vector<std::string> names;
		names.reserve(available.size());
		for (UINT format : available)
		{
			UINT advertised = format;
			localFormats.push_back({ advertised, advertised });
			char name[256]{};
			if (advertised >= 0xC000) GetClipboardFormatNameA(advertised, name, sizeof(name));
			names.emplace_back(name);
			formats.push_back({ advertised, names.back().empty() ? nullptr : names.back().data() });
		}
		CLIPRDR_FORMAT_LIST list{};
		list.numFormats = static_cast<UINT32>(formats.size()); list.formats = formats.data();
		state->send([&](CliprdrClientContext* channel) { return channel->ClientFormatList(channel, &list); });
	}
	void publish(UINT64 version, const std::vector<Format>& formats)
	{
		if (!systemClipboard || state->epoch != version || !(state->features & CLIPRDR_FLAG_REMOTE_TO_LOCAL)) return;
		auto* object = new RemoteObject(state, version, formats);
		if (SUCCEEDED(OleSetClipboard(object)))
		{
			if (current) current->Release();
			current = object;
			localSequence = GetClipboardSequenceNumber();
		}
		else object->Release();
	}
	void localData(UINT32 format)
	{
		std::vector<BYTE> bytes;
		bool ok = systemClipboard && (state->features & CLIPRDR_FLAG_LOCAL_TO_REMOTE) && localSequence == GetClipboardSequenceNumber();
		bool advertised = false;
		for (const auto& entry : localFormats) advertised = advertised || entry.local == format;
		ok = ok && advertised;
		if (ok && OpenClipboard(state->window))
		{
			if (format == descriptorFormat())
			{
				HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
				const UINT count = drop ? DragQueryFileW(drop, UINT_MAX, nullptr, 0) : 0;
				std::vector<std::wstring> paths;
				ok = count > 0 && count <= maxFiles;
				for (UINT index = 0; ok && index < count; ++index)
				{
					const UINT length = DragQueryFileW(drop, index, nullptr, 0);
					std::vector<WCHAR> name(static_cast<size_t>(length) + 1);
					ok = DragQueryFileW(drop, index, name.data(), static_cast<UINT>(name.size())) == length;
					paths.emplace_back(name.data());
				}
				CloseClipboard();
				if (ok) ok = collectFiles(paths, localFiles);
				std::vector<FILEDESCRIPTORW> descriptors;
				for (const auto& file : localFiles) descriptors.push_back(file.descriptor);
				if (ok) bytes = packDescriptors(descriptors);
			}
			else
			{
				HANDLE handle = GetClipboardData(format);
				const SIZE_T size = handle ? GlobalSize(handle) : 0;
				const BYTE* data = handle ? static_cast<const BYTE*>(GlobalLock(handle)) : nullptr;
				ok = data && size <= maxData;
				if (ok) bytes.assign(data, data + size);
				if (data) GlobalUnlock(handle);
				CloseClipboard();
			}
		}
		else ok = false;
		ok = ok && localSequence == GetClipboardSequenceNumber();
		CLIPRDR_FORMAT_DATA_RESPONSE response{};
		response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
		response.common.dataLen = ok ? static_cast<UINT32>(bytes.size()) : 0;
		response.requestedFormatData = ok ? bytes.data() : nullptr;
		state->send([&](CliprdrClientContext* channel) { return channel->ClientFormatDataResponse(channel, &response); });
	}
	void localContents(const CLIPRDR_FILE_CONTENTS_REQUEST& request)
	{
		std::vector<BYTE> bytes;
		bool ok = systemClipboard && (state->features & CLIPRDR_FLAG_LOCAL_TO_REMOTE) &&
		          (state->features & CLIPRDR_FLAG_LOCAL_TO_REMOTE_FILES) && localSequence == GetClipboardSequenceNumber() &&
		          request.listIndex < localFiles.size() && !request.haveClipDataId;
		if (ok)
		{
			const auto& file = localFiles[request.listIndex];
			if (request.dwFlags == FILECONTENTS_SIZE && request.cbRequested == 8)
			{
				const UINT64 size = (static_cast<UINT64>(file.descriptor.nFileSizeHigh) << 32) | file.descriptor.nFileSizeLow;
				bytes.resize(8); memcpy(bytes.data(), &size, 8);
			}
			else if (request.dwFlags == FILECONTENTS_RANGE)
				ok = readLocalFile(file, (static_cast<UINT64>(request.nPositionHigh) << 32) | request.nPositionLow, request.cbRequested, bytes);
			else ok = false;
		}
		CLIPRDR_FILE_CONTENTS_RESPONSE response{};
		response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
		response.streamId = request.streamId;
		response.cbRequested = ok ? static_cast<UINT32>(bytes.size()) : 0;
		response.requestedData = ok ? bytes.data() : nullptr;
		state->send([&](CliprdrClientContext* channel) { return channel->ClientFileContentsResponse(channel, &response); });
	}
	static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		auto* bridge = reinterpret_cast<rdk_clipboard*>(GetWindowLongPtrW(window, GWLP_USERDATA));
		if (message == WM_NCCREATE)
		{
			bridge = static_cast<rdk_clipboard*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
			SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(bridge));
		}
		if (bridge)
		{
			if (message == workMessage) { bridge->drain(); return 0; }
			if (message == WM_CLIPBOARDUPDATE) { try { bridge->announceLocal(); } catch (...) {} return 0; }
			if (message == stopMessage) { PostQuitMessage(0); return 0; }
		}
		return DefWindowProcW(window, message, wParam, lParam);
	}
	void run()
	{
		if (!SetThreadDesktop(desktop) || FAILED(OleInitialize(nullptr))) { SetEvent(ready); return; }
		localSequence = GetClipboardSequenceNumber();
		WNDCLASSW type{};
		type.lpfnWndProc = windowProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"rdkClipboard";
		RegisterClassW(&type);
		state->window = CreateWindowExW(0, type.lpszClassName, L"rdk clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE,
		                               nullptr, type.hInstance, this);
		if (state->window && systemClipboard && !AddClipboardFormatListener(state->window))
		{
			DestroyWindow(state->window); state->window = nullptr;
		}
		SetEvent(ready);
		MSG message;
		while (state->window && GetMessageW(&message, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&message); DispatchMessageW(&message);
		}
		if (systemClipboard && state->window) RemoveClipboardFormatListener(state->window);
		if (current)
		{
			if (OleIsCurrentClipboard(current) == S_OK) OleSetClipboard(nullptr);
			current->Release(); current = nullptr;
		}
		if (state->window) DestroyWindow(state->window);
		OleUninitialize();
	}
};

namespace {
rdkClipboard* bridge(CliprdrClientContext* channel) { return static_cast<rdkClipboard*>(channel->custom); }

UINT serverCapabilities(CliprdrClientContext* channel, const CLIPRDR_CAPABILITIES* capabilities)
{
	auto* clipboard = bridge(channel);
	if (!clipboard) return CHANNEL_RC_OK;
	const BYTE* entry = reinterpret_cast<const BYTE*>(capabilities->capabilitySets);
	for (UINT32 index = 0; entry && index < capabilities->cCapabilitiesSets; ++index)
	{
		const auto* capability = reinterpret_cast<const CLIPRDR_CAPABILITY_SET*>(entry);
		if (capability->capabilitySetLength < sizeof(CLIPRDR_CAPABILITY_SET)) break;
		if (capability->capabilitySetType == CB_CAPSTYPE_GENERAL && capability->capabilitySetLength >= CB_CAPSTYPE_GENERAL_LEN)
			clipboard->state->filesEnabled = (reinterpret_cast<const CLIPRDR_GENERAL_CAPABILITY_SET*>(entry)->generalFlags & CB_STREAM_FILECLIP_ENABLED) != 0;
		entry += capability->capabilitySetLength;
	}
	return CHANNEL_RC_OK;
}

UINT monitorReady(CliprdrClientContext* channel, const CLIPRDR_MONITOR_READY*)
{
	auto* clipboard = bridge(channel);
	if (!clipboard) return CHANNEL_RC_OK;
	CLIPRDR_GENERAL_CAPABILITY_SET general{};
	general.capabilitySetType = CB_CAPSTYPE_GENERAL; general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
	general.version = CB_CAPS_VERSION_2;
	general.generalFlags = CB_USE_LONG_FORMAT_NAMES | CB_STREAM_FILECLIP_ENABLED | CB_FILECLIP_NO_FILE_PATHS | CB_HUGE_FILE_SUPPORT_ENABLED;
	CLIPRDR_CAPABILITIES capabilities{};
	capabilities.cCapabilitiesSets = 1; capabilities.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general);
	const UINT result = clipboard->state->send([&](CliprdrClientContext* context) { return context->ClientCapabilities(context, &capabilities); });
	if (result == CHANNEL_RC_OK) { clipboard->state->synced = true; clipboard->post([clipboard] { clipboard->announceLocal(true); }); }
	return result;
}

UINT serverFormats(CliprdrClientContext* channel, const CLIPRDR_FORMAT_LIST* list)
{
	auto* clipboard = bridge(channel);
	if (!clipboard) return CHANNEL_RC_OK;
	bool ok = list->numFormats <= maxFormats && (!list->numFormats || list->formats);
	std::vector<Format> formats;
	UINT64 version = 0;
	try
	{
		for (UINT32 index = 0; ok && index < list->numFormats; ++index)
		{
			const auto& remote = list->formats[index];
			UINT local = remote.formatId;
			if (remote.formatName)
			{
				WCHAR name[256]{};
				if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, remote.formatName, -1, name, ARRAYSIZE(name))) continue;
				local = RegisterClipboardFormatW(name);
			}
			else if (local >= 0xC000) continue;
			if ((clipboard->state->features & CLIPRDR_FLAG_REMOTE_TO_LOCAL) &&
			    (supportedFormat(local) || (local == descriptorFormat() && clipboard->state->filesEnabled &&
			    (clipboard->state->features & CLIPRDR_FLAG_REMOTE_TO_LOCAL_FILES))))
				formats.push_back({ local, remote.formatId });
		}
		if (ok)
		{
			{ std::lock_guard<std::recursive_mutex> guard(clipboard->state->channelMutex); clipboard->state->formats = formats; version = ++clipboard->state->epoch; }
		}
	}
	catch (...) { ok = false; }
	CLIPRDR_FORMAT_LIST_RESPONSE response{}; response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
	const UINT result = clipboard->state->send([&](CliprdrClientContext* context) { return context->ClientFormatListResponse(context, &response); });
	if (ok && result == CHANNEL_RC_OK)
		clipboard->post([clipboard, version, formats] { clipboard->publish(version, formats); });
	return result;
}

UINT serverData(CliprdrClientContext* channel, const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	if (auto* clipboard = bridge(channel)) clipboard->state->reply(1, 0, response->common.msgFlags == CB_RESPONSE_OK,
	                                                            response->requestedFormatData, response->common.dataLen);
	return CHANNEL_RC_OK;
}
UINT serverContents(CliprdrClientContext* channel, const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
{
	if (auto* clipboard = bridge(channel)) clipboard->state->reply(2, response->streamId, response->common.msgFlags == CB_RESPONSE_OK,
	                                                            response->requestedData, response->cbRequested);
	return CHANNEL_RC_OK;
}
UINT serverDataRequest(CliprdrClientContext* channel, const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
	if (auto* clipboard = bridge(channel)) { const UINT32 format = request->requestedFormatId; clipboard->post([clipboard, format] { clipboard->localData(format); }); }
	return CHANNEL_RC_OK;
}
UINT serverContentsRequest(CliprdrClientContext* channel, const CLIPRDR_FILE_CONTENTS_REQUEST* request)
{
	if (auto* clipboard = bridge(channel)) { const auto copy = *request; clipboard->post([clipboard, copy] { clipboard->localContents(copy); }); }
	return CHANNEL_RC_OK;
}
UINT serverListResponse(CliprdrClientContext*, const CLIPRDR_FORMAT_LIST_RESPONSE*) { return CHANNEL_RC_OK; }
}

extern "C" rdkClipboard* rdk_clipboard_new(CliprdrClientContext* channel, BOOL systemClipboard)
{
	if (!channel || channel->custom) return nullptr;
	try
	{
		auto clipboard = std::make_unique<rdkClipboard>(systemClipboard != FALSE);
		if (!clipboard->ready) return nullptr;
		clipboard->state->channel = channel;
		if (channel->rdpcontext && channel->rdpcontext->settings)
			clipboard->state->features = freerdp_settings_get_uint32(channel->rdpcontext->settings, FreeRDP_ClipboardFeatureMask);
		clipboard->worker = std::thread([instance = clipboard.get()] { instance->run(); });
		WaitForSingleObject(clipboard->ready, INFINITE);
		if (!clipboard->state->window) { clipboard->worker.join(); return nullptr; }
		channel->custom = clipboard.get();
		channel->ServerCapabilities = serverCapabilities;
		channel->MonitorReady = monitorReady;
		channel->ServerFormatList = serverFormats;
		channel->ServerFormatListResponse = serverListResponse;
		channel->ServerFormatDataRequest = serverDataRequest;
		channel->ServerFormatDataResponse = serverData;
		channel->ServerFileContentsRequest = serverContentsRequest;
		channel->ServerFileContentsResponse = serverContents;
		return clipboard.release();
	}
	catch (...) { return nullptr; }
}

extern "C" void rdk_clipboard_free(rdkClipboard* clipboard)
{
	if (!clipboard) return;
	printf("rdk: shutdown: stopping clipboard worker\n");
	fflush(stdout);
	const auto state = clipboard->state;
	{
		std::lock_guard<std::recursive_mutex> guard(state->channelMutex);
		{
			std::lock_guard<std::mutex> responseGuard(state->responseMutex);
			state->stopped = true;
		}
		if (state->channel) state->channel->custom = nullptr;
		state->channel = nullptr;
	}
	state->responseReady.notify_all();
	PostMessageW(state->window, stopMessage, 0, 0);
	if (clipboard->worker.joinable()) clipboard->worker.join();
	delete clipboard;
	printf("rdk: shutdown: clipboard worker stopped\n");
	fflush(stdout);
}

HRESULT rdk_clipboard_remote_object(rdkClipboard* clipboard, IDataObject** object)
{
	if (!clipboard || !object) return E_POINTER;
	*object = nullptr;
	std::lock_guard<std::recursive_mutex> guard(clipboard->state->channelMutex);
	try { *object = new RemoteObject(clipboard->state, clipboard->state->epoch, clipboard->state->formats); return S_OK; }
	catch (...) { return E_OUTOFMEMORY; }
}