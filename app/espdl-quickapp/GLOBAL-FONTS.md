# 全局字体

设置 → 控制面板 → 个性化 → **全局字体**。默认普惠和原有六套 SD 字体使用同一入口；桌面、设置、锁屏、天气、家居读数、输入框、快应用及歌词跟随选择。图标使用独立字体，代码块保留等宽排版。

字体包继续放在 `/sdcard/fonts/lyrics/`，无需重新生成已有字体文件。多语言缺字补充字体为 `/sdcard/fonts/VelaGlobal-Regular.ttf`。支持 12、14、16、20、28、32、34、40、48、64、72、128 px；128 px 是历史变量 `font72` 对应的天气大字，歌词使用独立的真实 72 px。

选择在后台校验成功后生效。公开的字体地址不变，现有控件、输入草稿和页面状态保留，LVGL 重新计算文字布局。字体缓存和视图存放在已初始化的堆中。回退链为选中字体 → 内置字体 → 可用的多语言字体 → 图标；家居的度数符号保留普通文字字宽。

选择由后台线程写入 `/sdcard/fonts/global-font.txt`，版本格式为 `VelaGlobalFont 1 <index>`。临时文件刷写后通过重命名替换；快速切换合并为最新选择。下次启动在 SD 检查完成后恢复，不自动重写配置。写入失败会提示本次已生效但未保存，不写内部 Flash。

缺失、截断或校验失败的字体不会替换当前字体。使用中发生读取错误时全局恢复默认字体；不覆盖已保存的选择，以便 SD 恢复后下次启动重试。

## 验证

在 Linux/WSL 使用已有 prepared apps 与原生 LVGL 缓存：

```sh
python3 tools/test_global_fonts.py \
  --apps /path/to/prepared/apps \
  --cache /path/to/lvgl-host-cache \
  --font-root /path/to/sdcard/fonts/lyrics \
  --music-font /path/to/VelaGlobal-Regular.ttf \
  --output /path/to/isolated/output
```

夹具运行实际桌面 C/JS 和 TinyTTF，使用 ASan/UBSan，覆盖七种选择、十二字号、两套主题、120 次缓存切换、已存在的控件与草稿、切换后首次创建聊天字体、歌词/PV、设置菜单生命周期、跨进程恢复、无效配置、缺失/损坏字体和运行时读取失败。输出截图、日志及 `verification.json`。这不代替固件完整链接、烧录和实体屏幕验收。
