#pragma once
#include <stddef.h>
#include <stdint.h>
void script_clear_all(void);
int script_load(unsigned slot,unsigned argc,const uint8_t *code,size_t bytes);
int script_call(unsigned slot,const uint8_t *args,size_t argc,uint32_t *result,size_t *count);
int script_clear(unsigned slot);
