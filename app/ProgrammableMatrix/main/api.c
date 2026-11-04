#include "api.h"
#include "matrix.h"
#include "peripherals.h"
#include "board.h"
#include "script_service.h"
#include "script_vm.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include <string.h>

static uint16_t u16(const uint8_t *b){return (uint16_t)b[0]|((uint16_t)b[1]<<8);}
static uint32_t u32(const uint8_t *b){return (uint32_t)u16(b)|((uint32_t)u16(b+2)<<16);}
static uint64_t u64(const uint8_t *b){return (uint64_t)u32(b)|((uint64_t)u32(b+4)<<32);}
static void p16(uint8_t *b,uint16_t v){b[0]=v;b[1]=v>>8;}
static void p32(uint8_t *b,uint32_t v){p16(b,(uint16_t)v);p16(b+2,(uint16_t)(v>>16));}
static void p64(uint8_t *b,uint64_t v){p32(b,(uint32_t)v);p32(b+4,(uint32_t)(v>>32));}
static void routes_pack(uint8_t *b) {for(unsigned i=0;i<8;i++)p64(b+8*i,matrix_get()->net[i]);}

void api_safe(void) {
    script_clear_all();
    matrix_reset();
    peripherals_safe();
}
int api_dispatch(uint8_t cmd,const uint8_t *b,size_t n,uint8_t *o,size_t *outn)
{
    if(!o||!outn||!b)return J_BAD_ARG;
    *outn=0;
    switch(cmd) {
    case CMD_FUNCTION_LOAD: {
        if(n<8 || n!=4+(size_t)u16(b+2)*4)return J_BAD_ARG;
        return script_load(b[0],b[1],b+4,n-4);
    }
    case CMD_FUNCTION_CALL: {
        if(n<2 || n!=2+(size_t)b[1])return J_BAD_ARG;
        uint32_t values[SCRIPT_MAX_RESULTS];size_t count=0;
        int rc=script_call(b[0],b+2,b[1],values,&count);
        if(rc==J_OK){
            p16(o,(uint16_t)count);
            for(size_t i=0;i<count;i++)p32(o+2+i*4,values[i]);
            *outn=2+count*4;
        }
        return rc;
    }
    case CMD_FUNCTION_CLEAR: {
        if(n!=1)return J_BAD_ARG;
        return script_clear(b[0]);
    }
    case CMD_CAPS: {
        if(n)return J_BAD_ARG;
        o[0]=1;o[1]=BOARD_NODES;o[2]=BOARD_NETS;o[3]=BOARD_IO;
        p16(o+4,API_MAX_XFER);p16(o+6,API_PORT);
        for(unsigned i=0;i<BOARD_IO;i++)o[8+i]=(uint8_t)BOARD_IO_GPIOS[i];
        *outn=8+BOARD_IO;return J_OK;
    }
    case CMD_STATUS: {
        if(n)return J_BAD_ARG;
        p32(o,(uint32_t)(esp_timer_get_time()/1000));
        p32(o+4,esp_get_free_heap_size());
        routes_pack(o+8);
        uint8_t modes[BOARD_IO],leases[BOARD_IO];io_snapshot(modes,leases);
        memcpy(o+72,modes,BOARD_IO);
        memcpy(o+72+BOARD_IO,leases,BOARD_IO);
        *outn=72+2*BOARD_IO;return J_OK;
    }
    case CMD_ROUTE_SET: {
        if(n!=64)return J_BAD_ARG;
        routes_t r={0};for(unsigned i=0;i<8;i++)r.net[i]=u64(b+8*i);
        int rc=matrix_replace(&r);
        if(rc==J_OK){routes_pack(o);*outn=64;}return rc;
    }
    case CMD_ROUTE_CONNECT: {
        if(n!=2)return J_BAD_ARG;
        routes_t r=*matrix_get();int y=-1;
        int rc=routes_connect(&r,b[0],b[1],&y);
        if(rc!=J_OK)return rc;
        rc=matrix_replace(&r);
        if(rc==J_OK){o[0]=(uint8_t)y;routes_pack(o+1);*outn=65;}
        return rc;
    }
    case CMD_ROUTE_REMOVE: {
        if(n!=1)return J_BAD_ARG;
        routes_t r=*matrix_get();int rc=routes_remove(&r,b[0]);
        if(rc==J_OK)rc=matrix_replace(&r);
        if(rc==J_OK){routes_pack(o);*outn=64;}return rc;
    }
    case CMD_ROUTE_CLEAR: {
        if(n)return J_BAD_ARG;
        routes_t empty={0};return matrix_replace(&empty);
    }
    case CMD_IO_CONFIG: {
        if(n!=4)return J_BAD_ARG;
        return io_config(b[0],b[1],b[2],b[3]);
    }
    case CMD_IO_WRITE: {
        if(n!=8)return J_BAD_ARG;
        return io_write_mask(u32(b),u32(b+4));
    }
    case CMD_IO_READ: {
        if(n!=4)return J_BAD_ARG;
        uint32_t v=0;int rc=io_read_mask(u32(b),&v);
        if(rc==J_OK){p32(o,v);*outn=4;}return rc;
    }
    case CMD_PWM: {
        if(n!=8)return J_BAD_ARG;
        if(b[1]==255)return io_pwm_stop(b[0]);
        return io_pwm(b[0],b[1],u32(b+2),u16(b+6));
    }
    case CMD_ADC: {
        if(n!=1)return J_BAD_ARG;
        int raw=0;int rc=io_adc(b[0],&raw);
        if(rc==J_OK){p32(o,(uint32_t)raw);*outn=4;}return rc;
    }
    case CMD_I2C_SETUP: {
        if(n!=7)return J_BAD_ARG;
        uint32_t hz=u32(b+3);
        int rc=i2c_setup(b[0],b[1],b[2],hz);
        if(rc==J_OK)i2c_set_hz(b[0],hz);
        return rc;
    }
    case CMD_I2C_CLOSE: {
        if(n!=1)return J_BAD_ARG;
        return i2c_close(b[0]);
    }
    case CMD_I2C_XFER: {
        if(n<6)return J_BAD_ARG;
        uint16_t nt=u16(b+2),nr=u16(b+4);
        if(nt+nr>API_MAX_XFER || n!=6+(size_t)nt)return J_BAD_ARG;
        int rc=i2c_xfer(b[0],b[1],b+6,nt,o,nr);
        if(rc==J_OK)*outn=nr;
        return rc;
    }
    case CMD_SPI_SETUP: {
        if(n!=10)return J_BAD_ARG;
        return spi_setup(b[0],b[1],b[2],b[3],b[4],b[5],u32(b+6));
    }
    case CMD_SPI_CLOSE: {
        if(n!=1)return J_BAD_ARG;
        return spi_close(b[0]);
    }
    case CMD_SPI_XFER: {
        if(n<3)return J_BAD_ARG;
        uint16_t count=u16(b+1);
        if(count==0 || count>API_MAX_XFER || n!=3+(size_t)count)return J_BAD_ARG;
        int rc=spi_xfer(b[0],b+3,o,count);
        if(rc==J_OK)*outn=count;
        return rc;
    }
    case CMD_UART_SETUP: {
        if(n!=9)return J_BAD_ARG;
        return uart_setup(b[0],b[1],b[2],u32(b+3),b[7],b[8]);
    }
    case CMD_UART_CLOSE: {
        if(n!=1)return J_BAD_ARG;
        return uart_close(b[0]);
    }
    case CMD_UART_WRITE: {
        if(n<1 || n-1>API_MAX_XFER)return J_BAD_ARG;
        int wrote=0;int rc=uart_write_api(b[0],b+1,n-1,&wrote);
        if(rc==J_OK){p16(o,(uint16_t)wrote);*outn=2;}return rc;
    }
    case CMD_UART_READ: {
        if(n!=5)return J_BAD_ARG;
        unsigned count=u16(b+1),timeout=u16(b+3);
        if(count>API_MAX_XFER)return J_BAD_ARG;
        int read=0;int rc=uart_read_api(b[0],o,count,timeout,&read);
        if(rc==J_OK)*outn=read;
        return rc;
    }
    case CMD_SAFE: {
        if(n)return J_BAD_ARG;
        api_safe();return J_OK;
    }
    case CMD_FACTORY_RESET: {
        if(n!=4 || memcmp(b,"YES!",4)!=0)return J_BAD_ARG;
        // network layer responds, then NVS erased and device restarted.
        return J_OK;
    }
    default:return J_UNAVAILABLE;
    }
}
