#include "rdk_taskbar.h"
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int show)
{
	(void)instance;
	(void)previous;
	(void)commandLine;
	(void)show;
	int count = 0;
	WCHAR** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
	if (!arguments)
		return 1;
	int result = -1;
	if (count == 3)
	{
		if (_wcsicmp(arguments[1], L"/minimize") == 0)
			result = rdk_taskbar_dispatch(arguments[2], RDK_TASKBAR_MINIMIZE);
		else if (_wcsicmp(arguments[1], L"/reconnect") == 0)
			result = rdk_taskbar_dispatch(arguments[2], RDK_TASKBAR_RECONNECT);
	}
	LocalFree(arguments);
	return result < 0 ? 1 : 0;
}