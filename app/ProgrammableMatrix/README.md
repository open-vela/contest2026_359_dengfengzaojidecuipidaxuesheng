# JBOX-S3 — 无线io扩展与矩阵连线板

目标：ESP32-S3-WROOM-1-N16R8 + 4×CH446Q。ESP-IDF **v5.5.x**。

短按实体按钮授权临时配网/绑定；手机通过 BLE 给设备送入 Wi-Fi SSID/密码；电脑经局域网发现设备并自动领用、保存设备控制密钥。以后二次连接自动 HMAC 鉴权。

## 1. 一颗按钮的交互

**GPIO10** 作为专用、**低电平有效**配对按钮：

- 按钮的一端接 **GND**，另一端接 **GPIO10**；GPIO10 通过 **10kΩ 外部上拉**接 3.3V，并通过 **100nF 电容**接 GND。固件关闭内部上下拉；松开为高电平，按下为低电平。不得接 5V。
- **短按（约 0.1 秒～6 秒）**：如果从未配置 Wi-Fi，则开启 **180 秒 BLE 配网窗口**；如果已有 Wi-Fi，则开启 **120 秒局域网 API 绑定窗口**。
- **按住 6～12 秒后松开**：只清除 Wi-Fi 配置并重启，保留设备的控制密钥；下一次短按重新通过 BLE 配网。
- **按住至少 12 秒后松开**：恢复出厂设置，清除 Wi-Fi 和设备控制密钥；下一次短按重新配置与绑定。

按键逻辑为松开后识别短按或长按，带软件消抖。若初次 BLE 配网成功，程序**自动**额外打开 120 秒的局域网 API 绑定窗口，因此首次联网无需为 API 再按一次键。

**GPIO API**：9 个矩阵 EXT（GPIO1～9）+ 9 个直出（GPIO18、21、38～42、47、48）= **18 个可控制 GPIO**。CH446Q 控制、USB、UART0、PSRAM 管脚不变。

## 2. 首次使用（全部免码，不读串口）

1. 安装 ESP-IDF 5.5.x，编译烧录工程。第一次烧录需要 USB/下载接口，但配网与使用阶段不需要看串口。
2. 给板子上电，**短按 GPIO10 按钮**。手机安装乐鑫 **ESP BLE Provisioning** 应用，在蓝牙列表里发现名称为 `JBOX-xxxxxx` 的设备（后六位是 MAC 后缀）。
3. 选择 **Security 1（无需 PoP）**。不输入验证码，PoP 留空，应用通过 BLE 发给设备 Wi-Fi SSID 和 Wi-Fi 密码。ESP32-S3 仅支持 2.4GHz Wi-Fi。
4. 成功连接后 BLE 自动退出并释放内存。设备自动开启 **120 秒授权绑定窗口**。
5. 电脑连接同一可互访局域网，在工程根目录执行：

```bash
python client/jbox_api.py discover
python client/jbox_api.py info
python client/jbox_api.py status
# 如果希望明确执行绑定，也可先运行 python client/jbox_api.py pair
```

`discover` 通过 UDP 4748 广播自动找到设备 IP；**`info` 在首次使用、设备正处于按钮授权窗口时会自动执行绑定**，不需要你先执行 `pair`。如想明确手动触发，才使用 `pair` 命令。设备创建（或重用）内部 **256-bit 随机控制密钥**，客户端保存在用户目录 `~/.jbox/keys.json`，不在终端输出密钥。以后 `info/status/控制命令` 都自动读取密钥并鉴权，无需再次输入。

- 若 120 秒到期：在 Wi-Fi 已连接时**短按一次按钮**，再运行 `info`（会自动绑定）或 `pair`。
- 若有多台设备：`discover` 列出它们，用 `--host 192.168.x.x` 指定目标。已保存的密钥按设备 ID 持久化；IP 地址变化后自动发现并复用。
- 若电脑在不同 VLAN、广播被路由器阻止：使用 `--host 设备IP`，这个 IP 可在路由器客户端列表找到；不需要 UART。
- 如果必须做到用户连 Wi-Fi 密码都不输入，需要额外的路由器/手机支持（例如 WPS/DPP），**普通 BLE 配网本身无法知道路由器密码**。

