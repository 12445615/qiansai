#include "mq_2.h"
#include "adc.h"
#include <stdio.h>

static float MQ_2_Normalize_From_Voltage(float voltage)
{
  float ratio;

  if (voltage <= MQ_2_CLEAN_AIR_VOLTAGE)
  {
    return 0.0f;
  }

  if (voltage >= MQ_2_FULL_SCALE_VOLTAGE)
  {
    return 1.0f;
  }

  ratio = (voltage - MQ_2_CLEAN_AIR_VOLTAGE) /
          (MQ_2_FULL_SCALE_VOLTAGE - MQ_2_CLEAN_AIR_VOLTAGE);

  if (ratio < 0.0f)
  {
    return 0.0f;
  }
  if (ratio > 1.0f)
  {
    return 1.0f;
  }

  return ratio;
}

static float MQ_2_Normalized_To_Ppm(float ratio)
{
  if (ratio <= 0.0f)
  {
    return 0.0f;
  }

  return MQ_2_DETECT_MIN_PPM +
         ratio * (MQ_2_DETECT_MAX_PPM - MQ_2_DETECT_MIN_PPM);
}

void MQ_2_Init(void)
{
  HAL_ADCEx_Calibration_Start(&hadc1);
}

uint16_t MQ_2_Get_ADC_Value(void)
{
  uint32_t adc_sum = 0;
  uint8_t i;

  for (i = 0; i < MQ_2_SAMPLE_COUNT; i++)
  {
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
      continue;
    }

    if (HAL_ADC_PollForConversion(&hadc1, MQ_2_POLL_TIMEOUT_MS) == HAL_OK)
    {
      adc_sum += HAL_ADC_GetValue(&hadc1);
    }

    HAL_ADC_Stop(&hadc1);
  }

  return (uint16_t)(adc_sum / MQ_2_SAMPLE_COUNT);
}

float MQ_2_ADC_To_Voltage(uint16_t adc_value)
{
  return ((float)adc_value * MQ_2_ADC_REF_VOLTAGE / MQ_2_ADC_MAX_VALUE) * MQ_2_VOLTAGE_GAIN;
}

float MQ_2_Get_Voltage(void)
{
  return MQ_2_ADC_To_Voltage(MQ_2_Get_ADC_Value());
}

float MQ_2_Voltage_To_Smoke_Value(float voltage)
{
  return MQ_2_Normalize_From_Voltage(voltage) * MQ_2_SMOKE_MAX_VALUE;
}

float MQ_2_Voltage_To_Gas_Value(float voltage)
{
  return MQ_2_Normalized_To_Ppm(MQ_2_Normalize_From_Voltage(voltage));
}

float MQ_2_Get_Smoke_Value(void)
{
  return MQ_2_Voltage_To_Smoke_Value(MQ_2_Get_Voltage());
}

float MQ_2_Get_Gas_Value(void)
{
  return MQ_2_Voltage_To_Gas_Value(MQ_2_Get_Voltage());
}

void MQ_2_Print(void)
{
  float voltage = MQ_2_Get_Voltage();
  float smoke_value = MQ_2_Voltage_To_Smoke_Value(voltage);
  float gas_value = MQ_2_Voltage_To_Gas_Value(voltage);

  printf("MQ-2 Voltage: %.2f V\r\n", voltage);
  printf("MQ-2 Smoke Value: %.2f\r\n", smoke_value);
  printf("MQ-2 Gas Value: %.2f\r\n", gas_value);
}
