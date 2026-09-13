#include "rdk_clipboard.h"
#include "rdk_clipboard_files.h"
#include <freerdp/freerdp.h>
#include <wrl/client.h>
#include <shellapi.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <future>

using Microsoft::WRL::ComPtr;
using namespace rdkClipboardFiles;
#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

struct Server
{
	CliprdrClientContext channel{};
	std::vector<BYTE> descriptors;
	std::vector<BYTE> content;
	bool oversized = false;
	bool failed = false;
	bool silent = false;
	HANDLE requested = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	UINT32 capabilities = 0;
	UINT32 listResult = 0;
	UINT32 dataRequests = 0;
	UINT32 fileRequests = 0;
	HANDLE announced = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	std::vector<UINT32> localFormats;
	HANDLE responded = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	std::vector<BYTE> localData;
	UINT32 responseFlags = 0;
	UINT32 responseStream = 0;
	~Server() { CloseHandle(requested); CloseHandle(announced); CloseHandle(responded); }
	Server()
	{
		channel.handle = this;
		channel.ClientFormatList = [](CliprdrClientContext* channel, const CLIPRDR_FORMAT_LIST* list) -> UINT {
			auto* server = static_cast<Server*>(channel->handle);
			server->localFormats.clear();
			for (UINT32 index = 0; index < list->numFormats; ++index) server->localFormats.push_back(list->formats[index].formatId);
			SetEvent(server->announced); return CHANNEL_RC_OK;
		};
		channel.ClientFormatDataResponse = [](CliprdrClientContext* channel, const CLIPRDR_FORMAT_DATA_RESPONSE* response) -> UINT {
			auto* server = static_cast<Server*>(channel->handle);
			server->localData.clear(); server->responseFlags = response->common.msgFlags;
			if (response->common.dataLen) server->localData.assign(response->requestedFormatData, response->requestedFormatData + response->common.dataLen);
			SetEvent(server->responded); return CHANNEL_RC_OK;
		};
		channel.ClientFileContentsResponse = [](CliprdrClientContext* channel, const CLIPRDR_FILE_CONTENTS_RESPONSE* response) -> UINT {
			auto* server = static_cast<Server*>(channel->handle);
			server->localData.clear(); server->responseFlags = response->common.msgFlags; server->responseStream = response->streamId;
			if (response->cbRequested) server->localData.assign(response->requestedData, response->requestedData + response->cbRequested);
			SetEvent(server->responded); return CHANNEL_RC_OK;
		};
		channel.ClientCapabilities = [](CliprdrClientContext* channel, const CLIPRDR_CAPABILITIES* caps) -> UINT {
			auto* server = static_cast<Server*>(channel->handle);
			server->capabilities = reinterpret_cast<CLIPRDR_GENERAL_CAPABILITY_SET*>(caps->capabilitySets)->generalFlags;
			return CHANNEL_RC_OK;
		};
		channel.ClientFormatListResponse = [](CliprdrClientContext* channel, const CLIPRDR_FORMAT_LIST_RESPONSE* response) -> UINT {
			static_cast<Server*>(channel->handle)->listResult = response->common.msgFlags; return CHANNEL_RC_OK;
		};
		channel.ClientFormatDataRequest = [](CliprdrClientContext* channel, const CLIPRDR_FORMAT_DATA_REQUEST* request) -> UINT {
			auto* server = static_cast<Server*>(channel->handle);
			++server->dataRequests;
			const WCHAR text[] = L"clipboard \u00e5\u00e4\u00f6";
			CLIPRDR_FORMAT_DATA_RESPONSE response{};
			response.common.msgFlags = CB_RESPONSE_OK;
			response.common.dataLen = request->requestedFormatId == CF_UNICODETEXT ? sizeof(text) : static_cast<UINT32>(server->descriptors.size());
			response.requestedFormatData = request->requestedFormatId == CF_UNICODETEXT ? reinterpret_cast<const BYTE*>(text) : server->descriptors.data();
			return channel->ServerFormatDataResponse(channel, &response);
		};
		channel.ClientFileContentsRequest = [](CliprdrClientContext* channel, const CLIPRDR_FILE_CONTENTS_REQUEST* request) -> UINT {
			auto* server = static_cast<Server*>(channel->handle);
			++server->fileRequests;
			SetEvent(server->requested);
			if (server->silent) return CHANNEL_RC_OK;
			const UINT64 offset = (static_cast<UINT64>(request->nPositionHigh) << 32) | request->nPositionLow;
			const UINT64 size = server->content.size();
			CLIPRDR_FILE_CONTENTS_RESPONSE response{};
			response.common.msgFlags = server->failed ? CB_RESPONSE_FAIL : CB_RESPONSE_OK;
			response.streamId = request->streamId;
			if (request->dwFlags == FILECONTENTS_SIZE)
			{
				response.cbRequested = 8; response.requestedData = reinterpret_cast<const BYTE*>(&size);
			}
			else
			{
				response.cbRequested = server->oversized ? request->cbRequested + 1 :
				    static_cast<UINT32>(std::min<UINT64>(request->cbRequested, offset >= size ? 0 : size - offset));
				response.requestedData = server->content.data() + std::min(offset, size);
			}
			return channel->ServerFileContentsResponse(channel, &response);
		};
		FILEDESCRIPTORW file{};
		wcscpy_s(file.cFileName, L"folder\\report.bin");
		file.dwFlags = FD_FILESIZE | FD_ATTRIBUTES;
		file.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
		file.nFileSizeLow = blockSize + 37;
		descriptors = packDescriptors({ file });
		content.resize(file.nFileSizeLow);
		for (size_t index = 0; index < content.size(); ++index) content[index] = static_cast<BYTE>(index % 251);
	}
	bool connect()
	{
		CLIPRDR_GENERAL_CAPABILITY_SET general{ CB_CAPSTYPE_GENERAL, CB_CAPSTYPE_GENERAL_LEN, CB_CAPS_VERSION_2,
		    CB_STREAM_FILECLIP_ENABLED | CB_USE_LONG_FORMAT_NAMES | CB_HUGE_FILE_SUPPORT_ENABLED };
		CLIPRDR_CAPABILITIES caps{}; caps.cCapabilitiesSets = 1; caps.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general);
		CLIPRDR_MONITOR_READY ready{};
		return channel.ServerCapabilities(&channel, &caps) == CHANNEL_RC_OK && channel.MonitorReady(&channel, &ready) == CHANNEL_RC_OK && publish();
	}
	bool publish()
	{
		char name[] = "FileGroupDescriptorW";
		CLIPRDR_FORMAT formats[] = { { 0xD000, name }, { CF_UNICODETEXT, nullptr } };
		CLIPRDR_FORMAT_LIST list{}; list.numFormats = 2; list.formats = formats;
		return channel.ServerFormatList(&channel, &list) == CHANNEL_RC_OK && listResult == CB_RESPONSE_OK;
	}
};

