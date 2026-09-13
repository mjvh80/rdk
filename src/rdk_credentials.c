#include "rdk_credentials.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freerdp/client/cmdline.h>

void rdk_credentials_init(rdkCredentials* credentials)
{
	ZeroMemory(credentials, sizeof(*credentials));
	credentials->read = CredReadW;
	credentials->write = CredWriteW;
	credentials->free = CredFree;
	credentials->prompt = CredUIPromptForCredentialsW;
}

void rdk_credentials_clear(rdkCredentials* credentials)
{
	SecureZeroMemory(credentials->password, sizeof(credentials->password));
	SecureZeroMemory(credentials->username, sizeof(credentials->username));
	credentials->save = FALSE;
}

static char* rdk_utf8(const WCHAR* text)
{
	const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
	                                     NULL, 0, NULL, NULL);
	if (length <= 0)
		return NULL;
	char* result = malloc((size_t)length);
	if (result && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
	                                 result, length, NULL, NULL))
	{
		SecureZeroMemory(result, (size_t)length);
		free(result);
		return NULL;
	}
	return result;
}

static BOOL rdk_set_identity(rdkCredentials* credentials, rdpSettings* settings)
{
	char* combined = rdk_utf8(credentials->username);
	char* password = rdk_utf8(credentials->password);
	char* username = NULL;
	char* domain = NULL;
	BOOL ok = FALSE;
	if (combined && *combined && password && freerdp_parse_username(combined, &username, &domain))
	{
		ok = freerdp_settings_set_string(settings, FreeRDP_Username, username) &&
		     freerdp_settings_set_string(settings, FreeRDP_Domain, domain) &&
		     freerdp_settings_set_string(settings, FreeRDP_Password, password);
	}
	if (password)
		SecureZeroMemory(password, strlen(password));
	free(password);
	free(combined);
	free(username);
	free(domain);
	return ok;
}

static BOOL rdk_load_identity(rdkCredentials* credentials, const CREDENTIALW* stored)
{
	if (!stored->UserName || !*stored->UserName ||
	    wcslen(stored->UserName) >= ARRAYSIZE(credentials->username) ||
	    stored->CredentialBlobSize % sizeof(WCHAR) != 0 ||
	    stored->CredentialBlobSize >= sizeof(credentials->password) ||
	    (stored->CredentialBlobSize != 0 && !stored->CredentialBlob))
		return FALSE;
	wcscpy_s(credentials->username, ARRAYSIZE(credentials->username), stored->UserName);
	if (stored->CredentialBlobSize != 0)
		memcpy(credentials->password, stored->CredentialBlob, stored->CredentialBlobSize);
	return wcslen(credentials->password) * sizeof(WCHAR) == stored->CredentialBlobSize;
}

BOOL rdk_credentials_prepare(rdkCredentials* credentials, rdpSettings* settings, BOOL prompt)
{
	rdk_credentials_clear(credentials);
	const char* username = freerdp_settings_get_string(settings, FreeRDP_Username);
	const char* password = freerdp_settings_get_string(settings, FreeRDP_Password);
	if (!prompt && (password || freerdp_settings_get_string(settings, FreeRDP_PasswordHash)))
		return TRUE;
	const char* server = freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
	if (!server)
		return !prompt;
	WCHAR hostname[256];
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, server, -1, hostname, ARRAYSIZE(hostname)))
		return FALSE;
	CharLowerBuffW(hostname, (DWORD)wcslen(hostname));
	if (swprintf_s(credentials->target, ARRAYSIZE(credentials->target), L"rdk/%ls:%u", hostname,
	               freerdp_settings_get_uint32(settings, FreeRDP_ServerPort)) < 0)
		return FALSE;
	if (!prompt && (!username || !*username))
	{
		PCREDENTIALW stored = NULL;
		if (!credentials->read(credentials->target, CRED_TYPE_GENERIC, 0, &stored))
		{
			const DWORD error = GetLastError();
			if (error != ERROR_NOT_FOUND)
				fprintf(stderr, "rdk: Credential Manager lookup failed (%lu); trying Windows SSO\n", error);
			return TRUE;
		}
		const BOOL ok = rdk_load_identity(credentials, stored) && rdk_set_identity(credentials, settings);
		if (stored->CredentialBlob)
			SecureZeroMemory(stored->CredentialBlob, stored->CredentialBlobSize);
		credentials->free(stored);
		rdk_credentials_clear(credentials);
		if (!ok)
			fprintf(stderr, "rdk: saved credentials could not be read; use /credentials to replace them\n");
		return ok;
	}
	if (username && *username)
	{
		WCHAR user[CREDUI_MAX_USERNAME_LENGTH + 1];
		WCHAR domain[CREDUI_MAX_DOMAIN_TARGET_LENGTH + 1] = { 0 };
		const char* domainText = freerdp_settings_get_string(settings, FreeRDP_Domain);
		if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, username, -1, user, ARRAYSIZE(user)))
			return FALSE;
		if (domainText && *domainText &&
		    !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, domainText, -1, domain, ARRAYSIZE(domain)))
			return FALSE;
		if (swprintf_s(credentials->username, ARRAYSIZE(credentials->username),
		               *domain ? L"%ls\\%ls" : L"%ls%ls", domain, user) < 0)
			return FALSE;
	}
	CREDUI_INFOW info = { 0 };
	info.cbSize = sizeof(info);
	info.pszCaptionText = L"rdk - Remote Desktop credentials";
	info.pszMessageText = hostname;
	const DWORD result = credentials->prompt(&info, credentials->target, NULL, 0,
	    credentials->username, ARRAYSIZE(credentials->username),
	    credentials->password, ARRAYSIZE(credentials->password), &credentials->save,
	    CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_ALWAYS_SHOW_UI |
	    CREDUI_FLAGS_SHOW_SAVE_CHECK_BOX | CREDUI_FLAGS_DO_NOT_PERSIST);
	if (result != NO_ERROR)
	{
		rdk_credentials_clear(credentials);
		if (result == ERROR_CANCELLED)
			fprintf(stderr, "rdk: credential entry cancelled\n");
		else
			fprintf(stderr, "rdk: credential prompt failed (%lu)\n", result);
		return FALSE;
	}
	const BOOL ok = rdk_set_identity(credentials, settings);
	if (!ok || !credentials->save)
		rdk_credentials_clear(credentials);
	return ok;
}

BOOL rdk_credentials_save(rdkCredentials* credentials)
{
	BOOL ok = TRUE;
	if (credentials->save)
	{
		CREDENTIALW stored = { 0 };
		stored.Type = CRED_TYPE_GENERIC;
		stored.TargetName = credentials->target;
		stored.UserName = credentials->username;
		stored.CredentialBlob = (LPBYTE)credentials->password;
		stored.CredentialBlobSize = (DWORD)(wcslen(credentials->password) * sizeof(WCHAR));
		stored.Persist = CRED_PERSIST_LOCAL_MACHINE;
		ok = credentials->write(&stored, 0);
		if (!ok)
			fprintf(stderr, "rdk: could not save credentials (%lu); connection will continue\n", GetLastError());
	}
	rdk_credentials_clear(credentials);
	return ok;
}