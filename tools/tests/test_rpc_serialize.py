"""Test the patched transaction wrapper without radio access."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apps', type=Path)
    args = parser.parse_args()
    patch = Path(__file__).resolve().parents[1] / 'patches/c6/hosted-rpc-serialize.patch'
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        source = root / 'system/c6probe/esp_hosted_rpc.c'
        source.parent.mkdir(parents=True)
        source.write_bytes((args.apps / 'system/c6probe/esp_hosted_rpc.c').read_bytes())
        subprocess.run(['git', 'init', '-q', str(root)], check=True)
        command = ['git', '-C', str(root), 'apply']
        if subprocess.run(command + ['--check', str(patch)], capture_output=True).returncode == 0:
            subprocess.run(command + [str(patch)], check=True)
        else:
            existing = source.read_text()
            required = ('#include <pthread.h>', 'g_rpc_transaction_lock',
                        'rpc_transact_locked(',
                        'pthread_mutex_unlock(&g_rpc_transaction_lock)')
            if not all(marker in existing for marker in required):
                raise RuntimeError('RPC serialization patch is neither applicable nor present')
        text = source.read_text()
        lock_start = text.index('static pthread_mutex_t g_rpc_transaction_lock')
        lock_end = text.index('\n', lock_start) + 1
        wrapper_start = text.index('static FAR Rpc *rpc_transact(', lock_end)
        end = text.index('\n/****************************************************************************', wrapper_start)
        code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#define FAR
typedef struct { unsigned uid; int failure; } Rpc;
typedef struct { int unused; } c6_rpc_mailbox;
static c6_rpc_mailbox g_rpc;
static atomic_int active, completed;
static Rpc response;
static unsigned c6_rpc_next_uid(c6_rpc_mailbox *mailbox) {
 (void)mailbox;
 return 1;
}
static Rpc *rpc_transact_locked(const Rpc *req, const char *label) {
 (void)label;
 assert(atomic_fetch_add(&active, 1) == 0);
 struct timespec pause = {0, 100000}; nanosleep(&pause, NULL);
 assert(atomic_fetch_sub(&active, 1) == 1);
 completed++;
 return req->failure ? NULL : &response;
}
'''
        code += text[lock_start:lock_end]
        code += text[wrapper_start:end]
        code += r'''
static void *caller(void *arg) {
 (void)arg;
 for (int i = 0; i < 50; i++) {
  Rpc req = {.failure = i % 2};
  assert((rpc_transact(&req, "test") != NULL) == !req.failure);
 }
 return NULL;
}
int main(void) {
 pthread_t threads[4];
 for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, caller, NULL));
 for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
 assert(completed == 200 && active == 0);
 return 0;
}
'''
        (root / 'test.c').write_text(code)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                        str(root / 'test.c'), '-o', str(root / 'test')], check=True)
        subprocess.run([str(root / 'test')], check=True, timeout=10)
    print('PASS: 200 calls serialized across four threads, including failure returns')


if __name__ == '__main__':
    main()
