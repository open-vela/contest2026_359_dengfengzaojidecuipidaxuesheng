#pragma once
#include "route_plan.h"
#include <stdint.h>
#include <stddef.h>

// Exclusively called by network command task; NO concurrent API hardware writes.
// Valid IO IDs 0..18; indexes 0..8 are crossbar EXT1..EXT9.
void peripherals_init(void);
void peripherals_safe(void);
int io_config(unsigned id,unsigned mode,unsigned pull,unsigned initial);
int io_write_mask(uint32_t mask,uint32_t levels);
int io_read_mask(uint32_t mask,uint32_t *levels);
int io_adc(unsigned id,int *raw);
int io_pwm(unsigned channel,unsigned io,uint32_t freq,uint16_t duty);
int io_pwm_stop(unsigned channel);
void io_snapshot(uint8_t modes[BOARD_IO],uint8_t leases[BOARD_IO]);
int i2c_setup(unsigned bus,unsigned sda,unsigned scl,uint32_t hz);
int i2c_close(unsigned bus);
void i2c_set_hz(unsigned bus,uint32_t hz);
int i2c_xfer(unsigned bus,unsigned addr,const uint8_t *tx,size_t txlen,
             uint8_t *rx,size_t rxlen);
int spi_setup(unsigned bus,unsigned mosi,unsigned miso,unsigned sck,
              unsigned cs,unsigned mode,uint32_t hz);
int spi_close(unsigned bus);
int spi_xfer(unsigned bus,const uint8_t *tx,uint8_t *rx,size_t len);
int uart_setup(unsigned bus,unsigned tx,unsigned rx,uint32_t baud,
               unsigned parity,unsigned stopbits);
int uart_close(unsigned bus);
int uart_write_api(unsigned bus,const uint8_t *data,size_t len,int *out);
int uart_read_api(unsigned bus,uint8_t *data,size_t max,unsigned timeout_ms,int *out);
