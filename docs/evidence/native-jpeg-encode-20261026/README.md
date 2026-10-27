# 原生 JPEG 编码候选 3 证据

当前镜像 `bf542ed81a6b7b2799be54dcaae25609782e733b9fc465c978a3aafb0219cfd4`。
完整说明见 [NATIVE_JPEG_ENCODE_2026-10-26.md](../../NATIVE_JPEG_ENCODE_2026-10-26.md)。

- `DEPLOYED.json`：应用独立读回、数据区保留、暖启动与当前验收边界。
- `RUNTIME.json`：两轮各 19 项编码、22 项解码、PPA、真实 CSI/SD 文件、内存与原始日志哈希。
- `HOST.json` / `PILLOW.json`：13 项宿主回归、24 个头的独立兼容检查。
- `STYLE.json`：五个新增/修改驱动与公共头的官方 nxstyle 结果；与上一候选同字节文件复用检查。
- `ELF-AUDIT.json` / `XIP-INDEPENDENT-EVIDENCE.json` / `XIP-REVIEW.md`：最终镜像启动复制闭包及独立核对。
- `MANIFEST.json`：本目录机器证据的 SHA256，不含本说明。

原始制品在工作区 `output/jpeg-encode-20261026/candidate-3/`，完整 Flash 备份、
私有配置和照片均不导出。真实拍照样本仅本地软件验证后删除；未上传、未调用模型。

宿主、构建、读回、缓冲级像素与一次实拍分别记录。当前版本实体屏幕/内部 Flash
写擦、冷上电、新 BLE 身份首配待测；手机热点验收由用户跳过，仍未验证。
