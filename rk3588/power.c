#include "power.h"

#define POWER_GPIO_PORT GPIOA
#define POWER_GPIO_PIN  GPIO_PIN_1
#define POWER2_GPIO_PORT GPIOA
#define POWER2_GPIO_PIN  GPIO_PIN_7

static uint8_t s_power_on = 0U;
static uint8_t s_power2_on = 0U;

void Power_Init(void)
{
    Power_Off();
    Power2_Off();
}

void Power_On(void)
{
    HAL_GPIO_WritePin(POWER_GPIO_PORT, POWER_GPIO_PIN, GPIO_PIN_SET);
    s_power_on = 1U;
}

void Power_Off(void)
{
    HAL_GPIO_WritePin(POWER_GPIO_PORT, POWER_GPIO_PIN, GPIO_PIN_RESET);
    s_power_on = 0U;
}

uint8_t Power_IsOn(void)
{
    return s_power_on;
}

void Power2_On(void)
{
    HAL_GPIO_WritePin(POWER2_GPIO_PORT, POWER2_GPIO_PIN, GPIO_PIN_SET);
    s_power2_on = 1U;
}

void Power2_Off(void)
{
    HAL_GPIO_WritePin(POWER2_GPIO_PORT, POWER2_GPIO_PIN, GPIO_PIN_RESET);
    s_power2_on = 0U;
}

uint8_t Power2_IsOn(void)
{
    return s_power2_on;
}
