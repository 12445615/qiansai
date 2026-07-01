#include "buttons.h"

#include "contact.h"
#include "fan.h"
#include "power.h"

#define BUTTON_GPIO_PORT GPIOA
#define BUTTON_FIRE_TEST_PIN GPIO_PIN_4
#define BUTTON_RESET_PIN GPIO_PIN_5
#define BUTTON_DEBOUNCE_MS 120U
#define BUTTON_FIRE_TEST_HOLD_MS 8000U
#define BUTTON_BOOT_IGNORE_MS 2000U

typedef struct {
    uint16_t pin;
    GPIO_PinState stable_state;
    GPIO_PinState last_raw_state;
    uint32_t last_change_ms;
    uint8_t pressed_latched;
} ButtonDebounce;

static ButtonDebounce s_fire_test_button = {
    BUTTON_FIRE_TEST_PIN,
    GPIO_PIN_SET,
    GPIO_PIN_SET,
    0U,
    0U
};

static ButtonDebounce s_reset_button = {
    BUTTON_RESET_PIN,
    GPIO_PIN_SET,
    GPIO_PIN_SET,
    0U,
    0U
};

static uint32_t s_fire_test_hold_until_ms = 0U;

static uint8_t Buttons_PressedOnce(ButtonDebounce *button, uint32_t now_ms)
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
        if (raw_state == GPIO_PIN_SET) {
            button->pressed_latched = 0U;
        }
    }

    if (button->stable_state == GPIO_PIN_RESET && !button->pressed_latched) {
        button->pressed_latched = 1U;
        return 1U;
    }

    return 0U;
}

static void Buttons_InitOne(ButtonDebounce *button)
{
    button->stable_state = HAL_GPIO_ReadPin(BUTTON_GPIO_PORT, button->pin);
    button->last_raw_state = button->stable_state;
    button->last_change_ms = HAL_GetTick();
    button->pressed_latched = (button->stable_state == GPIO_PIN_RESET) ? 1U : 0U;
}

void Buttons_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitStruct.Pin = BUTTON_FIRE_TEST_PIN | BUTTON_RESET_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(BUTTON_GPIO_PORT, &GPIO_InitStruct);

    Buttons_InitOne(&s_fire_test_button);
    Buttons_InitOne(&s_reset_button);
}

void Buttons_Task(uint32_t now_ms)
{
    if (now_ms >= BUTTON_BOOT_IGNORE_MS &&
        Buttons_PressedOnce(&s_fire_test_button, now_ms)) {
        s_fire_test_hold_until_ms = now_ms + BUTTON_FIRE_TEST_HOLD_MS;
    }

    if (Buttons_PressedOnce(&s_reset_button, now_ms)) {
        Contact_BeginResetRequest();
        Contact_SendStatus(CONTACT_STM32_RESET_OK);
        Contact_SendActuatorState2(Power1_IsOn(), Power2_IsOn(), Fan_IsOn(), 0U);
    }
}

uint8_t Buttons_FireTestActive(uint32_t now_ms)
{
    return (s_fire_test_hold_until_ms != 0U &&
            (int32_t)(s_fire_test_hold_until_ms - now_ms) > 0) ? 1U : 0U;
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    (void)GPIO_Pin;
}
