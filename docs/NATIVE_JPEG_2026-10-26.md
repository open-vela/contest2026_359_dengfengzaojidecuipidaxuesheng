# ESP32-P4 原生 JPEG 解码适配

手机热点网页验收按用户要求跳过，仍为未验证。本项继续原难度排序的第 7 项。

后续已部署编码 candidate-3，并重新通过本解码回归；当前版本与新增编码能力见
[NATIVE_JPEG_ENCODE_2026-10-26.md](NATIVE_JPEG_ENCODE_2026-10-26.md)。
以下结果属于解码 candidate-5 的历史证据，不作为当前整版验收。

此前已烧录 JPEG candidate-5：应用独立读回、启动前 4 MiB 数据区一致、两核
XIP，以及两轮各 22 组硬件解码检查通过。这里只确认缓冲级像素和回收，尚未
完成当前固件的真实照片显示、2048/CSI 并行 Flash 写擦及实体观察验收。

实现遵循本机固定的 Espressif esp-hal-3rdparty HAL 和原生 NuttX VFS，未引入
FreeRTOS 队列或 JPEG pool 兼容层。官方能力参考：
[ESP32-P4 JPEG 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/jpeg.html)。
本适配是其中的解码子集，不能与官方完整编码/解码 API 划等号。

| 项目 | 本次实现与限制 |
|---|---|
| 设备入口 | `/dev/jpeg0`，同步 INFO / DECODE ioctl，flat build、P4 revision >= 3.0 |
| 图像 | 8 位 baseline、单次交错扫描；YCbCr 4:4:4 / 4:2:2 / 4:2:0，或单通道灰度 |
| 输出 | RGB565、RGB888；灰度输入仅 GRAY8 |
| 边界 | 宽高各 <= 4096，补齐后 <= 4M 像素，压缩输入 <= 4 MiB，像素总数满足固定 SDK 的 8 整除约束 |
| 补齐 | 按 MCU 补齐，返回可见宽高、补齐宽高、stride 和输出字节数；应用复制时裁掉补齐区域 |
| Huffman | 标准及优化表；全部缺失的 MJPEG 使用标准表，缺失部分已引用表则拒绝 |
| 不支持 | progressive、CMYK、多扫描、16 位量化表、任意采样/组件布局、编码及硬件缩放 |
| 解析 | 有界 DQT/DHT/SOF/SOS/DRI 与 EOI/重启标记校验；不宣称完整校验熵编码语义 |
| 缓存 | 自有 RAM 输入；128 字节对齐独立输出，明确 C2M、停 DMA 后 M2C；自检额外守护区也纳入同步 |
| 小图 | RX 块宽不得超过 MCU 补齐宽度，避免小图超时、灰度重排行错位 |
| 颜色 | JPEG 全范围 YCbCr 矩阵，RGB565 原生小端；不直接套用 HAL 的电视范围矩阵 |
| 资源 | 大解析元数据和 Huffman 临时区在 PSRAM 用户堆，DMA 描述符在内核 DMA 堆 |
| PPA 共用 | 一份 DMA2D RX0 IRQ、一个控制器租约；每次交接完整复位，防止 CSC/reorder 状态残留 |
| 并发含义 | 多调用者可串行使用 JPEG/PPA；不是两个硬件作业同时占用 RX0 |
| 超时 | 默认 250 ms，允许 1–5000 ms；停硬件、清 IRQ/回调、回收租约后返回错误 |
| 产品路径 | 全尺寸照片/MJPEG 可尝试硬件；失败或格式不支持回退原软件解码；缩小预览保持原有策略 |
| 显示保护 | DMA 不直接写活动 framebuffer，成功后才复制调用方输出 |
| 自检 | `desktop jpeg-test`：PPA → JPEG 样本/错误恢复 → PPA；无需手机、网络或配置写入 |

