#include "rdk_recovery.h"
#include <freerdp/error.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

BOOL rdk_recovery_configure(rdpContext* context)
{
	return freerdp_settings_set_bool(context->settings, FreeRDP_AutoReconnectionEnabled, TRUE);
}

BOOL rdk_recovery_retryable(UINT32 error, UINT32 serverError)
{
	if (serverError != ERRINFO_SUCCESS)
		return FALSE;
	switch (error)
	{
		case FREERDP_ERROR_CONNECT_TRANSPORT_FAILED:
		case FREERDP_ERROR_CONNECT_FAILED:
		case FREERDP_ERROR_DNS_ERROR:
		case FREERDP_ERROR_DNS_NAME_NOT_FOUND:
		case FREERDP_ERROR_CONNECT_KDC_UNREACHABLE:
		case FREERDP_ERROR_CONNECT_ACTIVATION_TIMEOUT:
			return TRUE;
		default:
			return FALSE;
	}
}

void rdk_recovery_init(rdkRecovery* recovery, UINT64 now)
{
	*recovery = (rdkRecovery){ .deadline = now + RDK_RECOVERY_LIMIT_MS };
}

BOOL rdk_recovery_next(rdkRecovery* recovery, UINT64 now, DWORD* delay)
{
	if (now >= recovery->deadline)
		return FALSE;
	const DWORD delays[] = { 1000, 2000, 4000, 8000, 15000 };
	const size_t index = recovery->attempts < ARRAYSIZE(delays) ? recovery->attempts : ARRAYSIZE(delays) - 1;
	*delay = delays[index];
	if (recovery->deadline - now <= *delay)
		return FALSE;
	++recovery->attempts;
	return TRUE;
}

struct rdkRecoveryDialog
{
	SRWLOCK lock;
	HANDLE thread;
	HANDLE ready;
	HANDLE cancelled;
	HANDLE abortEvent;
	HDESK desktop;
	UINT64 deadline;
	UINT64 retryAt;
	UINT attempt;
	DWORD status;
	BOOL done;
};

static void rdk_recovery_dialog_stop(rdkRecoveryDialog* dialog, DWORD status)
{
	AcquireSRWLockExclusive(&dialog->lock);
	if (dialog->status == ERROR_SUCCESS)
		dialog->status = status;
	SetEvent(dialog->cancelled);
	if (dialog->abortEvent)
		SetEvent(dialog->abortEvent);
	ReleaseSRWLockExclusive(&dialog->lock);
}

void rdk_recovery_dialog_cancel(rdkRecoveryDialog* dialog)
{
	rdk_recovery_dialog_stop(dialog, ERROR_CANCELLED);
}

DWORD rdk_recovery_dialog_status(rdkRecoveryDialog* dialog)
{
	AcquireSRWLockShared(&dialog->lock);
	const DWORD status = dialog->status;
	ReleaseSRWLockShared(&dialog->lock);
	return status;
}

HANDLE rdk_recovery_dialog_event(rdkRecoveryDialog* dialog)
{
	return dialog->cancelled;
}

static HRESULT CALLBACK rdk_recovery_dialog_callback(HWND window, UINT message, WPARAM parameter,
                                                    LPARAM data, LONG_PTR context)
{
	(void)data;
	rdkRecoveryDialog* dialog = (rdkRecoveryDialog*)context;
	if (message == TDN_CREATED)
	{
		SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
		SetEvent(dialog->ready);
	}
	else if (message == TDN_BUTTON_CLICKED)
	{
		AcquireSRWLockShared(&dialog->lock);
		const BOOL done = dialog->done;
		ReleaseSRWLockShared(&dialog->lock);
		if (done)
			return S_OK;
		if (parameter == IDCANCEL)
		{
			rdk_recovery_dialog_cancel(dialog);
			SendMessageW(window, TDM_ENABLE_BUTTON, IDCANCEL, FALSE);
			SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)L"Cancelling connection recovery...");
		}
		return S_FALSE;
	}
	else if (message == TDN_TIMER)
	{
		const UINT64 now = GetTickCount64();
		if (now >= dialog->deadline)
			rdk_recovery_dialog_stop(dialog, ERROR_TIMEOUT);
		AcquireSRWLockShared(&dialog->lock);
		const BOOL done = dialog->done;
		const DWORD status = dialog->status;
		const UINT attempt = dialog->attempt;
		const UINT64 retryAt = dialog->retryAt;
		if (status && dialog->abortEvent)
			SetEvent(dialog->abortEvent);
		ReleaseSRWLockShared(&dialog->lock);
		if (done)
		{
			SendMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0);
			return S_OK;
		}
		WCHAR text[192];
		if (status == ERROR_TIMEOUT)
			wcscpy_s(text, ARRAYSIZE(text), L"Recovery time limit reached. Stopping the connection attempt...");
		else if (status)
			wcscpy_s(text, ARRAYSIZE(text), L"Cancelling connection recovery...");
		else if (retryAt > now)
			swprintf_s(text, ARRAYSIZE(text), L"Retry %u in %llu seconds.\n%llu seconds remaining.",
			    attempt, (retryAt - now + 999) / 1000, (dialog->deadline - now + 999) / 1000);
		else
			swprintf_s(text, ARRAYSIZE(text), L"Connecting: attempt %u.\n%llu seconds remaining.",
			    attempt, (dialog->deadline - now + 999) / 1000);
		SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)text);
		if (status)
			SendMessageW(window, TDM_ENABLE_BUTTON, IDCANCEL, FALSE);
	}
	return S_OK;
}

