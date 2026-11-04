"""JBoxScript compiler. User programs are data, never Python eval/exec."""
from __future__ import annotations

from dataclasses import dataclass
import re
import struct

OPS = dict(zip(('low', 'high', 'input', 'output', 'open_drain', 'delay_us', 'sleep_ms',
                'wait_low', 'wait_high', 'capture_low', 'capture_high', 'sample_emit',
                'emit_const', 'timed_begin', 'timed_end', 'push', 'load', 'store',
                'add', 'sub', 'mul', 'shl', 'shr', 'and', 'or', 'xor', 'gt', 'eq',
                'assert', 'emit_value', 'measure_low', 'measure_high', 'sample'), range(1, 34)))
TOKEN = re.compile(r'\s+|//[^\n]*|\#[^\n]*|==|<<|>>|[A-Za-z_]\w*|\d+|[{}(),;=+*|^&>\-]')
PRECEDENCE = {'|': 1, '^': 2, '&': 3, '==': 4, '>': 5, '<<': 6, '>>': 6, '+': 7, '-': 7, '*': 8}
ARITHMETIC = {'|': 'or', '^': 'xor', '&': 'and', '==': 'eq', '>': 'gt',
              '<<': 'shl', '>>': 'shr', '+': 'add', '-': 'sub', '*': 'mul'}


@dataclass(frozen=True)
class CompiledFunction:
    name: str
    parameters: tuple[str, ...]
    code: bytes


