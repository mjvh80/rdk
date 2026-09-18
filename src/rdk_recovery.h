#ifndef RDK_RECOVERY_H
#define RDK_RECOVERY_H

#include <freerdp/freerdp.h>

#define RDK_RECOVERY_LIMIT_MS 120000

typedef struct
{
	UINT64 deadline;
	UINT attempts;
} rdkRecovery;

BOOL rdk_recovery_configure(rdpContext* context);
BOOL rdk_recovery_retryable(UINT32 error, UINT32 serverError);
void rdk_recovery_init(rdkRecovery* recovery, UINT64 now);
BOOL rdk_recovery_next(rdkRecovery* recovery, UINT64 now, DWORD* delay);
DWORD rdk_recovery_run(freerdp* instance, BOOL (*events)(void*), void* user);

typedef struct rdkRecoveryDialog rdkRecoveryDialog;

rdkRecoveryDialog* rdk_recovery_dialog_new(UINT64 deadline);
BOOL rdk_recovery_dialog_arm(rdkRecoveryDialog* dialog, HANDLE abortEvent);
void rdk_recovery_dialog_disarm(rdkRecoveryDialog* dialog);
void rdk_recovery_dialog_update(rdkRecoveryDialog* dialog, UINT attempt, UINT64 retryAt);
void rdk_recovery_dialog_cancel(rdkRecoveryDialog* dialog);
DWORD rdk_recovery_dialog_status(rdkRecoveryDialog* dialog);
HANDLE rdk_recovery_dialog_event(rdkRecoveryDialog* dialog);
void rdk_recovery_dialog_free(rdkRecoveryDialog* dialog);

#endif