#ifndef __BUTTONS_H__
#define __BUTTONS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

void Buttons_Init(void);
void Buttons_Task(uint32_t now_ms);
uint8_t Buttons_FireTestActive(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* __BUTTONS_H__ */
