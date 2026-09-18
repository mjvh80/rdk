#include "rdk_recovery.h"
#include <freerdp/client.h>
#include <freerdp/error.h>
#include <commctrl.h>
#include <winpr/wlog.h>
#include <stdio.h>

static UINT reconnectCalls;
static UINT32 reconnectError;
static BOOL cancelDuringConnect;
static UINT transientFailures;

static BOOL test_reconnect(freerdp* instance)
{
	++reconnectCalls;
	freerdp_set_last_error(instance->context, FREERDP_ERROR_SUCCESS);
	if (cancelDuringConnect)
	{
		HWND window = FindWindowW(NULL, L"rdk - Connection Recovery");
		if (window)
			SendMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0);
		return FALSE;
	}
	if (transientFailures)
	{
		--transientFailures;
		freerdp_set_last_error(instance->context, FREERDP_ERROR_CONNECT_TRANSPORT_FAILED);
		return FALSE;
	}
	freerdp_set_last_error(instance->context, reconnectError);
	return reconnectError == FREERDP_ERROR_SUCCESS;
}

#define freerdp_reconnect test_reconnect
#include "../src/rdk_recovery.c"
#undef freerdp_reconnect

static BOOL recovery_events(void* user)
{
	MSG message;
	while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
		DispatchMessageW(&message);
	return *(BOOL*)user;
}

static UINT keepaliveMessages;

static BOOL keepalive_message(const wLogMessage* message)
{
	if (message->TextString && strstr(message->TextString,
	    "Windows TCP keepalive configured: idle=5 interval=2 probes=3"))
		++keepaliveMessages;
	return TRUE;
}

static BOOL test_pre_connect(freerdp* instance)
{
	(void)instance;
	return TRUE;
}

static DWORD WINAPI reject_connection(LPVOID user)
{
	const SOCKET listener = *(SOCKET*)user;
	const SOCKET accepted = accept(listener, NULL, NULL);
	closesocket(listener);
	if (accepted == INVALID_SOCKET)
		return 1;
	closesocket(accepted);
	return 0;
}

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expression); return 1; \
} } while (0)

