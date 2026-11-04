"""Build and run portable C tests; requires GCC on PATH."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from client.jbox_lang import compile_function


def main():
    compiler = shutil.which('gcc')
    if not compiler:
        raise SystemExit('GCC is required on PATH to run the native tests')
    with tempfile.TemporaryDirectory(prefix='jbox-tests-') as directory:
        output = Path(directory)
        function = compile_function((ROOT / 'client/programs/dht11.jbox').read_text(encoding='utf-8'))
        fixture = output / 'dht11.bin'
        fixture.write_bytes(struct.pack('<BH', len(function.parameters), len(function.code) // 4) + function.code)
        for name, source, arguments in (
            ('routes', 'route_plan.c', []),
            ('script_vm', 'script_vm.c', [str(fixture)]),
        ):
            executable = output / (name + '.exe')
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main'), str(ROOT / 'main' / source),
                            str(ROOT / 'tests' / ('test_' + name + '.c')),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable), *arguments], check=True)


if __name__ == '__main__':
    main()
