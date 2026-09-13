/*
 * rdk - fullscreen, multi-monitor RDP client with a reliable input path.
 *
 * This is our own RDP client (not mstsc): it renders the remote desktop across
 * all local monitors and forwards input. Input rides the RDP server's trusted
 * input path, so it reaches elevated apps and the secure desktop - unlike
 * SendInput, which UIPI/integrity levels block.
 *
 * Alt+numpad characters are composed locally and sent as Unicode input.
 * Ordinary keys and shortcuts are sent as scancodes.
 *
 * Usage:
 *   rdk /v:HOST [/port:3389] /u:USER /p:PASS [/d:DOMAIN] [/cert:ignore]
 *       [/text:"STRING" | /input:"SEQUENCE"] [/startup-delay:MS] [/char-delay:MS]
 *
 * Minimize with Ctrl+Shift+F10; reconnect with Ctrl+Shift+F11; quit with Ctrl+Shift+F12.
 *
 * Prefer current Windows credentials to a password on the command line.
 * Certificates are auto-accepted for this proof of concept.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "rdk_client.h"
#include "rdk_credentials.h"
#include "rdk_camera.h"
#include "rdk_session.h"
#include "rdk_startup.h"
#include "rdk_taskbar.h"

#include <freerdp/client/cmdline.h>
#include <freerdp/scancode.h>

#define RDK_MAX_HANDLES (MAXIMUM_WAIT_OBJECTS - 1)

/* Options. /text auto-types once after connect (handy for testing the
 * Unicode path); it is not sent unless requested. */
static const WCHAR* g_autoText = NULL;
static BOOL g_autoTokens = FALSE;
static DWORD g_startupDelayMs = 3000;
static DWORD g_charDelayMs = 0;
static BOOL g_promptCredentials = FALSE;
static BOOL g_cameraEnabled = FALSE;

/* ---- helpers ------------------------------------------------------------ */

static char* wide_to_utf8(const WCHAR* w)
{
	if (!w)
		return NULL;
	const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (len <= 0)
		return NULL;
	char* s = (char*)malloc((size_t)len);
	if (!s)
		return NULL;
	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL))
	{
		free(s);
		return NULL;
	}
	return s;
}

static BOOL rdk_value_option(const WCHAR* argument, const WCHAR* name, const WCHAR** value)
{
	*value = NULL;
	const size_t length = wcslen(name);
	if (argument[0] == L'/' && _wcsnicmp(argument + 1, name, length) == 0 &&
	    (argument[length + 1] == L':' || argument[length + 1] == L'\0'))
	{
		if (argument[length + 1] == L':')
			*value = argument + length + 2;
		return TRUE;
	}
	return argument[0] == L'-' && argument[1] == L'-' && wcscmp(argument + 2, name) == 0;
}

static BOOL rdk_parse_delay(const WCHAR* text, DWORD* delay)
{
	if (*text < L'0' || *text > L'9')
		return FALSE;
	WCHAR* end = NULL;
	errno = 0;
	const unsigned long value = wcstoul(text, &end, 10);
	if (errno == ERANGE || *end != L'\0')
		return FALSE;
	*delay = (DWORD)value;
	return TRUE;
}

/* ---- FreeRDP callbacks -------------------------------------------------- */

