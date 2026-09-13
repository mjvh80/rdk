#include "../src/rdk_taskbar.cpp"
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

int main(int argc, char** argv)
{
	ComScope com;
	CHECK(com.ready());
	WCHAR value[32768];
	WCHAR executable[32768];
	CHECK(GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable)));
	const auto helper = std::filesystem::path(executable).parent_path() / L"rdk-taskbar.exe";
	std::ifstream image(helper, std::ios::binary);
	IMAGE_DOS_HEADER dos{};
	CHECK(image.read(reinterpret_cast<char*>(&dos), sizeof(dos)));
	CHECK(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= sizeof(dos));
	image.seekg(dos.e_lfanew);
	IMAGE_NT_HEADERS64 headers{};
	CHECK(image.read(reinterpret_cast<char*>(&headers), sizeof(headers)));
	CHECK(headers.Signature == IMAGE_NT_SIGNATURE);
	CHECK(headers.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
	CHECK(headers.OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI);
	const struct { rdkTaskbarAction action; const WCHAR* title; const WCHAR* option; } cases[] = {
		{ RDK_TASKBAR_MINIMIZE, L"Minimize", L"/minimize" },
		{ RDK_TASKBAR_RECONNECT, L"Reconnect", L"/reconnect" }
	};
	for (const auto& test : cases)
	{
		ComPtr<IShellLinkW> link;
		CHECK(SUCCEEDED(makeTask(test.action, &link)));
		CHECK(SUCCEEDED(link->GetPath(value, ARRAYSIZE(value), nullptr, SLGP_RAWPATH)));
		CHECK(_wcsicmp(value, helper.c_str()) == 0);
		CHECK(SUCCEEDED(link->GetArguments(value, ARRAYSIZE(value))));
		const std::wstring arguments = std::wstring(test.option) + L" \"" + executable + L"\"";
		CHECK(arguments == value);
		int show = 0;
		CHECK(SUCCEEDED(link->GetShowCmd(&show)) && show == SW_HIDE);
		ComPtr<IPropertyStore> properties;
		CHECK(SUCCEEDED(link.As(&properties)));
		PROPVARIANT title{};
		CHECK(SUCCEEDED(properties->GetValue(PKEY_Title, &title)));
		CHECK(title.vt == VT_LPWSTR && wcscmp(title.pwszVal, test.title) == 0);
		PropVariantClear(&title);
	}
	CHECK(rdk_taskbar_dispatch(executable, static_cast<rdkTaskbarAction>(0)) == -1);
	if (argc == 2 && strcmp(argv[1], "--publish") == 0)
	{
		WCHAR identity[80];
		swprintf_s(identity, ARRAYSIZE(identity), L"rdk.Test.Minimize.%lu", GetCurrentProcessId());
		ComPtr<ICustomDestinationList> cleanup;
		CHECK(SUCCEEDED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cleanup))));
		const HRESULT published = publishTasks(identity);
		const HRESULT deleted = cleanup->DeleteList(identity);
		fprintf(stdout, "Temporary jump list: publish=0x%08lX, cleanup=0x%08lX\n", published, deleted);
		CHECK(SUCCEEDED(published) && SUCCEEDED(deleted));
	}
	puts("Passed windowless GUI-helper subsystem, task path, credential-free arguments, title, and hidden-launch checks");
	return 0;
}