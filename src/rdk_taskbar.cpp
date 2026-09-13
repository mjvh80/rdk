#include "rdk_taskbar.h"
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;
constexpr WCHAR appId[] = L"rdk.Client";

struct ComScope
{
	HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
	bool ready() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};

HRESULT makeTask(rdkTaskbarAction action, IShellLinkW** output)
{
	*output = nullptr;
	if (action != RDK_TASKBAR_MINIMIZE && action != RDK_TASKBAR_RECONNECT) return E_INVALIDARG;
	const bool reconnect = action == RDK_TASKBAR_RECONNECT;
	WCHAR path[32768];
	const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
	if (!length || length >= ARRAYSIZE(path)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	std::wstring helper(path, length);
	const size_t separator = helper.find_last_of(L"\\/");
	if (separator == std::wstring::npos) return E_UNEXPECTED;
	helper.replace(separator + 1, std::wstring::npos, L"rdk-taskbar.exe");
	const DWORD attributes = GetFileAttributesW(helper.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
		return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	const std::wstring arguments = std::wstring(reconnect ? L"/reconnect \"" : L"/minimize \"") + path + L"\"";
	ComPtr<IShellLinkW> link;
	HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
	if (SUCCEEDED(result)) result = link->SetPath(helper.c_str());
	if (SUCCEEDED(result)) result = link->SetArguments(arguments.c_str());
	if (SUCCEEDED(result)) result = link->SetDescription(reconnect ? L"Reconnect rdk using the current settings" :
	    L"Minimize rdk and keep the remote connection open");
	if (SUCCEEDED(result)) result = link->SetIconLocation(path, 0);
	if (SUCCEEDED(result)) result = link->SetShowCmd(SW_HIDE);
	ComPtr<IPropertyStore> properties;
	if (SUCCEEDED(result)) result = link.As(&properties);
	PROPVARIANT title{};
	if (SUCCEEDED(result)) result = InitPropVariantFromString(reconnect ? L"Reconnect" : L"Minimize", &title);
	if (SUCCEEDED(result)) result = properties->SetValue(PKEY_Title, title);
	PropVariantClear(&title);
	if (SUCCEEDED(result)) result = properties->Commit();
	if (SUCCEEDED(result)) *output = link.Detach();
	return result;
}

HRESULT publishTasks(const WCHAR* identity)
{
	ComPtr<ICustomDestinationList> list;
	HRESULT result = CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list));
	if (SUCCEEDED(result)) result = list->SetAppID(identity);
	UINT slots = 0;
	ComPtr<IObjectArray> removed;
	if (SUCCEEDED(result)) result = list->BeginList(&slots, IID_PPV_ARGS(&removed));
	if (FAILED(result)) return result;
	ComPtr<IObjectCollection> tasks;
	result = CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tasks));
	const rdkTaskbarAction actions[] = { RDK_TASKBAR_MINIMIZE, RDK_TASKBAR_RECONNECT };
	for (const auto action : actions)
	{
		ComPtr<IShellLinkW> link;
		if (SUCCEEDED(result)) result = makeTask(action, &link);
		if (SUCCEEDED(result)) result = tasks->AddObject(link.Get());
	}
	ComPtr<IObjectArray> array;
	if (SUCCEEDED(result)) result = tasks.As(&array);
	if (SUCCEEDED(result)) result = list->AddUserTasks(array.Get());
	if (SUCCEEDED(result)) result = list->CommitList();
	if (FAILED(result)) list->AbortList();
	return result;
}

struct TaskbarRequest
{
	WCHAR executable[32768];
	rdkTaskbarAction action;
	int count = 0;
	bool failed = false;
};

BOOL CALLBACK dispatchWindow(HWND window, LPARAM parameter)
{
	WCHAR name[128];
	if (!GetClassNameW(window, name, ARRAYSIZE(name)) || wcscmp(name, L"rdkWindowClass") != 0)
		return TRUE;
	auto* request = reinterpret_cast<TaskbarRequest*>(parameter);
	DWORD processId = 0;
	GetWindowThreadProcessId(window, &processId);
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (!process) return TRUE;
	WCHAR path[32768];
	DWORD length = ARRAYSIZE(path);
	const BOOL queried = QueryFullProcessImageNameW(process, 0, path, &length);
	CloseHandle(process);
	if (!queried || _wcsicmp(path, request->executable) != 0) return TRUE;
	const UINT message = request->action == RDK_TASKBAR_RECONNECT ? RDK_WM_TASKBAR_RECONNECT : WM_SYSCOMMAND;
	const WPARAM command = request->action == RDK_TASKBAR_RECONNECT ? 0 : SC_MINIMIZE;
	if (PostMessageW(window, message, command, 0))
		++request->count;
	else
		request->failed = true;
	return TRUE;
}
}

extern "C" BOOL rdk_taskbar_attach(HWND window)
{
	ComScope com;
	if (!com.ready())
	{
		fprintf(stderr, "rdk: taskbar COM initialization failed (HRESULT=0x%08lX)\n", com.result);
		return FALSE;
	}
	ComPtr<IPropertyStore> properties;
	HRESULT result = SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&properties));
	PROPVARIANT identity{};
	if (SUCCEEDED(result)) result = InitPropVariantFromString(appId, &identity);
	if (SUCCEEDED(result)) result = properties->SetValue(PKEY_AppUserModel_ID, identity);
	PropVariantClear(&identity);
	static bool published = false;
	if (SUCCEEDED(result) && !published)
	{
		result = publishTasks(appId);
		published = SUCCEEDED(result);
		if (published)
		{
			printf("rdk: taskbar Minimize and Reconnect tasks registered\n");
			fflush(stdout);
		}
	}
	if (FAILED(result))
		fprintf(stderr, "rdk: taskbar tasks unavailable (HRESULT=0x%08lX); use Ctrl+Shift+F10/F11\n", result);
	return SUCCEEDED(result);
}

extern "C" int rdk_taskbar_minimize(void)
{
	WCHAR executable[32768];
	const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
	if (!length || length >= ARRAYSIZE(executable)) return -1;
	return rdk_taskbar_minimize_executable(executable);
}

extern "C" int rdk_taskbar_minimize_executable(const WCHAR* executable)
{
	return rdk_taskbar_dispatch(executable, RDK_TASKBAR_MINIMIZE);
}

extern "C" int rdk_taskbar_dispatch(const WCHAR* executable, rdkTaskbarAction action)
{
	if (action != RDK_TASKBAR_MINIMIZE && action != RDK_TASKBAR_RECONNECT) return -1;
	TaskbarRequest request{};
	request.action = action;
	if (!executable || !*executable || wcscpy_s(request.executable, ARRAYSIZE(request.executable), executable) != 0)
		return -1;
	if (!EnumWindows(dispatchWindow, reinterpret_cast<LPARAM>(&request)) || request.failed) return -1;
	return request.count;
}