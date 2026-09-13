#pragma once

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>

namespace rdkClipboardFiles {
constexpr size_t maxFormats = 128;
constexpr size_t maxFiles = 10000;
constexpr size_t maxData = 16 * 1024 * 1024;
constexpr ULONG blockSize = 1024 * 1024;

bool validName(const std::wstring& name);
bool parseDescriptors(const std::vector<BYTE>& data, std::vector<FILEDESCRIPTORW>& files);
std::vector<BYTE> packDescriptors(const std::vector<FILEDESCRIPTORW>& files);

struct LocalFile
{
	std::wstring path;
	FILEDESCRIPTORW descriptor{};
};
bool collectFiles(const std::vector<std::wstring>& paths, std::vector<LocalFile>& files);
bool readLocalFile(const LocalFile& file, UINT64 offset, ULONG requested, std::vector<BYTE>& data);
}