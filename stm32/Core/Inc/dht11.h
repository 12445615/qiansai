#ifndef __DHT11_H__
#define __DHT11_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>

typedef enum {
    DHT11_OK = 0,
    DHT11_ERROR_TIMEOUT,
    DHT11_ERROR_CHECKSUM,
    DHT11_ERROR_PARAM
} DHT11_Status;

typedef struct {
    uint8_t humidity_int;
    uint8_t humidity_dec;
    uint8_t temperature_int;
    uint8_t temperature_dec;
    uint16_t humidity_x10;
    int16_t temperature_x10;
} DHT11_Data;

void DHT11_Init(void);
DHT11_Status DHT11_Read(DHT11_Data *data);

#ifdef __cplusplus
}
#endif

#endif /* __DHT11_H__ */
