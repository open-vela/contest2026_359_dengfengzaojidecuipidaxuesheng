# JBoxScript：上传到 ESP32 的小程序

固件只含通用虚拟机，不内置 DHT11 协议。电脑把 `.jbox` 文本编译成字节码，通过已鉴权的 TCP 会话上传；函数保存在设备 RAM 的 0～3 号槽，可重复带参数调用。GPIO 时序、位运算与校验都在 ESP32 上执行，调用返回 `emit` 的整数列表。

```python
from pathlib import Path
from client.jbox_api import MatrixClient

with MatrixClient('192.168.1.100') as board:
    source = Path('client/programs/dht11.jbox').read_text(encoding='utf-8')
    read_sensor = board.upload_function(source, slot=0)
    # 此前需设置传感器电源、数据线、地线的矩阵路由，并等待上电稳定。
    temperature10, humidity10 = read_sensor(data=0)
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
| `let value = expression;` / `value = expression;` | 最多 16 个局部变量 |
| `+ - * << >> & \| ^ > ==` | 32 位无符号整数运算，移位数必须在 0～31 |
| `repeat(n) { ... }` | 常量循环 1～128 次，上传时展开 |
| `timed { ... }` | 暂停本核中断/任务切换的时序段；模式设置、毫秒睡眠须放在段外 |
| `assert(expression)` | 条件为 0 时本次调用返回硬件/数据错误 |
| `emit(expression)` | 向调用者返回一个整数，最多 128 个 |

每个函数最多 1024 条展开后的指令；单次静态执行预算 500ms；每个 timed 段最多 10ms；括号块最多 8 层。没有无限循环、递归、动态内存指令或任意机器码。固件会独立校验上传内容，GPIO 只允许访问对外开放的 IO，外设占用或 ADC 模式的 IO 会被拒绝。

这是有界软件时序执行器，不是任意协议或硬实时保证。高速 I²C/SPI/UART 继续使用现有硬件外设 API。时序段外的微秒操作可能被中断打断。

断开控制会话、重新绑定、执行 SAFE 或设备重启时，上传函数会清除，连线与 GPIO 恢复安全状态；下次连接后重新上传。脚本异常会退出 timed 段，将本次涉及 GPIO 恢复输入高阻；矩阵路由保留到断开/SAFE。

## DHT11 小窗口（实板调试中）

主机测试已验证模拟波形的解码、校验和及异常退出；设备已验证函数上传与整数计算。DHT11 实机仍未取得有效响应，DATA 矩阵通路和解释器的微秒执行时序尚待验证，不视为已经实测可用的驱动。

双击 `start_dht11.cmd`，或 `python client/dht11_viewer.py --host 设备IP`。先关闭 JBOX Studio 的设备连接，设备只允许一个控制会话。首次使用需短按 GPIO10 后连接完成绑定。IP 留空时自动选择局域网唯一发现的设备；多台设备时需手动填写 IP。

点击连接会先上传 `client/programs/dht11.jbox`，再配置以下连线并等待 2 秒：

| CN1 端子（奇偶直通） | 作用 | 路由 |
|---|---|---|
| 39 / 40 | VCC | 5V，node 19 ↔ 49 |
| 37 / 38 | DATA | EXT1 / GPIO1，node 18 ↔ 40 |
| 35 / 36 | NC | 不连接 |
| 33 / 34 | GND | node 16 ↔ 51 |

小窗口每 2.5 秒调用一次设备上的函数，仅显示温湿度和状态；解码与校验在 ESP 上完成。读取失败显示破折号，不会捏造数据。关闭窗口会断开连接并撤销传感器供电路由。

VCC 按实板需求改用 5V，以补偿矩阵开关的压降。DATA 仍使用 GPIO1 的内部 3.3V 弱上拉；可先用现有接线测试，需要改善上升沿时再增加约 4.7kΩ 到 3.3V 的外部上拉。DATA 不连接 5V 电源节点，上拉也不接到 5V。每次读取前先释放总线并保持空闲 2ms；超时或校验失败后，显示程序会重新开启开漏输出及内部上拉，避免固件的错误清理把 DATA 留在悬空状态。

协议依据：[Aosong DHT11 数据手册](https://www.electronicoscaldas.com/datasheet/DHT11_Aosong.pdf)，包括单线握手、40 位数据、校验和及大于 2 秒的采样间隔。

## 二进制命令

- `0x80 FUNCTION_LOAD`：`slot:u8, argc:u8, count:u16, code[count*4]`；每条指令为 `opcode:u8, argument_index:u8, operand:u16`，小端。
- `0x81 FUNCTION_CALL`：`slot:u8, argc:u8, io_ids[argc]`；成功负载为 `count:u16, values[count]:u32`。
- `0x82 FUNCTION_CLEAR`：`slot:u8`。

操作码见 `main/script_vm.h`；编译器见 `client/jbox_lang.py`。这些命令与原 GPIO/矩阵协议兼容。
