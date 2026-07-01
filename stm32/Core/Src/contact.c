#include "contact.h"

#include <string.h>
#include "fan.h"
#include "power.h"

#define CONTACT_RX_FRAME_SIZE 3U
#define CONTACT_ACTUATOR_RX_FRAME_SIZE 7U
#define CONTACT_ONLINE_PERIOD_MS 1000U
#define CONTACT_RK_TIMEOUT_MS 30000U
#define CONTACT_RESET_REPLY_WAIT_MS 5000U

static UART_HandleTypeDef *s_contact_uart = NULL;
static uint8_t s_rx_byte = 0U;
static uint8_t s_rx_frame[CONTACT_ACTUATOR_RX_FRAME_SIZE];
static uint8_t s_rx_index = 0U;
static volatile uint8_t s_new_command = 0U;
static ContactRkCommand s_last_command = CONTACT_RK_SAFE;
static ContactSensorData s_last_sensor = {0U, 0U, 0};
static ContactActuatorState s_last_actuator = {0U, 0U, 0U, 0U, 0U};
static uint32_t s_last_online_tick = 0U;
static uint32_t s_last_rk_rx_tick = 0U;
static uint8_t s_reset_pending = 0U;
static uint32_t s_reset_request_tick = 0U;

static HAL_StatusTypeDef Contact_SendFrame(const uint8_t *frame, uint16_t len)
{
    if (s_contact_uart == NULL || frame == NULL || len == 0U) {
        return HAL_ERROR;
    }

    return HAL_UART_Transmit(s_contact_uart,
                             (uint8_t *)frame,
                             len,
                             CONTACT_TX_TIMEOUT_MS);
}

static void Contact_RestartReceive(void)
{
    if (s_contact_uart != NULL) {
        (void)HAL_UART_Receive_IT(s_contact_uart, &s_rx_byte, 1U);
    }
}

static uint8_t Contact_IsValidRkCommand(uint8_t code)
{
    return (code <= CONTACT_RK_EMERGENCY_ACK ||
            code == CONTACT_LINK_HEARTBEAT_CODE);
}

static void Contact_ApplyActuatorTarget(const uint8_t *payload)
{
    if (payload[0]) {
        Power1_On();
    } else {
        Power1_Off();
    }

    if (payload[1]) {
        Power2_On();
    } else {
        Power2_Off();
    }

    if (payload[2]) {
        Fan_On();
    } else {
        Fan_Off();
    }

    s_last_actuator.power1 = Power1_IsOn();
    s_last_actuator.power2 = Power2_IsOn();
    s_last_actuator.power = Power_IsOn();
    s_last_actuator.fan = Fan_IsOn();
    s_last_actuator.alarm = payload[3] ? 1U : 0U;
}

static void Contact_HandleByte(uint8_t byte)
{
    if (s_rx_index == 0U) {
        if (byte == CONTACT_FRAME_HEAD) {
            s_rx_frame[s_rx_index++] = byte;
        }
        return;
    }

    if (s_rx_index < sizeof(s_rx_frame)) {
        s_rx_frame[s_rx_index++] = byte;
    } else {
        s_rx_index = 0U;
        return;
    }

    if (s_rx_index >= 2U &&
        s_rx_frame[1] == CONTACT_ACTUATOR_CODE &&
        s_rx_index < CONTACT_ACTUATOR_RX_FRAME_SIZE) {
        return;
    }

    if (s_rx_index == CONTACT_ACTUATOR_RX_FRAME_SIZE &&
        s_rx_frame[0] == CONTACT_FRAME_HEAD &&
        s_rx_frame[1] == CONTACT_ACTUATOR_CODE &&
        s_rx_frame[6] == CONTACT_FRAME_TAIL) {
        s_last_rk_rx_tick = HAL_GetTick();
        Contact_ApplyActuatorTarget(&s_rx_frame[2]);
        s_reset_pending = 0U;
        s_rx_index = 0U;
        return;
    }

    if (s_rx_index < CONTACT_RX_FRAME_SIZE) {
        return;
    }

    if (s_rx_frame[0] == CONTACT_FRAME_HEAD &&
        s_rx_frame[2] == CONTACT_FRAME_TAIL &&
        Contact_IsValidRkCommand(s_rx_frame[1])) {
        s_last_rk_rx_tick = HAL_GetTick();
        if (s_rx_frame[1] != CONTACT_LINK_HEARTBEAT_CODE) {
            if (!s_reset_pending) {
                s_last_command = (ContactRkCommand)s_rx_frame[1];
                s_new_command = 1U;
                Contact_OnCommand(s_last_command);
            }
        }
    }

    s_rx_index = 0U;
    if (byte == CONTACT_FRAME_HEAD) {
        s_rx_frame[s_rx_index++] = byte;
    }
}

