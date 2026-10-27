# ESP32-P4 原生 JPEG 编码适配

已完成原难度清单第 8 项的驱动、CSI 拍照保存接入和有界实板验证。当前为编码
candidate-3：应用独立读回、启动前数据区一致、两核 XIP、两轮各 19 组编码、
22 组解码回归，以及一次真实 CSI → JPEG → SD → 独立软件解码均通过。
当前镜像的 2048/CSI 并行内部 Flash 写擦、实体屏幕观察、冷上电和新 BLE
身份首配仍待验收，不能称为整版验收完成。手机热点网页按用户要求跳过，仍未验证。

本实现使用固定 SDK 的 JPEG HAL 和 NuttX VFS，不引入 FreeRTOS 编码池。
官方能力依据为 [Espressif JPEG 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/jpeg.html)。
固定 SDK 的源文件哈希和实际 HAL 编译结果记录在构建元数据及宿主证据中；
当前网页文档不代替固定版本源码契约。

| 能力 | 本次实现 | 边界与差异 |
|---|---|---|
| 设备 API | `/dev/jpeg0` 新增 `P4_JPEG_ENCODE` | 同步 ioctl；编码、解码、PPA 共用租约串行执行 |
| 彩色输入 | 原生小端 RGB565、BGR888 | SDK 所称原始 RGB888 编码输入实际按 BGR 排列，公共类型明确命名 |
| 采样 | YUV444、422、420 | 灰度输入仅对应灰度输出 |
| 质量 | 1–100，标准量化表缩放 | 不开放任意自定义量化/Huffman 表 |
| 尺寸 | 宽高各 ≤4096；MCU 补齐后 ≤4M 像素 | 输入为紧密排列的补齐帧；可见尺寸写入 JPEG SOF |
| 输出 | baseline JFIF；输出地址/容量 128 字节对齐，≤4 MiB | 无 progressive、硬件缩放、异步批处理或持续 MJPEG 编码服务 |
| 标记生成 | 有界 DQT/SOF/DHT/COM/SOS，标准 Huffman 全 16 档长度 | 头部补齐至 128 字节；DMA 可写容量扣除头部，防止尾部越界 |
| DMA | 内核堆对齐描述符；像素与输出在用户堆 | 明确 C2M，停 DMA 后 M2C；不写活动 framebuffer |
| 完成长度 | 按 RX 长度高/低字段合并，并核对容量与 EOI | 已实测超过 16 KiB 的纹理输出，不仅验证纯色小 JPEG |
| 失败 | 短输出返回 ENOSPC，输出长度为 0；默认超时 250 ms | 可配置 1–5000 ms；停硬件/清回调/释放租约后返回 |
| 应用包装 | 复制 RGB565，重复边缘补齐，返回自有 JPEG 缓冲 | 失败清空指针/长度，关闭 FD 并释放所有缓冲 |
| CSI 保存 | 原有裁切、缩放、风格处理后以质量 90 编码，宽 512 | 原有 MJPEG 原码保存和助手 BMP24 路径保持各自格式 |
| 存储 | 常驻异步服务；有 SD 优先 SD；容量检查、独占新建、fsync/close 回执 | SD 满/故障不静默写 Flash；编码后再检查 SD；保留旧照片 |
| 取消 | 不释放仍被 DMA 使用的缓冲；完成清理后才回执 | 取消不等于硬件作业在指令中间立即终止 |
| 软件验证 | 内置独立 TinyJpgDec 解码编码结果；本地 SD 文件也可只读验证 | 不经过硬件解码，不调用模型，不上传图像 |

生产入口为 `esp32p4_jpeg.c`、`esp32p4_jpeg_emit.c/.h`、公共 `p4_jpeg.h`、
`qpk_jpeg_encode.c` 和 `qpk_photo_store.c`。构建器继续显式使用
`--hardware-jpeg --psram-xip --ble-connections 2 --usb-camera`，JPEG 默认关闭。

## 当前固件与审计

制品：工作区 `output/jpeg-encode-20261026/candidate-3/`。

- 镜像 SHA256：`bf542ed81a6b7b2799be54dcaae25609782e733b9fc465c978a3aafb0219cfd4`。
- ELF SHA256：`f492e37350a6e57b7e58c2f9dd4ce93bf89a4d6ad60fa561128aaf0521e1b9ab`。
- BSS 末地址 `0x4ffabe5c`，应用末地址 `0x9ff900`，数据区仍为 `0xc00000..0x1000000`。
- 新 BLE 身份：`D2:08:3F:1A:74:C7` / `OpenVela Mouse-3F1A74C7`；旧配对文件保留，当前身份未首配。
- 675 项冻结生产源与构建元数据及当前仓库逐项相等；13 项宿主回归通过。
- 最终 ELF 的完整早期闭包含四处 jalr 和一处间接尾 jr，8 个回调保持内部地址；
  复制边界、真实镜像偏移、两核 PMP 及 cache-on SPI1 mutex 均独立复核。
