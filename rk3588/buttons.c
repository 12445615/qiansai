#include "buttons.h"

#include "contact.h"
#include "fan.h"
#include "power.h"

#define BUTTON_GPIO_PORT GPIOA
#define BUTTON_EMERGENCY_PIN GPIO_PIN_4
#define BUTTON_RESET_PIN GPIO_PIN_5
#define BUTTON_DEBOUNCE_MS 20U

typedef struct {
    uint16_t pin;
    GPIO_PinState stable_state;
    GPIO_PinState last_raw_state;
    uint32_t last_change_ms;
} ButtonDebounce;

static ButtonDebounce s_emergency_button = {
    BUTTON_EMERGENCY_PIN,
    GPIO_PIN_SET,
    GPIO_PIN_SET,
    0U
};

static ButtonDebounce s_reset_button = {
    BUTTON_RESET_PIN,
    GPIO_PIN_SET,
    GPIO_PIN_SET,
    0U
};

static uint8_t Buttons_Update(ButtonDebounce *button, uint32_t now_ms)
{
    GPIO_PinState raw_state;

    raw_state = HAL_GPIO_ReadPin(BUTTON_GPIO_PORT, button->pin);
    if (raw_state != button->last_raw_state) {
        button->last_raw_state = raw_state;
        button->last_change_ms = now_ms;
        return 0U;
    }

    if (raw_state != button->stable_state &&
        (uint32_t)(now_ms - button->last_change_ms) >= BUTTON_DEBOUNCE_MS) {
        button->stable_state = raw_state;
        return (raw_state == GPIO_PIN_RESET) ? 1U : 0U;
    }

    return 0U;
}

void Buttons_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitStruct.Pin = BUTTON_EMERGENCY_PIN | BUTTON_RESET_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(BUTTON_GPIO_PORT, &GPIO_InitStruct);

    s_emergency_button.stable_state = HAL_GPIO_ReadPin(BUTTON_GPIO_PORT,
                                                       BUTTON_EMERGENCY_PIN);
    s_emergency_button.last_raw_state = s_emergency_button.stable_state;
    s_emergency_button.last_change_ms = HAL_GetTick();

    s_reset_button.stable_state = HAL_GPIO_ReadPin(BUTTON_GPIO_PORT,
                                                   BUTTON_RESET_PIN);
    s_reset_button.last_raw_state = s_reset_button.stable_state;
    s_reset_button.last_change_ms = HAL_GetTick();
}

void Buttons_Task(uint32_t now_ms)
{
    if (Buttons_Update(&s_emergency_button, now_ms)) {
        Power_Off();
        Power2_Off();
        Fan_Off();
        Contact_SendStatus(CONTACT_STM32_EMERGENCY_STOP);
        Contact_SendActuatorState(Power_IsOn(), Power2_IsOn(), Fan_IsOn(), 1U);
    }

    if (Buttons_Update(&s_reset_button, now_ms)) {
        Contact_SendStatus(CONTACT_STM32_RESET_OK);
        Contact_SendActuatorState(Power_IsOn(), Power2_IsOn(), Fan_IsOn(), 0U);
    }
}
