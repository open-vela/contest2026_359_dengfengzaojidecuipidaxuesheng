# P4 主板连接 JBOX-S3

本实现让现有 ESP32-P4 openvela 主板通过 Wi-Fi／以太网控制 [ProgrammableMatrix](https://github.com/streetartist/ProgrammableMatrix) 的 JBOX-S3。协议依据提交 `2ca78dcb9c9b825b943da3b36e7baf19f558d91d` 的 `main/net.c`、`main/api.c` 与 `client/jbox_api.py`；本次在仓库内 S3 工程增加本地串口配网入口，不占 P4 排针，不改变其他服务配置。

## 一键配网与绑定

可复用的 Codex 操作流程见 [jbox-p4-control skill](../tools/skills/jbox-p4-control/SKILL.md)，
包含可直接在 P4 助手输入的发现与只读连接示例，以及用户串口绑定步骤。
1. 主板先连接要使用的 Wi-Fi。打开「设置 → JBOX 扩展板」，页面自动搜索局域网和蓝牙设备。
2. 首次配网时短按 JBOX 的 GPIO10 按键，或在其本地 UART0（115200 8N1）发送 `jbox provision` 加换行，启动限时 BLE 配网。在主板页面点击对应设备的「一键配网」。已有 Wi-Fi 但需要改成主板当前网络时使用 `jbox provision`，不需要先清空 Wi-Fi。
3. 主板自动读取自身当前已连接的 Wi-Fi，将同一个 SSID 和密码通过 Security1 加密会话发送给所选 JBOX。无需选择网络或填写密码，也不另开主板热点。
4. 等待 JBOX 联网、限时按键授权窗口、保存绑定、认证连接和状态回读，全部成功后才显示完成。已经联网的设备可点击「绑定并校验」。多块设备时仅需选择目标设备。

主板未连接 Wi-Fi、正在试连或配网中途切网时停止；不会使用旧凭据，也不会自动重发配网或绑定。取消后先清理本次连接再回执。密码只在原生服务内部短暂使用，不进入页面状态、日志、HTTP、JS 或 Agent。配网后的连接验证会释放临时控制连接，供助手和应用使用。

JBOX 固件的 Security1 使用 X25519/AES-CTR，当前上游没有 PoP；依靠本地按键／串口窗口与用户选择设备授权。原有 LAN 绑定仍要求可信局域网。已有 Wi-Fi 时短按或串口 `jbox pair` 只打开 LAN 绑定窗口。`jbox provision` 通过一次软件重启显式进入重新配网窗口，进入窗口不擦 NVS，保留控制密钥。命令和测试见 [S3 集成说明](../app/ProgrammableMatrix/LOCAL_CHANGES.md)。

2026-11-05 实板已验证串口进入 BLE 配网、P4 下发当前 Wi-Fi、JBOX 获取地址、P4 原生绑定和鉴权状态读取，以及普通助手／应用共用入口连接。JBOX 软件重启后使用原绑定再次鉴权通过。尚未验证 GPIO 输出、外接电路或断电冷启动；当前稳定性限制见下文。以下控制台与应用调用仍可用于已联网设备：

也可以在主板 NSH 控制台绑定：

```text
desktop jbox-bind 192.168.1.100 aabbccddeeff
```

密钥保存在主板 `/data/config` 的 portal `jbox` 配置节；不放 SD、不写日志、不返回给 JS／模型。HTTP 配置读取仅返回 `key_set`。首次绑定保存沿用 AI 配置协调，等待活动任务退出后再写 Flash；相同配置跳过写入。保存失败如实返回，可重新按键领取相同 S3 密钥后重试，不自动执行控制动作。目前保存一台扩展板的绑定。

## 应用和助手

完整方法与参数见 [硬件 API 的 JBOX-S3 部分](../app/espdl-quickapp/overlay/apps/system/espclaw/guide/HARDWARE_API.md#jbox-s3-局域网矩阵与外设扩展)。原有异步任务使用方式不变：

```javascript
// 用户已完成绑定。每一步先轮询，不能把任务 ID 当作完成结果。
const task = system.jbox.connect({});
// 在应用定时回调中执行：
const reply = system.hardware.poll(task);
// reply.done && reply.ok 后：
const generation = reply.result.generation;
const statusTask = system.jbox.status({generation});
// 状态、连线、GPIO、I2C、SPI、UART、JBoxScript 均为异步任务。
// 用户要求结束时：
const stopTask = system.jbox.safe({generation});
// stopTask 完成后，可再 disconnect，或按应用需求保持空闲长连接。
```

助手通过内置 board MCP 的 `hardware_capabilities`、`hardware_reference({section:8})`、`hardware_submit`、`hardware_poll` 使用相同实现，例如 `operation:"jbox.status"`。普通闲聊模式沿用原来的 C 层工具权限隔离。助手不能领取或读取绑定密钥，不能触发工厂重置。

### JBOX 专用 MCP 工具

助手首轮直接提供下列 28 个 `jbox_*` 工具的独立 schema，参数沿用上面的 QuickJS API；
也可通过 `mcp_list_tools({server:"board"})` 发现后，用 `mcp_call_tool` 包装调用。
发现结果有 `nextCursor` 时继续翻页，直到没有游标。它们复用现有硬件控制器和任务队列，
与通用 `hardware_submit` 的连接、generation 和资源所有权相同，不建立第二套会话。

| 专用工具 | 对应 `system.jbox` 方法 |
| --- | --- |
| `jbox_discover`、`jbox_connect`、`jbox_disconnect` | `discover`、`connect`、`disconnect` |
| `jbox_caps`、`jbox_status`、`jbox_safe` | `caps`、`status`、`safe` |
| `jbox_route_set`、`jbox_connect_nodes`、`jbox_remove_node`、`jbox_route_clear` | `routeSet`、`connectNodes`、`removeNode`、`routeClear` |
| `jbox_gpio_config`、`jbox_gpio_write`、`jbox_gpio_read`、`jbox_adc_read`、`jbox_pwm` | `gpioConfig`、`gpioWrite`、`gpioRead`、`adcRead`、`pwm` |
| `jbox_i2c_setup`、`jbox_i2c_transfer`、`jbox_i2c_close` | `i2cSetup`、`i2cTransfer`、`i2cClose` |
| `jbox_spi_setup`、`jbox_spi_transfer`、`jbox_spi_close` | `spiSetup`、`spiTransfer`、`spiClose` |
| `jbox_uart_setup`、`jbox_uart_write`、`jbox_uart_read`、`jbox_uart_close` | `uartSetup`、`uartWrite`、`uartRead`、`uartClose` |
| `jbox_function_load`、`jbox_function_call`、`jbox_function_clear` | `functionLoad`、`functionCall`、`functionClear` |

`hardware_poll`、`hardware_stop` 和 `hardware_release` 同时提供直接调用和 board MCP 调用。
通用 `hardware_submit` 保留兼容；没有 `jbox_pair`、密钥读取或工厂重置工具。
聊天与创作模式在工具桥接层和 Agent 层都拒绝这些直接工具及 board MCP。

下面是 `mcp_call_tool` 的参数示例；先完成 board 工具发现，仅调用用户当前要求的操作。
直接调用时以 `tool` 为函数名，传入内层 `arguments` 即可：

```json
{"server":"board","tool":"jbox_discover","arguments":{"timeoutMs":1500}}
```

返回 `{"ok":true,"id":N,"state":"submitted"}` 仅表示受理。用实际返回的任务编号 N 轮询：

```json
{"server":"board","tool":"hardware_poll","arguments":{"id":1}}
```

示例中的 `1` 必须换成这次实际编号。取得 `done:true`、`ok:true` 后读取 `result`，
确认发现目标并由用户完成控制台绑定，再连接已保存的设备：

```json
{"server":"board","tool":"jbox_connect","arguments":{}}
```

再次轮询到成功，取连接结果的 `generation`。以下 `7` 仅为示例，必须换成本次真实代次。
依次提交、轮询读取能力、状态和 IO0 的数字电平；这些调用不改变路由或 GPIO 模式：

```json
{"server":"board","tool":"jbox_caps","arguments":{"generation":7}}
{"server":"board","tool":"jbox_status","arguments":{"generation":7}}
{"server":"board","tool":"jbox_gpio_read","arguments":{"generation":7,"mask":1}}
```

配置／写入类工具使用独立 schema 校验必填参数、类型、取值范围、数组长度、额外及重复字段；
跨字段约束（例如总传输长度、路由节点跨网络重复、引脚冲突）仍由同一 JBOX 执行器在发送前检查，
因此提交成功后也必须轮询检查任务错误。字节数组最多4096字节，但编码后的完整工具 JSON 仍受16KiB上限约束。
取消任务用 `hardware_stop` 并继续轮询；完成回执只消费一次。
`safe`、`disconnect`、`hardware_release` 会改变已有硬件状态，不能作为只读检查的收尾步骤。

schema 与分派表源文件为 `tools/generate_jbox_tools.py`，生成的 `glass_jbox_tools.h` 同时供直接工具、
MCP 发现和参数校验使用。修改后运行该生成器；修改助手提示词还需运行 `tools/generate_agent_profiles.py`。

JBOX 的 io0～17 是 API ID（映射为 S3 GPIO1～9、18、21、38～42、47、48），不是 P4 GPIO。矩阵 node0～51 与这些 io ID 也不相同。18路 GPIO 与52节点、8网络都由 S3 实际能力／状态返回。不能把 SDK 应答当作真实传感器、连线和电气验收。

## 生命周期与错误

- 同一硬件控制器保存长连接，连续 RPC 使用递增序号、双向 HMAC 和严格的帧长度／命令／序号验证；TCP 半帧会累计接收。
- 每次连接分配新的 generation；旧任务的代次不能操作新连接。当前连接存在时再次 connect 返回 EISCONN，不隐式断开连线。
- 超时、取消、截断、错误 MAC 或畸形帧后关闭连接，不自动重连／重放。远端业务错误单独返回 remoteStatus；例如 -4 是 S3 拒绝高风险连线。
- 原有助手完成后可保留已完成硬件状态；显式 hardware_release／取消未完成轮次／应用退出才销毁控制器。销毁会关闭 JBOX TCP。
- S3 当前只支持一个控制会话；电脑 GUI 与 P4 不应同时占用。失联安全清理由上游执行，拔网线时的检测有 TCP keepalive 延迟。

## 验证入口

在 Linux／WSL 运行（apps 指向已有的 openvela apps）：

```bash
python3 tools/test_jbox.py --apps /path/to/apps --output /tmp/jbox-tests
python3 tools/test_jbox_tools.py --apps /path/to/apps --output /tmp/jbox-tool-tests
python3 tools/test_portal_storage.py --apps /path/to/apps --output /tmp/jbox-portal-tests
python3 tools/test_agent.py --apps /path/to/apps --output /tmp/jbox-agent-tests
```

协议测试用独立 Python hashlib/hmac 对端驱动实际 C 客户端，覆盖分片、鉴权、4096B 收发、参数拒绝、发现、业务错误、取消、超时、慢速滴流、损坏 MAC／帧和资源释放，并使用 ASan／UBSan。测试发现地址编译为127.0.0.1，不向真实局域网广播。存储测试使用临时 Flash 替身，不操作真实配置。固件仍使用原 `tools/build_bilibili_v3.py`，新增源码由桌面 Makefile 链接；编译通过与实板联调分别记录，不能把宿主测试当成已连接真实 JBOX。

专用工具测试链接真实 Agent、参数 schema 和异步调度器，逐个核对28个工具的直接／MCP 分派及参数保持，
检查分页、权限、无效参数、任务上限、并发所有权和取消清理。平台为隔离替身，不调用局域网、真实配置或设备。

## 2026-11-05 专用 MCP 补全验证

28个专用工具的直接／MCP路由、参数保持、schema一致性、分页、非法参数拒绝、模式权限、
控制器所有权、任务上限、取消及清理通过 ASan／UBSan 宿主测试。原有22项 JBOX 通信协议测试、
Agent／MCP桥接、助手服务与助手平台回归通过，skill格式和文档链接检查通过。
P4目标编译未完成：现有准备树缺少 `arch/risc-v/src/common/Toolchain.defs`，其架构头链接也缺少
`arch/chip/irq.h`。本次未重新链接或烧录固件；下面的镜像记录属于11月4日版本，不包含本次专用工具。

## 2026-11-04 离线验证结果

最终22项通信测试、28项配置页测试，以及助手权限、绑定存储、AI配置协调、语音取消／唤醒、Home Assistant 和素材缓存回归均通过。新增通信源码以 `-Wall -Wextra -Werror` 编译，保留 ASan／UBSan 检查；旧 SHA 依赖的历史格式警告单独隔离。

完整 ESP32-P4 固件已编译链接并通过分区、BSS、输入源码摘要与链接符号检查。镜像 SHA256 为 `b40a52c392eb9f44f54de27378d95d3517382f72674f10cde9660a1e5ff764b0`。双 AI、Medium 默认与 Low／Medium／High 最多4／5／6轮、109版ASR、音乐、锁屏及小鸟源码保持原有实现。三个实际链接的官方 DSP 内核指令与已部署109版逐字节一致。

重复构建暴露的 DSP 依赖扫描问题已修复：构建器将原有兼容头限定在私有 DSP 目录，并用生成的源码包含入口保持官方文件完整；不向其他模块增加全局 IDF 头搜索路径。`tools/test_asr_dsp_dependencies.py` 先复现旧布局的扫描失败，再验证重复准备、无私有 CFLAGS 时的依赖生成与三个内核编译。工具参考的会话说明也已与实际的“完成后可保持长连接，显式关闭／取消时释放”行为同步。

本次仅做宿主与离线构建验证，未烧录、未操作板上配置，也未完成实体 JBOX-S3 联调。当前板上109版保持不变；重新构建产生的新 BLE 测试身份不能当作原身份的配对兼容性验收。

## 2026-11-05 一键配网宿主验证（早期阶段）

本机通过 12 项 Security1 协议测试（独立 protobuf/密码学对端，包含 2000 个畸形输入）、原生常驻工作线程测试，以及真实 Hosted protobuf 的当前 STA 凭据读取与原有试连回滚回归。验证了未联网拒绝、试连排斥、切网终止、凭据边界、取消清理、连接所有权及动作不重放。PSA 适配单元使用实际 MbedTLS 头编译；未执行生产 PSA 后端。

本轮尚未完成 Linux ASan/UBSan、全量 BLE/Agent 回归、界面渲染、固件构建、烧录和真实 JBOX 联调。WSL 构建调用被自动审批拦下，返回工作区额度不足。本轮宿主结果不能作为已部署或实板成功证据。

## 2026-11-05 后续实板验证与当前限制

上述早期阶段的构建／实板缺口已继续验证：BLE 8 连接、JMX1、原生配网 worker、当前 STA 凭据／试连回滚、portal 存储、Agent 与 JBOX 工具回归通过；Linux 测试覆盖 ASan/UBSan。Security1 协议另有 12 项独立 protobuf／密码学测试及 2000 个畸形输入。宿主测试不代表真实外接电路验收。

P4 使用已校验并烧录的固件 SHA256 `91fbadbb942759ba48a2636affceb71be9cbee6251885abd2324fdbe7e8a174a`。S3 UART0 的 `jbox provision` 实际启动 `JBOX-E0BA70` 广播，P4 经加密 BLE 下发其当前 Wi-Fi，S3 获取 DHCP 地址；随后本地绑定保存 Flash、鉴权及 `status` 读回成功，页面发布 READY。普通 `desktop hardware-call jbox.connect {}` 返回 `authenticated:true`；S3 软件重启后不重新领取密钥，使用保存的绑定再次鉴权成功。临时诊断会话结束即释放，绑定保留。

提交前另以大赛最新 S3 基线加本次三份串口／配网源码独立构建，排除本机其他未提交脚本功能修改。该 S3 候选 SHA256 `fa9b52c5331755d1fd53126108188481e83f7517f79d49c67a11e3fa7e3c443b`，app-only 写入校验和 NVS 摘要保留检查通过；串口帮助、联网恢复、P4 原绑定鉴权状态读回及普通入口连接均有实板验证。它不是把本机其他开发修改一起提交的快照。

当前有两项未修复的 P4 运行时限制，不能把以上结果称为所有状态下一次点击必成：

- Wi-Fi worker 的旧状态可能导致首次提示未联网，即使接口已有地址。本地 Wi-Fi 页「查询状态」刷新后才能继续；被拒绝的尝试没有下发凭据。
- 连续运行后出现 socket 资源分配失败：实板 `sock_file_poll` setup 返回 -16，配网页面最终报告 -104，另一只读发现返回 -12。内核固定 devif callback 池为 32，尚未证明耗尽来源或修复底层问题。P4 软件重启后发现、绑定和鉴权通过。遇到失败应先排查或显式重新开始，不能自动重发配置／绑定。

GPIO10 实际按键、180 秒窗口到期、断电冷启动、GPIO／矩阵输出、真实传感器和显示闪屏观察仍需分别验收。配网权限保持本地 UI／串口授权，Agent／MCP 不能借通用 UI 点击进入此流程；无 PoP BLE 与 LAN 绑定仅用于可信现场环境。


## 2026-11-05 CN 映射、JBoxScript 与 DHT11 实板验收

当前 `jbox.caps` 增加 boardMap：CN1 的第 r 行（r=0..19）奇偶脚 2r+1/2r+2 内部相连，均对应 node r；CN2 同行对应 node 20+r。EXT1..9 对应 node40..48、IO0..8，实际 GPIO 仍读取 ioGpios。node49=5V、node50=3.3V、node51=GND。这些编号不能互换，5V 不得接 DATA。

新增第29个专用工具 `jbox_function_compile`，直接调用、board MCP 与 QuickJS `system.jbox.functionCompile` 使用相同 C 校验。参数包含当前 generation、slot，以及二选一的 source 或 program:"dht11"。原生有界编译器支持参考工程 JBoxScript，包括 RMT capture/pulse 操作；编译后仅上传，不执行或自动连线。远端 S3 必须包含本次同步的 RMT VM/驱动；v1 caps 没有远端 RMT 版本协商，不能仅凭 caps 宣称旧 S3 支持。functionCall 可指定 delayMs（0..5000），取消期间不发送调用，不自动重试。

用户确认实物接线 CN1-1=VCC、CN1-3=DATA、CN1-5=GND；按参考工程 VCC=5V、DATA 上拉3.3V，IO8=GPIO9。对应 nets 为 [[0,49],[1,48],[2,51]]。先读取状态，已有线路须明确处理，routeSet 替换全部线路。编译 program:dht11 到 slot0 后，functionCall 使用 ios:[8]、上电 delayMs:2000；结果 [temperature10,humidity10] 分别除以10得到 °C 与 %RH。继续采样须显式授权并保持传感器间隔，失败不重放。

宿主：原生 C/Python 编译器156项有效语料逐字节对照、26项拒绝、2000项随机输入，以及10000项 ASan/UBSan 随机执行通过；真实 S3 VM 校验器接受 DHT11 的940条指令。24项协议回归、29工具直接/MCP权限与参数测试、Agent 全套回归和 S3 native/script 测试通过。

固件：P4 独立工作区构建通过，镜像 SHA256 `61c11b2ec5485fcf6ff3b1b488ba8955d9927c4c49db349548b6637f6c6ee8d4`。app-only 烧录与独立读回校验通过，用户数据摘要前后相同；软件复位后 UI、Wi-Fi、XIP 启动状态正常。候选 BLE 身份 D9:47:8C:B0:01:4B，广播 OpenVela Mouse-8CB0014B；未进行该身份的电脑首次配对/HID验收。

实板：P4 原生诊断经实际 JBOX 依次读到26.4°C/51.0%、26.4°C/51.0%、26.3°C/51.0%，三次 RMT 数据校验通过，诊断结束释放连接。随后真实助手单轮完成 caps、脚本编译上传、路由及状态回读和一次采样，返回原始 [268,540]，即26.8°C/54.0%，CHAT_DONE、error=0。该助手按既有约定保留连接及供电线路，没有自动重读。

S3 当前实测镜像 SHA256 `b91049510bdab1d3df75dcbd2a1d5d27bcf9833285b8f90e48921efc2060a21c`，本轮未重烧 S3。此结果覆盖上述接线与一次真实 AI 流程，不代表其他外设、冷断电、实体屏幕或旧 BLE 身份兼容性验收。此前 Wi-Fi worker 旧状态及 socket 资源问题没有在本次修复。
