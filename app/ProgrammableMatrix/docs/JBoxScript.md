# JBoxScript：上传到 ESP32 的小程序

固件只含通用虚拟机，不内置 DHT11 协议。电脑把 `.jbox` 文本编译成字节码，通过已鉴权的 TCP 会话上传；函数保存在设备 RAM 的 0～3 号槽，可重复带参数调用。GPIO 时序、位运算与校验都在 ESP32 上执行，调用返回 `emit` 的整数列表。

```python
from pathlib import Path
from client.jbox_api import MatrixClient

with MatrixClient('192.168.1.100') as board:
    source = Path('client/programs/dht11.jbox').read_text(encoding='utf-8')
    read_sensor = board.upload_function(source, slot=0)
    # 此前需设置传感器电源、数据线、地线的矩阵路由，并等待上电稳定。
    temperature10, humidity10 = read_sensor(data=8)
    print(temperature10 / 10, humidity10 / 10)
```

GPIO 参数使用 **API IO ID 0～17**，不是物理 GPIO 编号。IO0 对应 GPIO1，IO9 对应 GPIO18，详见 README。函数可以有 0～4 个不同的 GPIO 参数。

```text
fn blink(led) {
    output(led);
    repeat(2) {
        high(led);
        sleep_ms(100);
        low(led);
        sleep_ms(100);
    }
    emit(1);
}
```

支持：

| 语法 | 含义 |
|---|---|
| `input(pin)` / `output(pin)` / `open_drain(pin)` | 输入高阻 / 推挽输出初始低 / 开漏输出初始释放并启用内部上拉 |
| `low(pin)` / `high(pin)` / `release(pin)` | 输出低 / 输出高；开漏下 high、release 都为释放，不主动推高 |
| `sleep_ms(n)` | 在任务可调度状态下等待，1～250ms，非精确延时 |
| `delay_us(n)` | 微秒忙等，1～10000us |
| `wait(pin, high/low, timeout_us)` | 等到目标电平，超时则终止本次调用 |
| `measure(pin, high/low, timeout_us)` | 等到目标电平，并返回等待的微秒数 |
| `sample(pin)` | 返回当前电平 0/1 |
| `capture(pin, idle_us)` | 要求开漏模式；先启动 RMT 再释放引脚，按 1us 分辨率记录波形。连续保持同电平 `idle_us` 后结束，范围 1～10000us，调用等待上限约 50ms；不能放在 timed 中 |
| `pulse_count()` | 最近一次 capture 的脉冲数量；每次调用最多 192 段，缓冲区填满会报溢出 |
| `pulse_us(index)` / `pulse_level(index)` | 读取最近波形中第 index 段的微秒长度 / 电平；从 0 开始，越界终止调用 |
| `let value = expression;` / `value = expression;` | 最多 16 个局部变量 |
| `+ - * << >> & \| ^ > ==` | 32 位无符号整数运算，移位数必须在 0～31 |
| `repeat(n) { ... }` | 常量循环 1～128 次，上传时展开 |
| `timed { ... }` | 暂停本核中断/任务切换的时序段；模式设置、毫秒睡眠须放在段外 |
| `assert(expression)` | 条件为 0 时本次调用返回硬件/数据错误 |
| `emit(expression)` | 向调用者返回一个整数，最多 128 个 |

每个函数最多 1024 条展开后的指令；单次静态执行预算 500ms；每个 timed 段最多 10ms；括号块最多 8 层。没有无限循环、递归、动态内存指令或任意机器码。固件会独立校验上传内容，GPIO 只允许访问对外开放的 IO，外设占用或 ADC 模式的 IO 会被拒绝。

这是有界时序执行器；wait/measure 仍为软件轮询，capture 使用 RMT 硬件计时。采集完成后再执行脉冲解码，解码速度不影响已记录波形。它不是任意协议或硬实时保证。高速 I²C/SPI/UART 继续使用现有硬件外设 API。时序段外的微秒操作可能被中断打断。

