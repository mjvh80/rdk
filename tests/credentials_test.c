#include "rdk_credentials.h"

#include <stdio.h>
#include <string.h>

static rdkCredentials credentials;
static rdpSettings* settings;
static CREDENTIALW stored;
static WCHAR storedPassword[32];
static const WCHAR* enteredUser;
static const WCHAR* expectedPrefill;
static BOOL haveStored;
static BOOL remember;
static BOOL writeOk;
static BOOL freedAndCleared;
static BOOL targetOk;
static BOOL promptFlagsOk;
static BOOL prefillOk;
static BOOL writtenValueOk;
static DWORD promptResult;
static UINT reads;
static UINT writes;
static UINT prompts;

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return FALSE; \
} } while (0)

static BOOL target_matches(LPCWSTR target)
{
	return wcscmp(target, L"rdk/server.example:3390") == 0;
}

static BOOL WINAPI read_credential(LPCWSTR target, DWORD type, DWORD flags, PCREDENTIALW* result)
{
	++reads;
	targetOk = target_matches(target) && type == CRED_TYPE_GENERIC && flags == 0;
	if (!haveStored)
	{
		SetLastError(ERROR_NOT_FOUND);
		return FALSE;
	}
	*result = &stored;
	return TRUE;
}

static void WINAPI free_credential(PVOID memory)
{
	freedAndCleared = memory == &stored;
	for (DWORD index = 0; index < stored.CredentialBlobSize; ++index)
		freedAndCleared = freedAndCleared && stored.CredentialBlob[index] == 0;
}

static BOOL WINAPI write_credential(PCREDENTIALW value, DWORD flags)
{
	++writes;
	writtenValueOk = target_matches(value->TargetName) && value->Type == CRED_TYPE_GENERIC &&
	    value->Persist == CRED_PERSIST_LOCAL_MACHINE && flags == 0 &&
	    wcscmp(value->UserName, L"REMOTE\\person") == 0 &&
	    value->CredentialBlobSize == wcslen(L"test-password") * sizeof(WCHAR) &&
	    memcmp(value->CredentialBlob, L"test-password", value->CredentialBlobSize) == 0;
	if (!writeOk)
		SetLastError(ERROR_ACCESS_DENIED);
	return writeOk;
}

static DWORD WINAPI prompt_credential(PCREDUI_INFOW info, PCWSTR target, PVOID reserved,
    DWORD error, PWSTR username, ULONG userCount, PWSTR password, ULONG passwordCount,
    BOOL* save, DWORD flags)
{
	++prompts;
	promptFlagsOk = info && target_matches(target) && !reserved && error == 0 &&
	    flags == (CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_ALWAYS_SHOW_UI |
	              CREDUI_FLAGS_SHOW_SAVE_CHECK_BOX | CREDUI_FLAGS_DO_NOT_PERSIST);
	prefillOk = wcscmp(username, expectedPrefill) == 0;
	wcscpy_s(username, userCount, enteredUser);
	wcscpy_s(password, passwordCount, L"test-password");
	*save = remember;
	return promptResult;
}

static BOOL reset(void)
{
	rdk_credentials_init(&credentials);
	credentials.read = read_credential;
	credentials.write = write_credential;
	credentials.free = free_credential;
	credentials.prompt = prompt_credential;
	reads = writes = prompts = 0;
	haveStored = remember = freedAndCleared = targetOk = promptFlagsOk = prefillOk = writtenValueOk = FALSE;
	writeOk = TRUE;
	promptResult = NO_ERROR;
	enteredUser = L"REMOTE\\person";
	expectedPrefill = L"";
	ZeroMemory(&stored, sizeof(stored));
	wcscpy_s(storedPassword, ARRAYSIZE(storedPassword), L"test-password");
	stored.UserName = L"REMOTE\\person";
	stored.CredentialBlob = (LPBYTE)storedPassword;
	stored.CredentialBlobSize = (DWORD)(wcslen(storedPassword) * sizeof(WCHAR));
	settings = freerdp_settings_new(0);
	CHECK(settings);
	CHECK(freerdp_settings_set_string(settings, FreeRDP_ServerHostname, "SERVER.example"));
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, 3390));
	return TRUE;
}

static BOOL cleared(void)
{
	CHECK(!credentials.save);
	for (size_t index = 0; index < ARRAYSIZE(credentials.password); ++index)
		CHECK(credentials.password[index] == 0);
	return TRUE;
}

static BOOL identity_matches(void)
{
	CHECK(strcmp(freerdp_settings_get_string(settings, FreeRDP_Username), "person") == 0);
	CHECK(strcmp(freerdp_settings_get_string(settings, FreeRDP_Domain), "REMOTE") == 0);
	CHECK(strcmp(freerdp_settings_get_string(settings, FreeRDP_Password), "test-password") == 0);
	return TRUE;
}