static DWORD WINAPI rdk_recovery_dialog_thread(LPVOID parameter)
{
	rdkRecoveryDialog* dialog = parameter;
	if (!SetThreadDesktop(dialog->desktop))
	{
		rdk_recovery_dialog_stop(dialog, GetLastError());
		SetEvent(dialog->ready);
		return 1;
	}
	TASKDIALOGCONFIG config = { 0 };
	config.cbSize = sizeof(config);
	config.hInstance = GetModuleHandleW(NULL);
	config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER | TDF_SIZE_TO_CONTENT;
	config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
	config.pszWindowTitle = L"rdk - Connection Recovery";
	config.pszMainInstruction = L"Connection interrupted";
	config.pszContent = L"Waiting to reconnect...";
	config.pszMainIcon = TD_INFORMATION_ICON;
	config.pfCallback = rdk_recovery_dialog_callback;
	config.lpCallbackData = (LONG_PTR)dialog;
	config.nDefaultButton = IDCANCEL;
	const HRESULT result = TaskDialogIndirect(&config, NULL, NULL, NULL);
	if (FAILED(result))
		rdk_recovery_dialog_stop(dialog, ERROR_GEN_FAILURE);
	SetEvent(dialog->ready);
	return FAILED(result) ? 1 : 0;
}

rdkRecoveryDialog* rdk_recovery_dialog_new(UINT64 deadline)
{
	rdkRecoveryDialog* dialog = calloc(1, sizeof(*dialog));
	if (!dialog)
		return NULL;
	InitializeSRWLock(&dialog->lock);
	dialog->deadline = deadline;
	dialog->desktop = GetThreadDesktop(GetCurrentThreadId());
	dialog->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	dialog->cancelled = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (dialog->ready && dialog->cancelled)
		dialog->thread = CreateThread(NULL, 0, rdk_recovery_dialog_thread, dialog, 0, NULL);
	if (!dialog->thread || WaitForSingleObject(dialog->ready, INFINITE) != WAIT_OBJECT_0 ||
	    rdk_recovery_dialog_status(dialog))
	{
		rdk_recovery_dialog_free(dialog);
		return NULL;
	}
	return dialog;
}

BOOL rdk_recovery_dialog_arm(rdkRecoveryDialog* dialog, HANDLE abortEvent)
{
	HANDLE duplicate = NULL;
	if (!DuplicateHandle(GetCurrentProcess(), abortEvent, GetCurrentProcess(), &duplicate,
	    0, FALSE, DUPLICATE_SAME_ACCESS))
		return FALSE;
	AcquireSRWLockExclusive(&dialog->lock);
	if (dialog->abortEvent)
		CloseHandle(dialog->abortEvent);
	dialog->abortEvent = duplicate;
	if (dialog->status)
		SetEvent(dialog->abortEvent);
	ReleaseSRWLockExclusive(&dialog->lock);
	return TRUE;
}

void rdk_recovery_dialog_disarm(rdkRecoveryDialog* dialog)
{
	AcquireSRWLockExclusive(&dialog->lock);
	if (dialog->abortEvent)
		CloseHandle(dialog->abortEvent);
	dialog->abortEvent = NULL;
	ReleaseSRWLockExclusive(&dialog->lock);
}