**关于免码模式的安全边界**：BLE Security 1 在没有 PoP 时可保护被动窃听，但**不具备对主动中间人攻击的身份验证**。此外当前 TCP `PAIR` 在可信 LAN 内明文传送新绑定的随机密钥，只有**按钮开启的 120 秒内第一位请求者**能领取；攻击者若在这段时间能访问 LAN，也可能抢先领取或窃听。务必在可信 Wi-Fi / 隔离实验网络使用，绝不能把 TCP 4747 或 UDP 4748 暴露到公网。需要抵御恶意局域网参与者时，应升级为经过设备身份验证的 TLS 或 BLE OOB 方案；仅有按键不等同于密码学认证。

## 3. ESP-IDF 编译、烧录

```powershell
cd ProgrammableMatrixS3
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM5 flash
```

`COM5` 按实际端口修改。`sdkconfig.defaults` 预置 ESP32-S3、16MB Flash、Octal 8MB PSRAM、BLE、Wi-Fi。UART0（GPIO43/44）仍保留用于开发调试，但**所有正常配网/绑定操作都不需要 UART**。

`partitions.csv` 为 N16R8 分配 **4MB 应用分区**（起始地址 `0x10000`，无 OTA），其余 Flash 暂留备用；NVS 与 PHY 分区保持默认地址。BLE 配网使用 BLE 4.2 广播接口，已在默认配置中启用。

配对按钮不需要预存设备名、密码、ID、配对码，也无需印刷 QR 码。BLE 名称由设备 MAC 自动生成。

## 4. ESP32-S3 GPIO 资源

| 用途 | 物理 GPIO | GPIO API ID |
|---|---|---|
| 9 路进入 CH446Q 矩阵 | GPIO1,2,3,4,5,6,7,8,9 | 0～8 |
| **配对按钮**（低电平触发） | **GPIO10** | **不对外开放** |
| 9 路直出可编程 IO | GPIO18,21,38,39,40,41,42,47,48 | 9～17 |
| CH446Q DAT/CK/RST | GPIO11/12/13 | 保留 |
| CH446Q U2/U3/U4/U5 STB | GPIO14/15/16/17 | 保留 |
| 原生 USB D−/D+ | GPIO19/20 | 保留 |
| UART0 TX/RX | GPIO43/44 | 保留 |
| BOOT | GPIO0 | 保留 |
| PSRAM 占用 | GPIO35/36/37 | 不使用 |
| 启动特殊脚 | GPIO45/46 | 保留 |

GPIO3 是 strapping 引脚，外部矩阵必须保证上电期间不被错误驱动。GPIO39～42 和 JTAG 可能互斥。

## 5. CH446Q 节点映射、8 网络

52 个路由 Node ID：0～19 为 CN1 的 20 对端子，20～39 为 CN2 的 20 对端子，40～48 为 9 个 EXT，49=5V，50=3.3V，51=GND。

CN1、CN2 各有 40 个物理脚，**相邻奇偶脚在 PCB 上直接连通**：1↔2、3↔4……39↔40。节点编号为 `CN1: (脚号-1)//2`，`CN2: 20+(脚号-1)//2`。按照片方向，上排从左到右为 39、37……1，下排为 40、38……2。硬件直通不能通过软件断开。

| CH446Q | X0～X15 的节点 |
|---|---|
| U2 | CN1 端子对 1/2～31/32（node 0～15） |
| U3 | CN1 端子对 33/34～39/40（node 16～19）+ CN2 端子对 1/2～23/24（node 20～31） |
| U4 | CN2 端子对 25/26～39/40（node 32～39）+ EXT1～8（node 40～47） |
| U5 | X0=EXT9（node 48）、X1=5V（49）、X2=3.3V（50）、X3=GND（51）；X6～X11、X12～X13 为部分 CN1/CN2 节点的重复引出，固件保持这些重复开关断开 |

