#!/usr/bin/env python3
"""Opt-in cancellation receipt for the existing Vela prompt.input API."""

def apply(source):
    replacements = [
        ('  JSValue input_callback;\n', '  JSValue input_callback;\n  bool input_notify_cancel;\n'),
        ('  JSValue argument = JS_UNDEFINED;\n',
         '  JSValue argument = JS_NULL;\n  bool notify_cancel = g_qpk.input_notify_cancel;\n'),
        ('  g_qpk.input_callback = JS_UNDEFINED;\n  lv_obj_delete(g_qpk.input_shade);',
         '  g_qpk.input_callback = JS_UNDEFINED;\n  g_qpk.input_notify_cancel = false;\n  lv_obj_delete(g_qpk.input_shade);'),
        ('  if (submit)\n    {\n      result = qpk_call_args(callback, QPK_HANG_BUDGET, 1, &argument);',
         '  if (submit || notify_cancel)\n    {\n      result = qpk_call_args(callback, QPK_HANG_BUDGET, 1, &argument);'),
        ('  g_qpk.input_callback = JS_DupValue(context, argv[1]);\n',
         '  g_qpk.input_callback = JS_DupValue(context, argv[1]);\n'
         '  JSValue notify_value = JS_GetPropertyStr(context, argv[0], "notifyCancel");\n'
         '  g_qpk.input_notify_cancel = JS_ToBool(context, notify_value) > 0;\n'
         '  JS_FreeValue(context, notify_value);\n'),
        ('  JS_SetPropertyStr(context, global, "prompt", object);',
         '  JS_SetPropertyStr(context, object, "cancelNotification", JS_TRUE);\n'
         '  JS_SetPropertyStr(context, global, "prompt", object);'),
    ]
    for before, after in replacements:
        if source.count(before) != 1:
            raise ValueError('unsupported prompt implementation: ' + before[:70])
        source = source.replace(before, after)
    return source
