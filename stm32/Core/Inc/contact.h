#ifndef __CONTACT_H__
#define __CONTACT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

#define CONTACT_FRAME_HEAD 0xA5U
#define CONTACT_FRAME_TAIL 0x5AU

#define CONTACT_SENSOR_CODE   0xB0U
#define CONTACT_ACTUATOR_CODE 0xC0U
#define CONTACT_LINK_HEARTBEAT_CODE 0x09U

#define CONTACT_TX_TIMEOUT_MS 20U

typedef enum {
    CONTACT_RK_SAFE = 0x00,
    CONTACT_RK_PPE_DENY = 0x01,
    CONTACT_RK_FIRE_WORK_ZONE = 0x02,
    CONTACT_RK_FIRE_OUT_ZONE = 0x03,
    CONTACT_RK_INTRUSION = 0x04,
    CONTACT_RK_ENV_DANGER = 0x05,
    CONTACT_RK_FAULT = 0x06,
    CONTACT_RK_RESERVED = 0x07,
    CONTACT_RK_EMERGENCY_ACK = 0x08,
    CONTACT_RK_LINK_HEARTBEAT = CONTACT_LINK_HEARTBEAT_CODE
} ContactRkCommand;

typedef enum {
    CONTACT_STM32_ONLINE_SAFE = 0x80,
    CONTACT_STM32_RUNNING = 0x81,
    CONTACT_STM32_POWER_OFF = 0x82,
    CONTACT_STM32_INTERLOCK = 0x83,
    CONTACT_STM32_EMERGENCY_STOP = 0x85,
    CONTACT_STM32_FAULT = 0x86,
    CONTACT_STM32_RESET_WAIT = 0x87,
    CONTACT_STM32_RESET_OK = 0x88
} ContactStm32Status;

typedef struct {
    uint8_t power;
    uint8_t power1;
    uint8_t power2;
    uint8_t fan;
    uint8_t alarm;
} ContactActuatorState;

typedef struct {
    uint16_t smoke;
    uint16_t gas;
    int16_t temperature_x10;
} ContactSensorData;

void Contact_Init(UART_HandleTypeDef *huart);
void Contact_Task(uint32_t now_ms);
void Contact_UART_RxCpltCallback(UART_HandleTypeDef *huart);
void Contact_BeginResetRequest(void);
uint8_t Contact_IsRkOnline(uint32_t now_ms);

HAL_StatusTypeDef Contact_SendStatus(ContactStm32Status status);
HAL_StatusTypeDef Contact_SendSensorData(uint16_t smoke,
                                         uint16_t gas,
                                         int16_t temperature_x10);
HAL_StatusTypeDef Contact_SendActuatorState(uint8_t power,
                                            uint8_t fan,
                                            uint8_t alarm);
HAL_StatusTypeDef Contact_SendActuatorState2(uint8_t power1,
                                             uint8_t power2,
                                             uint8_t fan,
                                             uint8_t alarm);

ContactRkCommand Contact_GetLastCommand(void);
uint8_t Contact_HasNewCommand(void);
ContactSensorData Contact_GetLastSensorData(void);
ContactActuatorState Contact_GetLastActuatorState(void);

void Contact_OnCommand(ContactRkCommand command);

/*
 * LoRa 串口建议配置�? * USART3 -> PB10(TX), PB11(RX)
 * 只保�?USART3，原 USART1/PA9/PA10 可不用�? */

#ifdef __cplusplus
}
#endif

#endif /* __CONTACT_H__ */

