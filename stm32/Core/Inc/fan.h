#ifndef __FAN_H__
#define __FAN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

#define FAN_DUTY_MAX 1000U

void Fan_Init(void);
void Fan_On(void);
void Fan_Off(void);
void Fan_SetDutyPermille(uint16_t duty_permille);
uint16_t Fan_GetDutyPermille(void);
uint8_t Fan_IsOn(void);

#ifdef __cplusplus
}
#endif

#endif /* __FAN_H__ */
