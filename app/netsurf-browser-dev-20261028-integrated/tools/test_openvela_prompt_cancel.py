#!/usr/bin/env python3
"""Run the patched real qpk_input_finish with a small ownership/callback shim."""
import argparse, subprocess
from pathlib import Path
from openvela_prompt_cancel import apply
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--runtime', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args(); out = a.output; out.mkdir(parents=True, exist_ok=True)
source = a.runtime.read_text()
if 'bool input_notify_cancel;' not in source: source = apply(source)
start = source.index('static void qpk_input_finish(bool submit)')
end = source.index('static void qpk_input_cancel(', start)
function = source[start:end]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef int JSContext;
typedef int JSValue;
#define JS_NULL 2
#define JS_UNDEFINED 3
#define QPK_HANG_BUDGET 7
static struct { JSContext *context; void *input_shade, *input_textarea;
 JSValue input_callback; bool input_notify_cancel; } g_qpk;
static int calls, deleted, received, reenter;
static JSValue JS_DupValue(JSContext *c, JSValue v) { (void)c; return v; }
static JSValue JS_NewString(JSContext *c, const char *s) { (void)c; (void)s; return 100; }
static const char *lv_textarea_get_text(void *p) { (void)p; return "typed"; }
static void JS_FreeValue(JSContext *c, JSValue v) { (void)c; (void)v; }
static void lv_obj_delete(void *p) { assert(p); deleted++; }
static JSValue qpk_call_args(JSValue cb, int budget, int n, JSValue *arg) {
 assert(cb == 42 && budget == 7 && n == 1 && !g_qpk.input_shade);
 assert(!g_qpk.input_notify_cancel); calls++; received = *arg;
 if(reenter) { g_qpk.input_shade=(void *)2; g_qpk.input_notify_cancel=true; }
 return JS_UNDEFINED;
}
'''
suffix = r'''
int main(void) {
 JSContext context = 0; g_qpk.context=&context;
 g_qpk.input_shade=(void *)1; g_qpk.input_callback=42;
 qpk_input_finish(false); assert(calls==0 && deleted==1); /* legacy unchanged */
 g_qpk.input_shade=(void *)1; g_qpk.input_callback=42; g_qpk.input_notify_cancel=true;
 qpk_input_finish(false); assert(calls==1 && received==JS_NULL && deleted==2);
 qpk_input_finish(false); assert(calls==1 && deleted==2); /* exactly once */
 g_qpk.input_shade=(void *)1; g_qpk.input_callback=42;
 qpk_input_finish(true); assert(calls==2 && received==100 && deleted==3);
 g_qpk.input_shade=(void *)1; g_qpk.input_callback=42; g_qpk.input_notify_cancel=true; reenter=1;
 qpk_input_finish(false); assert(calls==3 && g_qpk.input_notify_cancel && g_qpk.input_shade==(void *)2);
 puts("PASS: real QPK finish: legacy silent cancel, opted-in null cancel, submit, exactly once, reentrant prompt");
}
'''
(out/'prompt.c').write_text(prefix+function+suffix)
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
    str(out/'prompt.c'), '-o', str(out/'prompt')], check=True)
subprocess.run([str(out/'prompt')], check=True)
