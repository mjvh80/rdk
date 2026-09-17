#include <freerdp/client.h>
#include <windows.h>
#include <powrprof.h>
#include <stdio.h>

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "%s:%d: %s (error=%lu)\n", __func__, __LINE__, #expression, GetLastError()); return 1; \
} } while (0)

static DWORD registrationError;
static UINT unregisterCount;
static BOOL nativeRegistration;

static DWORD test_register(DWORD flags, HANDLE recipient, PHPOWERNOTIFY registration)
{
	if (nativeRegistration)
		return PowerRegisterSuspendResumeNotification(flags, recipient, registration);
	if (flags != DEVICE_NOTIFY_CALLBACK || !recipient)
		return ERROR_INVALID_PARAMETER;
	*registration = (HPOWERNOTIFY)1;
	return registrationError;
}

static DWORD test_unregister(HPOWERNOTIFY registration)
{
	if (nativeRegistration)
		return PowerUnregisterSuspendResumeNotification(registration);
	if (registration == (HPOWERNOTIFY)1)
		++unregisterCount;
	return ERROR_SUCCESS;
}

#define PowerRegisterSuspendResumeNotification test_register
#define PowerUnregisterSuspendResumeNotification test_unregister
#include "../src/rdk_power.c"
#undef PowerRegisterSuspendResumeNotification
#undef PowerUnregisterSuspendResumeNotification

static DWORD WINAPI notify_suspend(LPVOID parameter)
{
	rdkPower* power = parameter;
	return power->subscription.Callback(power->subscription.Context, PBT_APMSUSPEND, NULL);
}

int main(void)
{
	registrationError = ERROR_ACCESS_DENIED;
	CHECK(!rdk_power_new() && GetLastError() == ERROR_ACCESS_DENIED);
	registrationError = ERROR_SUCCESS;
	rdkPower* power = rdk_power_new();
	CHECK(power && !rdk_power_interrupted(power) && rdk_power_wait_resume(power));
	HANDLE abortEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(abortEvent && rdk_power_arm(power, abortEvent));
	CHECK(rdk_power_callback(power, PBT_POWERSETTINGCHANGE, NULL) == ERROR_SUCCESS);
	CHECK(!rdk_power_interrupted(power) && WaitForSingleObject(abortEvent, 0) == WAIT_TIMEOUT);
	HANDLE thread = CreateThread(NULL, 0, notify_suspend, power, 0, NULL);
	CHECK(thread);
	CHECK(WaitForSingleObject(abortEvent, 5000) == WAIT_OBJECT_0);
	CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
	CloseHandle(thread);
	CHECK(rdk_power_interrupted(power));
	CHECK(WaitForSingleObject(power->resumedEvent, 0) == WAIT_TIMEOUT);
	CHECK(rdk_power_callback(power, PBT_APMRESUMEAUTOMATIC, NULL) == ERROR_SUCCESS);
	CHECK(rdk_power_wait_resume(power) && rdk_power_interrupted(power));
	rdk_power_disarm(power);
	ResetEvent(abortEvent);
	CHECK(rdk_power_callback(power, PBT_APMRESUMESUSPEND, NULL) == ERROR_SUCCESS);
	CHECK(WaitForSingleObject(abortEvent, 0) == WAIT_TIMEOUT);
	CHECK(rdk_power_arm(power, abortEvent) && !rdk_power_interrupted(power));
	CHECK(rdk_power_callback(power, PBT_APMRESUMESUSPEND, NULL) == ERROR_SUCCESS);
	CHECK(!rdk_power_interrupted(power) && WaitForSingleObject(abortEvent, 0) == WAIT_TIMEOUT);
	CHECK(rdk_power_callback(power, PBT_APMRESUMEAUTOMATIC, NULL) == ERROR_SUCCESS);
	CHECK(rdk_power_interrupted(power) && WaitForSingleObject(abortEvent, 0) == WAIT_OBJECT_0);
	rdk_power_disarm(power);
	ResetEvent(abortEvent);
	CHECK(rdk_power_callback(power, PBT_APMSUSPEND, NULL) == ERROR_SUCCESS);
	CHECK(rdk_power_arm(power, abortEvent) && rdk_power_interrupted(power));
	CHECK(WaitForSingleObject(abortEvent, 0) == WAIT_OBJECT_0);
	CloseHandle(abortEvent);
	CHECK(rdk_power_callback(power, PBT_APMRESUMEAUTOMATIC, NULL) == ERROR_SUCCESS);
	rdk_power_free(power);
	CHECK(unregisterCount == 1);
	rdk_power_free(NULL);

	power = rdk_power_new();
	CHECK(power);
	RDP_CLIENT_ENTRY_POINTS entry = { 0 };
	entry.Size = sizeof(entry);
	entry.Version = RDP_CLIENT_INTERFACE_VERSION;
	entry.ContextSize = sizeof(rdpClientContext);
	for (UINT iteration = 0; iteration < 3; ++iteration)
	{
		rdpContext* context = freerdp_client_context_new(&entry);
		CHECK(context && !freerdp_shall_disconnect_context(context));
		CHECK(rdk_power_arm(power, freerdp_abort_event(context)));
		CHECK(!rdk_power_interrupted(power));
		CHECK(rdk_power_callback(power, PBT_APMSUSPEND, NULL) == ERROR_SUCCESS);
		CHECK(freerdp_shall_disconnect_context(context));
		freerdp_client_context_free(context);
		CHECK(rdk_power_callback(power, PBT_APMRESUMEAUTOMATIC, NULL) == ERROR_SUCCESS);
		CHECK(rdk_power_wait_resume(power));
		rdk_power_disarm(power);
	}
	rdk_power_free(power);

	nativeRegistration = TRUE;
	power = rdk_power_new();
	CHECK(power);
	rdk_power_free(power);
	puts("Passed native notification registration, FreeRDP cancellation, suspend/resume, duplicate resume, and stale handle tests");
	return 0;
}