#pragma once
#include <stdint.h>
#include <stddef.h>

// Binary command set, all little endian. Replies start with int32 status.
enum {
  CMD_HELLO=0x01, CMD_AUTH=0x02, CMD_PAIR=0x03,
  CMD_CAPS=0x10, CMD_STATUS=0x11,
  CMD_ROUTE_SET=0x20, CMD_ROUTE_CONNECT=0x21,
  CMD_ROUTE_REMOVE=0x22, CMD_ROUTE_CLEAR=0x23,
  CMD_IO_CONFIG=0x30, CMD_IO_WRITE=0x31, CMD_IO_READ=0x32,
  CMD_PWM=0x33, CMD_ADC=0x34,
  CMD_I2C_SETUP=0x40, CMD_I2C_CLOSE=0x41, CMD_I2C_XFER=0x42,
  CMD_SPI_SETUP=0x50, CMD_SPI_XFER=0x51, CMD_SPI_CLOSE=0x52,
  CMD_UART_SETUP=0x60, CMD_UART_WRITE=0x61,
  CMD_UART_READ=0x62, CMD_UART_CLOSE=0x63,
  CMD_SAFE=0x70, CMD_FACTORY_RESET=0x7f,
  CMD_FUNCTION_LOAD=0x80, CMD_FUNCTION_CALL=0x81, CMD_FUNCTION_CLEAR=0x82,
};
#define API_MAX_PAYLOAD 8192u
#define API_MAX_XFER 4096u
#define API_PORT 4747u

// Output uses a caller-provided 8192 byte response buffer (without status).
int api_dispatch(uint8_t cmd,const uint8_t *req,size_t n,
                 uint8_t *out,size_t *outn);
void api_safe(void);