void rdk_recovery_dialog_update(rdkRecoveryDialog* dialog, UINT attempt, UINT64 retryAt)
{
	AcquireSRWLockExclusive(&dialog->lock);
	dialog->attempt = attempt;
	dialog->retryAt = retryAt;
	ReleaseSRWLockExclusive(&dialog->lock);
}

void rdk_recovery_dialog_free(rdkRecoveryDialog* dialog)
{
	if (!dialog)
		return;
	AcquireSRWLockExclusive(&dialog->lock);
	dialog->done = TRUE;
	ReleaseSRWLockExclusive(&dialog->lock);
	if (dialog->thread)
	{
		WaitForSingleObject(dialog->thread, INFINITE);
		CloseHandle(dialog->thread);
	}
	rdk_recovery_dialog_disarm(dialog);
	if (dialog->ready)
		CloseHandle(dialog->ready);
	if (dialog->cancelled)
		CloseHandle(dialog->cancelled);
	free(dialog);
}

DWORD rdk_recovery_run(freerdp* instance, BOOL (*events)(void*), void* user)
{
	rdpContext* context = instance->context;
	if (!rdk_recovery_retryable(freerdp_get_last_error(context), freerdp_error_info(instance)))
		return ERROR_CONNECTION_ABORTED;
	rdkRecovery recovery;
	rdk_recovery_init(&recovery, GetTickCount64());
	rdkRecoveryDialog* dialog = rdk_recovery_dialog_new(recovery.deadline);
	if (!dialog)
		return ERROR_GEN_FAILURE;
	DWORD result = ERROR_TIMEOUT;
	DWORD delay = 0;
	while (rdk_recovery_next(&recovery, GetTickCount64(), &delay))
	{
		const UINT64 retryAt = GetTickCount64() + delay;
		rdk_recovery_dialog_update(dialog, recovery.attempts, retryAt);
		printf("rdk: recovery: attempt %u in %lu ms\n", recovery.attempts, delay);
		fflush(stdout);
		for (;;)
		{
			if (!events(user))
				rdk_recovery_dialog_cancel(dialog);
			result = rdk_recovery_dialog_status(dialog);
			if (result != ERROR_SUCCESS)
				goto finished;
			const UINT64 now = GetTickCount64();
			if (now >= recovery.deadline)
			{
				result = ERROR_TIMEOUT;
				goto finished;
			}
			if (now >= retryAt)
				break;
			const DWORD wait = retryAt - now < 100 ? (DWORD)(retryAt - now) : 100;
			const HANDLE cancelled = rdk_recovery_dialog_event(dialog);
			if (MsgWaitForMultipleObjectsEx(1, &cancelled, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
			{
				result = ERROR_GEN_FAILURE;
				goto finished;
			}
		}
		rdk_recovery_dialog_update(dialog, recovery.attempts, 0);
		if (!rdk_recovery_dialog_arm(dialog, freerdp_abort_event(context)))
		{
			result = ERROR_GEN_FAILURE;
			break;
		}
		const BOOL connected = freerdp_reconnect(instance);
		rdk_recovery_dialog_disarm(dialog);
		if (!events(user))
			rdk_recovery_dialog_cancel(dialog);
		result = rdk_recovery_dialog_status(dialog);
		if (result != ERROR_SUCCESS)
			break;
		if (GetTickCount64() >= recovery.deadline)
		{
			result = ERROR_TIMEOUT;
			break;
		}
		if (connected)
		{
			printf("rdk: recovery: connection restored after %u attempt(s); startup input will not be replayed\n", recovery.attempts);
			fflush(stdout);
			break;
		}
		const UINT32 error = freerdp_get_last_error(context);
		printf("rdk: recovery: attempt %u failed: %s\n", recovery.attempts, freerdp_get_last_error_name(error));
		fflush(stdout);
		if (!rdk_recovery_retryable(error, freerdp_error_info(instance)))
		{
			result = ERROR_CONNECTION_ABORTED;
			break;
		}
		result = ERROR_TIMEOUT;
	}
finished:
	if (result != ERROR_SUCCESS)
		SetEvent(freerdp_abort_event(context));
	rdk_recovery_dialog_free(dialog);
	return result;
}