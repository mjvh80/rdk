#include "rdk_power.h"
#include <powrprof.h>
#include <stdlib.h>

struct rdkPower
{
	SRWLOCK lock;
	HPOWERNOTIFY notification;
	DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS subscription;
	HANDLE abortEvent;
	HANDLE resumedEvent;
	BOOL suspended;
	BOOL interrupted;
};

static ULONG CALLBACK rdk_power_callback(PVOID context, ULONG type, PVOID setting)
{
	(void)setting;
	rdkPower* power = context;
	if (type != PBT_APMSUSPEND && type != PBT_APMRESUMEAUTOMATIC && type != PBT_APMRESUMESUSPEND)
		return ERROR_SUCCESS;
	AcquireSRWLockExclusive(&power->lock);
	if (type == PBT_APMRESUMESUSPEND && !power->suspended)
	{
		ReleaseSRWLockExclusive(&power->lock);
		return ERROR_SUCCESS;
	}
	power->suspended = type == PBT_APMSUSPEND;
	if (power->suspended)
		ResetEvent(power->resumedEvent);
	else
		SetEvent(power->resumedEvent);
	if (power->abortEvent)
	{
		power->interrupted = TRUE;
		SetEvent(power->abortEvent);
	}
	ReleaseSRWLockExclusive(&power->lock);
	return ERROR_SUCCESS;
}

rdkPower* rdk_power_new(void)
{
	rdkPower* power = calloc(1, sizeof(*power));
	if (!power)
		return NULL;
	InitializeSRWLock(&power->lock);
	power->resumedEvent = CreateEventW(NULL, TRUE, TRUE, NULL);
	if (!power->resumedEvent)
	{
		free(power);
		return NULL;
	}
	power->subscription.Callback = rdk_power_callback;
	power->subscription.Context = power;
	const DWORD error = PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK,
	    &power->subscription, &power->notification);
	if (error != ERROR_SUCCESS)
	{
		CloseHandle(power->resumedEvent);
		free(power);
		SetLastError(error);
		return NULL;
	}
	return power;
}

BOOL rdk_power_arm(rdkPower* power, HANDLE abortEvent)
{
	if (!power || !abortEvent)
		return FALSE;
	HANDLE duplicate = NULL;
	if (!DuplicateHandle(GetCurrentProcess(), abortEvent, GetCurrentProcess(), &duplicate,
	    0, FALSE, DUPLICATE_SAME_ACCESS))
		return FALSE;
	AcquireSRWLockExclusive(&power->lock);
	if (power->abortEvent)
		CloseHandle(power->abortEvent);
	power->abortEvent = duplicate;
	power->interrupted = power->suspended;
	if (power->interrupted)
		SetEvent(power->abortEvent);
	ReleaseSRWLockExclusive(&power->lock);
	return TRUE;
}

BOOL rdk_power_interrupted(rdkPower* power)
{
	if (!power)
		return FALSE;
	AcquireSRWLockShared(&power->lock);
	const BOOL interrupted = power->interrupted;
	ReleaseSRWLockShared(&power->lock);
	return interrupted;
}

void rdk_power_disarm(rdkPower* power)
{
	if (!power)
		return;
	AcquireSRWLockExclusive(&power->lock);
	if (power->abortEvent)
	{
		CloseHandle(power->abortEvent);
		power->abortEvent = NULL;
	}
	ReleaseSRWLockExclusive(&power->lock);
}

BOOL rdk_power_wait_resume(rdkPower* power)
{
	return power && WaitForSingleObject(power->resumedEvent, INFINITE) == WAIT_OBJECT_0;
}

void rdk_power_free(rdkPower* power)
{
	if (!power)
		return;
	PowerUnregisterSuspendResumeNotification(power->notification);
	rdk_power_disarm(power);
	CloseHandle(power->resumedEvent);
	free(power);
}