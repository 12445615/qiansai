#include "power.h"

#define POWER_GPIO_PORT GPIOA
#define POWER1_GPIO_PIN GPIO_PIN_1
#define POWER2_GPIO_PIN GPIO_PIN_7

static uint8_t s_power1_on = 0U;
static uint8_t s_power2_on = 0U;

void Power_Init(void)
{
    Power_Off();
}

void Power_On(void)
{
    Power1_On();
    Power2_On();
}

void Power_Off(void)
{
    Power1_Off();
    Power2_Off();
}

uint8_t Power_IsOn(void)
{
    return (s_power1_on && s_power2_on) ? 1U : 0U;
}

void Power1_On(void)
{
    HAL_GPIO_WritePin(POWER_GPIO_PORT, POWER1_GPIO_PIN, GPIO_PIN_SET);
    s_power1_on = 1U;
}

void Power1_Off(void)
{
    HAL_GPIO_WritePin(POWER_GPIO_PORT, POWER1_GPIO_PIN, GPIO_PIN_RESET);
    s_power1_on = 0U;
}

uint8_t Power1_IsOn(void)
{
    return s_power1_on;
}

void Power2_On(void)
{
    HAL_GPIO_WritePin(POWER_GPIO_PORT, POWER2_GPIO_PIN, GPIO_PIN_SET);
    s_power2_on = 1U;
}

void Power2_Off(void)
{
    HAL_GPIO_WritePin(POWER_GPIO_PORT, POWER2_GPIO_PIN, GPIO_PIN_RESET);
    s_power2_on = 0U;
}

uint8_t Power2_IsOn(void)
{
    return s_power2_on;
}
