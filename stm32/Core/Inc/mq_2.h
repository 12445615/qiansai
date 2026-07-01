#ifndef __MQ_2_H__
#define __MQ_2_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

#define MQ_2_ADC_MAX_VALUE        4095.0f
#define MQ_2_ADC_REF_VOLTAGE      3.3f

/*
 * If MQ-2 AO is divided by two before entering the STM32 ADC pin,
 * change this value to 2.0f.
 */
#define MQ_2_VOLTAGE_GAIN         1.0f

#define MQ_2_CLEAN_AIR_VOLTAGE    0.00f
#define MQ_2_FULL_SCALE_VOLTAGE   3.00f
#define MQ_2_SMOKE_MAX_VALUE      100.0f
#define MQ_2_DETECT_MIN_PPM       300.0f
#define MQ_2_DETECT_MAX_PPM       10000.0f

#define MQ_2_SAMPLE_COUNT         10U
#define MQ_2_POLL_TIMEOUT_MS      10U

void MQ_2_Init(void);
uint16_t MQ_2_Get_ADC_Value(void);
float MQ_2_ADC_To_Voltage(uint16_t adc_value);
float MQ_2_Get_Voltage(void);
float MQ_2_Voltage_To_Smoke_Value(float voltage);
float MQ_2_Voltage_To_Gas_Value(float voltage);
float MQ_2_Get_Smoke_Value(void);
float MQ_2_Get_Gas_Value(void);
void MQ_2_Print(void);

#ifdef __cplusplus
}
#endif

#endif /* __MQ_2_H__ */