冻结候选和原始证据位于工作区 `output/jpeg-adaptation-20261026/`。candidate-1
未烧录，整机构建停在 HAL 依赖路径；candidate-2 改用显式 HAL 源路径后构建通过，
但实板有 13 项失败，不能视为可用。candidate-3 仅构建未烧录；candidate-4 修正
RGB565 字节序和全范围颜色后，还存在 6 项小图超时/灰度错位。candidate-5 对
小图限制块宽，保留旧小图并加入宽图/大图后通过全部检查；失败日志均保留。
新增 6 个芯片驱动文件在原生 Linux 临时目录经官方 nxstyle 检查全部通过；原先
Windows 挂载执行权限/路径造成的检查噪声与修正前输出一并保留。

candidate-5 的 9 项宿主回归全部通过：18 个真实 JPEG 解析样本（13 接受、5 拒绝）、
3 万组随机/变异、DMA2D 并发交接/IRQ 回收、应用失败不发布/补齐裁剪、完整软件
预览、相机缓存/GUI 边界、XIP、蓝牙身份及连接池。ASan/UBSan 和实际 P4 单元编译
均有记录。另在 JPEG candidate-4 补跑视觉协议/PNG 编码宿主检查，相关脚本、
夹具和视觉源码在 candidate-5 中逐字一致，未调用真实模型。宿主检查与下面
硬件结果分别记录，不互相代替。

candidate-5 镜像 SHA256：
`1165ffbe30d98dc713b01cbbdbdaa75adff9cd6c59f6577df9a56b028a9d8902`。
ELF SHA256：`d63ed6cc71f421752ce93085229b2ca31802246dccb9b170d93915b19e6bdc2a`。
应用末地址 `0x9fde00`，数据区保留 `0xc00000..0x1000000`；BSS 末地址
`0x4ffabe5c`。新鼠标身份 `OpenVela Mouse-88445E9E` /
`E4:E4:88:44:5E:9E`，不沿用其他候选的首配验收。

两轮均完成以下 22 个解码组合，失败数为 0：32 像素小图的 YUV444/422/420、
30×24 补齐图、优化 Huffman、无 DHT；96 像素宽的三种采样；32/128 像素宽
灰度，以及 1024×600 YUV420（输出补齐为 1024×608）。彩色图检查 RGB565
与 RGB888，灰度检查 GRAY8。RGB888 被比较像素最大误差 0，RGB565 最大 6，
灰度最大 1；彩色分块图比较内部区域以避开不同合法色度插值的边缘差异，灰度
和大图比较全部可见像素。128 字节尾部守护区全部通过。

每轮先用完整头/SOS 加空熵触发硬件超时，确认不发布输出、回收控制器，再完成
22 项有效解码；前后 PPA 的颜色/缩放/平面补齐测试均 mismatch=0。第二轮前后
用户堆 used=5,502,088 字节不变，内核堆 used 下降 40 字节；这是有界重复测试，
不代表长时无泄漏证明。两核 ready/protected=1/3，cache-on/SPI1 mutex 保留；
运行时 FNV-1a 32 位 text/rodata 指纹 ad635a4a/364f2778 与最终二进制段独立
计算一致（不是 CRC32）。证据在 candidate-5/RUNTIME.json 和两份 jpeg-test 日志。

最终 ELF 的完整启动复制闭包、四个 jalr 与一个尾 jr、八个内部调谐回调、
六处长度/边界引用、双核保护入口、SPI1 cache-on mutex 已复核。七个 XIP
运行时/链接输入和 20 个调谐源文件与原准备树一致；Kconfig/Make.defs 因新增
JPEG 规则而变化，不能声称九个输入全部与旧版相同。无新增安全烧写、C6 OTA
或重分区。当前实板像素、应用独立读回和启动前数据区一致性已记录；冷启动、
当前新身份配对、真实照片显示及当前固件实体显示/Flash 写擦验收仍待补，不能
套用旧候选结果。下一项为硬件 JPEG 编码，本轮未将编码标为完成。
