#include "fan.h"
#include "tim.h"
#include "contact.h"
#include "power.h"

#define FAN_TIM_HANDLE htim2
#define FAN_TIM_CHANNEL TIM_CHANNEL_4
#define FAN_DEFAULT_DUTY FAN_DUTY_MAX

static uint16_t s_fan_duty_permille = 0U;

static uint32_t Fan_DutyToCompare(uint16_t duty_permille)
{
    uint32_t period = __HAL_TIM_GET_AUTORELOAD(&FAN_TIM_HANDLE);
    uint32_t ticks = period + 1U;
    uint32_t compare;

    if (duty_permille > FAN_DUTY_MAX) {
        duty_permille = FAN_DUTY_MAX;
    }

    compare = (ticks * duty_permille) / FAN_DUTY_MAX;
    if (compare > period) {
        compare = period;
    }

    return compare;
}

void Fan_Init(void)
{
    s_fan_duty_permille = 0U;
    __HAL_TIM_SET_COMPARE(&FAN_TIM_HANDLE, FAN_TIM_CHANNEL, 0U);
    (void)HAL_TIM_PWM_Start(&FAN_TIM_HANDLE, FAN_TIM_CHANNEL);
}

void Fan_On(void)
{
    Fan_SetDutyPermille(FAN_DEFAULT_DUTY);
}

void Fan_Off(void)
{
    Fan_SetDutyPermille(0U);
}

void Fan_SetDutyPermille(uint16_t duty_permille)
{
    if (duty_permille > FAN_DUTY_MAX) {
        duty_permille = FAN_DUTY_MAX;
    }

    s_fan_duty_permille = duty_permille;
    __HAL_TIM_SET_COMPARE(&FAN_TIM_HANDLE,
                          FAN_TIM_CHANNEL,
                          Fan_DutyToCompare(duty_permille));
}

uint16_t Fan_GetDutyPermille(void)
{
    return s_fan_duty_permille;
}

uint8_t Fan_IsOn(void)
{
    return (s_fan_duty_permille > 0U) ? 1U : 0U;
}

void Contact_OnCommand(ContactRkCommand command)
{
    switch (command) {
    case CONTACT_RK_SAFE:
    case CONTACT_RK_FIRE_WORK_ZONE:
        Power_On();
        Fan_On();
        break;

    case CONTACT_RK_FIRE_OUT_ZONE:
    case CONTACT_RK_ENV_DANGER:
        Power_Off();
        Fan_Off();
        break;

    case CONTACT_RK_PPE_DENY:
        Power_Off();
        Fan_On();
        break;

    case CONTACT_RK_INTRUSION:
    case CONTACT_RK_FAULT:
    case CONTACT_RK_RESERVED:
    case CONTACT_RK_EMERGENCY_ACK:
    default:
        Power_Off();
        Fan_Off();
        break;
    }
}

