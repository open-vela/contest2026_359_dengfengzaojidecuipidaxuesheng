# P4 主板连接 JBOX-S3

本实现让现有 ESP32-P4 openvela 主板通过 Wi-Fi／以太网控制 [ProgrammableMatrix](https://github.com/streetartist/ProgrammableMatrix) 的 JBOX-S3。协议依据提交 `2ca78dcb9c9b825b943da3b36e7baf19f558d91d` 的 `main/net.c`、`main/api.c` 与 `client/jbox_api.py`；不改 S3 固件、不占 P4 排针，不改双 AI、ASR、桌面和播放器的配置。

## 首次连接

1. S3 按上游流程完成 BLE 配网，P4 与 S3 接入可互访局域网。
2. 在 P4 应用调用 `system.jbox.discover({timeoutMs:1500})`，轮询任务取得设备 IP 与 deviceId。多设备时让用户选择，不默认操作第一台。
3. 短按 S3 的 GPIO10，在120秒窗口内显式调用 `system.jbox.pair({host:"192.168.1.100",deviceId:"aabbccddeeff"})`。只返回保存结果。绑定使用上游的明文 LAN 密钥领取流程，要求可信局域网。
4. 调用 `system.jbox.connect({})` 使用已保存绑定，保存返回的 generation。IP 改变时重新 discover，再显式 connect 指定新 host 与原 deviceId；不自动重发旧连线。
5. 后续各调用携带 generation，并轮询到 done、ok；应用需要持续连线时保持运行，退出会断开并触发 S3 的安全清理。

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
python3 tools/test_portal_storage.py --apps /path/to/apps --output /tmp/jbox-portal-tests
python3 tools/test_agent.py --apps /path/to/apps --output /tmp/jbox-agent-tests
```

协议测试用独立 Python hashlib/hmac 对端驱动实际 C 客户端，覆盖分片、鉴权、4096B 收发、参数拒绝、发现、业务错误、取消、超时、慢速滴流、损坏 MAC／帧和资源释放，并使用 ASan／UBSan。测试发现地址编译为127.0.0.1，不向真实局域网广播。存储测试使用临时 Flash 替身，不操作真实配置。固件仍使用原 `tools/build_bilibili_v3.py`，新增源码由桌面 Makefile 链接；编译通过与实板联调分别记录，不能把宿主测试当成已连接真实 JBOX。

## 2026-11-04 离线验证结果

最终22项通信测试、28项配置页测试，以及助手权限、绑定存储、AI配置协调、语音取消／唤醒、Home Assistant 和素材缓存回归均通过。新增通信源码以 `-Wall -Wextra -Werror` 编译，保留 ASan／UBSan 检查；旧 SHA 依赖的历史格式警告单独隔离。

完整 ESP32-P4 固件已编译链接并通过分区、BSS、输入源码摘要与链接符号检查。镜像 SHA256 为 `b40a52c392eb9f44f54de27378d95d3517382f72674f10cde9660a1e5ff764b0`。双 AI、Medium 默认与 Low／Medium／High 最多4／5／6轮、109版ASR、音乐、锁屏及小鸟源码保持原有实现。三个实际链接的官方 DSP 内核指令与已部署109版逐字节一致。

重复构建暴露的 DSP 依赖扫描问题已修复：构建器将原有兼容头限定在私有 DSP 目录，并用生成的源码包含入口保持官方文件完整；不向其他模块增加全局 IDF 头搜索路径。`tools/test_asr_dsp_dependencies.py` 先复现旧布局的扫描失败，再验证重复准备、无私有 CFLAGS 时的依赖生成与三个内核编译。工具参考的会话说明也已与实际的“完成后可保持长连接，显式关闭／取消时释放”行为同步。

本次仅做宿主与离线构建验证，未烧录、未操作板上配置，也未完成实体 JBOX-S3 联调。当前板上109版保持不变；重新构建产生的新 BLE 测试身份不能当作原身份的配对兼容性验收。
