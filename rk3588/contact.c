#include "contact.h"

#include <string.h>

#define CONTACT_RX_FRAME_MAX_SIZE 7U
#define CONTACT_ONLINE_PERIOD_MS 1000U

static UART_HandleTypeDef *s_contact_uart = NULL;
static uint8_t s_rx_byte = 0U;
static uint8_t s_rx_frame[CONTACT_RX_FRAME_MAX_SIZE];
static uint8_t s_rx_index = 0U;
static volatile uint8_t s_new_command = 0U;
static ContactRkCommand s_last_command = CONTACT_RK_SAFE;
static ContactSensorData s_last_sensor = {0U, 0U, 0};
static ContactActuatorState s_last_actuator = {0U, 0U, 0U};
static uint32_t s_last_online_tick = 0U;

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
    return code <= CONTACT_RK_EMERGENCY_ACK;
}

static void Contact_HandleSimpleCommand(uint8_t code)
{
    if (Contact_IsValidRkCommand(code)) {
        s_last_command = (ContactRkCommand)code;
        s_new_command = 1U;
        Contact_OnCommand(s_last_command);
    }
}

static void Contact_HandleByte(uint8_t byte)
{
    if (s_rx_index == 0U) {
        if (byte == CONTACT_FRAME_HEAD) {
            s_rx_frame[s_rx_index++] = byte;
        }
        return;
    }

    s_rx_frame[s_rx_index++] = byte;

    if (s_rx_index == 3U &&
        s_rx_frame[0] == CONTACT_FRAME_HEAD &&
        s_rx_frame[2] == CONTACT_FRAME_TAIL) {
        Contact_HandleSimpleCommand(s_rx_frame[1]);
        s_rx_index = 0U;
        return;
    }

    if (s_rx_index == CONTACT_RX_FRAME_MAX_SIZE &&
        s_rx_frame[0] == CONTACT_FRAME_HEAD &&
        s_rx_frame[1] == CONTACT_ACTUATOR_CODE &&
        s_rx_frame[6] == CONTACT_FRAME_TAIL) {
        Contact_OnActuatorTarget(s_rx_frame[2], s_rx_frame[3], s_rx_frame[4], s_rx_frame[5]);
        s_last_actuator.power = s_rx_frame[2] ? 1U : 0U;
        s_last_actuator.fan = s_rx_frame[4] ? 1U : 0U;
        s_last_actuator.alarm = s_rx_frame[5] ? 1U : 0U;
        Contact_SendActuatorState(s_last_actuator.power,
                                  s_last_actuator.power2,
                                  s_last_actuator.fan,
                                  s_last_actuator.alarm);
        s_rx_index = 0U;
        return;
    }

    if (s_rx_index < CONTACT_RX_FRAME_MAX_SIZE) {
        return;
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
    Contact_RestartReceive();
    (void)Contact_SendStatus(CONTACT_STM32_ONLINE_SAFE);
}

void Contact_Task(uint32_t now_ms)
{
    if ((uint32_t)(now_ms - s_last_online_tick) >= CONTACT_ONLINE_PERIOD_MS) {
        s_last_online_tick = now_ms;
        (void)Contact_SendStatus(CONTACT_STM32_ONLINE_SAFE);
    }
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
                                            uint8_t power2,
                                            uint8_t fan,
                                            uint8_t alarm)
{
    uint8_t frame[7];

    s_last_actuator.power = power ? 1U : 0U;
    s_last_actuator.power2 = power2 ? 1U : 0U;
    s_last_actuator.fan = fan ? 1U : 0U;
    s_last_actuator.alarm = alarm ? 1U : 0U;

    frame[0] = CONTACT_FRAME_HEAD;
    frame[1] = CONTACT_ACTUATOR_CODE;
    frame[2] = s_last_actuator.power;
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

__weak void Contact_OnActuatorTarget(uint8_t power1,
                                     uint8_t power2,
                                     uint8_t fan,
                                     uint8_t alarm)
{
    (void)power1;
    (void)power2;
    (void)fan;
    (void)alarm;
}