class Compiler:
    def __init__(self, source):
        if not isinstance(source, str) or len(source) > 32768:
            raise ValueError('程序文本最多 32768 个字符')
        self.tokens, offset = [], 0
        while offset < len(source):
            match = TOKEN.match(source, offset)
            if not match:
                raise ValueError(f'无法识别的字符，位置 {offset}: {source[offset:offset + 12]!r}')
            token = match.group()
            if not token.isspace() and not token.startswith(('//', '#')):
                self.tokens.append(token)
            offset = match.end()
        self.pos, self.code, self.params, self.locals = 0, [], {}, {}
        self.depth = 0

    def peek(self):
        return self.tokens[self.pos] if self.pos < len(self.tokens) else ''

    def take(self, expected=None):
        token = self.peek()
        if not token or (expected is not None and token != expected):
            raise ValueError(f'期待 {expected or "语句"}，实际为 {token or "程序末尾"}')
        self.pos += 1
        return token

    def identifier(self):
        name = self.take()
        if not re.fullmatch(r'[A-Za-z_]\w*', name):
            raise ValueError(f'无效名称：{name}')
        return name

    def number(self):
        text = self.take()
        if not text.isdecimal() or not 0 <= int(text) <= 65535:
            raise ValueError('常量必须是 0～65535 的整数')
        return int(text)

    def emit(self, kind, arg=0, value=0):
        self.code.append((OPS[kind], arg, value))
        if len(self.code) > 1024:
            raise ValueError('展开后的函数超过 1024 条指令')

    def pin(self):
        name = self.take()
        if name not in self.params:
            raise ValueError(f'{name} 不是函数的 GPIO 参数')
        return self.params[name]

    def wait_args(self):
        pin = self.pin()
        self.take(',')
        state = self.take().lower()
        if state not in ('0', '1', 'low', 'high'):
            raise ValueError('电平应为 low/high 或 0/1')
        self.take(',')
        duration = self.number()
        self.take(')')
        return pin, state in ('1', 'high'), duration

    def expression(self, minimum=0):
        if self.peek() == '(':
            self.take('('); self.expression(); self.take(')')
        elif self.peek().isdecimal():
            self.emit('push', value=self.number())
        else:
            name = self.identifier()
            if self.peek() == '(':
                self.take('(')
                if name == 'measure':
                    pin, high, duration = self.wait_args()
                    self.emit('measure_high' if high else 'measure_low', pin, duration)
                elif name == 'sample':
                    pin = self.pin(); self.take(')'); self.emit('sample', pin)
                else:
                    raise ValueError(f'表达式中不支持函数：{name}')
            elif name in self.locals:
                self.emit('load', value=self.locals[name])
            else:
                raise ValueError(f'变量未定义：{name}')
        while self.peek() in PRECEDENCE and PRECEDENCE[self.peek()] >= minimum:
            operator = self.take()
            self.expression(PRECEDENCE[operator] + 1)
            self.emit(ARITHMETIC[operator])

    def block(self):
        self.depth += 1
        if self.depth > 8:
            raise ValueError('代码块嵌套不能超过 8 层')
        self.take('{')
        while self.peek() != '}':
            name = self.identifier()
            if name == 'repeat':
                self.take('('); count = self.number(); self.take(')')
                if not 1 <= count <= 128:
                    raise ValueError('repeat 次数必须为 1～128')
                start = len(self.code)
                self.block()
                body = self.code[start:]
                if len(self.code) + len(body) * (count - 1) > 1024:
                    raise ValueError('展开后的函数超过 1024 条指令')
                self.code.extend(body * (count - 1))
            elif name == 'timed':
                self.emit('timed_begin'); self.block(); self.emit('timed_end')
            elif name == 'let':
                variable = self.identifier()
                if variable in self.locals or variable in self.params or len(self.locals) >= 16:
                    raise ValueError('变量名重复或超过 16 个局部变量')
                self.take('='); self.expression(); self.take(';')
                self.locals[variable] = len(self.locals)
                self.emit('store', value=self.locals[variable])
            elif name in self.locals and self.peek() == '=':
                self.take('='); self.expression(); self.take(';')
                self.emit('store', value=self.locals[name])
            else:
                self.take('(')
                if name in ('emit', 'assert'):
                    self.expression(); self.take(')')
                    self.emit('emit_value' if name == 'emit' else 'assert')
                elif name in ('low', 'high', 'release', 'input', 'output', 'open_drain'):
                    pin = self.pin(); self.take(')')
                    self.emit('high' if name == 'release' else name, pin)
                elif name in ('delay_us', 'sleep_ms'):
                    duration = self.number(); self.take(')'); self.emit(name, value=duration)
                elif name == 'wait':
                    pin, high, duration = self.wait_args()
                    self.emit('wait_high' if high else 'wait_low', pin, duration)
                else:
                    raise ValueError(f'不支持的指令：{name}')
                self.take(';')
        self.take('}')
        self.depth -= 1

    def compile(self):
        self.take('fn'); name = self.identifier(); self.take('(')
        if self.peek() != ')':
            while True:
                param = self.identifier()
                if param in self.params or len(self.params) >= 4:
                    raise ValueError('参数重复或超过 4 个 GPIO 参数')
                self.params[param] = len(self.params)
                if self.peek() != ',':
                    break
                self.take(',')
        self.take(')'); self.block()
        if self.peek():
            raise ValueError('每次上传一个函数，函数后不能有其他文本')
        timed, section, total, outputs = False, 0, 0, 0
        for kind, arg, value in self.code:
            cost = 1
            if kind == OPS['timed_begin']:
                if timed:
                    raise ValueError('timed 不能嵌套')
                timed, section = True, 0
            elif kind == OPS['timed_end']:
                timed = False
            elif kind == OPS['sleep_ms']:
                if timed or not 1 <= value <= 250:
                    raise ValueError('sleep_ms 只能在 timed 外使用，范围 1～250ms')
                cost += value * 1000
            elif kind in (6, 8, 9, 31, 32):
                if not 1 <= value <= 10000:
                    raise ValueError('微秒等待必须在 1～10000us 内')
                cost += value
            if timed and kind in (3, 4, 5):
                raise ValueError('GPIO 模式必须在 timed 外设置')
            if kind == OPS['emit_value']:
                outputs += 1
            total += cost
            if timed:
                section += cost
            if total > 500000 or section > 10000 or outputs > 128:
                raise ValueError('函数超过执行预算：总计 500ms，timed 每段 10ms，返回值 128 个')
        if not self.code:
            raise ValueError('函数不能为空')
        return CompiledFunction(name, tuple(self.params), b''.join(struct.pack('<BBH', *op) for op in self.code))


def compile_function(source: str) -> CompiledFunction:
    return Compiler(source).compile()


class RemoteFunction:
    def __init__(self, client, slot, compiled):
        self.client, self.slot, self.compiled = client, slot, compiled
        self.__name__ = compiled.name

    def __call__(self, *args, **kwargs):
        names = self.compiled.parameters
        if len(args) > len(names) or set(kwargs) - set(names):
            raise TypeError('函数参数不匹配')
        bound = dict(zip(names, args))
        if bound.keys() & kwargs.keys():
            raise TypeError('函数参数重复')
        bound.update(kwargs)
        if set(bound) != set(names):
            raise TypeError(f'需要 GPIO 参数：{", ".join(names)}')
        return self.client.call_function(self.slot, [bound[name] for name in names])
