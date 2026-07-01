#ifndef __POWER_H__
#define __POWER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

void Power_Init(void);
void Power_On(void);
void Power_Off(void);
uint8_t Power_IsOn(void);
void Power1_On(void);
void Power1_Off(void);
uint8_t Power1_IsOn(void);
void Power2_On(void);
void Power2_Off(void);
uint8_t Power2_IsOn(void);

#ifdef __cplusplus
}
#endif

#endif /* __POWER_H__ */