void Contact_Init(UART_HandleTypeDef *huart)
{
    s_contact_uart = huart;
    s_rx_index = 0U;
    s_new_command = 0U;
    s_last_command = CONTACT_RK_SAFE;
    memset(&s_last_sensor, 0, sizeof(s_last_sensor));
    memset(&s_last_actuator, 0, sizeof(s_last_actuator));
    s_last_online_tick = HAL_GetTick();
    s_last_rk_rx_tick = HAL_GetTick();
    Contact_RestartReceive();
    (void)Contact_SendStatus(CONTACT_STM32_ONLINE_SAFE);
}

void Contact_Task(uint32_t now_ms)
{
    if (s_reset_pending &&
        (uint32_t)(now_ms - s_reset_request_tick) >= CONTACT_RESET_REPLY_WAIT_MS) {
        s_reset_pending = 0U;
    }
    if ((uint32_t)(now_ms - s_last_online_tick) >= CONTACT_ONLINE_PERIOD_MS) {
        s_last_online_tick = now_ms;
        (void)Contact_SendStatus(CONTACT_STM32_ONLINE_SAFE);
    }

    if (!Contact_IsRkOnline(now_ms)) {
        return;
    }
}

uint8_t Contact_IsRkOnline(uint32_t now_ms)
{
    return ((uint32_t)(now_ms - s_last_rk_rx_tick) <= CONTACT_RK_TIMEOUT_MS) ? 1U : 0U;
}

void Contact_BeginResetRequest(void)
{
    s_reset_pending = 1U;
    s_reset_request_tick = HAL_GetTick();
}

void Contact_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == s_contact_uart) {
        Contact_HandleByte(s_rx_byte);
        Contact_RestartReceive();
    }
}

HAL_StatusTypeDef Contact_SendStatus(ContactStm32Status status)
{
    uint8_t frame[3];

    if (status != CONTACT_STM32_ONLINE_SAFE &&
        !Contact_IsRkOnline(HAL_GetTick())) {
        return HAL_TIMEOUT;
    }

    frame[0] = CONTACT_FRAME_HEAD;
    frame[1] = (uint8_t)status;
    frame[2] = CONTACT_FRAME_TAIL;

    return Contact_SendFrame(frame, sizeof(frame));
}

HAL_StatusTypeDef Contact_SendSensorData(uint16_t smoke,
                                         uint16_t gas,
                                         int16_t temperature_x10)
{
    uint8_t frame[9];
    uint16_t temp_raw = (uint16_t)temperature_x10;

    if (!Contact_IsRkOnline(HAL_GetTick())) {
        return HAL_TIMEOUT;
    }

    s_last_sensor.smoke = smoke;
    s_last_sensor.gas = gas;
    s_last_sensor.temperature_x10 = temperature_x10;

    frame[0] = CONTACT_FRAME_HEAD;
    frame[1] = CONTACT_SENSOR_CODE;
    frame[2] = (uint8_t)(smoke & 0xFFU);
    frame[3] = (uint8_t)((smoke >> 8) & 0xFFU);
    frame[4] = (uint8_t)(gas & 0xFFU);
    frame[5] = (uint8_t)((gas >> 8) & 0xFFU);
    frame[6] = (uint8_t)(temp_raw & 0xFFU);
    frame[7] = (uint8_t)((temp_raw >> 8) & 0xFFU);
    frame[8] = CONTACT_FRAME_TAIL;

    return Contact_SendFrame(frame, sizeof(frame));
}

HAL_StatusTypeDef Contact_SendActuatorState(uint8_t power,
                                            uint8_t fan,
                                            uint8_t alarm)
{
    return Contact_SendActuatorState2(power, power, fan, alarm);
}

HAL_StatusTypeDef Contact_SendActuatorState2(uint8_t power1,
                                             uint8_t power2,
                                             uint8_t fan,
                                             uint8_t alarm)
{
    uint8_t frame[7];

    if (!Contact_IsRkOnline(HAL_GetTick())) {
        return HAL_TIMEOUT;
    }

    s_last_actuator.power1 = power1 ? 1U : 0U;
    s_last_actuator.power2 = power2 ? 1U : 0U;
    s_last_actuator.power = (s_last_actuator.power1 && s_last_actuator.power2) ? 1U : 0U;
    s_last_actuator.fan = fan ? 1U : 0U;
    s_last_actuator.alarm = alarm ? 1U : 0U;

    frame[0] = CONTACT_FRAME_HEAD;
    frame[1] = CONTACT_ACTUATOR_CODE;
    frame[2] = s_last_actuator.power1;
    frame[3] = s_last_actuator.power2;
    frame[4] = s_last_actuator.fan;
    frame[5] = s_last_actuator.alarm;
    frame[6] = CONTACT_FRAME_TAIL;

    return Contact_SendFrame(frame, sizeof(frame));
}

ContactRkCommand Contact_GetLastCommand(void)
{
    return s_last_command;
}

uint8_t Contact_HasNewCommand(void)
{
    uint8_t value = s_new_command;
    s_new_command = 0U;
    return value;
}

ContactSensorData Contact_GetLastSensorData(void)
{
    return s_last_sensor;
}

ContactActuatorState Contact_GetLastActuatorState(void)
{
    return s_last_actuator;
}

__weak void Contact_OnCommand(ContactRkCommand command)
{
    (void)command;
}

