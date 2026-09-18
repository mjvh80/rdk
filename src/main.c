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
#include "rdk_graphics.h"
#include "rdk_latency.h"
#include "rdk_crash.h"
#include "rdk_power.h"
#include "rdk_recovery.h"
#include "rdk_camera.h"
#include "rdk_session.h"
#include "rdk_startup.h"
#include "rdk_taskbar.h"

#include <freerdp/client/cmdline.h>
#include <freerdp/scancode.h>
#include <freerdp/error.h>

#define RDK_MAX_HANDLES (MAXIMUM_WAIT_OBJECTS - 1)

/* Options. /text auto-types once after connect (handy for testing the
 * Unicode path); it is not sent unless requested. */
static const WCHAR* g_autoText = NULL;
static BOOL g_autoTokens = FALSE;
static DWORD g_startupDelayMs = 3000;
static DWORD g_charDelayMs = 0;
static BOOL g_promptCredentials = FALSE;
static BOOL g_cameraEnabled = FALSE;
static BOOL g_latency = FALSE;
static BOOL g_recover = FALSE;

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
	rdk_graphics_log_settings(settings);
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

static int rdk_run(rdkContext* rdk, rdkPower* power, BOOL replayStartup)
{
	rdpContext* context = (rdpContext*)rdk;
	rdkStartup startup;
	rdk_startup_init(&startup, replayStartup ? g_autoText : NULL, g_autoTokens, g_startupDelayMs, g_charDelayMs);
	BOOL activated = FALSE;
	if (startup.next && *startup.next)
	{
		printf("rdk: startup input queued for this connection; waiting for the desktop, focus, and physical key release\n");
		fflush(stdout);
	}

	while (!rdk_power_interrupted(power) && !freerdp_shall_disconnect_context(context) && !rdk->quit)
	{
		const DWORD captureError = rdk_capture_error(&rdk->capture);
		if (captureError)
		{
			rdk->stopReason = "keyboard capture failed";
			fprintf(stderr, "rdk: keyboard capture failed (%lu)\n", captureError);
			return 1;
		}
		if (!activated && freerdp_is_active_state(context))
		{
			if (replayStartup && !IsIconic(rdk->hwnd))
				rdk_gdi_activate(rdk);
			activated = TRUE;
		}
		HANDLE handles[RDK_MAX_HANDLES];
		const DWORD nCount = freerdp_get_event_handles(context, handles, RDK_MAX_HANDLES);
		if (nCount == 0)
		{
			rdk->stopReason = "FreeRDP returned no event handles";
			fprintf(stderr, "rdk: failed to get event handles\n");
			return 1;
		}

		const BOOL pending = startup.next && *startup.next;
		const DWORD timeout = rdk_startup_wait(&startup, GetTickCount64(), pending && rdk_startup_ready(rdk));
		const DWORD status =
		    MsgWaitForMultipleObjectsEx(nCount, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		if (rdk_power_interrupted(power))
			break;
		if (status == WAIT_FAILED)
		{
			rdk->stopReason = "Windows event wait failed";
			char error[128];
			snprintf(error, sizeof(error), "rdk: MsgWaitForMultipleObjectsEx failed (Windows error=%lu)", GetLastError());
			fprintf(stderr, "%s\n", error);
			rdk_crash_message(error);
			return 1;
		}

		MSG msg;
		UINT processed = 0;
		while (processed++ < 64 && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (msg.message == RDK_WM_KEY)
				rdk_latency_message(RDK_LATENCY_QUEUE, msg.time);
			if (msg.message == WM_QUIT)
			{
				rdk->stopReason = "Windows quit message received";
				rdk->quit = TRUE;
				break;
			}
			if ((!rdk->sessionMenu || !IsDialogMessageW(rdk->sessionMenu, &msg)) &&
			    (!rdk->notice || !IsDialogMessageW(rdk->notice, &msg)))
				DispatchMessageW(&msg);
			if (rdk->quit)
				break;
		}
		if (rdk->quit || rdk_power_interrupted(power))
			break;
		const UINT64 devicesStarted = rdk_latency_begin();
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
		rdk_latency_end(RDK_LATENCY_DEVICES, devicesStarted);
		if (rdk_power_interrupted(power))
			break;

		const UINT64 eventsStarted = rdk_latency_begin();
		const BOOL eventsOk = freerdp_check_event_handles(context);
		rdk_latency_end(RDK_LATENCY_NETWORK, eventsStarted);
		if (rdk_power_interrupted(power))
			break;
		if (!eventsOk)
		{
			const UINT32 err = freerdp_get_last_error(context);
			rdk->stopReason = err == FREERDP_ERROR_SUCCESS ?
			    "FreeRDP event processing ended the session" : "FreeRDP event processing failed";
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
				rdk->stopReason = "startup input could not be sent";
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
		if (g_latency)
			rdk_latency_report(GetTickCount64(), FALSE);
	}
	if (!rdk->stopReason)
		rdk->stopReason = rdk->reconnectRequested ? "reconnect requested" :
		    rdk->quit ? "local shutdown requested" : "FreeRDP requested disconnect";
	return rdk->inputFailed || (!rdk->quit && freerdp_get_last_error(context) != FREERDP_ERROR_SUCCESS) ? 1 : 0;
}

/* ---- entry point -------------------------------------------------------- */

static void rdk_report_exit(const char* action, const char* reason, int result, const rdpContext* context)
{
	char message[2048];
	snprintf(message, sizeof(message), "rdk: %s: %s (exit=%d)", action, reason, result);
	fprintf(result ? stderr : stdout, "%s\n", message);
	rdk_crash_message(message);
	if (context)
	{
		const UINT32 clientError = freerdp_get_last_error(context);
		const UINT32 serverError = context->instance ? freerdp_error_info(context->instance) : 0;
		snprintf(message, sizeof(message), "rdk: FreeRDP error=0x%08lX %s: %s; server disconnect=0x%08lX %s: %s",
		    (unsigned long)clientError, clientError ? freerdp_get_last_error_name(clientError) : "none",
		    clientError ? freerdp_get_last_error_string(clientError) : "no client error reported",
		    (unsigned long)serverError, serverError ? freerdp_get_error_info_name(serverError) : "none",
		    serverError ? freerdp_get_error_info_string(serverError) : "no server reason reported");
		fprintf(result ? stderr : stdout, "%s\n", message);
		rdk_crash_message(message);
		const DWORD captureError = rdk_capture_error(&((rdkContext*)context)->capture);
		if (captureError)
		{
			snprintf(message, sizeof(message), "rdk: keyboard capture Windows error=%lu", captureError);
			fprintf(stderr, "%s\n", message);
			rdk_crash_message(message);
		}
	}
	fflush(stdout);
	fflush(stderr);
}

typedef struct
{
	rdkContext* rdk;
	rdkPower* power;
} rdkRecoveryWindow;

static BOOL rdk_recovery_events(void* user)
{
	rdkRecoveryWindow* window = user;
	MSG message;
	UINT processed = 0;
	while (processed++ < 64 && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
	{
		if (message.message == WM_QUIT)
		{
			window->rdk->quit = TRUE;
			window->rdk->stopReason = "Windows quit message received during recovery";
			break;
		}
		DispatchMessageW(&message);
	}
	return !window->rdk->quit && !rdk_power_interrupted(window->power);
}

static int rdk_run_connected(rdkContext* rdk, rdkPower* power)
{
	rdpContext* context = (rdpContext*)rdk;
	BOOL replayStartup = TRUE;
	for (;;)
	{
		const int result = rdk_run(rdk, power, replayStartup);
		if (!g_recover || rdk_power_interrupted(power) || rdk->reconnectRequested ||
		    (rdk->quit && !rdk->inputFailed) ||
		    !rdk_recovery_retryable(freerdp_get_last_error(context), freerdp_error_info(context->instance)))
			return result;
		rdk_report_exit("recovering", "connection interrupted; attempting transport recovery", result, context);
		rdk_crash_stage("recovering-connection");
		rdk->quit = FALSE;
		rdk->inputFailed = FALSE;
		rdk->stopReason = NULL;
		(void)rdk_gdi_recovery(rdk, TRUE);
		rdkRecoveryWindow window = { rdk, power };
		const DWORD status = rdk_recovery_run(context->instance, rdk_recovery_events, &window);
		if (status != ERROR_SUCCESS)
		{
			if (!rdk->stopReason)
				rdk->stopReason = status == ERROR_CANCELLED ? "connection recovery cancelled" :
				    status == ERROR_TIMEOUT ? "connection recovery time limit reached" :
				    status == ERROR_CONNECTION_ABORTED ? "connection recovery stopped after a non-retryable error" :
				    "connection recovery controller failed";
			return status == ERROR_CANCELLED ? 0 : 1;
		}
		if (!rdk_gdi_recovery(rdk, FALSE))
			return 1;
		rdk_crash_message("rdk: connection recovered; startup input suppressed for this recovery");
		rdk_crash_stage("connected-event-loop");
		replayStartup = FALSE;
	}
}

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
	       "  /gfx:avc444           opt in to H.264 AVC444 for text/color detail (GDI display)\n"
	       "  /gfx:avc420           opt in to H.264 AVC420 for comparison\n"
	       "  (no /gfx option)      keep previous non-AVC graphics behavior\n"
	       "  /latency              log timing summaries every 5s (no key values/text)\n"
	       "  /recover              retry interrupted sessions for up to 2 minutes (Cancel available)\n"
	       "  /crash-dump           also write a minidump on fatal exceptions (sensitive memory)\n"
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
	       "Ctrl+Alt+End sends Ctrl+Alt+Delete remotely (once per press).\n"
	       "Shift+right-click a window's taskbar entry for Send Ctrl+Alt+Delete.\n"
	       "Ctrl+Shift+F9 opens the session menu; Escape closes it.\n"
	       "Minimize with Ctrl+Shift+F10; reconnect with Ctrl+Shift+F11; quit with Ctrl+Shift+F12.\n");
}

static int rdk_main(int argc, wchar_t** argv)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	if (argc <= 1)
	{
		rdk_usage();
		rdk_report_exit("exiting", "help displayed; no connection requested", 0, NULL);
		return 0;
	}

	/* Split our own options out; everything else is forwarded to FreeRDP. */
	char** fargv = (char**)calloc((size_t)argc + 1, sizeof(char*));
	if (!fargv)
	{
		rdk_report_exit("exiting", "out of memory allocating command-line storage", 1, NULL);
		return 1;
	}

	int fargc = 0;
	int rc = 1;
	const char* exitReason = "command-line allocation or UTF-8 conversion failed";
	BOOL exitReported = FALSE;
	BOOL listCameras = FALSE;
	BOOL listMicrophones = FALSE;
	BOOL startupDelaySet = FALSE;
	BOOL showRdkHelp = FALSE;
	rdpContext* context = NULL;
	rdpSettings* reconnectSettings = NULL;
	rdkPower* power = NULL;
	rdkCredentials credentials;
	rdk_credentials_init(&credentials);
	rdk_crash_stage("parsing-command-line");
	fargv[fargc++] = wide_to_utf8(argv[0] ? argv[0] : L"rdk");
	if (!fargv[0])
		goto cleanup;

	for (int i = 1; i < argc; ++i)
	{
		if (_wcsicmp(argv[i], L"/help") == 0 || wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"/?") == 0)
			showRdkHelp = TRUE;
		if (_wcsicmp(argv[i], L"/latency") == 0)
		{
			g_latency = TRUE;
			continue;
		}
		if (_wcsicmp(argv[i], L"/recover") == 0)
		{
			g_recover = TRUE;
			continue;
		}
		if (_wcsicmp(argv[i], L"/crash-dump") == 0)
		{
			rdk_crash_enable_dump();
			fprintf(stderr, "rdk: crash dumps enabled; dumps may contain credentials and remote-session data; keep them private\n");
			continue;
		}
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
				exitReason = textOption ? "/text requires a value" : inputOption ? "/input requires a value" :
				    startupOption ? "/startup-delay requires a value" : "/char-delay requires a value";
				fprintf(stderr, "rdk: %ls requires a value; use %ls:VALUE\n", option, option);
				goto cleanup;
			}
			if (!value)
				value = argv[++i];
			if (textOption || inputOption)
			{
				if (g_autoText && g_autoTokens != inputOption)
				{
					exitReason = "/input and /text cannot be combined";
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
					exitReason = startupOption ? "/startup-delay must be an integer from 0 to 4294967295" :
					    "/char-delay must be an integer from 0 to 4294967295";
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
			exitReason = "invalid /input token sequence";
			fprintf(stderr, "rdk: invalid /input at UTF-16 offset %zu; use <delay>, named keys, or << for a literal '<'\n", errorOffset);
			goto cleanup;
		}
		if (!startupDelaySet)
			g_startupDelayMs = 0;
	}
	if (listCameras || listMicrophones)
	{
		rdk_crash_stage("listing-local-devices");
		rc = 0;
		if (listMicrophones && !rdk_microphone_list()) rc = 1;
		if (listCameras && !rdk_camera_list()) rc = 1;
		exitReason = rc ? "local device listing failed" : "local device listing completed";
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

	exitReason = "FreeRDP context creation failed";
	rdk_crash_stage("creating-context");
	context = freerdp_client_context_new(&entry);
	if (!context)
	{
		fprintf(stderr, "rdk: failed to create FreeRDP context\n");
		rc = 1;
		goto cleanup;
	}

	exitReason = "FreeRDP command-line parsing failed";
	rdk_crash_stage("parsing-freerdp-options");
	const int status = rdk_graphics_parse(context->settings, fargc, fargv);
	if (status != 0)
	{
		if (showRdkHelp)
			rdk_usage();
		freerdp_client_settings_command_line_status_print(context->settings, status, fargc, fargv);
		rc = (status <= COMMAND_LINE_STATUS_PRINT && status >= COMMAND_LINE_STATUS_PRINT_LAST) ? 0 : 1;
		if (rc == 0 && freerdp_settings_get_bool(context->settings, FreeRDP_ListMonitors))
		{
			rc = rdk_list_monitors() ? 0 : 1;
			exitReason = rc ? "monitor listing failed" : "monitor listing completed";
		}
		else if (rc == 0)
			exitReason = "requested information displayed; no connection requested";
		goto cleanup;
	}

	exitReason = "credential preparation failed or was cancelled";
	rdk_crash_stage("preparing-credentials");
	if (!rdk_credentials_prepare(&credentials, context->settings, g_promptCredentials))
		goto cleanup;
	if (g_cameraEnabled)
	{
		exitReason = "camera channel configuration failed";
		const char* channel[] = { "rdpecam" };
		if (!freerdp_client_add_dynamic_channel(context->settings, ARRAYSIZE(channel), channel))
			goto cleanup;
	}

	exitReason = "could not preserve connection settings";
	reconnectSettings = freerdp_settings_clone(context->settings);
	if (!reconnectSettings)
		goto cleanup;
	power = rdk_power_new();
	if (!power)
	{
		const char* message = "rdk: sleep/resume notifications unavailable; automatic sleep cancellation is disabled";
		fprintf(stderr, "%s (Windows error=%lu)\n", message, GetLastError());
		rdk_crash_message(message);
	}
	for (;;)
	{
		if (g_recover && !rdk_recovery_configure(context))
		{
			exitReason = "could not configure connection recovery";
			rc = 1;
			goto cleanup;
		}
		if (power && !rdk_power_arm(power, freerdp_abort_event(context)))
		{
			exitReason = "could not arm sleep/resume connection cancellation";
			rc = 1;
			goto cleanup;
		}
		if (!rdk_latency_enable(g_latency))
		{
			exitReason = "latency timer initialization failed";
			fprintf(stderr, "rdk: latency timer initialization failed\n");
			goto cleanup;
		}
		if (g_latency)
		{
			printf("rdk: latency diagnostics enabled: 5s summaries; avg/max in ms, slow >= 50ms; no key values or text\n");
			fflush(stdout);
		}
		exitReason = "RDP connection failed";
		rdk_crash_stage("connecting");
		const BOOL connected = freerdp_connect(context->instance);
		if (!connected && !rdk_power_interrupted(power))
		{
			const UINT32 err = freerdp_get_last_error(context);
			fprintf(stderr, "rdk: connection failed: %s\n", freerdp_get_last_error_name(err));
			if (err == FREERDP_ERROR_AUTHENTICATION_FAILED)
				fprintf(stderr, "rdk: retry with /credentials to enter or replace saved credentials\n");
			rc = 1;
			goto cleanup;
		}
		rdkContext* rdk = (rdkContext*)context;
		if (connected)
		{
			(void)rdk_credentials_save(&credentials);
			printf("rdk: connected on %u monitor(s), %ux%u. Ctrl+Shift+F9 for session menu; Ctrl+Shift+F10 to minimize; Ctrl+Shift+F11 to reconnect; Ctrl+Shift+F12 to quit.\n",
			       freerdp_settings_get_uint32(context->settings, FreeRDP_MonitorCount),
			       freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth),
			       freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight));
			rdk_crash_stage("connected-event-loop");
			rc = rdk_run_connected(rdk, power);
		}
		const BOOL powerInterrupted = rdk_power_interrupted(power);
		const BOOL powerRecovery = powerInterrupted && !(rdk->quit && !rdk->inputFailed && !rdk->reconnectRequested);
		if (powerInterrupted)
		{
			SetEvent(freerdp_abort_event(context));
			if (powerRecovery)
			{
				rdk->stopReason = "PC sleep/resume interrupted the RDP connection";
				rc = 0;
			}
			rdk->focused = FALSE;
			rdk->mouseButtons = 0;
			rdk_capture_set_active(&rdk->capture, FALSE);
			if (rdk->hwnd && GetCapture() == rdk->hwnd)
				ReleaseCapture();
		}
		exitReason = rdk->stopReason ? rdk->stopReason : "session loop ended without a specific reason";
		rdk_report_exit(powerRecovery ? "disconnecting" : rdk->reconnectRequested && rc == 0 ? "reconnecting" : "exiting", exitReason, rc, context);
		exitReported = TRUE;
		if (g_latency)
			rdk_latency_report(GetTickCount64(), TRUE);
		if (!g_autoTokens)
			g_autoText = NULL;
		printf("rdk: shutdown: stopping keyboard capture\n");
		fflush(stdout);
		rdk_crash_stage("stopping-keyboard-capture");
		rdk_capture_stop(&rdk->capture);
		printf("rdk: shutdown: releasing remote keys\n");
		fflush(stdout);
		rdk_crash_stage("releasing-remote-keys");
		if (!powerInterrupted && !rdk->recovering && freerdp_is_active_state(context) && !rdk_keyboard_release_all(&rdk->keyboard))
		{
			rc = 1;
			exitReason = "failed to release remote keys during shutdown";
			exitReported = FALSE;
		}
		const BOOL reconnect = powerRecovery || (rdk->reconnectRequested && rc == 0);
		printf("rdk: shutdown: disconnecting RDP\n");
		fflush(stdout);
		rdk_crash_stage("disconnecting");
		if (connected)
			freerdp_disconnect(context->instance);
		printf("rdk: shutdown: RDP disconnected\n");
		fflush(stdout);
		if (!reconnect)
			break;
		printf("rdk: shutdown: freeing RDP context for reconnect\n");
		fflush(stdout);
		rdk_crash_stage("freeing-context-for-reconnect");
		rdk_power_disarm(power);
		freerdp_client_context_free(context);
		context = NULL;
		if (powerRecovery)
		{
			rdk_crash_stage("waiting-for-resume");
			printf("rdk: old connection closed after sleep/resume; waiting to offer reconnect\n");
			fflush(stdout);
			if (!rdk_power_wait_resume(power))
			{
				exitReason = "waiting for PC resume failed";
				rc = 1;
				exitReported = FALSE;
				goto cleanup;
			}
			rdk_crash_stage("awaiting-resume-reconnect-choice");
			const int choice = MessageBoxW(NULL,
			    L"The RDP connection was interrupted by PC sleep or resume.\n\nReconnect to the remote session?",
			    L"rdk - Connection Interrupted", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
			if (choice != IDYES)
			{
				exitReason = choice ? "reconnect after sleep/resume declined" : "could not display resume reconnect prompt";
				rc = choice ? 0 : 1;
				exitReported = FALSE;
				goto cleanup;
			}
			rdk_crash_message("rdk: reconnect after sleep/resume accepted");
		}
		printf("rdk: shutdown: RDP context freed; reconnecting\n");
		fflush(stdout);
		rdk_crash_stage("recreating-context");
		context = rdk_session_recreate(&entry, reconnectSettings);
		exitReported = FALSE;
		if (!context)
		{
			exitReason = "could not recreate FreeRDP context for reconnect";
			rc = 1;
			goto cleanup;
		}
	}