int nativeTest()
{
	HWINSTA originalStation = GetProcessWindowStation();
	HDESK originalDesktop = GetThreadDesktop(GetCurrentThreadId());
	HWINSTA station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
	CHECK(station && SetProcessWindowStation(station));
	HDESK desktop = CreateDesktopW(L"rdkClipboardTest", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
	CHECK(desktop && SetThreadDesktop(desktop));
	CHECK(SUCCEEDED(OleInitialize(nullptr)));
	Server server;
	rdkClipboard* clipboard = rdk_clipboard_new(&server.channel, TRUE);
	CHECK(clipboard && server.connect());
	CHECK(WaitForSingleObject(server.announced, 2000) == WAIT_OBJECT_0);
	HWND window = FindWindowExW(HWND_MESSAGE, nullptr, L"rdkClipboard", nullptr);
	CHECK(window);
	DWORD_PTR result = 0;
	CHECK(SendMessageTimeoutW(window, WM_APP + 80, 0, 0, SMTO_ABORTIFHUNG, 3000, &result));
	ComPtr<IDataObject> object;
	CHECK(SUCCEEDED(OleGetClipboard(&object)));
	FORMATETC descriptor{ static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	STGMEDIUM medium{};
	CHECK(SUCCEEDED(object->GetData(&descriptor, &medium)));
	CHECK(static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal))->cItems == 1);
	GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium);
	FORMATETC contents{ static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS)), nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM };
	CHECK(SUCCEEDED(object->GetData(&contents, &medium)));
	ComPtr<IStream> stream; stream.Attach(medium.pstm); medium = {};
	std::vector<BYTE> bytes(server.content.size());
	ULONG read = 0;
	CHECK(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read) == S_OK);
	CHECK(bytes == server.content);
	stream.Reset(); object.Reset();
	WCHAR temp[MAX_PATH], path[MAX_PATH];
	CHECK(GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"rdk", 0, path));
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	CHECK(file != INVALID_HANDLE_VALUE);
	DWORD written = 0;
	CHECK(WriteFile(file, server.content.data(), static_cast<DWORD>(server.content.size()), &written, nullptr));
	CHECK(CloseHandle(file));
	HWND owner = CreateWindowExW(0, L"STATIC", L"clipboard test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
	CHECK(owner && OpenClipboard(owner) && EmptyClipboard());
	const size_t length = (wcslen(path) + 2) * sizeof(WCHAR);
	HGLOBAL dropHandle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + length);
	auto* drop = static_cast<DROPFILES*>(GlobalLock(dropHandle));
	CHECK(drop); drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
	memcpy(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES), path, length - sizeof(WCHAR));
	GlobalUnlock(dropHandle); ResetEvent(server.announced);
	CHECK(SetClipboardData(CF_HDROP, dropHandle) && CloseClipboard());
	CHECK(WaitForSingleObject(server.announced, 2000) == WAIT_OBJECT_0);
	CHECK(std::find(server.localFormats.begin(), server.localFormats.end(), descriptor.cfFormat) != server.localFormats.end());
	CHECK(std::find(server.localFormats.begin(), server.localFormats.end(), contents.cfFormat) != server.localFormats.end());
	CLIPRDR_FORMAT_DATA_REQUEST request{}; request.requestedFormatId = descriptor.cfFormat;
	CHECK(server.channel.ServerFormatDataRequest(&server.channel, &request) == CHANNEL_RC_OK);
	CHECK(WaitForSingleObject(server.responded, 2000) == WAIT_OBJECT_0 && server.responseFlags == CB_RESPONSE_OK);
	std::vector<FILEDESCRIPTORW> files;
	CHECK(parseDescriptors(server.localData, files) && files.size() == 1 && files[0].nFileSizeLow == server.content.size());
	CLIPRDR_FILE_CONTENTS_REQUEST range{};
	range.streamId = 91; range.dwFlags = FILECONTENTS_RANGE; range.nPositionLow = blockSize; range.cbRequested = 100;
	ResetEvent(server.responded);
	CHECK(server.channel.ServerFileContentsRequest(&server.channel, &range) == CHANNEL_RC_OK);
	CHECK(WaitForSingleObject(server.responded, 2000) == WAIT_OBJECT_0 && server.responseFlags == CB_RESPONSE_OK && server.responseStream == 91);
	CHECK(server.localData == std::vector<BYTE>(server.content.begin() + blockSize, server.content.end()));
	range.cbRequested = blockSize + 1; ResetEvent(server.responded);
	CHECK(server.channel.ServerFileContentsRequest(&server.channel, &range) == CHANNEL_RC_OK);
	CHECK(WaitForSingleObject(server.responded, 2000) == WAIT_OBJECT_0 && server.responseFlags == CB_RESPONSE_FAIL);
	rdk_clipboard_free(clipboard);
	CHECK(DeleteFileW(path));
	DestroyWindow(owner);
	OleUninitialize();
	CHECK(SetThreadDesktop(originalDesktop));
	CHECK(SetProcessWindowStation(originalStation));
	CloseDesktop(desktop); CloseWindowStation(station);
	puts("Passed isolated native clipboard publication, cross-apartment file reads, and local file export");
	return 0;
}

