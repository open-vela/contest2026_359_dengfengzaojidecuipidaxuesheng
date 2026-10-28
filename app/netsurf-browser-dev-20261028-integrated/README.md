# 已验收 NetSurf 整合源码（2026-10-28）

这是实际安装到 P4 的 `news-reader-restore` 模块源码快照。模块 SHA-256 为
`856ce0f205ff1badfa2e9910afa05508a25c534d2a7c0cd113528f9525683c0f`，
配套统一固件 SHA-256 为
`c5d012c0a3b69d102174b765dd26a7fec76128cefdce42e8aef9b4dc6467b9aa`。

`platform/openvela/` 使用构建时冻结的版本，特别保留当时的两个 HTTP 文件，
没有混入之后尚未验收的网络修改。`validation/source-sha256.json` 记录原始输入字节；
`.gitattributes` 保留这些源文件的行尾。原生加载器位于外层 Vela overlay 的
`apps/system/desktop/browser/`。其他历史浏览器导出目录保持本地原样，不包含在本提交。

模块构建依赖 NetSurf 上游源码及补丁化核心库，准备方式见随附 tools、patches
和原浏览器工程的工作区说明。本目录不包含编译缓存、ELF、固件或 App ZIP，
也不包含板上 Flash 备份、账号配置或私有密钥。

硬件补验：原生模块执行、受控网页、阅读/原页切换通过；正式入口恢复，用户偏好
指纹不变。网页改名/删除等七个场景为宿主测试范围，详见 validation。
