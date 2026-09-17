#ifndef RDK_POWER_H
#define RDK_POWER_H

#include <windows.h>

typedef struct rdkPower rdkPower;

rdkPower* rdk_power_new(void);
BOOL rdk_power_arm(rdkPower* power, HANDLE abortEvent);
BOOL rdk_power_interrupted(rdkPower* power);
void rdk_power_disarm(rdkPower* power);
BOOL rdk_power_wait_resume(rdkPower* power);
void rdk_power_free(rdkPower* power);

#endif