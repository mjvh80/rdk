#include "rdk_crash.h"

#include <dbghelp.h>
#include <shlobj.h>
#include <stdio.h>

static HANDLE reportFile = INVALID_HANDLE_VALUE;
static HANDLE errorOutput = INVALID_HANDLE_VALUE;
static WCHAR dumpPath[32768];
static volatile LONG dumpEnabled;
static volatile LONG reportingCrash;
static PVOID volatile currentStage = "startup";
static LPTOP_LEVEL_EXCEPTION_FILTER previousFilter;

static void write_bytes(const char* text, DWORD length)
{
	DWORD written = 0;
	if (reportFile != INVALID_HANDLE_VALUE)
		(void)WriteFile(reportFile, text, length, &written, NULL);
	if (InterlockedCompareExchange(&reportingCrash, 0, 0) && errorOutput != INVALID_HANDLE_VALUE && errorOutput)
		(void)WriteFile(errorOutput, text, length, &written, NULL);
}

static void write_text(const char* text)
{
	DWORD length = 0;
	while (text[length])
		++length;
	write_bytes(text, length);
}

static void write_hex(const char* name, UINT64 value)
{
	char digits[19] = "0x0000000000000000\n";
	static const char alphabet[] = "0123456789ABCDEF";
	for (int index = 17; index >= 2; --index)
	{
		digits[index] = alphabet[value & 15];
		value >>= 4;
	}
	write_text(name);
	write_text("=");
	write_bytes(digits, sizeof(digits));
}

static void write_wide(const char* name, const WCHAR* text)
{
	char encoded[4];
	write_text(name);
	write_text("=");
	while (*text)
	{
		const int count = text[0] >= 0xD800 && text[0] <= 0xDBFF &&
		    text[1] >= 0xDC00 && text[1] <= 0xDFFF ? 2 : 1;
		const int length = WideCharToMultiByte(CP_UTF8, 0, text, count, encoded, sizeof(encoded), NULL, NULL);
		if (length > 0)
			write_bytes(encoded, (DWORD)length);
		text += count;
	}
	write_text("\n");
}

static const char* exception_name(DWORD code)
{
	switch (code)
	{
		case EXCEPTION_ACCESS_VIOLATION: return "access violation";
		case EXCEPTION_IN_PAGE_ERROR: return "memory paging I/O error";
		case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
		case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
		case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
		case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point division by zero";
		case 0xE06D7363: return "unhandled C++ exception";
		default: return "unhandled structured exception";
	}
}

static void write_exception_stack(const CONTEXT* exceptionContext)
{
	if (!exceptionContext)
	{
		write_text("stack_status=no exception context\n");
		return;
	}
	__try
	{
		CONTEXT context = *exceptionContext;
		write_text("stack_begin=faulting thread (module offsets; no arguments or locals)\n");
		for (UINT frame = 0; frame < 32 && context.Rip; ++frame)
		{
			write_hex("stack_frame", frame);
			write_hex("stack_address", context.Rip);
			MEMORY_BASIC_INFORMATION memory;
			if (VirtualQuery((LPCVOID)context.Rip, &memory, sizeof(memory)) && memory.Type == MEM_IMAGE)
			{
				write_hex("stack_module_offset", context.Rip - (UINT_PTR)memory.AllocationBase);
				WCHAR module[1024];
				const DWORD length = GetModuleFileNameW((HMODULE)memory.AllocationBase, module, ARRAYSIZE(module));
				if (length && length < ARRAYSIZE(module))
					write_wide("stack_module", module);
			}
			const DWORD64 previousStack = context.Rsp;
			DWORD64 imageBase = 0;
			PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, NULL);
			if (function)
			{
				PVOID handlerData = NULL;
				DWORD64 establisherFrame = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context,
				    &handlerData, &establisherFrame, NULL);
			}
			else
			{
				context.Rip = *(const DWORD64*)context.Rsp;
				context.Rsp += sizeof(DWORD64);
			}
			if (context.Rsp <= previousStack)
			{
				write_text("stack_status=stopped (stack did not advance)\n");
				return;
			}
		}
		write_text(context.Rip ? "stack_status=frame limit reached\n" : "stack_status=complete\n");
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		write_text("stack_status=stopped (unreadable stack or unwind metadata)\n");
	}
}

static BOOL ensure_directory(const WCHAR* path)
{
	if (CreateDirectoryW(path, NULL))
		return TRUE;
	const DWORD attributes = GetFileAttributesW(path);
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

BOOL rdk_crash_init(const WCHAR* directory)
{
	if (reportFile != INVALID_HANDLE_VALUE)
		return TRUE;
	errorOutput = GetStdHandle(STD_ERROR_HANDLE);
	previousFilter = SetUnhandledExceptionFilter(rdk_crash_filter);
	WCHAR folder[32768];
	if (directory)
	{
		if (wcscpy_s(folder, ARRAYSIZE(folder), directory) != 0)
			return FALSE;
	}
	else
	{
		PWSTR local = NULL;
		if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local)))
			return FALSE;
		const int length = swprintf_s(folder, ARRAYSIZE(folder), L"%ls\\rdk", local);
		CoTaskMemFree(local);
		if (length < 0 || !ensure_directory(folder) || wcscat_s(folder, ARRAYSIZE(folder), L"\\diagnostics") != 0)
			return FALSE;
	}
	if (!ensure_directory(folder))
		return FALSE;
	SYSTEMTIME time;
	GetSystemTime(&time);
	WCHAR base[32768];
	if (swprintf_s(base, ARRAYSIZE(base), L"%ls\\rdk-%04u%02u%02u-%02u%02u%02u-%03u-%lu",
	    folder, time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
	    time.wMilliseconds, GetCurrentProcessId()) < 0)
		return FALSE;
	WCHAR path[32768];
	if (swprintf_s(path, ARRAYSIZE(path), L"%ls.log", base) < 0 ||
	    swprintf_s(dumpPath, ARRAYSIZE(dumpPath), L"%ls.dmp", base) < 0)
		return FALSE;
	reportFile = CreateFileW(path, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ, NULL,
	    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	if (reportFile == INVALID_HANDLE_VALUE)
		return FALSE;
	write_text("rdk diagnostic report v1; UTC; no command line or credential values recorded\n");
	write_hex("pid", GetCurrentProcessId());
	WCHAR executable[32768];
	const DWORD length = GetModuleFileNameW(NULL, executable, ARRAYSIZE(executable));
	if (length && length < ARRAYSIZE(executable))
		write_wide("executable", executable);
	write_text("build=" __DATE__ " " __TIME__ "\n");
	printf("rdk: diagnostic report: %ls\n", path);
	fflush(stdout);
	rdk_crash_stage("startup");
	return TRUE;
}