static BOOL rdk_global_init(void)
{
	WSADATA wsa;
	return (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
}

static void rdk_global_uninit(void)
{
	WSACleanup();
}

static BOOL rdk_pre_connect(freerdp* instance)
{
	((rdkContext*)instance->context)->cameraEnabled = g_cameraEnabled;
	if (g_cameraEnabled && !rdk_camera_register())
		return FALSE;
	rdpSettings* settings = instance->context->settings;
	if (!rdk_keyboard_configure(settings))
		return FALSE;
	return rdk_configure_display(settings, &((rdkContext*)instance->context)->displayBounds);
}

static BOOL rdk_authenticate_ex(freerdp* instance, char** username, char** password, char** domain,
                                rdp_auth_reason reason)
{
	(void)instance;
	(void)username;
	(void)password;
	(void)domain;
	(void)reason;
	/* Credentials come from the command line; nothing to prompt for. */
	return TRUE;
}

static DWORD rdk_verify_certificate_ex(freerdp* instance, const char* host, UINT16 port,
                                       const char* common_name, const char* subject,
                                       const char* issuer, const char* fingerprint, DWORD flags)
{
	(void)instance;
	(void)host;
	(void)port;
	(void)common_name;
	(void)subject;
	(void)issuer;
	(void)fingerprint;
	(void)flags;
	return 2;
}

static DWORD rdk_verify_changed_certificate_ex(freerdp* instance, const char* host, UINT16 port,
                                               const char* common_name, const char* subject,
                                               const char* issuer, const char* new_fingerprint,
                                               const char* old_subject, const char* old_issuer,
                                               const char* old_fingerprint, DWORD flags)
{
	(void)instance;
	(void)host;
	(void)port;
	(void)common_name;
	(void)subject;
	(void)issuer;
	(void)new_fingerprint;
	(void)old_subject;
	(void)old_issuer;
	(void)old_fingerprint;
	(void)flags;
	return 2;
}

static BOOL rdk_client_new(freerdp* instance, rdpContext* context)
{
	(void)context;
	instance->PreConnect = rdk_pre_connect;
	instance->PostConnect = rdk_gdi_post_connect;
	instance->PostDisconnect = rdk_gdi_post_disconnect;
	instance->AuthenticateEx = rdk_authenticate_ex;
	instance->VerifyCertificateEx = rdk_verify_certificate_ex;
	instance->VerifyChangedCertificateEx = rdk_verify_changed_certificate_ex;
	return TRUE;
}

static void rdk_client_free(freerdp* instance, rdpContext* context)
{
	(void)instance;
	(void)context;
}

/* ---- run loop ----------------------------------------------------------- */

/* Single-threaded loop that services both FreeRDP network events and Win32
 * window messages until the user quits or the connection drops. */
static BOOL rdk_startup_ready(rdkContext* rdk)
{
	return rdk->desktopReady && rdk->focused && rdk_keyboard_idle(&rdk->keyboard) &&
	       freerdp_is_active_state((rdpContext*)rdk) && rdk_startup_keys_released();
}

static int rdk_run(rdkContext* rdk)
{
	rdpContext* context = (rdpContext*)rdk;
	rdkStartup startup;
	rdk_startup_init(&startup, g_autoText, g_autoTokens, g_startupDelayMs, g_charDelayMs);
	BOOL activated = FALSE;
	if (startup.next && *startup.next)
	{
		printf("rdk: startup input queued for this connection; waiting for the desktop, focus, and physical key release\n");
		fflush(stdout);
	}

	while (!freerdp_shall_disconnect_context(context) && !rdk->quit)
	{
		const DWORD captureError = rdk_capture_error(&rdk->capture);
		if (captureError)
		{
			fprintf(stderr, "rdk: keyboard capture failed (%lu)\n", captureError);
			return 1;
		}
		if (!activated && freerdp_is_active_state(context))
		{
			rdk_gdi_activate(rdk);
			activated = TRUE;
		}
		HANDLE handles[RDK_MAX_HANDLES];
		const DWORD nCount = freerdp_get_event_handles(context, handles, RDK_MAX_HANDLES);
		if (nCount == 0)
		{
			fprintf(stderr, "rdk: failed to get event handles\n");
			return 1;
		}

		const BOOL pending = startup.next && *startup.next;
		const DWORD timeout = rdk_startup_wait(&startup, GetTickCount64(), pending && rdk_startup_ready(rdk));
		const DWORD status =
		    MsgWaitForMultipleObjectsEx(nCount, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		if (status == WAIT_FAILED)
		{
			fprintf(stderr, "rdk: MsgWaitForMultipleObjectsEx failed\n");
			return 1;
		}

		MSG msg;
		UINT processed = 0;
		while (processed++ < 64 && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				rdk->quit = TRUE;
				break;
			}
			if (!rdk->notice || !IsDialogMessageW(rdk->notice, &msg))
				DispatchMessageW(&msg);
			if (rdk->quit)
				break;
		}
		if (rdk->quit)
			break;
		if (rdk_media_watch_changed(rdk->mediaWatch, GetTickCount64()))
			rdk_media_notice(rdk);
		if (rdk->cameraWatching && rdk->cameraCheckAt && GetTickCount64() >= rdk->cameraCheckAt)
		{
			WCHAR* snapshot = rdk_camera_snapshot();
			if (snapshot)
			{
				const UINT64 now = GetTickCount64();
				const BOOL inventoryChanged = wcscmp(rdk->cameraChange.current, snapshot) != 0;
				if (inventoryChanged)
				{
					printf("rdk: local camera inventory changed (camera added or removed)\n");
					if (!rdk_camera_list())
						fprintf(stderr, "rdk: updated camera inventory is unavailable\n");
					fflush(stdout);
				}
				if (rdk_media_change_update(&rdk->cameraChange, snapshot, now - 1500) &&
				    rdk_media_change_ready(&rdk->cameraChange, now))
				{
					printf("rdk: camera configuration changed or a session camera was interrupted; offering reconnect\n");
					fflush(stdout);
					rdk_media_notice(rdk);
				}
				free(snapshot);
			}
			else
				fprintf(stderr, "rdk: camera change detected but inventory could not be queried\n");
			rdk->cameraCheckAt = 0;
		}

		if (!freerdp_check_event_handles(context))
		{
			const UINT32 err = freerdp_get_last_error(context);
			if (err != FREERDP_ERROR_SUCCESS)
				fprintf(stderr, "rdk: event handling failed: %s\n",
				        freerdp_get_last_error_name(err));
			return (err == FREERDP_ERROR_SUCCESS) ? 0 : 1;
		}

		if (startup.next && *startup.next)
		{
			const BOOL wasStarted = startup.started;
			if (!rdk_startup_step(&startup, &rdk->keyboard, GetTickCount64(), rdk_startup_ready(rdk)))
			{
				fprintf(stderr, "rdk: failed to send startup input (invalid UTF-16, unsupported Unicode input, or connection failure)\n");
				return 1;
			}
			if (!wasStarted && startup.started)
			{
				printf("rdk: startup input replay started\n");
				fflush(stdout);
			}
			if (!*startup.next)
			{
				printf("rdk: startup input replay completed\n");
				fflush(stdout);
			}
		}
	}
	return rdk->inputFailed ? 1 : 0;
}

/* ---- entry point -------------------------------------------------------- */

static void rdk_usage(void)
{
	printf("rdk - fullscreen, multi-monitor RDP client with reliable input\n\n"
	       "Usage:\n"
	       "  rdk /v:HOST [/port:3389] [/u:USER [/p:PASS] [/d:DOMAIN]] [/cert:ignore]\n"
	       "      [/text:\"STRING\" | /input:\"SEQUENCE\"] [/startup-delay:MS] [/char-delay:MS]\n\n"
	       "Options:\n"
	       "  (no /u and no /p)     use saved rdk credentials, otherwise Windows SSO\n"
	       "  /credentials         prompt for credentials, with optional Windows storage\n"
	       "  /text:STRING         auto-type this once after connect (Unicode events)\n"
	       "  /input:SEQUENCE      send text/keys once after connect, e.g. \"<delay><enter>\"\n"
	       "                        <delay> waits 1 second; <enter>, <esc>, <tab>, <F1>..<F12>\n"
	       "                        also arrows, home/end, pageup/pagedown, insert/delete,\n"
	       "                        backspace, space; use << for a literal <\n"
	       "  /startup-delay:MS    initial wait (default: /text 3000, /input 0)\n"
	       "  /char-delay:MS       delay between startup characters/keys (default 0)\n"
	       "  /screen:1,2           use these Windows display numbers (first is remote primary)\n"
	       "  /monitors:1,2         alias for /screen, using the same Windows numbers\n"
	       "  /list:monitor         list active Windows display numbers and exit\n"
	       "  /microphone           redirect microphone; notify on default-device changes\n"
	       "  /list:microphone      show local input devices, default, mute and format queries\n"
	       "  /sound                play remote audio locally\n"
	       "  /camera               redirect local cameras via Media Foundation (opt-in)\n"
	       "  /list:camera          list local cameras without starting capture\n"
	       "  /...                  any standard FreeRDP option (connection, security, ...)\n\n"
	       "Legacy --name value forms remain supported for rdk options.\n"
	       "Left Alt+numpad codes are composed locally; other keys use scancodes.\n"
	       "Startup input pauses while the window is unfocused or a key is held.\n"
	       "Certificates are auto-accepted for this proof of concept.\n"
	       "Minimize with Ctrl+Shift+F10; reconnect with Ctrl+Shift+F11; quit with Ctrl+Shift+F12.\n");
}

int wmain(int argc, wchar_t** argv)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	if (argc == 2 && _wcsicmp(argv[1], L"/taskbar:minimize") == 0)
		return rdk_taskbar_minimize() < 0 ? 1 : 0;

	if (argc <= 1)
	{
		rdk_usage();
		return 0;
	}

	/* Split our own options out; everything else is forwarded to FreeRDP. */
	char** fargv = (char**)calloc((size_t)argc + 1, sizeof(char*));
	if (!fargv)
		return 1;

	int fargc = 0;
	int rc = 1;
	BOOL listCameras = FALSE;
	BOOL listMicrophones = FALSE;
	BOOL startupDelaySet = FALSE;
	BOOL showRdkHelp = FALSE;
	rdpContext* context = NULL;
	rdpSettings* reconnectSettings = NULL;
	rdkCredentials credentials;
	rdk_credentials_init(&credentials);
	fargv[fargc++] = wide_to_utf8(argv[0] ? argv[0] : L"rdk");
	if (!fargv[0])
		goto cleanup;

	for (int i = 1; i < argc; ++i)
	{
		if (_wcsicmp(argv[i], L"/help") == 0 || wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"/?") == 0)
			showRdkHelp = TRUE;
		if (_wcsicmp(argv[i], L"/camera") == 0)
		{
			g_cameraEnabled = TRUE;
			continue;
		}
		if (_wcsicmp(argv[i], L"/list:camera") == 0)
		{
			listCameras = TRUE;
			continue;
		}
		if (_wcsicmp(argv[i], L"/list:microphone") == 0)
		{
			listMicrophones = TRUE;
			continue;
		}
		if (_wcsicmp(argv[i], L"/credentials") == 0 || wcscmp(argv[i], L"--credentials") == 0)
		{
			g_promptCredentials = TRUE;
			continue;
		}
		const WCHAR* textValue;
		const WCHAR* inputValue;
		const WCHAR* startupValue;
		const WCHAR* charValue;
		const BOOL textOption = rdk_value_option(argv[i], L"text", &textValue);
		const BOOL inputOption = rdk_value_option(argv[i], L"input", &inputValue);
		const BOOL startupOption = rdk_value_option(argv[i], L"startup-delay", &startupValue);
		const BOOL charOption = rdk_value_option(argv[i], L"char-delay", &charValue);
		if (textOption || inputOption || startupOption || charOption)
		{
			const WCHAR* option = textOption ? L"/text" : inputOption ? L"/input" :
			                      startupOption ? L"/startup-delay" : L"/char-delay";
			const WCHAR* value = textOption ? textValue : inputOption ? inputValue :
			                     startupOption ? startupValue : charValue;
			if (!value && (argv[i][0] == L'/' || i + 1 >= argc))
			{
				fprintf(stderr, "rdk: %ls requires a value; use %ls:VALUE\n", option, option);
				goto cleanup;
			}
			if (!value)
				value = argv[++i];
			if (textOption || inputOption)
			{
				if (g_autoText && g_autoTokens != inputOption)
				{
					fprintf(stderr, "rdk: /input and /text cannot be combined\n");
					goto cleanup;
				}
				g_autoText = value;
				g_autoTokens = inputOption;
			}
			else
			{
				if (!rdk_parse_delay(value, startupOption ? &g_startupDelayMs : &g_charDelayMs))
				{
					fprintf(stderr, "rdk: %ls requires an integer from 0 to 4294967295\n", option);
					goto cleanup;
				}
				if (startupOption)
					startupDelaySet = TRUE;
			}
		}
		else
		{
			char* argument = wide_to_utf8(argv[i]);
			if (argument && _strnicmp(argument, "/screen:", 8) == 0)
			{
				const size_t capacity = strlen(argument) + 3;
				char* mapped = malloc(capacity);
				if (mapped)
					snprintf(mapped, capacity, "/monitors:%s", argument + 8);
				free(argument);
				argument = mapped;
			}
			fargv[fargc++] = argument;
			if (!fargv[fargc - 1])
				goto cleanup;
		}
	}

	if (g_autoTokens)
	{
		size_t errorOffset = 0;
		if (!rdk_startup_validate(g_autoText, &errorOffset))
		{
			fprintf(stderr, "rdk: invalid /input at UTF-16 offset %zu; use <delay>, named keys, or << for a literal '<'\n", errorOffset);
			goto cleanup;
		}
		if (!startupDelaySet)
			g_startupDelayMs = 0;
	}
	if (listCameras || listMicrophones)
	{
		rc = 0;
		if (listMicrophones && !rdk_microphone_list()) rc = 1;
		if (listCameras && !rdk_camera_list()) rc = 1;
		goto cleanup;
	}
	RDP_CLIENT_ENTRY_POINTS entry = { 0 };
	entry.Size = sizeof(entry);
	entry.Version = RDP_CLIENT_INTERFACE_VERSION;
	entry.ContextSize = sizeof(rdkContext);
	entry.GlobalInit = rdk_global_init;
	entry.GlobalUninit = rdk_global_uninit;
	entry.ClientNew = rdk_client_new;
	entry.ClientFree = rdk_client_free;

	context = freerdp_client_context_new(&entry);
	if (!context)
	{
		fprintf(stderr, "rdk: failed to create FreeRDP context\n");
		rc = 1;
		goto cleanup;
	}

	const int status =
	    freerdp_client_settings_parse_command_line(context->settings, fargc, fargv, FALSE);
	if (status != 0)
	{
		if (showRdkHelp)
			rdk_usage();
		freerdp_client_settings_command_line_status_print(context->settings, status, fargc, fargv);
		rc = (status <= COMMAND_LINE_STATUS_PRINT && status >= COMMAND_LINE_STATUS_PRINT_LAST) ? 0 : 1;
		if (rc == 0 && freerdp_settings_get_bool(context->settings, FreeRDP_ListMonitors))
			rc = rdk_list_monitors() ? 0 : 1;
		goto cleanup;
	}

	if (!rdk_credentials_prepare(&credentials, context->settings, g_promptCredentials))
		goto cleanup;
	if (g_cameraEnabled)
	{
		const char* channel[] = { "rdpecam" };
		if (!freerdp_client_add_dynamic_channel(context->settings, ARRAYSIZE(channel), channel))
			goto cleanup;
	}

	reconnectSettings = freerdp_settings_clone(context->settings);
	if (!reconnectSettings)
		goto cleanup;
	for (;;)
	{
		if (!freerdp_connect(context->instance))
		{
			const UINT32 err = freerdp_get_last_error(context);
			fprintf(stderr, "rdk: connection failed: %s\n", freerdp_get_last_error_name(err));
			if (err == FREERDP_ERROR_AUTHENTICATION_FAILED)
				fprintf(stderr, "rdk: retry with /credentials to enter or replace saved credentials\n");
			rc = 1;
			goto cleanup;
		}
		(void)rdk_credentials_save(&credentials);
		printf("rdk: connected on %u monitor(s), %ux%u. Ctrl+Shift+F10 to minimize; Ctrl+Shift+F11 to reconnect; Ctrl+Shift+F12 to quit.\n",
		       freerdp_settings_get_uint32(context->settings, FreeRDP_MonitorCount),
		       freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth),
		       freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight));
		rdkContext* rdk = (rdkContext*)context;
		rc = rdk_run(rdk);
		if (!g_autoTokens)
			g_autoText = NULL;
		printf("rdk: shutdown: stopping keyboard capture\n");
		fflush(stdout);
		rdk_capture_stop(&rdk->capture);
		printf("rdk: shutdown: releasing remote keys\n");
		fflush(stdout);
		if (freerdp_is_active_state(context) && !rdk_keyboard_release_all(&rdk->keyboard))
			rc = 1;
		const BOOL reconnect = rdk->reconnectRequested && rc == 0;
		printf("rdk: shutdown: disconnecting RDP\n");
		fflush(stdout);
		freerdp_disconnect(context->instance);
		printf("rdk: shutdown: RDP disconnected\n");
		fflush(stdout);
		if (!reconnect)
			break;
		printf("rdk: shutdown: freeing RDP context for reconnect\n");
		fflush(stdout);
		freerdp_client_context_free(context);
		printf("rdk: shutdown: RDP context freed; reconnecting\n");
		fflush(stdout);
		context = rdk_session_recreate(&entry, reconnectSettings);
		if (!context)
		{
			rc = 1;
			goto cleanup;
		}
	}

cleanup:
	freerdp_settings_free(reconnectSettings);
	rdk_credentials_clear(&credentials);
	if (context)
	{
		printf("rdk: shutdown: freeing RDP context\n");
		fflush(stdout);
		freerdp_client_context_free(context);
		printf("rdk: shutdown: RDP context freed\n");
		fflush(stdout);
	}
	for (int i = 0; i < fargc; ++i)
		free(fargv[i]);
	free(fargv);
	return rc;
}
