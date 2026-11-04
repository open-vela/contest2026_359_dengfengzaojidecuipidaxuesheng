#!/usr/bin/env python3
"""Exercise production cloud HTTP policy against an explicit webclient fixture."""
from pathlib import Path
import os
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[1]
desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
with tempfile.TemporaryDirectory(prefix='store-http-') as tmp:
    root=Path(tmp);(root/'netutils').mkdir()
    (root/'netutils/webclient.h').write_text('''#pragma once
#include <stdbool.h>
#include <stddef.h>
struct webclient_tls_connection;
struct webclient_tls_ops {int placeholder;};
#define WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1 1
struct webclient_context {
 const char *url;int protocol_version,timeout_sec;const char *const *headers;unsigned nheaders;
 char *buffer;int buflen;const struct webclient_tls_ops *tls_ops;void *tls_ctx;
 int (*sink_callback)(char **,int,int,int *,void *);void *sink_callback_arg;
 int (*header_callback)(const char *,bool,void *);unsigned http_status;
};
void webclient_set_defaults(struct webclient_context *);
int webclient_perform(struct webclient_context *);
''')
    exe=root/'test'
    subprocess.run(['cc','-std=gnu11','-D_GNU_SOURCE','-DGLASS_STORE_HTTP_TEST','-Wall','-Wextra','-Werror','-O1','-g',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root),'-I'+str(desktop),
                    str(desktop/'glass_store_http.c'),str(repo/'app/espdl-quickapp/tests/store_http_host.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