int main(void)
{
	const UINT32 transient[] = { FREERDP_ERROR_CONNECT_TRANSPORT_FAILED, FREERDP_ERROR_CONNECT_FAILED,
	    FREERDP_ERROR_DNS_ERROR, FREERDP_ERROR_DNS_NAME_NOT_FOUND, FREERDP_ERROR_CONNECT_KDC_UNREACHABLE,
	    FREERDP_ERROR_CONNECT_ACTIVATION_TIMEOUT };
	for (size_t index = 0; index < ARRAYSIZE(transient); ++index)
	{
		CHECK(rdk_recovery_retryable(transient[index], ERRINFO_SUCCESS));
		CHECK(!rdk_recovery_retryable(transient[index], ERRINFO_RPC_INITIATED_DISCONNECT));
		CHECK(!rdk_recovery_retryable(transient[index], ERRINFO_LOGOFF_BY_USER));
		CHECK(!rdk_recovery_retryable(transient[index], ERRINFO_IDLE_TIMEOUT));
		CHECK(!rdk_recovery_retryable(transient[index], ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION));
	}
	const UINT32 terminal[] = { FREERDP_ERROR_SUCCESS, FREERDP_ERROR_CONNECT_CANCELLED,
	    FREERDP_ERROR_AUTHENTICATION_FAILED, FREERDP_ERROR_CONNECT_WRONG_PASSWORD,
	    FREERDP_ERROR_CONNECT_PASSWORD_EXPIRED, FREERDP_ERROR_CONNECT_ACCOUNT_LOCKED_OUT,
	    FREERDP_ERROR_TLS_CONNECT_FAILED, FREERDP_ERROR_SECURITY_NEGO_CONNECT_FAILED,
	    FREERDP_ERROR_PRE_CONNECT_FAILED, FREERDP_ERROR_POST_CONNECT_FAILED,
	    FREERDP_ERROR_RPC_INITIATED_LOGOFF, FREERDP_ERROR_RPC_INITIATED_DISCONNECT };
	for (size_t index = 0; index < ARRAYSIZE(terminal); ++index)
		CHECK(!rdk_recovery_retryable(terminal[index], ERRINFO_SUCCESS));
	rdkRecovery recovery;
	UINT64 now = 500;
	rdk_recovery_init(&recovery, now);
	const DWORD expected[] = { 1000, 2000, 4000, 8000, 15000, 15000 };
	for (size_t index = 0; index < ARRAYSIZE(expected); ++index)
	{
		DWORD delay = 0;
		CHECK(rdk_recovery_next(&recovery, now, &delay));
		CHECK(delay == expected[index] && recovery.attempts == index + 1);
		now += delay;
	}
	DWORD delay = 0;
	CHECK(!rdk_recovery_next(&recovery, recovery.deadline - 15000, &delay));
	CHECK(!rdk_recovery_next(&recovery, recovery.deadline, &delay));
	CHECK(!rdk_recovery_next(&recovery, recovery.deadline + 1, &delay));
	rdk_recovery_init(&recovery, now);
	CHECK(recovery.attempts == 0 && recovery.deadline == now + RDK_RECOVERY_LIMIT_MS);
	HWINSTA originalStation = GetProcessWindowStation();
	HDESK originalDesktop = GetThreadDesktop(GetCurrentThreadId());
	HWINSTA station = CreateWindowStationW(NULL, 0, WINSTA_ALL_ACCESS, NULL);
	CHECK(station && SetProcessWindowStation(station));
	HDESK desktop = CreateDesktopW(L"rdkRecoveryTest", NULL, NULL, 0, GENERIC_ALL, NULL);
	CHECK(desktop && SetThreadDesktop(desktop));
	rdkRecoveryDialog* dialog = rdk_recovery_dialog_new(GetTickCount64() + 10000);
	CHECK(dialog && !rdk_recovery_dialog_status(dialog));
	HANDLE abortEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(abortEvent && rdk_recovery_dialog_arm(dialog, abortEvent));
	rdk_recovery_dialog_update(dialog, 1, GetTickCount64() + 1000);
	CHECK(WaitForSingleObject(abortEvent, 0) == WAIT_TIMEOUT);
	rdk_recovery_dialog_cancel(dialog);
	CHECK(WaitForSingleObject(abortEvent, 1000) == WAIT_OBJECT_0);
	CHECK(WaitForSingleObject(rdk_recovery_dialog_event(dialog), 0) == WAIT_OBJECT_0);
	CHECK(rdk_recovery_dialog_status(dialog) == ERROR_CANCELLED);
	CloseHandle(abortEvent);
	rdk_recovery_dialog_cancel(dialog);
	rdk_recovery_dialog_disarm(dialog);
	rdk_recovery_dialog_free(dialog);
	dialog = rdk_recovery_dialog_new(GetTickCount64() + 1000);
	CHECK(dialog);
	abortEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(abortEvent && rdk_recovery_dialog_arm(dialog, abortEvent));
	CHECK(WaitForSingleObject(rdk_recovery_dialog_event(dialog), 5000) == WAIT_OBJECT_0);
	CHECK(rdk_recovery_dialog_status(dialog) == ERROR_TIMEOUT);
	CHECK(WaitForSingleObject(abortEvent, 0) == WAIT_OBJECT_0);
	rdk_recovery_dialog_free(dialog);
	CloseHandle(abortEvent);
	dialog = rdk_recovery_dialog_new(GetTickCount64() + 10000);
	CHECK(dialog);
	rdk_recovery_dialog_free(dialog);
	RDP_CLIENT_ENTRY_POINTS entry = { 0 };
	entry.Size = sizeof(entry);
	entry.Version = RDP_CLIENT_INTERFACE_VERSION;
	entry.ContextSize = sizeof(rdpClientContext);
	rdpContext* context = freerdp_client_context_new(&entry);
	CHECK(context);
	WSADATA socketData;
	CHECK(WSAStartup(MAKEWORD(2, 2), &socketData) == 0);
	CHECK(rdk_recovery_configure(context));
	CHECK(freerdp_settings_get_bool(context->settings, FreeRDP_AutoReconnectionEnabled));
	CHECK(rdk_recovery_configure(context));
	wLog* tcpLog = WLog_Get("com.freerdp.core");
	CHECK(tcpLog && WLog_SetLogLevel(tcpLog, WLOG_DEBUG));
	CHECK(WLog_SetLogAppenderType(tcpLog, WLOG_APPENDER_CALLBACK));
	wLogCallbacks callbacks = { 0 };
	callbacks.message = keepalive_message;
	CHECK(WLog_ConfigureAppender(WLog_GetLogAppender(tcpLog), "callbacks", &callbacks));
	for (UINT enabled = 0; enabled < 2; ++enabled)
	{
		SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		CHECK(listener != INVALID_SOCKET);
		struct sockaddr_in address = { 0 };
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		CHECK(bind(listener, (struct sockaddr*)&address, sizeof(address)) == 0);
		CHECK(listen(listener, 1) == 0);
		int length = sizeof(address);
		CHECK(getsockname(listener, (struct sockaddr*)&address, &length) == 0);
		HANDLE listenerThread = CreateThread(NULL, 0, reject_connection, &listener, 0, NULL);
		CHECK(listenerThread);
		rdpContext* probe = freerdp_client_context_new(&entry);
		CHECK(probe);
		probe->instance->PreConnect = test_pre_connect;
		CHECK(freerdp_settings_set_string(probe->settings, FreeRDP_ServerHostname, "127.0.0.1"));
		CHECK(freerdp_settings_set_uint32(probe->settings, FreeRDP_ServerPort, ntohs(address.sin_port)));
		CHECK(freerdp_settings_set_uint32(probe->settings, FreeRDP_TcpConnectTimeout, 1000));
		CHECK(freerdp_settings_set_string(probe->settings, FreeRDP_Username, "rdk-loopback-test"));
		if (enabled)
			CHECK(rdk_recovery_configure(probe));
		CHECK(!freerdp_connect(probe->instance));
		printf("Verified Windows keepalive applications: %u (recovery enabled=%u)\n", keepaliveMessages, enabled);
		CHECK((keepaliveMessages != 0) == (enabled != 0));
		CHECK(WaitForSingleObject(listenerThread, 5000) == WAIT_OBJECT_0);
		DWORD listenerResult = 1;
		CHECK(GetExitCodeThread(listenerThread, &listenerResult) && listenerResult == 0);
		CloseHandle(listenerThread);
		freerdp_client_context_free(probe);
	}
	CHECK(WLog_SetLogAppenderType(tcpLog, WLOG_APPENDER_CONSOLE));
	CHECK(WLog_SetLogLevel(tcpLog, WLOG_WARN));
	WSACleanup();
	BOOL keepRunning = TRUE;
	freerdp_set_last_error(context, FREERDP_ERROR_CONNECT_TRANSPORT_FAILED);
	CHECK(rdk_recovery_run(context->instance, recovery_events, &keepRunning) == ERROR_SUCCESS);
	CHECK(reconnectCalls == 1);
	CHECK(!freerdp_shall_disconnect_context(context));
	reconnectError = FREERDP_ERROR_AUTHENTICATION_FAILED;
	freerdp_set_last_error(context, FREERDP_ERROR_CONNECT_TRANSPORT_FAILED);
	CHECK(rdk_recovery_run(context->instance, recovery_events, &keepRunning) == ERROR_CONNECTION_ABORTED);
	CHECK(reconnectCalls == 2);
	CHECK(freerdp_shall_disconnect_context(context));
	ResetEvent(freerdp_abort_event(context));
	freerdp_set_last_error(context, FREERDP_ERROR_CONNECT_TRANSPORT_FAILED);
	keepRunning = FALSE;
	CHECK(rdk_recovery_run(context->instance, recovery_events, &keepRunning) == ERROR_CANCELLED);
	CHECK(reconnectCalls == 2);
	keepRunning = TRUE;
	cancelDuringConnect = TRUE;
	ResetEvent(freerdp_abort_event(context));
	freerdp_set_last_error(context, FREERDP_ERROR_CONNECT_TRANSPORT_FAILED);
	CHECK(rdk_recovery_run(context->instance, recovery_events, &keepRunning) == ERROR_CANCELLED);
	CHECK(reconnectCalls == 3 && freerdp_shall_disconnect_context(context));
	cancelDuringConnect = FALSE;
	reconnectError = FREERDP_ERROR_SUCCESS;
	transientFailures = 1;
	ResetEvent(freerdp_abort_event(context));
	freerdp_set_last_error(context, FREERDP_ERROR_SUCCESS);
	freerdp_set_last_error(context, FREERDP_ERROR_CONNECT_TRANSPORT_FAILED);
	CHECK(rdk_recovery_run(context->instance, recovery_events, &keepRunning) == ERROR_SUCCESS);
	CHECK(reconnectCalls == 5 && transientFailures == 0);
	freerdp_client_context_free(context);
	CHECK(SetThreadDesktop(originalDesktop) && SetProcessWindowStation(originalStation));
	CloseDesktop(desktop);
	CloseWindowStation(station);
	puts("Passed transient recovery classification, server disconnect precedence, backoff, and deadline tests");
	return 0;
}