四颗同名 Y0～Y7 全部并联成 `NET0`～`NET7`；不同 NET 不能互连。软件最多管理 8 个相互独立网络，同一网络可同时含多个节点；电源冲突、两个 MCU EXT 直接相互连接等有安全限制。

CH446Q LQFP44 引脚：X0～X15 为 `31,30,29,28,27,26,3,4,5,6,7,8,25,24,1,2`；Y0～Y7 为 `33,35,37,39,17,15,13,11`；VDD=38→5V、GND=12→GND、VEE=16→GND、P/-S=10→GND，DAT=36→GPIO11、CK=34→GPIO12、RST=41→GPIO13、STB=14 分别→GPIO14～17；AX0～AX3=43/18/19/42→GND、AY0～AY2=20/21/40→GND，NC=9/22/23/32/44 悬空。**按照 CH446Q 当前采购批次官方资料再核验引脚**。

CH446Q 的 RST 高电平有效，板上建议硬件上电时保持清零的复位电路；4 路 STB 下拉。CH446Q 不能当成低阻电源线，5V/3.3V/GND 应独立限流，EXT1～9 要做过压保护，否则 5V 连入 ESP32 可能永久损坏芯片。

## 6. 可视化连线工具与 Python SDK

Windows 双击工程根目录的 `start_gui.cmd`，或运行：

```powershell
python client/jbox_gui.py
# 也可预填设备 IP：
python client/jbox_gui.py --host 192.168.1.100
```

工具使用 Python 自带的 Tkinter，无需额外安装界面依赖。电脑需与设备位于同一可互访局域网。

1. 点击“搜索设备”或输入 IP；首次使用时短按并松开 GPIO10，再点击“按钮授权绑定”。后续点击“连接”即可。
2. 在右侧选择 NET 1～8，点击 CN1/CN2 的端子或下方矩阵 GPIO 加入网络。相邻奇偶孔始终同步；再次点击移除，点击其他网络中的端子会将它移到当前组。
3. 点击“应用连线”。界面会读取设备状态确认；编辑、打开方案都只改变本地设计，应用后才改变实物连线。
4. “保存方案 / 打开方案”使用 JSON 文件；“清空当前组”编辑设计；“全部断开 / 高阻”立即释放设备连线和 GPIO。
5. 小排针按实物显示，上排从左到右为 **GND、48、47、42、41**，下排为 **18、21、38、39、40**。这些 GPIO 为直出引脚，点击后在右侧设置输入/推挽输出/开漏输出与初始电平，不进入矩阵路由。GND 与矩阵 GND 是同一电源节点。

窗口保持 TCP 长连接并定期读取状态；关闭窗口或断网后，固件自动清空矩阵连线并恢复 GPIO 高阻。CN1/CN2 的奇偶孔是硬件直通，不能断开。当前工具沿用固件的电源与 MCU EXT 连线限制，最多 8 个独立网络。固件节点 ID 和 CH446Q 映射没有改变，仅将原 H2/H6 的命名更正为 CN1/CN2 并补全真实脚号。

### Python SDK 示例

```python
from client.jbox_api import discover_devices, MatrixClient

devices = discover_devices()
assert len(devices) == 1, "需要指定目标设备"
with MatrixClient(devices[0]['host']) as board:
    print(board.caps())

    # EXT1 (node40) 连 CN1-1/2(node0)，EXT2 (node41) 连 CN1-3/4(node1)
    board.route_set([[40, 0], [41, 1]])
    board.i2c_setup(bus=0, sda=0, scl=1, hz=50000)
    print([hex(x) for x in board.i2c_scan(0)])
    board.i2c_close(0)

    # IO9 为直出 GPIO18，与 GPIO10 配对按钮无关
    board.gpio_config(9, mode=1, initial=0)
    board.gpio_write({9: 1})
    board.safe()
```

