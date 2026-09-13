#include "rdk_clipboard_files.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

namespace rdkClipboardFiles {
bool validName(const std::wstring& name)
{
	if (name.empty() || name.size() >= MAX_PATH || name.front() == L'\\' || name.back() == L'\\')
		return false;
	size_t begin = 0;
	while (begin < name.size())
	{
		const size_t end = name.find(L'\\', begin);
		const std::wstring part = name.substr(begin, end == std::wstring::npos ? end : end - begin);
		if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ')
			return false;
		for (WCHAR character : part)
			if (character < 32 || wcschr(L"/:*?\"<>|", character)) return false;
		std::wstring stem = part.substr(0, part.find(L'.'));
		std::transform(stem.begin(), stem.end(), stem.begin(), towupper);
		if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
		    (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
		     stem[3] >= L'0' && stem[3] <= L'9'))
			return false;
		if (end == std::wstring::npos) break;
		begin = end + 1;
	}
	return true;
}

bool parseDescriptors(const std::vector<BYTE>& data, std::vector<FILEDESCRIPTORW>& files)
{
	files.clear();
	if (data.size() < sizeof(UINT32)) return false;
	UINT32 count = 0;
	memcpy(&count, data.data(), sizeof(count));
	if (!count || count > maxFiles || data.size() != sizeof(count) + count * sizeof(FILEDESCRIPTORW))
		return false;
	std::vector<FILEDESCRIPTORW> validated;
	std::set<std::wstring> names;
	for (UINT32 index = 0; index < count; ++index)
	{
		FILEDESCRIPTORW file;
		memcpy(&file, data.data() + sizeof(count) + index * sizeof(file), sizeof(file));
		if (wcsnlen(file.cFileName, MAX_PATH) == MAX_PATH || !validName(file.cFileName)) return false;
		std::wstring key = file.cFileName;
		std::transform(key.begin(), key.end(), key.begin(), towupper);
		if (!names.insert(key).second || (file.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
		file.dwFlags &= FD_ATTRIBUTES | FD_FILESIZE | FD_CREATETIME | FD_ACCESSTIME | FD_WRITESTIME;
		file.dwFlags |= FD_PROGRESSUI;
		validated.push_back(file);
	}
	files = std::move(validated);
	return true;
}

std::vector<BYTE> packDescriptors(const std::vector<FILEDESCRIPTORW>& files)
{
	if (files.empty() || files.size() > maxFiles) return {};
	const UINT32 count = static_cast<UINT32>(files.size());
	std::vector<BYTE> data(sizeof(count) + count * sizeof(FILEDESCRIPTORW));
	memcpy(data.data(), &count, sizeof(count));
	memcpy(data.data() + sizeof(count), files.data(), files.size() * sizeof(FILEDESCRIPTORW));
	return data;
}

static bool addFile(const std::filesystem::path& path, const std::wstring& name, std::vector<LocalFile>& files)
{
	if (files.size() >= maxFiles || !validName(name)) return false;
	WIN32_FILE_ATTRIBUTE_DATA info{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
	    (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
	LocalFile file;
	file.path = path.wstring();
	file.descriptor.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;
	file.descriptor.dwFileAttributes = info.dwFileAttributes;
	file.descriptor.nFileSizeHigh = info.nFileSizeHigh;
	file.descriptor.nFileSizeLow = info.nFileSizeLow;
	file.descriptor.ftLastWriteTime = info.ftLastWriteTime;
	wcscpy_s(file.descriptor.cFileName, name.c_str());
	files.push_back(file);
	if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
	{
		for (const auto& child : std::filesystem::directory_iterator(path))
			if (!addFile(child.path(), name + L"\\" + child.path().filename().wstring(), files)) return false;
	}
	return true;
}

bool collectFiles(const std::vector<std::wstring>& paths, std::vector<LocalFile>& files)
{
	files.clear();
	try
	{
		for (const auto& path : paths)
		{
			const auto absolute = std::filesystem::absolute(path).lexically_normal();
			if (!addFile(absolute, absolute.filename().wstring(), files)) { files.clear(); return false; }
		}
		std::vector<FILEDESCRIPTORW> descriptors;
		for (const auto& file : files) descriptors.push_back(file.descriptor);
		return parseDescriptors(packDescriptors(descriptors), descriptors);
	}
	catch (...) { files.clear(); return false; }
}

bool readLocalFile(const LocalFile& file, UINT64 offset, ULONG requested, std::vector<BYTE>& data)
{
	data.clear();
	if (requested > blockSize || (file.descriptor.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
	HANDLE handle = CreateFileW(file.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return false;
	BY_HANDLE_FILE_INFORMATION info{};
	LARGE_INTEGER position;
	position.QuadPart = static_cast<LONGLONG>(offset);
	bool ok = position.QuadPart >= 0 && GetFileInformationByHandle(handle, &info) &&
	          !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
	          info.nFileSizeHigh == file.descriptor.nFileSizeHigh && info.nFileSizeLow == file.descriptor.nFileSizeLow &&
	          CompareFileTime(&info.ftLastWriteTime, &file.descriptor.ftLastWriteTime) == 0 &&
	          SetFilePointerEx(handle, position, nullptr, FILE_BEGIN);
	if (ok)
	{
		data.resize(requested);
		DWORD read = 0;
		ok = ReadFile(handle, data.data(), requested, &read, nullptr) != FALSE;
		data.resize(read);
	}
	CloseHandle(handle);
	return ok;
}
}