#include "dht11.h"

#define DHT11_START_LOW_MS        18U
#define DHT11_START_RELEASE_US    20U
#define DHT11_TIMEOUT_US          1000U
#define DHT11_BIT_SAMPLE_US       40U

static void DHT11_DelayUs(uint32_t us)
{
    uint32_t start;
    uint32_t ticks;
    uint32_t hclk;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    hclk = HAL_RCC_GetHCLKFreq();
    if (hclk == 0U) {
        hclk = SystemCoreClock;
    }
    start = DWT->CYCCNT;
    ticks = us * (hclk / 1000000U);
    while ((DWT->CYCCNT - start) < ticks) {
    }
}

static void DHT11_SetOutput(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitStruct.Pin = DHT11_DATA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DHT11_DATA_GPIO_Port, &GPIO_InitStruct);
}

static void DHT11_SetInput(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitStruct.Pin = DHT11_DATA_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(DHT11_DATA_GPIO_Port, &GPIO_InitStruct);
}

static void DHT11_WritePin(GPIO_PinState state)
{
    HAL_GPIO_WritePin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin, state);
}

static GPIO_PinState DHT11_ReadPin(void)
{
    return HAL_GPIO_ReadPin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin);
}

static DHT11_Status DHT11_WaitLevel(GPIO_PinState level, uint32_t timeout_us)
{
    while (DHT11_ReadPin() != level) {
        if (timeout_us == 0U) {
            return DHT11_ERROR_TIMEOUT;
        }
        timeout_us--;
        DHT11_DelayUs(1U);
    }

    return DHT11_OK;
}

static DHT11_Status DHT11_ReadBit(uint8_t *bit)
{
    DHT11_Status status;

    status = DHT11_WaitLevel(GPIO_PIN_RESET, DHT11_TIMEOUT_US);
    if (status != DHT11_OK) {
        return status;
    }

    status = DHT11_WaitLevel(GPIO_PIN_SET, DHT11_TIMEOUT_US);
    if (status != DHT11_OK) {
        return status;
    }

    DHT11_DelayUs(DHT11_BIT_SAMPLE_US);
    *bit = (DHT11_ReadPin() == GPIO_PIN_SET) ? 1U : 0U;

    return DHT11_OK;
}

static DHT11_Status DHT11_ReadByte(uint8_t *value)
{
    uint8_t i;
    uint8_t bit;
    DHT11_Status status;

    *value = 0U;
    for (i = 0U; i < 8U; i++) {
        status = DHT11_ReadBit(&bit);
        if (status != DHT11_OK) {
            return status;
        }
        *value = (uint8_t)((*value << 1) | bit);
    }

    return DHT11_OK;
}

void DHT11_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    DHT11_SetOutput();
    DHT11_WritePin(GPIO_PIN_SET);
}

DHT11_Status DHT11_Read(DHT11_Data *data)
{
    uint8_t i;
    uint8_t buf[5];
    uint8_t checksum;
    uint32_t primask;
    DHT11_Status status = DHT11_OK;

    if (data == NULL) {
        return DHT11_ERROR_PARAM;
    }

    DHT11_SetOutput();
    DHT11_WritePin(GPIO_PIN_RESET);
    HAL_Delay(DHT11_START_LOW_MS);

    primask = __get_PRIMASK();
    __disable_irq();

    DHT11_WritePin(GPIO_PIN_SET);
    DHT11_DelayUs(DHT11_START_RELEASE_US);
    DHT11_SetInput();

    status = DHT11_WaitLevel(GPIO_PIN_RESET, DHT11_TIMEOUT_US);
    if (status != DHT11_OK) {
        goto done;
    }
    status = DHT11_WaitLevel(GPIO_PIN_SET, DHT11_TIMEOUT_US);
    if (status != DHT11_OK) {
        goto done;
    }

    for (i = 0U; i < 5U; i++) {
        status = DHT11_ReadByte(&buf[i]);
        if (status != DHT11_OK) {
            goto done;
        }
    }

    checksum = (uint8_t)(buf[0] + buf[1] + buf[2] + buf[3]);
    if (checksum != buf[4]) {
        status = DHT11_ERROR_CHECKSUM;
        goto done;
    }

    data->humidity_int = buf[0];
    data->humidity_dec = buf[1];
    data->temperature_int = buf[2];
    data->temperature_dec = buf[3];
    data->humidity_x10 = (uint16_t)(buf[0] * 10U + buf[1]);
    data->temperature_x10 = (int16_t)(buf[2] * 10U + buf[3]);

done:
    if (primask == 0U) {
        __enable_irq();
    }
    return status;
}