static BOOL sso_without_cache(void)
{
	CHECK(rdk_credentials_prepare(&credentials, settings, FALSE));
	CHECK(reads == 1 && targetOk && prompts == 0 && writes == 0);
	CHECK(!freerdp_settings_get_string(settings, FreeRDP_Username));
	CHECK(!freerdp_settings_get_string(settings, FreeRDP_Password));
	CHECK(cleared());
	return TRUE;
}

static BOOL cached_identity(void)
{
	haveStored = TRUE;
	CHECK(rdk_credentials_prepare(&credentials, settings, FALSE));
	CHECK(reads == 1 && targetOk && freedAndCleared && prompts == 0);
	CHECK(identity_matches());
	CHECK(cleared());
	CHECK(rdk_credentials_save(&credentials));
	CHECK(writes == 0);
	return TRUE;
}

static BOOL explicit_password(void)
{
	CHECK(freerdp_settings_set_string(settings, FreeRDP_Username, "explicit"));
	CHECK(freerdp_settings_set_string(settings, FreeRDP_Password, "explicit-test-password"));
	haveStored = TRUE;
	CHECK(rdk_credentials_prepare(&credentials, settings, FALSE));
	CHECK(reads == 0 && prompts == 0 && writes == 0);
	CHECK(strcmp(freerdp_settings_get_string(settings, FreeRDP_Username), "explicit") == 0);
	return TRUE;
}

static BOOL prompt_without_save(void)
{
	CHECK(freerdp_settings_set_string(settings, FreeRDP_Username, "person"));
	CHECK(freerdp_settings_set_string(settings, FreeRDP_Domain, "REMOTE"));
	expectedPrefill = L"REMOTE\\person";
	CHECK(rdk_credentials_prepare(&credentials, settings, FALSE));
	CHECK(prompts == 1 && promptFlagsOk && prefillOk && reads == 0);
	CHECK(identity_matches());
	CHECK(cleared());
	CHECK(rdk_credentials_save(&credentials));
	CHECK(writes == 0);
	return TRUE;
}

static BOOL save_after_success(void)
{
	haveStored = TRUE;
	remember = TRUE;
	CHECK(rdk_credentials_prepare(&credentials, settings, TRUE));
	CHECK(reads == 0 && prompts == 1 && promptFlagsOk && prefillOk && writes == 0);
	CHECK(identity_matches());
	CHECK(credentials.save);
	CHECK(rdk_credentials_save(&credentials));
	CHECK(writes == 1 && writtenValueOk);
	CHECK(cleared());
	return TRUE;
}

static BOOL discard_after_failure(void)
{
	remember = TRUE;
	CHECK(rdk_credentials_prepare(&credentials, settings, TRUE));
	rdk_credentials_clear(&credentials);
	CHECK(rdk_credentials_save(&credentials));
	CHECK(writes == 0);
	CHECK(cleared());
	return TRUE;
}

static BOOL cancel_prompt(void)
{
	remember = TRUE;
	promptResult = ERROR_CANCELLED;
	CHECK(!rdk_credentials_prepare(&credentials, settings, TRUE));
	CHECK(prompts == 1 && writes == 0);
	CHECK(!freerdp_settings_get_string(settings, FreeRDP_Password));
	CHECK(cleared());
	return TRUE;
}

static BOOL corrupt_cache(void)
{
	haveStored = TRUE;
	stored.CredentialBlobSize = 3;
	CHECK(!rdk_credentials_prepare(&credentials, settings, FALSE));
	CHECK(reads == 1 && freedAndCleared && prompts == 0 && writes == 0);
	CHECK(!freerdp_settings_get_string(settings, FreeRDP_Password));
	CHECK(cleared());
	return TRUE;
}

static BOOL save_failure(void)
{
	remember = TRUE;
	writeOk = FALSE;
	CHECK(rdk_credentials_prepare(&credentials, settings, TRUE));
	CHECK(!rdk_credentials_save(&credentials));
	CHECK(writes == 1 && writtenValueOk);
	CHECK(identity_matches());
	CHECK(cleared());
	return TRUE;
}

static BOOL upn_identity(void)
{
	enteredUser = L"person@example.net";
	CHECK(rdk_credentials_prepare(&credentials, settings, TRUE));
	CHECK(prefillOk);
	CHECK(strcmp(freerdp_settings_get_string(settings, FreeRDP_Username), "person@example.net") == 0);
	const char* domain = freerdp_settings_get_string(settings, FreeRDP_Domain);
	CHECK(!domain || !*domain);
	CHECK(cleared());
	return TRUE;
}

int main(void)
{
	BOOL (*tests[])(void) = { sso_without_cache, cached_identity, explicit_password,
		prompt_without_save, save_after_success, discard_after_failure, cancel_prompt,
		corrupt_cache, save_failure, upn_identity };
	for (size_t index = 0; index < ARRAYSIZE(tests); ++index)
	{
		if (!reset())
			return 1;
		const BOOL ok = tests[index]();
		rdk_credentials_clear(&credentials);
		freerdp_settings_free(settings);
		if (!ok)
			return 1;
	}
	printf("Passed %zu credential tests (mock Windows APIs)\n", ARRAYSIZE(tests));
	return 0;
}