对于通过 CH446Q 的 I²C，必须外接物理 SDA/SCL 上拉电阻（如 4.7kΩ 作为初始参考），不能将 I²C 线直接短接到 3.3V 电源。通过矩阵传输的 SPI、UART 要从低速验证。

其他支持的 API（Python SDK）：`connect_nodes`、`remove_node`、`route_clear`、`gpio_config/read/write`、`adc_read`、`pwm`、`i2c_setup/xfer/close`、`spi_setup/xfer/close`、`uart_setup/read/write/close`、`safe`、`factory_reset`。详见 `client/jbox_api.py`、`client/demo_i2c.py`、`client/demo_spi_uart.py`。

**重要**：设备只允许一个 TCP 控制会话，断开会清空路由并把 GPIO 恢复为高阻；想持续维持连线，必须保持一个长连接。命令行执行完就断开，适合诊断，不适合常驻电路。

## 7. 纯二进制协议与鉴权

### 上传小程序：JBoxScript

支持把独立 `.jbox` 小程序上传到 ESP32，随后像调用函数一样带参数调用。固件只包含通用执行器；GPIO 时序、DHT11 解码和校验由上传的小程序执行，协议不内置在固件中。支持常量循环、局部变量、位运算、采样和 `emit` 返回值。完整语法与约束见 [JBoxScript 文档](docs/JBoxScript.md)。

```python
from client.jbox_api import MatrixClient

with MatrixClient('192.168.1.100') as board:
    program = board.upload_function('fn add_example(){emit(20 + 22);}')
    print(program())  # [42]，由 ESP32 计算后返回
```

DHT11 简洁显示窗口：双击 `start_dht11.cmd`，或运行 `python client/dht11_viewer.py --host 192.168.1.100 --connect`。窗口会上传 `client/programs/dht11.jbox`，配置 CN1 的 39=5V、37=DATA(GPIO1)、35=NC、33=GND。DATA 使用 GPIO1 内部的 3.3V 弱上拉，可按需要增加约 4.7kΩ 到 3.3V 的外部上拉。采集、解码与校验均在 ESP 上完成，电脑显示结果。需要此版本固件，运行前请断开主连线工具的控制会话。

DHT11 示例仍处于实板调试阶段：上传函数和整数计算已在设备验证，DHT11 实机目前未收到有效响应，尚未确认 DATA 矩阵通路和解释器的微秒时序。主机波形模拟通过不代表传感器实测通过。窗口 IP 留空时自动选择局域网唯一发现的设备，也可手动填写。

### 传输与授权

- **设备发现**：UDP 端口 4748，广播字节 `JBOX-DISCOVER`；设备单播 ASCII `JBOX-HERE|<12位十六进制MAC>|<paired0/1>|<window0/1>`。不包含任何密钥。
- **控制**：TCP 端口 4747。协议 `JMX1`，固定 16 字节头 + payload（最长 8192B）+ 认证后 16 字节 HMAC 标签；整数小端；header=`magic[4],version:u8,cmd:u8,flags:u16,seq:u32,payload_len:u32`。
- **物理绑定**：按钮打开 120 秒窗口，TCP `CMD_PAIR=0x03`，无负载、无需 MAC，成功应答 `int32 status=0` 后跟 32B 随机密钥；只能第一客户端领取，立即锁窗；客户端自动存本地文件且不打印。**绑定交换没有 TLS，只适合可信 LAN**。进入配对窗口会中止原控制会话并将 IO 恢复到安全状态，以便让新控制端绑定。
- **普通鉴权**：连接后 `HELLO=0x01` 取 16B nonce；`AUTH=0x02` 提交 `HMAC-SHA256(device_key, nonce||"JMX-AUTH-v1")`；会话密钥 `HMAC-SHA256(device_key, nonce||"JMX-SESSION-v1")`；后续所有请求/应答均带 `HMAC(session_key,header||payload)[:16]`，序号单调递增。HMAC 保护身份和完整性，但**不加密控制内容**。
- **性能**：长连接、`TCP_NODELAY`，不采用逐 bit 网络模拟。I²C/SPI/UART 使用 ESP32-S3 硬件外设；GPIO 支持位掩码批量读写；矩阵通过先断后连两阶段刷新。CPU/无线网络和模拟开关电气指标会决定实际速率。