cleanup:
	if (!exitReported)
		rdk_report_exit("exiting", exitReason, rc, context);
	rdk_power_free(power);
	rdk_crash_stage("cleanup");
	freerdp_settings_free(reconnectSettings);
	rdk_credentials_clear(&credentials);
	if (context)
	{
		printf("rdk: shutdown: freeing RDP context\n");
		fflush(stdout);
		rdk_crash_stage("freeing-context");
		freerdp_client_context_free(context);
		printf("rdk: shutdown: RDP context freed\n");
		fflush(stdout);
	}
	for (int i = 0; i < fargc; ++i)
		free(fargv[i]);
	free(fargv);
	return rc;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc == 2 && _wcsicmp(argv[1], L"/taskbar:minimize") == 0)
		return rdk_taskbar_minimize() < 0 ? 1 : 0;
	if (!rdk_crash_init(NULL))
		fprintf(stderr, "rdk: persistent diagnostic report unavailable; exit reasons will still be printed to the console\n");
	__try
	{
		const int result = rdk_main(argc, argv);
		rdk_crash_finish(result);
		return result;
	}
	__except (rdk_crash_filter(GetExceptionInformation()))
	{
		const DWORD code = GetExceptionCode();
		TerminateProcess(GetCurrentProcess(), code);
		return 1;
	}
}
