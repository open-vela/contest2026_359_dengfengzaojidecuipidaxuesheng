#pragma once
#include <stddef.h>
#include <stdint.h>

#define SCRIPT_MAX_OPS 1024u
#define SCRIPT_MAX_ARGS 4u
#define SCRIPT_MAX_RESULTS 128u
#define SCRIPT_MAX_TIME_US 500000u
#define SCRIPT_TIMED_US 10000u

enum script_opcode {
    SC_LOW=1, SC_HIGH, SC_INPUT, SC_OUTPUT, SC_OPEN_DRAIN,
    SC_DELAY_US, SC_SLEEP_MS, SC_WAIT_LOW, SC_WAIT_HIGH,
    SC_CAPTURE_LOW, SC_CAPTURE_HIGH, SC_SAMPLE, SC_EMIT,
    SC_TIMED_BEGIN, SC_TIMED_END,
    SC_PUSH, SC_LOAD, SC_STORE, SC_ADD, SC_SUB, SC_MUL, SC_SHL, SC_SHR,
    SC_AND, SC_OR, SC_XOR, SC_GT, SC_EQ, SC_ASSERT, SC_EMIT_VALUE,
    SC_MEASURE_LOW, SC_MEASURE_HIGH, SC_SAMPLE_VALUE,
};

// Instructions are four wire bytes: opcode, argument index, value (u16 LE).
typedef struct {
    uint8_t argc;
    uint16_t count;
    uint8_t code[SCRIPT_MAX_OPS*4];
} script_program_t;

typedef struct {
    void *ctx;
    uint32_t (*now_us)(void *ctx);
    int (*mode)(void *ctx,unsigned arg,unsigned mode);
    int (*write)(void *ctx,unsigned arg,unsigned level);
    int (*read)(void *ctx,unsigned arg,unsigned *level);
    void (*delay_us)(void *ctx,unsigned us);
    void (*sleep_ms)(void *ctx,unsigned ms);
    void (*timed)(void *ctx,int enter);
    int (*cancelled)(void *ctx);
} script_backend_t;

int script_validate(const script_program_t *program);
int script_execute(const script_program_t *program,const script_backend_t *backend,
                   uint32_t result[SCRIPT_MAX_RESULTS],size_t *count);