### 命令集合

| 命令 | ID | 二进制请求字段 |
|---|---|---|
| HELLO / AUTH / PAIR | `01/02/03` | 分别无/32B Proof/无 |
| CAPS / STATUS | `10/11` | 无 |
| ROUTE_SET | `20` | `u64 masks[8]` |
| CONNECT / REMOVE_NODE / ROUTE_CLEAR | `21/22/23` | `u8 a,b` / `u8 node` / 无 |
| IO_CONFIG / IO_WRITE / IO_READ | `30/31/32` | `u8 io,mode,pull,initial` / `u32 mask,levels` / `u32 mask` |
| PWM / ADC_READ | `33/34` | `u8 channel,io;u32 hz;u16 duty` / `u8 io` |
| I2C_SETUP / I2C_CLOSE / I2C_XFER | `40/41/42` | `u8 bus,sda,scl;u32 hz` / `u8 bus` / `u8 bus,addr;u16 txlen,rxlen;u8 tx[]` |
| SPI_SETUP / SPI_XFER / SPI_CLOSE | `50/51/52` | `u8 bus,mosi,miso,sck,cs,mode;u32 hz` / `u8 bus;u16 len;u8 tx[]` / `u8 bus` |
| UART_SETUP / UART_WRITE / UART_READ / UART_CLOSE | `60/61/62/63` | `u8 bus,tx,rx;u32 baud;u8 parity,stopbits` / `u8 bus;u8 data[]` / `u8 bus;u16 max_count,timeout_ms` / `u8 bus` |
| SAFE / FACTORY_RESET | `70/7F` | 无 / ASCII `YES!`（已鉴权） |
| FUNCTION_LOAD / CALL / CLEAR | `80/81/82` | 详见 `docs/JBoxScript.md`，均需鉴权 |

GPIO IO ID 范围 **0～17**。SPI 总线支持 SPI2/3、DMA，UART1/2 支持缓冲收发、保留 UART0；I²C0/1 主机事务和重复起始；ADC1 和 4 路 PWM。硬件输出速度上限不是实测保证。

错误码：`0` 成功，`-1` 参数错误，`-2` 资源占用，`-3` 硬件故障，`-4` 高风险连线，`-5` 不支持，`-6` 超时，`-9` 8 网络资源用尽，`-10` 未配置。通信安全错误与无授权绑定返回认证错误。

## 8. 纯主机测试

```bash
python -m unittest discover -s tests -p 'test_*.py' -v
# Requires GCC on PATH; runs the portable C route and VM tests in a temp directory.
python tests/run_native_tests.py
```

### 推荐实板验收顺序

1. 仅给 MCU 和 CH446Q 控制电路上电，验证 4 个 STB、RST 时序及 52 节点映射。
2. 测 GPIO10 按钮短按 BLE 180 秒广告、无 PoP、BLE 配网后 IP 获取及 120 秒 LAN 绑定；断电重启后确认不再要求配网/密钥。
3. 测短按重新开放 LAN 绑定，长按 6 秒仅擦 Wi-Fi、12 秒完整恢复出厂设置。
4. 测 18 GPIO 高阻默认、矩阵路由安全检查、电源误接防护；随后以低电流 LED 实验。
5. 测 I²C 10～50kHz、UART 9600～115200、SPI 100kHz 起逐步验证；长时间稳定性压测与意外 Wi-Fi 断开安全状态。

无屏幕/无 UART 提示时，建议硬件后续增加状态 LED：等待配网、配网中、联网、配对成功、错误各有不同闪烁状态（此版不占用第 9 个直出 IO 实现 LED）。

官方文档：[ESP-IDF Wi-Fi BLE Provisioning](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/provisioning/wifi_provisioning.html)；[CH446Q](https://www.wch.cn/downloads/CH446DS1_PDF.html)。
