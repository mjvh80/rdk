#ifndef RDK_CREDENTIALS_H
#define RDK_CREDENTIALS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <wincred.h>
#include <freerdp/settings.h>

typedef struct
{
	WCHAR target[512];
	WCHAR username[CREDUI_MAX_USERNAME_LENGTH + 1];
	WCHAR password[CREDUI_MAX_PASSWORD_LENGTH + 1];
	BOOL save;
	BOOL (WINAPI *read)(LPCWSTR, DWORD, DWORD, PCREDENTIALW*);
	BOOL (WINAPI *write)(PCREDENTIALW, DWORD);
	void (WINAPI *free)(PVOID);
	DWORD (WINAPI *prompt)(PCREDUI_INFOW, PCWSTR, PVOID, DWORD, PWSTR, ULONG,
	                      PWSTR, ULONG, BOOL*, DWORD);
} rdkCredentials;

void rdk_credentials_init(rdkCredentials* credentials);
void rdk_credentials_clear(rdkCredentials* credentials);
BOOL rdk_credentials_prepare(rdkCredentials* credentials, rdpSettings* settings, BOOL prompt);
BOOL rdk_credentials_save(rdkCredentials* credentials);

#endif