- 运行时 text/rodata 的 FNV-1a32 为 `2fe361f7` / `32580a7e`，与实际二进制段独立计算一致。
- app-only 烧录前保存完整 16 MiB，独立读回应用 SHA 一致；首次启动前全部
  4 MiB 数据区与备份一致。回退镜像固定为此前解码 candidate-5；没有更改分区、C6 或 eFuse。

候选 1 只构建未烧录；候选 2 完成两轮各 16 组编码与 22 组解码。候选 3
增加纹理样本及 SD 文件的软件验证命令，所有当前结果均重新执行，未套用旧候选验收。

## 验证结果

| 层级 | 当前结果 | 限制 |
|---|---|---|
| CPU 标记生成 | 四种采样 × 六档质量，共 24 个头；实际解析器往返、所有短容量和越界守护通过 | 宿主 ASan/UBSan，不代表硬件编码 |
| 独立头兼容 | Pillow/libjpeg 校验 24 个头的量化、采样、尺寸、模式均通过 | 与硬件像素验证分开 |
| 驱动/应用宿主 | DMA2D 交接、编码包装所有权、照片异步取消/失败/存储、相机边界、XIP、身份/连接数等 13 项通过 | 使用受控替身，不调用真实模型 |
| 编码实板 | 两轮各 19 项，失败 0；RGB565/BGR888 × 444/422/420 × 质量 50/90、灰度、30×24 补齐和 1024×600 | 彩色分块图避开色度插值边缘；纹理图比较全部像素 |
| 大纹理 | RGB565 89296 B、BGR888 89333 B，最大通道误差 49/41，平均误差 6.33/5.67 | 质量 90 有损编码，不能要求像素逐字一致 |
| 错误恢复 | 输出仅给 128 B 熵容量，得到 ENOSPC、size=0，随后有效作业成功 | 测试预期错误不记为正常作业失败 |
| 产品包装 | 非 MCU 整除的 30×24 经实际包装编码、独立软件解码通过 | 输入复制与边缘重复也有宿主验证 |
| 编解码交替 | 22 组既有硬件解码通过；空熵超时后恢复；PPA 六组前后检查全部 mismatch=0 | 共享硬件串行，不代表同时执行两个作业 |
| 真实拍照 | 活跃 CSI 采集 → SD JPEG 6069 B → 软件解码 512×300，像素 FNV `c6f0fd14` | 只确认可保存/可解码，不推断场景内容或主观画质 |
| 清理 | 相机回到 busy=0/alive=0；仅删除本次独占新建 `photo-00000001-4.jpg`，确认不存在 | 既有照片保留；日志不包含图像/配置/私钥 |
| 显示与 Flash | 本轮 underruns=0、pan_errors=0，最大显示 IRQ 间隔 16991 us；Flash 写/擦均为 0 | 没有执行本版本 Flash 写擦，不能判其稳定性通过 |
| 内存 | 首轮编码用户/内核 used 完全相同；交替轮用户 +4240 B、内核 −40 B | 后台服务同时运行，此有界结果不是长时无泄漏证明 |

无 USB 摄像头接入，未验证 UVC 实拍。物理屏幕、冷上电、新身份蓝牙首配与
2048/CSI 并行 Flash 写擦仍待条件允许时补验；本报告不以 SD 写入代替内部 Flash。

安全证据副本见 [evidence/native-jpeg-encode-20261026/README.md](evidence/native-jpeg-encode-20261026/README.md)。
原始备份留在工作区，不入仓；没有复制真实照片、完整配置或 Flash 内容到证据目录。

## 有线网络共享问题

板卡具备 IP101GR/RJ45 10/100M 和 C6 Wi-Fi，硬件可用于“有线上网 → 本机热点 → 手机”。
见[板卡官方说明](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-function-ev-board/user_guide.html)。
当前配置 `ESPRESSIF_EMAC=y`，但 `NET_IPFORWARD` 关闭，热点 DHCP router/DNS
均为 0，所以当前固件不提供客户端互联网共享。NuttX 官方提供
[NAT 与 IP forwarding](https://nuttx.apache.org/docs/latest/components/net/nat.html)；
后续需接通 eth0 获址/默认路由、IPv4 NAT/转发、热点网关/DNS，并处理断线恢复与
网段冲突。网线需接已有互联网出口的路由器 LAN/交换机；仅插网线不会自动产生互联网。
本轮未实现或启用该功能。