void rdk_crash_enable_dump(void)
{
	InterlockedExchange(&dumpEnabled, 1);
	rdk_crash_message("memory_dump=enabled (may contain sensitive process memory)");
}

void rdk_crash_message(const char* message)
{
	write_text(message);
	write_text("\n");
	if (reportFile != INVALID_HANDLE_VALUE)
		(void)FlushFileBuffers(reportFile);
}

void rdk_crash_stage(const char* stage)
{
	InterlockedExchangePointer(&currentStage, (PVOID)stage);
	if (reportFile == INVALID_HANDLE_VALUE)
		return;
	char line[256];
	SYSTEMTIME time;
	GetSystemTime(&time);
	const int length = snprintf(line, sizeof(line), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ stage=%s",
	    time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, stage);
	if (length > 0 && (size_t)length < sizeof(line))
		rdk_crash_message(line);
}

LONG rdk_crash_filter(EXCEPTION_POINTERS* exception)
{
	if (!exception || !exception->ExceptionRecord ||
	    InterlockedCompareExchange(&reportingCrash, 1, 0) != 0)
		return EXCEPTION_CONTINUE_SEARCH;
	write_text("rdk: fatal native exception; terminating (not attempting recovery)\nreason=");
	write_text(exception_name(exception->ExceptionRecord->ExceptionCode));
	write_text("\nstage=");
	write_text((const char*)InterlockedCompareExchangePointer(&currentStage, NULL, NULL));
	write_text("\n");
	write_hex("exception_code", exception->ExceptionRecord->ExceptionCode);
	if ((exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
	     exception->ExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
	    exception->ExceptionRecord->NumberParameters >= 2)
	{
		const ULONG_PTR operation = exception->ExceptionRecord->ExceptionInformation[0];
		write_text(operation == 0 ? "operation=read\n" : operation == 1 ? "operation=write\n" :
		    operation == 8 ? "operation=execute\n" : "operation=unknown\n");
		write_hex("access_address", exception->ExceptionRecord->ExceptionInformation[1]);
	}
	write_hex("exception_address", (UINT_PTR)exception->ExceptionRecord->ExceptionAddress);
	write_hex("thread_id", GetCurrentThreadId());
	MEMORY_BASIC_INFORMATION memory;
	if (VirtualQuery(exception->ExceptionRecord->ExceptionAddress, &memory, sizeof(memory)))
	{
		write_hex("module_base", (UINT_PTR)memory.AllocationBase);
		write_hex("module_offset", (UINT_PTR)exception->ExceptionRecord->ExceptionAddress - (UINT_PTR)memory.AllocationBase);
		WCHAR module[1024];
		const DWORD length = GetModuleFileNameW((HMODULE)memory.AllocationBase, module, ARRAYSIZE(module));
		if (length && length < ARRAYSIZE(module))
			write_wide("module", module);
	}
	if (reportFile != INVALID_HANDLE_VALUE)
		(void)FlushFileBuffers(reportFile);
	write_exception_stack(exception->ContextRecord);
	if (reportFile != INVALID_HANDLE_VALUE)
		(void)FlushFileBuffers(reportFile);
	if (InterlockedCompareExchange(&dumpEnabled, 0, 0) && *dumpPath)
	{
		HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD error = ERROR_SUCCESS;
		if (dump == INVALID_HANDLE_VALUE)
			error = GetLastError();
		else
		{
			MINIDUMP_EXCEPTION_INFORMATION info = { GetCurrentThreadId(), exception, FALSE };
			if (!MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
			    (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), &info, NULL, NULL))
				error = GetLastError();
			CloseHandle(dump);
		}
		write_wide(error == ERROR_SUCCESS ? "rdk: crash dump written" : "rdk: crash dump FAILED at", dumpPath);
		write_hex("dump_error", error);
		if (reportFile != INVALID_HANDLE_VALUE)
			(void)FlushFileBuffers(reportFile);
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

void rdk_crash_finish(int exitCode)
{
	write_hex("exit_code", (DWORD)exitCode);
	rdk_crash_stage("process-exit");
	SetUnhandledExceptionFilter(previousFilter);
	if (reportFile != INVALID_HANDLE_VALUE)
	{
		CloseHandle(reportFile);
		reportFile = INVALID_HANDLE_VALUE;
	}
}