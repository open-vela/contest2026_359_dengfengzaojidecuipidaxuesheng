# NetSurf 独立浏览器 App 0.4.0 开发版

包名：`com.netsurf.browser`。提供独立桌面入口和浏览页面，支持地址输入、搜索、
前进后退、刷新/停止、地址保存和页面触摸。

执行 `python3 tools/package_quickapp.py --native-module build/browser-module-native --output dist/native-app`
生成包含浏览器引擎的 ZIP。解压后，将
`com.netsurf.browser` 目录复制到设备 `/sdcard/qpk/` 或 `/data/qpk/`，刷新桌面应用列表。
清单使用顶层 `entry: app.js`，同时保留 `router.entry`。

安装目录中的 `native/browser.elf` 提供浏览器引擎；配套 Vela 固件只提供加载器和共享系统接口。
模块与固件需匹配 `native/host.abi`，更新模块后须重启设备。普通未启用加载器的 Vela 固件不能运行此包。
字体默认路径为
`/sdcard/fonts/VelaGlobal-Regular.ttf`，HTTPS CA 文件为
`/sdcard/netsurf/ca-bundle.pem`。设备需已联网且系统时间正确。

当前支持 HTML/CSS、PNG、baseline JPEG、SVG、HTTP(S) 和基础网页 JavaScript。
网页脚本引擎也包含在原生模块中。仅更新 `app.js` 不会升级原生引擎。
已通过内联/外部脚本、DOM/样式修改、按钮事件、Promise、定时器和 fetch 的宿主测试。
每个网页 VM 的内存限额为 6 MiB，字节码任务预算为 750 ms；超限脚本会被终止。
首次进入空白页，点击地址栏输入网址或搜索内容。输入域名默认使用 HTTPS，搜索通过 Bing。

0.4.0 ZIP 约 2.36 MB，原生 ELF 安装文件约 6.25 MB；字体与 CA 文件另计。
ELF 包含加载需要的重定位和符号表，不能用 strip --strip-all 进一步裁掉这些数据。
该模块从用户堆装入，代码/常量加静态数据约 2.57 MB，加载期间符号表等临时内存另计。
成功加载后驻留至重启；关闭页面不代表整套引擎内存释放。

已通过宿主功能测试、真实 dlopen 加载边界测试和 P4 模块/加载器固件交叉链接。
尚未在 P4 上执行该 ELF；PSRAM 执行权限、两核缓存一致性、加载峰值内存、显示触摸与联网仍需实板验证。
原生集成、构建和测试说明见 `platform/openvela_module/README.md`。