断开控制会话、重新绑定、执行 SAFE 或设备重启时，上传函数会清除，连线与 GPIO 恢复安全状态；下次连接后重新上传。脚本异常会退出 timed 段，将本次涉及 GPIO 恢复输入高阻；矩阵路由保留到断开/SAFE。

## DHT11 小窗口

DHT11 小程序现用 capture 一次收下完整波形，再进行握手检查、40 位解码和校验，采集期间不逐位执行解释器指令。主机测试覆盖慢速解码、可选起始低电平片段、不完整波形、校验失败及异常退出。需要烧录支持 RMT 指令的固件；实板状态见 README。

双击 `start_dht11.cmd`，或 `python client/dht11_viewer.py --host 设备IP`。先关闭 JBOX Studio 的设备连接，设备只允许一个控制会话。首次使用需短按 GPIO10 后连接完成绑定。IP 留空时自动选择局域网唯一发现的设备；多台设备时需手动填写 IP。

点击连接会先上传 `client/programs/dht11.jbox`，再配置以下连线并等待 2 秒：

| CN1 端子（奇偶直通） | 作用 | 路由 |
|---|---|---|
| 39 / 40 | VCC | 5V，node 19 ↔ 49 |
| 37 / 38 | DATA | EXT9 / GPIO9，node 18 ↔ 48 |
| 35 / 36 | GND | node 17 ↔ 51 |
| 33 / 34 | 未用 | 不连接 |

小窗口每 2.5 秒调用一次设备上的函数，仅显示温湿度和状态；解码与校验在 ESP 上完成。读取失败显示破折号，不会捏造数据。关闭窗口会断开连接并撤销传感器供电路由。

VCC 按实板需求改用 5V，以补偿矩阵开关的压降。DATA 外部上拉到 3.3V，GPIO9 的内部弱上拉也开启。实板已验证这组接线可读取温湿度。旧板 GPIO1、GPIO5 的矩阵通路未收到响应；更换第二块同型号板后，GPIO1、GPIO5、GPIO9 各连续读取 3 次均校验通过，故障现象未在新板复现，旧板具体故障点尚未定位。DATA 不连接 5V 电源节点，上拉也不接到 5V。每次读取前先释放总线并保持空闲 2ms；超时或校验失败后，显示程序会重新开启开漏输出及内部上拉，避免固件的错误清理把 DATA 留在悬空状态。

`--data-gpio 9`（默认）经矩阵 EXT9 接 CN1-37；`--data-gpio 48` 为 GPIO48 直出，需将 DATA 物理接到 GPIO48，程序会撤销 CN1-37 的数据路由。供电和地仍保持 CN1-39、CN1-35。

协议依据：[Aosong DHT11 数据手册](https://www.electronicoscaldas.com/datasheet/DHT11_Aosong.pdf)，包括单线握手、40 位数据、校验和及大于 2 秒的采样间隔。

## 二进制命令

- `0x80 FUNCTION_LOAD`：`slot:u8, argc:u8, count:u16, code[count*4]`；每条指令为 `opcode:u8, argument_index:u8, operand:u16`，小端。
- `0x81 FUNCTION_CALL`：`slot:u8, argc:u8, io_ids[argc]`；成功负载为 `count:u16, values[count]:u32`。
- `0x82 FUNCTION_CLEAR`：`slot:u8`。

新增操作码 34=capture(pin, idle_us)、35=pulse_count、36=pulse_us、37=pulse_level。35 向栈压入数量，36/37 将栈顶索引替换为对应脉冲字段。capture 的波形缓冲区仅在单次函数调用内有效，遇到取消、超时或溢出时不会返回残缺结果。

操作码见 `main/script_vm.h`；编译器见 `client/jbox_lang.py`。这些命令与原 GPIO/矩阵协议兼容。
