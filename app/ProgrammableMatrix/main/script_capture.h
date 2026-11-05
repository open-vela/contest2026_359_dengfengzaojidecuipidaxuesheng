#pragma once
#include "script_vm.h"
int script_capture(unsigned io,unsigned idle_us,script_pulse_t *pulses,size_t *count,
                   int (*cancelled)(void *),void *ctx);