int main(int argc, char** argv)
{
	if (argc == 2 && strcmp(argv[1], "--native") == 0) return nativeTest();
	CHECK(SUCCEEDED(OleInitialize(nullptr)));
	const DWORD sequence = GetClipboardSequenceNumber();
	Server server;
	rdkClipboard* clipboard = rdk_clipboard_new(&server.channel, FALSE);
	CHECK(clipboard && server.connect());
	CHECK(server.capabilities & CB_STREAM_FILECLIP_ENABLED);
	CHECK(!(server.capabilities & CB_CAN_LOCK_CLIPDATA));
	ComPtr<IDataObject> object;
	CHECK(SUCCEEDED(rdk_clipboard_remote_object(clipboard, &object)));
	ComPtr<IEnumFORMATETC> enumerator;
	CHECK(SUCCEEDED(object->EnumFormatEtc(DATADIR_GET, &enumerator)));
	FORMATETC text{ CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	STGMEDIUM medium{};
	CHECK(SUCCEEDED(object->GetData(&text, &medium)));
	CHECK(wcscmp(static_cast<WCHAR*>(GlobalLock(medium.hGlobal)), L"clipboard \u00e5\u00e4\u00f6") == 0);
	GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium);
	FORMATETC descriptor{ static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	CHECK(SUCCEEDED(object->GetData(&descriptor, &medium)));
	CHECK(static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal))->cItems == 1);
	GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium);
	CHECK(SUCCEEDED(object->GetData(&descriptor, &medium)));
	ReleaseStgMedium(&medium);
	CHECK(server.dataRequests == 2);
	FORMATETC contents{ static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS)), nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM };
	CHECK(SUCCEEDED(object->GetData(&contents, &medium)));
	ComPtr<IStream> stream; stream.Attach(medium.pstm); medium = {};
	std::vector<BYTE> bytes(server.content.size() + 10);
	ULONG received = 0;
	CHECK(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &received) == S_FALSE);
	CHECK(received == server.content.size() && server.fileRequests == 2);
	CHECK(memcmp(bytes.data(), server.content.data(), received) == 0);
	CHECK(stream->Read(bytes.data(), 1, &received) == S_FALSE && received == 0);
	LARGE_INTEGER offset{}; offset.QuadPart = -7;
	CHECK(SUCCEEDED(stream->Seek(offset, STREAM_SEEK_END, nullptr)));
	CHECK(stream->Read(bytes.data(), 7, &received) == S_OK && received == 7);
	CHECK(memcmp(bytes.data(), server.content.data() + server.content.size() - 7, 7) == 0);
	ComPtr<IStream> clone;
	CHECK(SUCCEEDED(stream->Clone(&clone)));
	STATSTG stat{};
	CHECK(SUCCEEDED(clone->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart == server.content.size());
	offset.QuadPart = 0;
	CHECK(SUCCEEDED(stream->Seek(offset, STREAM_SEEK_SET, nullptr)));
	server.oversized = true;
	CHECK(FAILED(stream->Read(bytes.data(), 1, &received)) && received == 0);
	server.oversized = false; server.failed = true;
	CHECK(FAILED(stream->Read(bytes.data(), 1, &received)) && received == 0);
	server.failed = false;
	CHECK(server.publish());
	CHECK(stream->Read(bytes.data(), 1, &received) == E_ABORT);
	object.Reset(); stream.Reset(); clone.Reset();
	CHECK(SUCCEEDED(rdk_clipboard_remote_object(clipboard, &object)));
	CHECK(SUCCEEDED(object->GetData(&contents, &medium)));
	stream.Attach(medium.pstm); medium = {};
	server.silent = true; ResetEvent(server.requested);
	auto reading = std::async(std::launch::async, [&] { ULONG count = 0; BYTE value; return stream->Read(&value, 1, &count); });
	CHECK(WaitForSingleObject(server.requested, 2000) == WAIT_OBJECT_0);
	rdk_clipboard_free(clipboard);
	CHECK(reading.wait_for(std::chrono::seconds(2)) == std::future_status::ready && reading.get() == E_ABORT);
	CHECK(server.channel.custom == nullptr);
	CHECK(object->GetData(&text, &medium) == E_ABORT);
	object.Reset(); stream.Reset();
	for (size_t attempt = 0; attempt < 32; ++attempt)
	{
		clipboard = rdk_clipboard_new(&server.channel, FALSE);
		CHECK(clipboard && server.connect());
		CHECK(SUCCEEDED(rdk_clipboard_remote_object(clipboard, &object)));
		CHECK(SUCCEEDED(object->GetData(&contents, &medium)));
		stream.Attach(medium.pstm); medium = {};
		ResetEvent(server.requested);
		std::array<std::future<HRESULT>, 4> readers;
		for (auto& reader : readers)
		{
			ComPtr<IStream> pending;
			CHECK(SUCCEEDED(stream->Clone(&pending)));
			reader = std::async(std::launch::async, [pending] {
				ULONG count = 0;
				BYTE value = 0;
				return pending->Read(&value, 1, &count);
			});
		}
		CHECK(WaitForSingleObject(server.requested, 2000) == WAIT_OBJECT_0);
		const UINT64 closingAt = GetTickCount64();
		rdk_clipboard_free(clipboard);
		for (auto& reader : readers)
			CHECK(reader.wait_for(std::chrono::seconds(2)) == std::future_status::ready && reader.get() == E_ABORT);
		CHECK(GetTickCount64() - closingAt < 2000);
		CHECK(server.channel.custom == nullptr);
		CHECK(stream->Read(bytes.data(), 1, &received) == E_ABORT);
		object.Reset(); stream.Reset();
	}
	rdpContext context{};
	context.settings = freerdp_settings_new(0);
	CHECK(context.settings);
	server.channel.rdpcontext = &context;
	CHECK(freerdp_settings_set_uint32(context.settings, FreeRDP_ClipboardFeatureMask, CLIPRDR_FLAG_REMOTE_TO_LOCAL));
	clipboard = rdk_clipboard_new(&server.channel, FALSE);
	CHECK(clipboard && server.connect());
	CHECK(SUCCEEDED(rdk_clipboard_remote_object(clipboard, &object)));
	CHECK(object->QueryGetData(&text) == S_OK);
	CHECK(object->QueryGetData(&descriptor) == DV_E_FORMATETC);
	rdk_clipboard_free(clipboard); object.Reset();
	CHECK(freerdp_settings_set_uint32(context.settings, FreeRDP_ClipboardFeatureMask, CLIPRDR_FLAG_LOCAL_TO_REMOTE | CLIPRDR_FLAG_LOCAL_TO_REMOTE_FILES));
	clipboard = rdk_clipboard_new(&server.channel, FALSE);
	CHECK(clipboard && server.connect());
	CHECK(SUCCEEDED(rdk_clipboard_remote_object(clipboard, &object)));
	CHECK(object->QueryGetData(&text) == DV_E_FORMATETC);
	CHECK(object->QueryGetData(&descriptor) == DV_E_FORMATETC);
	rdk_clipboard_free(clipboard); object.Reset();
	freerdp_settings_free(context.settings);
	CHECK(GetClipboardSequenceNumber() == sequence);
	OleUninitialize();
	puts("Passed clipboard OLE text, file stream, seek, bounds, replacement, and disconnect tests");
	return 0;
}