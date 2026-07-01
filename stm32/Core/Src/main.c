/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "mq_2.h"
#include "contact.h"
#include "dht11.h"
#include "fan.h"
#include "power.h"
#include "buttons.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define RK_BOOT_GRACE_MS 30000U
#define RK_OFFLINE_CONFIRM_PERIOD_MS 1000U
#define RK_OFFLINE_CONFIRM_LIMIT 3U
#define FIRE_TEST_BUTTON_PORT GPIOA
#define FIRE_TEST_BUTTON_PIN GPIO_PIN_4
#define FIRE_TEST_SMOKE_VALUE 120U
#define FIRE_TEST_GAS_VALUE 1600U
#define FIRE_TEST_TEMP_X10 700

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint16_t mq2_adc_value = 0U;
float mq2_voltage = 0.0f;//��ѹֵ
float mq2_smoke_value = 0.0f;//����
float mq2_gas_value = 0.0f;
uint32_t mq2_last_tick = 0;//�ϴεĶ�ȡʱ��
uint32_t dht11_last_tick = 0;
DHT11_Data dht11_data = {0};
DHT11_Status dht11_status = DHT11_ERROR_TIMEOUT;
uint16_t dht11_humidity_x10 = 0;
int16_t dht11_temperature_x10 = 0;
uint8_t rk_offline_safe_applied = 0U;
uint8_t rk_offline_confirm_count = 0U;
uint32_t rk_last_offline_check_tick = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_TIM2_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
  MQ_2_Init();
  DHT11_Init();
  Power_Init();
  Fan_Init();
  Buttons_Init();
  Contact_Init(&huart3);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    Buttons_Task(HAL_GetTick());

    uint32_t now_ms = HAL_GetTick();
    uint8_t rk_online = Contact_IsRkOnline(now_ms);

    if ((uint32_t)(now_ms - 0U) < RK_BOOT_GRACE_MS)
    {
      rk_online = 1U;
    }

    if (rk_online)
    {
      rk_offline_confirm_count = 0U;
      rk_offline_safe_applied = 0U;
    }
    else if ((uint32_t)(now_ms - rk_last_offline_check_tick) >= RK_OFFLINE_CONFIRM_PERIOD_MS)
    {
      rk_last_offline_check_tick = now_ms;
      if (rk_offline_confirm_count < RK_OFFLINE_CONFIRM_LIMIT)
      {
        rk_offline_confirm_count++;
      }
      if (rk_offline_confirm_count >= RK_OFFLINE_CONFIRM_LIMIT && !rk_offline_safe_applied)
      {
        Power_Off();
        Fan_Off();
        rk_offline_safe_applied = 1U;
      }
    }

    if (HAL_GetTick() - dht11_last_tick >= 2000U)
    {
      dht11_last_tick = HAL_GetTick();
      dht11_status = DHT11_Read(&dht11_data);
      if (dht11_status == DHT11_OK)
      {
        dht11_humidity_x10 = dht11_data.humidity_x10;
        dht11_temperature_x10 = dht11_data.temperature_x10;
      }
    }
    if (HAL_GetTick() - mq2_last_tick >= 1000U)
    {
      mq2_last_tick = HAL_GetTick();
      mq2_adc_value = MQ_2_Get_ADC_Value();
      mq2_voltage = MQ_2_ADC_To_Voltage(mq2_adc_value);
      mq2_smoke_value = MQ_2_Voltage_To_Smoke_Value(mq2_voltage);
      mq2_gas_value = MQ_2_Voltage_To_Gas_Value(mq2_voltage);
      uint16_t report_smoke = (uint16_t)mq2_smoke_value;
      uint16_t report_gas = (uint16_t)mq2_gas_value;
      int16_t report_temp_x10 = dht11_temperature_x10;
      if (Buttons_FireTestActive(HAL_GetTick()))
      {
        report_smoke = FIRE_TEST_SMOKE_VALUE;
        report_gas = FIRE_TEST_GAS_VALUE;
        report_temp_x10 = FIRE_TEST_TEMP_X10;
      }
      Contact_SendSensorData(report_smoke,
                             report_gas,
                             report_temp_x10);
      Contact_SendActuatorState2(Power1_IsOn(), Power2_IsOn(), Fan_IsOn(), 0U);
    }
    Contact_Task(HAL_GetTick());
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  Contact_UART_RxCpltCallback(huart);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
