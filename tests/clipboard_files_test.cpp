#include "rdk_clipboard_files.h"
#include <cstdio>
#include <cstring>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

int main()
{
	using namespace rdkClipboardFiles;
	CHECK(validName(L"folder\\report.txt"));
	CHECK(validName(L"r\u00e9sum\u00e9.txt"));
	for (const auto* name : { L"", L"..\\outside", L"C:\\file", L"\\\\server\\file", L"file:stream",
	                         L"dir\\..\\file", L"CON.txt", L"dir\\LPT1", L"name.", L"name ", L"dir//file" })
		CHECK(!validName(name));
	FILEDESCRIPTORW file{};
	wcscpy_s(file.cFileName, L"report.txt");
	file.dwFlags = FD_FILESIZE | FD_ATTRIBUTES;
	file.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
	file.nFileSizeHigh = 2;
	file.nFileSizeLow = 17;
	std::vector<FILEDESCRIPTORW> parsed;
	const auto data = packDescriptors({ file });
	CHECK(parseDescriptors(data, parsed) && parsed.size() == 1 && parsed[0].nFileSizeHigh == 2);
	CHECK(!parseDescriptors(std::vector<BYTE>(data.begin(), data.end() - 1), parsed));
	CHECK(!parseDescriptors(packDescriptors({ file, file }), parsed));
	wcscpy_s(file.cFileName, L"..\\outside");
	CHECK(!parseDescriptors(packDescriptors({ file }), parsed));
	memset(file.cFileName, 'A', sizeof(file.cFileName));
	CHECK(!parseDescriptors(packDescriptors({ file }), parsed));
	puts("Passed clipboard descriptor and path validation tests");
	return 0;
}