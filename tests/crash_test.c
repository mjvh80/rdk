#include "rdk_crash.h"
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s (error=%lu)\n", __func__, __LINE__, #expression, GetLastError()); return 1; \
} } while (0)

static DWORD WINAPI crash_worker(LPVOID parameter)
{
	(void)parameter;
	RaiseException(0xE0424242, EXCEPTION_NONCONTINUABLE, 0, NULL);
	return 0;
}

int wmain(int argc, WCHAR** argv)
{
	if (argc == 4 && wcscmp(argv[1], L"--child") == 0)
	{
		SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
		if (wcscmp(argv[3], L"unwritable") == 0)
		{
			CHECK(!rdk_crash_init(argv[0]));
			rdk_crash_message("report unavailable");
			rdk_crash_finish(0);
			return 0;
		}
		CHECK(rdk_crash_init(argv[2]));
		rdk_crash_stage("synthetic-worker");
		if (wcscmp(argv[3], L"bad-stack") == 0)
		{
			CONTEXT context;
			RtlCaptureContext(&context);
			context.Rsp = 1;
			EXCEPTION_RECORD record = { 0 };
			record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
			record.ExceptionAddress = (PVOID)context.Rip;
			EXCEPTION_POINTERS exception = { &record, &context };
			CHECK(rdk_crash_filter(&exception) == EXCEPTION_EXECUTE_HANDLER);
			TerminateProcess(GetCurrentProcess(), EXCEPTION_ACCESS_VIOLATION);
			return 2;
		}
		if (wcscmp(argv[3], L"dump") == 0)
			rdk_crash_enable_dump();
		if (wcscmp(argv[3], L"normal") == 0)
		{
			rdk_crash_message("rdk: exiting: synthetic failure (exit=7)");
			rdk_crash_finish(7);
			return 7;
		}
		if (wcscmp(argv[3], L"main") == 0)
		{
			__try
			{
				const ULONG_PTR details[] = { 1, 0 };
				RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, ARRAYSIZE(details), details);
			}
			__except (rdk_crash_filter(GetExceptionInformation()))
			{
				TerminateProcess(GetCurrentProcess(), GetExceptionCode());
			}
			return 2;
		}
		HANDLE thread = CreateThread(NULL, 0, crash_worker, NULL, 0, NULL);
		CHECK(thread);
		WaitForSingleObject(thread, INFINITE);
		return 2;
	}
	WCHAR temporary[MAX_PATH];
	CHECK(GetTempPathW(ARRAYSIZE(temporary), temporary));
	WCHAR directory[MAX_PATH];
	CHECK(swprintf_s(directory, ARRAYSIZE(directory), L"%lsrdk-crash-test-%lu-%llu", temporary,
	    GetCurrentProcessId(), (unsigned long long)GetTickCount64()) > 0);
	CHECK(CreateDirectoryW(directory, NULL));
	WCHAR executable[32768];
	CHECK(GetModuleFileNameW(NULL, executable, ARRAYSIZE(executable)));
	const WCHAR* modes[] = { L"report", L"dump", L"normal", L"main", L"unwritable", L"bad-stack" };
	for (size_t index = 0; index < ARRAYSIZE(modes); ++index)
	{
		WCHAR command[32768];
		CHECK(swprintf_s(command, ARRAYSIZE(command), L"\"%ls\" --child \"%ls\" %ls", executable, directory, modes[index]) > 0);
		STARTUPINFOW startup = { 0 };
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process = { 0 };
		CHECK(CreateProcessW(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
		CloseHandle(process.hThread);
		const DWORD wait = WaitForSingleObject(process.hProcess, 15000);
		if (wait != WAIT_OBJECT_0)
			TerminateProcess(process.hProcess, 1);
		CHECK(wait == WAIT_OBJECT_0);
		DWORD exitCode = 0;
		CHECK(GetExitCodeProcess(process.hProcess, &exitCode));
		CloseHandle(process.hProcess);
		CHECK(exitCode == (index == 2 ? 7 : (index == 3 || index == 5) ? EXCEPTION_ACCESS_VIOLATION : index == 4 ? 0 : 0xE0424242));
		if (index == 4)
			continue;
		WCHAR pattern[MAX_PATH];
		CHECK(swprintf_s(pattern, ARRAYSIZE(pattern), L"%ls\\*", directory) > 0);
		WIN32_FIND_DATAW entry;
		HANDLE search = FindFirstFileW(pattern, &entry);
		CHECK(search != INVALID_HANDLE_VALUE);
		UINT reports = 0;
		UINT dumps = 0;
		do
		{
			if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			WCHAR path[MAX_PATH];
			CHECK(swprintf_s(path, ARRAYSIZE(path), L"%ls\\%ls", directory, entry.cFileName) > 0);
			HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
			CHECK(file != INVALID_HANDLE_VALUE);
			char content[8192] = { 0 };
			DWORD read = 0;
			CHECK(ReadFile(file, content, sizeof(content) - 1, &read, NULL));
			if (wcsstr(entry.cFileName, L".dmp"))
			{
				HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
				CHECK(mapping);
				void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
				CHECK(view);
				void* stream = NULL;
				ULONG length = 0;
				CHECK(MiniDumpReadDumpStream(view, ExceptionStream, NULL, &stream, &length));
				CHECK(length >= sizeof(MINIDUMP_EXCEPTION_STREAM));
				CHECK(((MINIDUMP_EXCEPTION_STREAM*)stream)->ExceptionRecord.ExceptionCode == 0xE0424242);
				UnmapViewOfFile(view);
				CloseHandle(mapping);
			}
			CloseHandle(file);
			if (wcsstr(entry.cFileName, L".log"))
			{
				++reports;
				CHECK(strstr(content, "stage=synthetic-worker"));
				if (index == 2)
					CHECK(strstr(content, "exiting: synthetic failure") && strstr(content, "exit_code=0x0000000000000007") && !strstr(content, "fatal native exception"));
				else if (index == 3)
					CHECK(strstr(content, "reason=access violation") && strstr(content, "operation=write") && strstr(content, "exception_code=0x00000000C0000005"));
				else if (index == 5)
					CHECK(strstr(content, "stack_status=stopped (unreadable stack or unwind metadata)"));
				else
					CHECK(strstr(content, "fatal native exception") && strstr(content, "exception_code=0x00000000E0424242") && strstr(content, "module_offset="));
				if (index != 2 && index != 5)
				{
					CHECK(strstr(content, "stack_frame=0x0000000000000001"));
					CHECK(strstr(content, "stack_status=complete"));
					char encodedPath[32768];
				CHECK(WideCharToMultiByte(CP_UTF8, 0, executable, -1, encodedPath, sizeof(encodedPath), NULL, NULL));
					char expected[32768];
					CHECK(snprintf(expected, sizeof(expected), "stack_module=%s", encodedPath) > 0);
					CHECK(strstr(content, expected));
				}
				if (index == 1)
				{
					CHECK(strstr(content, "dump_error=0x0000000000000000"));
					WCHAR expectedPath[MAX_PATH];
					CHECK(wcscpy_s(expectedPath, ARRAYSIZE(expectedPath), path) == 0);
					WCHAR* extension = wcsrchr(expectedPath, L'.');
					CHECK(extension && wcscpy_s(extension, 5, L".dmp") == 0);
					char encodedPath[MAX_PATH * 4];
					CHECK(WideCharToMultiByte(CP_UTF8, 0, expectedPath, -1, encodedPath, sizeof(encodedPath), NULL, NULL));
					char expected[MAX_PATH * 4 + 64];
					CHECK(snprintf(expected, sizeof(expected), "rdk: crash dump written=%s\n", encodedPath) > 0);
					CHECK(strstr(content, expected));
					CHECK(!strstr(content, "rdk: crash dump FAILED"));
				}
				else
					CHECK(!strstr(content, "rdk: crash dump written="));
			}
			else if (wcsstr(entry.cFileName, L".dmp"))
			{
				++dumps;
				CHECK(read > 32 && memcmp(content, "MDMP", 4) == 0);
			}
			CHECK(DeleteFileW(path));
		} while (FindNextFileW(search, &entry));
		FindClose(search);
		CHECK(reports == 1 && dumps == (index == 1 ? 1u : 0u));
	}
	CHECK(RemoveDirectoryW(directory));
	puts("Passed main/worker fault-context stacks, damaged stack handling, minidump stream, ordinary exit, and unavailable report path");
	return 0;
}