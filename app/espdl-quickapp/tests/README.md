# v3 应用宿主测试

本目录保存 JS 回归、C 宿主用例与少量测试输入，不是实板测试结果目录。
返回[应用快照说明](../README.md)，已有输出见 [evidence](../evidence/README.md)。

## 全部应用

`render_apps.c` 使用生产 LVGL 页面，生成明暗主题的首页、末页、安装应用、长名称和删除确认截图。检查卡片边界、固定网格位置、说明与删除按钮无重叠、删除确认、按住按钮后翻页的旧点击拦截、删除后页码收敛，以及 200 次翻页时控件数量保持不变。真实指针事件覆盖图标、标题、卡片、空白、页码和删除按钮上的左右滑动、首末页边界、每次滑动只翻一页、纵向滑动不启动应用、后续轻点仍可打开应用、删除忙时禁止翻页和边缘返回。运行 `python3 tools/render_all_ui.py --apps <prepared-apps> --cache <lvgl-host-cache> --output <review-output> --harness-source app/espdl-quickapp/tests/render_apps.c`。不执行真实应用删除或设备操作。

## 内置音乐应用与桌面卡片

`render_music_app.c` 使用生产 LVGL 页面和离线播放服务夹具，检查“全部应用 → 音乐”注册、桌面与应用共用暂停／继续及曲目信息、进度、空闲／加载／失败／结束／长歌名、全屏歌词、配置页往返、按入口返回桌面或应用列表，以及两种主题下 60 次开关／重建的控件与定时器清理。桌面首次动态标签写入前先完成布局，避免 `LONG_DOT` 在零宽度坐标上省略文字；歌词及背景播放沿用原服务。

当前封面卡片使用当前曲目的 `cover_url`、独立 `GLASS_COVER_HOME` 所有者及原有异步 JPEG/SD 缓存。用例还检查封面迟到时不依赖播放 revision、同 URL 切歌清除旧图、无封面与零尺寸回退、隐藏桌面不发请求、前台图片不被桌面重建取消，以及底栏缩略图共享主封面像素且先于主画布释放。默认生成离线矩形 RGB565 夹具，可通过 `MUSIC_COVER_FIXTURE` 指定已有的、带宽高头的 RGB565 封面用于预览；不改变生产封面来源。封面点击和右上角箭头打开音乐应用，卡片提供上一首／暂停继续／下一首。

桌面卡片采用“素雅漫透”方案。主按钮的播放三角形和暂停双竖条直接围绕圆心绘制；两种主题均检查实际 RGB565 图案像素的上下留白差不超过 1 像素，并验证暂停双条的水平居中及暂停／继续后的图案切换，避免只检查文字控件坐标而遗漏字形基线偏移。

`tools/test_lock_photo.py` 使用真实 JPEG 解码器与 HTTP 替身，覆盖桌面／音乐／锁屏结果与取消代次隔离、单后台线程忙时重试、缓存和损坏图片。`render_lock_music.c` 回归锁屏封面、迟到图片和30次页面重开。

运行：`python3 tools/render_all_ui.py --apps <prepared-apps> --cache <lvgl-host-cache> --output <review-output> --harness-source app/espdl-quickapp/tests/render_music_app.c`。宿主测试不验证真实网络搜歌、扬声器播放或实板触摸。

## 麦克风接管与连续采音

`tools/test_esp_tts_voice.py --prepared <锁定的官方输入> --output <目录>` 检查真实语音文件的校验、缓存复用、60 秒空闲回收、正在借用时不回收、最后归还后重新计时，以及持锁加载期间维护调用立即返回。使用可控单调时钟，无需实际等待一分钟。`tools/test_ble_central.py --output <目录>` 同时检查命令线程空闲时零次分派，以及连续 200 个扫描／取消命令不丢唤醒；硬件描述符仍由原蓝牙常驻任务组拥有。

`tools/test_voice_requests.py --output <目录>` 使用生产请求调度代码检查 A/B/C 后来者优先、资源释放顺序、旧结果/旧停止隔离和音频环形队列边界。

`tools/test_voice_pipeline.py --output <目录>` 使用真实采音/发送线程，驱动和网络采用替身；模拟 1 秒连接和每包 70ms 阻塞写，检查起音、顺序和短尾完整。这不验证实板驱动或真人转写准确率。

`tools/test_wake_mini.py --apps <apps> --cache <LVGL缓存> --assets <小球素材> --harness-source app/espdl-quickapp/tests/wake_mini_preemption_host.c --output <目录>` 仅检查新全局唤醒交接路径：接管其他页面、让位后来的输入、不发送过期文本、完整结果仅发送一次、大球页路由。唤醒事件为宿主替身，实板需真实麦克风触发。

## 长句识别与字体回退

`tools/test_voice_transcript.py --apps <准备好的apps> --output <输出目录>`
编译生产 ASR 文本拼接与完成判断，使用 ASan/UBSan 验证长句末尾否定、
临时结果纠正、重复稳定包、漏句、缺 final、精确容量和超长拒绝，以及音频锁
等待超时与取消。`render_grok_conversation.c` 检查连续说话超过 12 秒仍聆听、
1.5 秒短暂停顿不结束；在 16/20/28 三档字体检查 72 个常用符号与全角空格样本并渲染
两种主题的 AI 回复。原文和字号保持不变；不代表覆盖所有 Unicode 或真人 ASR 准确率。

## 音乐搜索与助手控制流程

本地助手服务、定时任务、天气／播报、拍照分类与别名的入口和验证命令见
[ASSISTANT-TOOLS.md](../ASSISTANT-TOOLS.md#开发验证)。`assistant_service_host.c` 使用真实持久化并检查
写入失败、动作完成回执丢失与新进程重启；`assistant_platform_host.c` 运行真实采集／分类线程与 BMP 保存，
设备、模型和天气边界为离线替身。`agent_bridge_host.c` 另检查 assistant_* 模式限制、直接调用、
重复字段拒绝和超过单页大小的 board 工具发现分页。默认提示内的口语理解规则仍须另行做模型／真人评测。

`tools/test_music_background.py --apps <prepared-apps>` 使用真实音乐线程、MP3 解码与 MCP 入口，
HTTP／PCM 使用离线替身。`MUSIC_SELECTED_RESULT_OK` 检查一次搜索后按 result_id 播放同一歌曲与歌手、
不再检索、track_id 对应、单次使用、过期／替换／搜索失败后的拒绝、参数校验及普通播放重新检索。
它不验证第三方音乐接口、人工听音或模型是否逐条遵循提示。
助手桥接、模式隔离与米家控制回读沿用 `tools/test_agent.py --apps <prepared-apps> --output <目录>`；
运行时提示修改后须执行 `tools/generate_agent_profiles.py` 同步内嵌头文件。

## 桌面清理回归

蓝牙设置默认打开设备连接列表；无线触摸板保留单独入口。`render_bluetooth_list.c` 经 `tools/render_all_ui.py --harness-source` 运行真实 LVGL 列表和异步邮箱，以离线 BLE 夹具检查双主题、名称/地址/信号、未命名与同名设备、扫描中连接、取消初始化、连接失败、异步断开、32 行滚动、按下后被替换的设备、扫描代次过期、HID 共存、后台工作期间关页及 60 次复开。所有 BLE 操作断言运行在 UI 线程之外；HCI/GATT 仍由现有常驻主机任务组执行。

2026-10-25 补充蓝牙完整交互：空闲且未被触摸板占用时，打开设备页自动搜索；连接成功后可点击“配对设备”。实际加密和绑定状态分别决定“安全连接成功”与“配对成功”，仅请求被接受不能报成功。`render_bluetooth_list.c` 新增配对拒绝、超时、重试、取消、仅加密未绑定、配对期间关页重开，以及旧配对回执不得影响新设备的回归；断开完成后列表恢复正常透明度。宿主 ASan 使用 128 KB 工作线程栈，NuttX 固件维持原有 16 KB。

蓝牙列表按当前连接、已配对、有名称、已知可连接排序，同级保持发现顺序；未命名且未配对的广播默认折叠，展开后仍按连接能力排列。明确不可连接的条目标记“仅广播”并禁用连接。外观和服务 UUID 仅作为广播声明提示，未知类型不猜厂商或型号。配对标记来自实际安全状态或存储中的身份地址（包括控制器已解析的身份地址）；无法解析的隐私随机地址不能仅凭名称当作已配对设备。32 条容量满后只用更高优先级设备替换低优先级条目，并显示保留上限提示。

`test_ble_central.py` 检查广播／扫描响应合并不丢连接能力、畸形长度、Appearance／服务类型、已保存身份配对、满额优先保留及不可连接设备拒绝。原生 LVGL 测试另覆盖折叠／展开、无名称已配对置顶、同名地址区分、广播按钮禁用，以及停止扫描期间同槽地址被替换后拒绝旧连接请求。宿主测试不替代实体扫描和连接验收。

`tools/test_ble_hid_mailbox.py --output <目录>` 提取实际 HID 请求、停止与公开 API，配合真实 `mouse_core.c` 在独立宿主线程运行。覆盖 30 次开始／停止、所有协议操作和角色释放归属主机线程、停止异常但已无广播／连接时释放角色、按键释放／点击及停止使旧启动失效。桌面／NSH 只写有界请求并唤醒主机已有命令线程，不再从外部任务组投递 NPL 的 mq 描述符；测试不运行 NPL 定时回调，确保请求无需等它触发。`test_ble_central.py` 同时验证这一条件变量能唤醒空闲主机并调用 HID 处理，空闲时仍不轮询空命令。原 50 ms callout 保留为主机侧 watchdog。

`tools/test_ble_central.py --output <目录>` 另运行生产 NimBLE 边界，检查实际连接对象快照、现有角色互斥与释放、连接/搜索代次、GATT、配对与取消清理。`render_wireless_ui.c` 保留 Wi-Fi 和 HID 触摸板回归。宿主夹具与交叉编译不代表已连接真实外围设备或完成实板配对。

`render_desktop_cleanup.c` 通过 `tools/render_all_ui.py --harness-source` 运行真实 LVGL 页面，检查明暗主题、全屏页面尺寸、关于页完整文字、设置四个入口与返回、60 轮关于/设置复开，以及环境图表的空记录、部分样本和满记录。界面渲染不访问串口或网络服务。音乐沿用 `render_music_full.c` 回归。

`render_shell_transitions.c` 检查从全屏 ESPClaw 切到普通快应用、家居和相机时的尺寸与返回来源；60 次无控件应用启动回滚后，原聊天页面和全屏尺寸必须保留，页面数量不能增长，快应用运行时必须停止。另检查从首页取消启动后恢复桌面。该测试使用真实 LVGL/QuickJS 和离线设备夹具，不调用模型或真实摄像头。

`render_navigation_lifetime.c` 通过真实 LVGL 异步队列检查离开页面后取消待执行导航、有效的设置入口仍能打开、文件刷新取消、设置关闭回调归属、页面定时器释放，以及锁屏进入/删除后的导航取消。覆盖双主题和 60 次交错切换；蓝牙与播放器截图只使用当前生产入口。

## 三套实时歌词效果

设置 → 个性化 → 歌词字体提供默认普惠和六款可选字体；全屏播放页不放字体菜单。`tools/prepare_lyric_fonts.py --cache <缓存> --output <目录> --header app/espdl-quickapp/overlay/apps/system/desktop/glass_lyric_font_assets.h` 校验锁定来源并生成静态 TTF、OFL、清单、头文件与 `lyric-fonts.zip`（fontTools 4.60.1）。解压后把 `fonts/lyrics` 合并到 SD 卡的 `/fonts/lyrics`，保留原有文件；可选字库不链接进固件。

`render_lyric_fonts.c` 经 `tools/render_all_ui.py --harness-source ... --lyric-fonts <生成的sdcard/fonts/lyrics>` 使用真实 TinyTTF SD 流和字库，覆盖设置中切换、5 档字号、双主题、半弧歌词、三套 PV、字体回退、关闭设置后的控件清理及 120 次缓存切换。`tools/test_lyric_font_failures.py --renderer <渲染程序> --font-root <同一字库目录> --base <内置字体> --output <目录>` 在独立夹具中检查缺失、截断和同大小损坏拒绝，不改原字库。新字库使用一个后台校验线程与有界页缓存；缓存按家族固定上限保留，页面重开不重复创建字体。

普通全屏歌词沿左半椭圆滑入、聚焦、滑出，水平弧深 112px；歌词保持水平，文字列预留弧线空间。采用 420ms 缓出移动、后续行每行递增 36ms 延迟（最多 108ms），并以 360ms 渐变突出当前行；原文与译文共用行位移。`render_music_full.c` 另检查半弧对称、逐帧进出方向与文字边界、快速连续换句的双轴位置连续、滚动复用不重启动画、距离渐淡，以及暂停、手动拖动、跳转、减少动态效果和带动画复开时的清理，并输出真实 LVGL 动画帧。

`render_music_full.c` 通过真实 LVGL 与 ASan／UBSan 检查明暗主题的聚焦、强调、逐字效果；覆盖任意中英文、短句、长句、完整 UTF-8 前缀、暂停不更新、回跳、减少动态效果、切歌清理和 60 次复开。默认路径不读取歌曲帧库、不创建解码线程或动画帧缓冲。旧 SD 预渲染播放分支及其界面测试已移除；离线文件格式校验独立保留在 JIZURA 工具中。

`tools/test_music_native_device.py --phase styles` 在 P4 上逐套检查实际歌词、暂停、绘制统计和内存，`--phase cycles` 另记录 60 次页面开关与堆采样。结果分别记录，不将宿主测试当作实板稳定性或网络首播延迟。

## 原创天气卡片

`render_weather.c` 经 `tools/render_all_ui.py --harness-source` 使用真实 LVGL、
生产页面与已有 JPEG 解码器，覆盖双主题、晴天/多云/阴天的独立背景、晴夜与多云
夜景、雨量/雪量分级、阵雨/阵雪、冰雹/冻雨、雾霾、浮尘/扬沙、风与风暴场景，
共 30 套原创画面、29 种显示状态及 49 个天气描述别名；每种均检查昼夜，另覆盖
离线缓存、未知数据、负温度、组件切换和原生刷新入口。全部场景检查首页与详情，
一屏详情另检查边界、缺失数据和更新失败，使用真实指针点击刷新和返回。首页与详情
各连续 120 次相同快照更新不得重绘天空与毛玻璃、重复解码/模糊或增加对象；60 次重建和打开/关闭
详情检查图片缓存释放，启用 ASan/UBSan。`tools/cross_compile_weather.py` 只读取
准备树配置并交叉编译桌面文件，不链接固件、不烧录或更改准备树。

天气渲染器同时输出 `perf.json`：重复测量真实毛玻璃构造与天空切换，取五组均值的
中位数；时序仅代表带 ASan/UBSan 的本机测试环境。相同内容、不同版本号也必须
保持零重绘，资产切换与昼夜变化检查缓冲复用。比较两次生成的 `.rgb565` 文件可验证
全部 254 张图的逐像素一致性。`tools/test_weather_decode.py --jpeg <320x240标准JPEG>
--output <目录>` 验证共享解码器的四档缩放、重复/截断输入、区域边界、只读内存解码
及摄像头缺失 DHT 表补全；交叉编译同时检查桌面和 `qpk_mjpeg.c`。

`render_weather_visibility.c` 使用同一真实 LVGL/生产页面，检查完全遮挡时不更新首页、
保留返回手势图像、移开/取消手势、半透明、立即返回同步及替换页面后恢复，覆盖双主题。
快照缓存使用运行时堆内存，详情关闭时释放；测试另覆盖缓存分配失败时继续更新文字和天空。
天气性能基准还连续发布 100 次不同天气快照，要求被覆盖的首页保持零次背景解码。

## 中文播报完整性与取消

`tools/test_wake_handoff.py --output <目录>` 在 ASan/UBSan 下运行从生产源码提取的单次唤醒循环、完成状态发布及按请求取消，覆盖模型缺失、音频忙、计算失败、提前/中途取消，以及完成后取消撤销唤醒交接。另用 `voice_capture_cancel_host.c` 运行生产采集的打开/关闭和 `ws_wait_start`，检查停止音乐等待、获取音频所有权、分配驱动缓冲后取消的资源释放，以及网络已就绪与取消同时发生时拒绝进入采集。仅硬件、网络和模型适配器为夹具，不链接 MultiNet。`render_grok_conversation.c` 使用真实 LVGL 和生产语音页，在明暗主题下检查显式开启、麦克风释放后才启动 ASR、连续播报再聆听、口头结束、无语音返回等待唤醒、迟到事件/新任务隔离、14 秒连接延迟后的完整聆听窗口和退出清理；这些测试不验证真人检出率或云端识别质量。

`tools/test_speech_host.py` 使用锁定中文模型执行真实 G2P、时长前端和解码器，ASan/UBSan 检查真实计算中途取消、调用者 arena 清理，以及 `speech_compute_host.c` 的嵌套作用域、八线程隔离和重新使用。生成音频须与未加取消检查的同一模型 PCM 逐字节一致。数学核生成规则由 `tools/build_speech_runtime.py` 校验锁定原始文件的大小、SHA-256 和循环布局后执行，不修改缓存原件。

`speech_conv_host.c` 将 256 采样分块卷积与单独编译的锁定上游参考核逐位比较：560 组覆盖短输入、256 边界及尾块、20992 采样长行、1/3/5/7 核、1/2/3/6/12 膨胀、零权重和累加模式。保留每个输出采样原有的输入通道与卷积核累加顺序，未启用 fast-math，不增加模型 arena。完整中文音频仍与上一版 PCM 逐字节一致。分块同时增加取消检查点。

`python -m unittest discover -s tools/tests -p test_speech_deploy.py -v` 不访问串口，覆盖 TTS 运行文件与许可选择、同大小损坏拒绝、清单与编译资产表不符拒绝、缺失资源及保留已有冲突文件。实板首 PCM 耗时、计算中取消、音频互斥及相机复开结果保存在工作区 `output/speech-tts-20261016/`；最新分块版为 `output/speech-tts-latency-20261016/`。人工听音和真人唤醒不由这些测试代替。

## Node.js 回归

在仓库根目录、具备 `node --test` 的 Node.js 环境运行：

```bash
node --test app/espdl-quickapp/tests/dafeiyu.test.cjs app/espdl-quickapp/tests/package.test.cjs
node --test app/homeassistant/tests/homeassistant.test.cjs
```

打包测试会按字节检查 JS 与 C 资源。LF/CRLF 的变化可能导致资源一致性失败，特别是
Windows 自动转换换行符的检出；应核对实际源码字节和 Git 属性，不能删除断言掩盖差异。
根级 Home Assistant 的生成能力以[当前脚本说明](../../homeassistant/README.md)为准。

## C 与 QuickJS 用例

`run_security.py` 要求宿主 `cc`、AddressSanitizer/UndefinedBehaviorSanitizer 及匹配的
QuickJS 源码目录；`--quickjs` 指向源码而不是可执行程序。在满足依赖的 Linux/WSL 中，
可从仓库根目录运行：

```bash
python3 app/espdl-quickapp/tests/run_security.py --quickjs /path/to/quickjs-source
```

其他 `test_*.js` 和 C 用例可能需要额外参数或独立编译，应先阅读具体文件。
依赖缺失、用例跳过、构建失败和断言失败须分别记录；宿主通过不代表外设或真实服务已联通。

## SD 动画调度与画面对比

`grok_schedule_host.c` 直接运行真实 LVGL 定时器和 SD 读取线程，比较改动前后的
帧序列、RGB565+A8 像素哈希及最终 framebuffer 哈希。它覆盖五种展示方式、全部
32 种表情、自旋/注视、隐藏/暂停/减少动态效果、输入唤醒和 tick 回绕。
读取线程在每个虚拟毫秒后完成工作，以排除宿主磁盘速度和线程调度的随机影响。

先将改动前的 `glass_grok_player.inc` 保存到独立目录，再在 Linux/WSL 运行：

```bash
python3 tools/test_grok_schedule.py \
  --apps /path/to/prepared/apps \
  --cache /path/to/prepared/lvgl-host-cache \
  --assets /path/to/grok/sdcard \
  --before /path/to/before-player-directory \
  --output /path/to/comparison-output
```

需要已经准备好的带 ASan/UBSan 的 `lvgl-*.o`、匹配的 `lv_conf.h` 和五个 `.gbs`
素材文件。输出包括逐帧 trace、编译/执行日志和 `comparison.json`。回调和 seek
次数是确定性宿主工作量指标，不能作为实板 CPU 占用或功耗降幅。

## 原生交互动效

`glass_motion.h` 提供统一的按压、页面进入和提示条动效。它使用共享时长和定点 Hermite
插值，保留中途变更目标时的位置与速度，按压 80 ms、归位 180 ms、页面进入 240 ms；提示条进入
200 ms、退出 140 ms。这些时长是本工程对 ESP32-P4 的适配值。

控制器固定保留 12 个槽位，只在动效运行时唤醒共享定时器。页面使用短距离位移，
提示条只改变标签本身的背景/文字透明度；不创建整页截图或整组透明度缓冲。
快应用按钮通过可选原生 hook 复用反馈，原点击回调和触摸冒泡保持原有执行路径。
按压期间服务返回的新颜色会成为归位后的状态，避免还原旧的设备状态。
“减少动态效果”会立即结束现有反馈并禁用后续位移。页面删除会取消延后启动和活跃槽位。

```bash
python3 tools/test_interaction_motion.py \
  --apps /path/to/prepared/apps \
  --cache /path/to/prepared/lvgl-host-cache \
  --output /path/to/motion-test-output

python3 tools/render_all_ui.py \
  --apps /path/to/prepared/apps \
  --cache /path/to/prepared/lvgl-host-cache \
  --grok-assets /path/to/grok/sdcard \
  --harness-source app/espdl-quickapp/tests/render_interaction_motion.c \
  --output /path/to/motion-render-output
```

前者覆盖像素还原、状态颜色更新、100 次中断按压、禁用/取消、tick 回绕、槽位饱和、
对象删除和空闲暂停；后者在真实桌面与 QuickJS 桥上检查两种主题的页面、对话框、
提示条、快应用按钮和减少动态效果。宿主逐帧预览不代表实板帧率或长期稳定性验收。

原生应用页分页选中节点为 20×6 长条，其他节点为 6×6 圆点；240 ms 形状过渡和 160 ms 颜色过渡保持节点居中。应用页复用页面、网格和分页器，只重建当前页的网格内容；内容短距离移动时，分页长条保持连续。

`ui.setPos(id,x,y,true)`、`ui.setSize(id,w,h,true)`、`ui.setColor(id,color,true)` 仅对装饰对象启用动画；点击目标保持直接更新。重复目标不重置截止时间，几何反向保留速度。滑条和色盘拖动立即更新；设备回报、点击预设和开关状态可平滑过渡，数值文本保留真实值。

`ui.range(id,stop,accent,background,animate)` 对装饰轨道的 0–255 渐变边界插值，保证 0% 和 100% 端点颜色正确，复用现有 12 槽原生调度器。`ui.present(handles,offset)` 最多接受 128 个句柄和 ±24 px 位移，使用一个批量任务；只移动可见装饰对象，按钮和命中区域保持直接交互。两个新增绑定均在全部 JS 参数转换后验证句柄世代，拒绝删除、重用、超限及异常 getter；只提供显示能力，不增加设备控制权限。

详情打开、选项切换和返回采用相反方向的短位移。边缘返回在 72 px 内跟手最多 24 px，取消时 180 ms 归位，确认时 140 ms 完成后执行返回；页面删除取消手势与待执行回调，锁屏不拖动底层页面，对话框保留原有返回语义。

刷新节奏跟随 `LV_DEF_REFR_PERIOD` 并限于 16–33 ms（本板为 33 ms）。固定槽位、固定批量存储，无逐帧堆分配、截图、整页透明度或额外定时器；空闲暂停。关闭动效立即完成目标和复位。宿主 ASan/UBSan 测试覆盖双主题、原生指针分发、滑条端点和反向、批量成员删除、减少动效、参数转换期间删除及任务饱和；宿主预览不代替实板帧率测试。

第二批动效：侧栏复用第一个菜单底块，在固定文字和命中区域下移动；设备列表翻页使用 `ui.present(handles,offset,true)` 横向批量位移，页脚与分页器不加入该批次。省略第三参继续纵向过渡；轴向切换、中断和删除均还原对应轴的本地样式，不影响另一轴。

详情模式选项底色使用同一个 160 ms 装饰填色通道，不更改选项请求和设备回报逻辑；首次显示、设备复用与不可用状态直接呈现。滤芯余量条只对真实数值变化插值，零值隐藏填充并使用合法的最小目标宽度。

顶部快捷控制面板以 240 ms 展开、140 ms 收起；收起中可原位置反向展开，不重建面板。音乐、设置、锁屏等按钮立即执行原回调，系统/主题切换直接清理面板，销毁事件清空引用；减少动效时开关均立即生效。沿用原定时器与 12 个任务槽，未增加循环装饰动画、截图缓存或线程。

米家分页改为每个分页器保留一根 20×6 选中长条，背景圆点固定 6×6。主列表、详情及选项分页共用同一实现，切换时只对长条位置插值，不改变宽高或淡出/重建选中标记；快速反向保留位置和速度。详情/选项重绘保留分页器的逻辑可见状态，关闭视图时才复位，避免每次渲染都跳到终点。

新增验证覆盖双主题左→中→右→中→左、长条句柄不变与宽度不变、选项分页重复重绘、原生中途反向的连续性，以及减少动效下立即定位。普通切页的分页动画从两个几何加两个颜色任务降为一个几何任务；命中区域与服务调用保持原行为。

## 米家右侧瀑布流

当前原生运行时将主列表改为右侧连续瀑布流：侧栏、标题和页脚固定，按卡片高度向最短列排布，随宽度调整列数。主列表不再分页；详情、选项及桌面应用分页仍保留。缺少 viewport API 的旧运行时继续使用原分页布局。

原生 LVGL viewport 提供裁剪、惯性滚动和滚动条。前端复用 12 个卡片槽位及 64 个按钮槽位，不为每个设备建立原生对象；只在可见窗口变化时重新绑定内容。滑条拖动期间锁定滚动，释放或取消时解锁；打开详情再返回保留滚动位置，侧栏切换重置位置。

卡片滑条在触点移动达到阈值后判断方向：横向达到 5 px 开始实时调节，纵向达到 8 px 且超过横向的 1.2 倍则让给原生滚动，不发送设备指令。小幅抖动不更新亮度，点击松手仍可选值；快速纵向松手及取消不提交。详情滑条保持原有直接拖动。卡片实时数值保留开关状态文字，白色手柄保持在滑轨前方；双主题像素断言检查复用后的实际显示。

`ui.rangeLinked(track,stop,accent,background,animate,thumb)` 将填充和白色手柄交给同一个原生标量任务，保留反向时的位置与速度，每帧只根据同一插值计算两者位置；不增加定时器或槽位。两个对象必须为同父级的装饰对象，手柄句柄限定数值，并在所有参数转换之后检查双方世代及父级。删除手柄解除引用，减少动效直接完成目标；旧运行时继续使用独立的原有绑定。

同一主页、同一连接世代的布局刷新，以最靠近视口顶部且仍存在的设备维持屏幕高度；上方设备删除或高度变化不会直接顶走当前内容。手势期间不强制重定位，侧栏切换继续回到顶部。卡片显示等待执行、正在提交、正在核对及真实状态核对后的已确认；场景仅显示已受理，失败显示结果未确认。反馈最多保留 32 条，通过已有 tick 到期清理，无额外循环或重试请求。

滚动通知仅在虚拟窗口的设备映射改变时安排 JS 绘制，已有卡片由 LVGL 原生移动；小范围滚动不触发页面重绘。每轮仍按列数分批绘制，但新进入视口的卡片先于保留卡片及预加载卡片处理，池大小保持 12。所有右侧原生按钮共用按下目标检查：记录设备、连接世代和滚动位置，禁止在按压期间复用槽位后控制其他设备，也禁止滚动或取消手势触发场景及步进按钮。新按下手势重新捕获目标，不影响后续正常点击。

输入清理统一取消滑块、色盘及待提交手势，并立即释放原生滚动锁。断线、请求失败、设置页、详情切换、销毁及非法触点都会清理；后台同步导致设备移除、离线、滑条能力消失或量程/步长改变时，在下一轮渲染或输入事件中取消。迟到的松手不提交旧值。有效滑块及色盘手势期间暂停页面重绘，保留手指下的本地预览；取消后恢复最新服务器状态。回归覆盖设置按钮和接口两种入口，并在真实 LVGL 中检查切页及断线时的 SCROLLABLE 标志立即恢复。

弱网下对同一设备、同一操作键、同一服务和同一参数的尚未完成目标去重，包括亮度及模式参数；参数键按名称比较，参数值逐项比较。去重只针对最新目标：反向后再返回旧值仍替换尚未发送的操作，完成后新的显式操作可以再次提交。沿用原 16 项队列限制和串行请求，不增加权限或自动重试。等待达到 4 秒时，现有 tick 仅触发一次反馈更新：未发送显示等待前序操作，已发送显示等待设备回报；仍以服务器状态核对决定成功，超时及错误继续采用原失败路径。

设备查找复用本地同步数据：顶部提供名称/实体 ID 搜索、房间和关注筛选。房间来自设备注册表，搜索覆盖设备组全部成员；关注入口位于详情页，取消关注会清理该设备组成员的关注记录。搜索回调检查连接世代。筛选只影响本地展示，不改变同步范围和设备控制权限，清除筛选恢复完整列表。筛选面板复用固定按钮，打开时隐藏并锁定整个瀑布流父容器，所有底层卡片及滚动条一起隐藏；关闭后恢复原滚动位置。面板高度按实际选项收紧，最小屏幕分页显示两个房间，其他屏幕三个，避免按钮重叠。

无设备、未连接、无匹配结果及空日志提供不同说明；无匹配结果可清除筛选，未连接可进入连接设置。顶部四个入口均使用互不重叠的 44×44 触控区。无文字的原生命中按钮设为透明，避免筛选面板关闭后复用按钮遮住导航文字。通用详情优先展示主设备的调节和动作，附加字段通过“更多参数”展开；专用详情保留既有主控制布局，附加设备字段可收起。宿主测试验证最小尺寸、筛选分页、空态恢复、关注分组及参数展开都不产生设备控制指令。

`glass_ui_metrics.h` 在显示线程监听 LVGL 实际绘制/刷新事件，无采样定时器。部署后运行 `desktop ui render-metrics-reset`，操作一段时间，再运行 `desktop ui render-metrics`，可读取绘制次数、平均/最大 CPU 绘制耗时、平均/最大完整刷新耗时及原生控件数量；通过已有 UI 命令队列读取，不跨线程访问 LVGL。结合 `free` 检查持续运行内存。刷新包含显示等待，绘制次数不应直接当作面板刷新率，也不能据此声称测得手指到屏幕的实际延迟。未烧录时只能记录旧固件基线。

实板升级必须保留既有数据分区。当前板卡的 SMARTFS 格式扇区位于 `0xC00000`，大小 `0x400000`；把挂载起点改为 `0xE00000` 会读到原文件系统中间，导致挂载失败及本地设置不可读。不得通过格式化解决此问题。`build_bilibili_v3.py` 默认 `--storage-layout preserve`，保持准备好的配置；`12m-4m` 和 `14m-2m` 必须显式选择，选择布局本身不迁移数据。打包工具沿用构建元数据中的真实边界，固件末尾不得越过数据起点。`tools/tests/test_prepared_storage_layout.py` 覆盖两种既有布局保持、缺失/不一致配置拒绝和显式覆盖。

标题及状态为右侧电源按钮预留同样宽度，避免重叠和重复的开关文字；较长读数先缩至 24/20 字号，再按宽度省略。宿主输出 `WATERFALL_HOST_PERF` 记录 24 次窗口更新耗时和对象数，这是启用 ASan/UBSan 的宿主工作量指标，不是实板帧率。真实串口 `desktop cpu-status` 与 `free` 可用于部署前后基线对照；未经烧录的旧固件数据不能作为新布局的性能结果。

`ui.viewport`、`ui.attach`、`ui.scrollInfo`、`ui.scrollTo`、`ui.scrollExtent`、`ui.scrollLock` 限制父子关系及尺寸，保留装饰对象的逻辑坐标。滚动通知延后到 LVGL 异步队列，合并待处理通知，并在参数转换和回调之后检查对象世代；删除 viewport 会取消尚未执行的通知。滚动和布局不增加设备控制权限。

```bash
python3 tools/render_all_ui.py \
  --apps /path/to/prepared/apps \
  --cache /path/to/prepared/lvgl-host-cache \
  --grok-assets /path/to/grok/sdcard \
  --harness-source app/espdl-quickapp/tests/render_waterfall.c \
  --output /path/to/waterfall-render-output
```

该宿主检查使用真实 LVGL 与 QuickJS，覆盖双主题 512 张卡片、原生指针拖动、顶部/中部/底部、固定对象池、详情目标和返回位置、参数转换期间删除及滚动回调删除自身。ASan/UBSan 检查通过不代表实板帧率或功耗验收；固件仅编译，不自动烧录。

### Built-in HA compile cache

`run_compile_cache.py --quickjs <vendored-quickjs>` runs the production cache with real QuickJS under ASan/UBSan. It verifies fresh globals across 30 independent runtimes, ineligible-source bypass, invalid-version recovery, syntax errors, eviction before execution, the 512 KiB cap, and compilation/deserialization of the full reviewed HA source. Optional `--objects <render-output>` reuses sanitizer objects from `render_all_ui.py`.

Only a built-in HA launch with the exact reviewed SHA256 and all existing grants may use the process-local cache. Every launch still checks identity and source authorization and creates a fresh runtime/context. No disk bytecode is accepted, and credentials, live JS objects and UI state are never retained. First launch still parses source; repeat launches reuse at most 512 KiB of serialized code until reboot. Console startup logs distinguish `cache=hit/miss`; compile timing includes cache serialization/deserialization.

### Entity lookup and static frontend data

Live entity/scene lookups use null-prototype indexes rebuilt lazily on committed revision or array replacement. Source state remains authoritative: late callbacks resolve the latest row, and disconnect/configuration changes cannot reuse old entities. The 512-sensor regression checks bounded full-list traversal, refreshed reading provenance, removal and configuration reset. Static profile/reference catalogs use safely JSON-encoded data with identical contents, avoiding object-literal initialization bytecode. The common profile generator owns this encoding and still regenerates exact-source authorization. Commented obsolete layouts are removed from the shipped script.

## 其他原生页面交互回归

`render_other_pages.c` 通过 `render_all_ui.py --harness-source` 运行真实页面与 ASan/UBSan。覆盖首页、设置、应用、文件、网络、资源、相册和音乐的明暗主题；系统入口导航继续使用真实生产回调。应用列表固定保留 9 个槽位，连续 30 次翻页验证对象身份不变，按下后换页或移除目标不会触发替换应用/删除确认。文件卡片刷新后，旧按压的短按和长按均被拒绝，新按压仍可正常打开。亮度只预览数字，取消恢复已提交值；硬件 PWM 仍只在松手时执行，固定亮度设备继续禁用滑块。开关点击范围扩大且相邻区域不重叠。

该宿主用例不测硬件亮度、蓝牙/网络连接或文件操作性能。原始全页渲染器中的旧 HA 坐标断言尚需更新；其他页面使用独立聚焦用例验证，没有删除原始断言。

## 无浮动页面与床头灯紧凑布局（当前行为）

所有页面、弹窗、卡片和分页不再添加装饰性位移；首页卡片取消错峰上浮，按压反馈仅改变颜色，提示条仅改变透明度，顶部快捷控制面板原位打开/关闭。原生滚动、范围值/分页指示器变化和用户主动边缘拖动仍属于功能输入。旧文档中的页面进入/上浮描述由本节取代。

`render_bedside_layout.c` 按实板状态栏 32px + 快应用标题 64px，验证真实 504px 内容区、明暗主题和实际 LVGL 指针命中。常用预设和自选色盘保持同时展示，空间不足时缩小色点与间距，效果/辅助功能入口保留独立空间；预设白色按钮带边框，灯具主要界面显式隐藏未使用分页器，避免左侧残留胶囊。顶部首页品牌标识已移除。139 项 JS 测试包含紧凑布局按钮交叠、取消上浮和分页位移检查。

## 相机录像

相机录像回归：`python app/espdl-quickapp/tests/run_camera_video.py --output /tmp/camera-video-test` 使用真实录像线程、帧队列和 AVI 写入器，并模拟麦克风/存储状态。ASan/UBSan 覆盖 CSI 1024×600、USB 640×480 输入、缺存储、麦克风失败、立即停止、重复启动拦截、保存、再次录制与权限撤销；FFmpeg 检查文件完整解码、视频和音频参数及小于 110ms 的时长差。`camera_upgrade.test.cjs` 另检查照片/视频切换、保存期间禁用启动与错误反馈，`render_camera_upgrade.c` 用生产 LVGL/QuickJS 检查视频界面。实板录像验证需单独执行，宿主麦克风为模拟。物理控制台的 `desktop camera-video-mode/start/stop` 由长期桌面任务调度，防止临时 NSH 任务退出时销毁录制线程；必须已打开内置相机并开始预览才能录制。

## 统一应用侧栏

`glass_sidebar.h` 定义原生侧栏 164px、内边距 12px、功能项 140×44px、间距 8px、圆角 12px；设置、性能、音乐、ESPClaw 和原生 HA 使用同一表面/选中态样式。米家 JS 的对应常量由几何契约测试与原生头文件核对，涵盖 640×400、800×450、1024×504 和 1024×600。原生图标独立使用图标字体，避免统一文字字体后出现缺字框。

`render_other_pages.c` 额外验证设置/音乐/AI 侧栏宽度、按钮尺寸、圆角和明暗底色，保留真实导航回调；性能侧栏图表属于信息组件，仅统一宽度，保留图表高度。没有侧栏的全屏相册、相机和文件工具栏继续采用原有导航方式。米家选中态原位更新，不恢复浮动效果。

### 侧栏滑动选择

侧栏按住纵向移动超过 5px 后，光标直接跟随指针；松手吸附到最近的可用按钮并执行一次原有导航。经过按钮时不切页，不增加惯性或页面上浮。普通点击保留；取消、页面销毁及移出侧栏松手会清理拖动。原生侧栏临时光标每次手势仅创建一次，米家复用既有选中背景，移动过程只更新坐标。

`render_other_pages.c` 用真实 LVGL 指针验证原生跟手、松手单次回调、普通点击、移出取消和销毁清理；`render_waterfall.c` 用真实 LVGL/QuickJS 验证跨按钮仍保持捕获、松手后不会补发起点点击，均覆盖明暗主题。141 项 JS 回归另覆盖取消、失效代次和导航按钮复用；`pressLock` 只用于侧栏导航，槽位复用到其他用途时重置。


## 端侧语音控制

`render_local_voice.c` 编译生产 LVGL 页面与离线指令分发逻辑，模拟语音事件和 HA 网络。覆盖明暗主题、三类显式绑定、六种设备动作、请求所有权、过期与重复事件、目标状态去重、实际状态核对、丢失控制响应、不可用设备、未确认结果不重发、退出取消、返回桌面和停止聆听。候选列表排除车库门，绑定本身不发送控制。运行方式与其他聚焦渲染器相同：

```sh
python tools/render_all_ui.py --apps /path/to/apps --cache /path/to/native-lvgl-cache --output /tmp/local-voice-ui --harness-source app/espdl-quickapp/tests/render_local_voice.c --grok-assets /path/to/sdcard
python tools/test_ha_configuration.py --apps /path/to/apps --output /tmp/ha-service-test
python tools/test_wake_handoff.py --output /tmp/wake-handoff-test
```

HA 服务测试使用真实 `hass_service.c`，包含带配置版本的控制提交检查：旧版本/零版本在调用传输层之前被拒绝，当前版本才可发送。渲染器优先使用所选源码旁的 HA 头文件，避免已同步工作区的旧接口掩盖源码变化。

停止回归另覆盖：识别事件残留时点击停止、停止/返回桌面与控制前状态响应同时到达、控制已发送后的停止提示、停止后新旧事件均不执行，以及显式重启后恢复控制。两种主题均运行这些断言；网络动作使用模拟服务。

TTS 的 `speech_tts_host.c` 使用真实中文模型验证首次与缓存命中后的 PCM 逐采样一致，并在 PCM 回调中请求清除缓存：正在使用的权重必须保留到合成结束，后续分段仍能正确生成，返回后缓存必须为空。重复清除也须安全。通过 `tools/test_speech_host.py` 在 ASan/UBSan 下运行。

双线程卷积沿用同一逐位参考，并在完整中文推理内覆盖计算中取消。`speech_compute_host.c` 另用原子屏障确认两个任务都已进入，随后触发取消，验证 join 后能开启新的双任务计算范围并完成全部工作。板端验证检查 CPU 0/1 记录、推理期间 UI 响应、首音与内部阶段耗时、取消、模型缓存回收、麦克风和相机复开。

物理控制台可用 `desktop ui speech` 打开页面，`speech-start`/`speech-stop` 执行本页启停，`speech-status` 只读页面所有权、请求阶段、候选数量和绑定数量。`speech-bind-light`/`speech-bind-cover`/`speech-bind-fan` 只打开选择器并读取候选，`speech-bind-back` 关闭选择器；这些命令不自动绑定设备或发送控制。实板持续采音与反复启停不代替真人唤醒、指令识别或实际设备动作验收。


### 合并状态岛

`render_statusbar.c` 使用真实 LVGL 输入验证浅色/深色状态岛：默认无边框、按下才显示圆角边框、松开异步进入控制面板，背景音乐继续播放，锁屏隐藏与恢复。32 张截图覆盖强弱 RSSI、蓝牙已连接数量、电量和独立充电闪电；首页内容上移 40px。RSSI 每 5 秒从已有串行 RPC 邮箱读取，链接代次变化立即失效；未测得信号不猜测强度。蓝牙 HID 后端当前仅支持一个连接，扫描数量不作为连接数。电池通过 BATIOC_CAPACITY/BATIOC_STATE 读取；未启用电量计或读数不可用时显示 --%，不会虚构电量或充电。当前 overlay 配置尚未启用 CONFIG_BATTERY_GAUGE，实板电量及充电显示需匹配实际电源硬件。

`status_rssi_host.c` 链接准备树的实际 protobuf 实现，验证线上请求编码、强弱信号、超时、错误响应、非法数值和响应释放，使用 ASan/UBSan。


### 桌面负载优化与功能保持（2026-10-18）

`render_weather_visibility.c` 覆盖不透明全屏页遮挡首页时延后绘制、横向和纵向移出后继续刷新、透明页不跳过、取消侧滑以及返回时立即呈现最新数据；服务轮询继续运行。性能页保留每秒采样与 60 点历史，固定历史及重复快照不重画图表，数据或所选指标变化仍刷新。

`render_statusbar.c` 在两种主题下验证 120 次相同状态更新不重画状态岛，并继续检查 RSSI、蓝牙、电池、充电、音乐图标、真实输入和 40 次锁屏恢复。`test_grok_schedule.py` 增加隐藏时定时器暂停和再次显示后的恢复检查；757 个帧/状态校验保持一致，可见时 10 秒 100 帧的原有节奏和控制响应保持不变。隐藏切换允许一次用于暂停的回调，稳定隐藏后无需轮询。

宿主测试只说明绘制和调度契约；实际 CPU 前后对比、固件哈希及实板反复返回验证另存于 `evidence/desktop-load-20261018.json`。性能页自身持续采样和画图，测量结果不代表所有页面的负载。

### 多语言和双语歌词（2026-10-18）

`render_music_font.c` 使用真实 SD 流式字体后端，验证 16／20／28／48／72 号字体的原 27 个缺字、日韩／拉丁／越南／希腊／西里尔／土耳其语样例、图标回退、双语开关和三套效果字幕边界。运行 `render_all_ui.py` 时传入 `--music-font <生成字体的测试副本>`；副本名使用 `test-font.ttf`，供 `test_music_font_failures.py` 验证缺失、截断及同长度损坏，最终恢复副本。

`music_background_host.c` 另外验证 192 对同时间戳原文／译文、独立 `.zh.lrc`、接口 tlyric、严格时间对齐、OpenCC 短语转换及日韩原文保护。`render_lock_music.c` 覆盖播放快照改为随锁屏分配／释放后的页面生命周期；此夹具需预置 `cover.rgb565`。

`test_music_languages_device.py` 在真实 P4 上使用已有公开演示音频和原创日／韩测试文本，验证 SD 译文读取、简体显示、双语开关、三套效果、PCM 进度、暂停及第二行韩文实际排版。保留先前控制台提示符丢首字节的失败记录；修正后的测试只识别提示符，不因超时自动重放已派发的操作。此短测不代表长时间压力测试、所有歌曲拥有译文或真人确认扬声器声音。


### Wi-Fi 与蓝牙新界面（2026-10-19）

`render_wireless_ui.c` 使用实际 LVGL 页面覆盖两种主题的网络列表、选网、手动添加、连接提交、密码清除、忙碌禁用、失败与 IPv4 状态，以及蓝牙配对、安全连接、触摸板可用、Boot 模式和主机挂起。中文输入法候选栏与密码框、键盘分别占用独立区域。40 次 Wi-Fi／蓝牙打开关闭检查页面定时器与控件释放；设备服务使用离线夹具，不发出无线请求。通过 `tools/render_all_ui.py --harness-source app/espdl-quickapp/tests/render_wireless_ui.c` 运行，并用 `render_navigation_lifetime.c` 验证设置入口与切页生命周期。

2026-10-25 补充完整 Wi-Fi 流程：打开页面自动扫描，提交连接后自动轮询关联和本次 DHCP 租约，成功显示 IP 并返回列表。回归包含旧地址不能提前报成功、链路快照 `-EAGAIN` 重试、45 秒关联超时、60 秒取址超时、超时后重试、确认断开，以及连接中离开页面后再进入。`python3 tools/tests/test_desktop_backend.py` 检查无线状态与断开仅操作 `C6NET_IFNAME`（`wlan0`），不读写有线 `eth0`；`python3 tools/tests/test_network_lifetime.py` 检查实际 DHCP 生命周期。工作线程回归通过 `cc -std=gnu11 -Wall -Wextra -Werror -pthread tools/tests/test_desktop_worker.c tools/c6/desktop_worker.c -o <binary>` 编译运行。固件构建器同步 `tools/c6/desktop_{worker,backend}.*` 并纳入源码哈希及清理判据，避免复用旧基线中的接口与结构定义。


### 原图风格横屏相机与滑动倍率（2026-10-19）

相机页面与实时 RGB565 预览共用 `camera/reference.jpg` 的原图按钮图块；快门按用户要求改为纯红圆形，街拍标签及其原点击区域已移除。倍率列替代 mm 焦距，支持 1×／1.5×／2×／3×、上下拖动和点击；当前档位用原样深底红字显示。`tools/generate_camera_reference.py` 编译只读素材与抗锯齿倍率字形，字形可用 `--font` 指定系统字体。

按标注移除 LEICA、左侧展开、闪光灯和曝光按钮及其点击区域。右上是切换相机，中上是照片／录制切换，右下打开本机照片相册；三圆按钮循环原色、鲜艳和黑白，处理实际取景、照片与录像像素。原图的其他布局与纯红快门保留。

`camera_upgrade.test.cjs` 验证设备分页、点选收起、设备代次、录像保存时禁止重启、发现节流、色彩、相册请求，以及倍率拖动／点选／取消／边界和设置页手势归属。`render_camera_upgrade.c` 用实际 LVGL 输入验证倍率上下滑动、选设备、启动／停止、相册预览返回与退出还原桌面安全区，两种主题各 12 个状态。

`run_camera_controls.py` 验证两层共用纯红快门像素、共用触摸坐标、CSI／USB 的全部倍率和图像边界，以及原地缩放与显示写回；缩放使用固定 1140 字节栈上列映射，无新增整帧分配。USB 每次解码后重新合成 UI，避免取景线及快门被后续帧覆盖。

`run_camera_controls.py` 另验证三种色彩下的全部倍率预览，以及相册等待预览释放、录像收尾后只消费一次的导航条件。`run_camera_photo_zoom.py` 检查三种色彩下共 24 个 CSI／USB 倍率照片的文件尺寸、画幅和采样像素。USB 原色 1× 保留输入 JPEG，放大或其他色彩以 RGB565 BMP 保存；CSI 沿用 BMP。`run_camera_video.py` 检查实际录像队列的裁切与黑白像素，并在不同倍率下验证录像、麦克风失败、无 SD 卡、停止／复开、授权撤销、AVI 解码与音画时长。测试使用离线合成画面，不代表实机画质或帧率。

### 精简数码相机界面（2026-10-21）

右侧无字画幅图标循环选择 16:10／16:9／4:3／1:1，四个圆点标记当前档位；首次选择前保留原始画幅。取景角标、居中裁切、BMP 照片及 AVI 录像共用比例与裁切函数，USB 的解码黑边不参与裁切。照片分别为 512×320、512×288、512×384、512×512，录像分别为 320×200、320×180、320×240、320×320。录像开始时固定本次尺寸，保存结束前禁止切换；帧队列按实际尺寸在堆中分配，不为所有比例预留最大帧。原色／1× USB 直存 JPEG 仅用于原始画幅，选定比例后改存对应 BMP。`run_camera_controls.py` 校验三类输入、四种比例、四档倍率的真实像素边界；照片测试检查 32 组显式比例裁切，录像测试解码全部四种比例的 AVI 并检查音画时长。`render_camera_upgrade.c` 在两种主题下逐档检查 LVGL 四角像素与快门，交互测试维持 64 个控件预算。

USB 1× 预览的六款滤镜和全部画幅统一走减采样 JPEG 快速路径：在解码得到的小图上转换颜色，再直接按裁切坐标输出最终取景区，省去完整画布展开、搬移和重复颜色运算。每帧固定倍率／滤镜／画幅快照，解码中途的选择在下一帧生效，图像和边框不会使用不同参数。拍照、录像与放大继续使用原完整画布路径。`test_mjpeg_preview.c` 逐像素比较六款色彩、原始及四款画幅、三档解码采样，覆盖奇数 MCU 边界、非法裁切、截断输入和完整照片路径隔离；`run_camera_controls.py` 检查解码期间切换后的边框与色彩图块一致性。

色彩按钮点击后在右侧展开竖列六个 40×40 的正方形迷你色块，提供原色、鲜艳、黑白、暖色、冷色、复古。色块使用纯色，不包含模拟风景缩略图或文字，无面板背景；红色细边框表示当前选择，点选应用并收起，外部点击仅收起。展开时保留原位大快门并暂停倍率手势。LVGL 与实时 RGB565 合成器共用色块和坐标，预览、照片和录像共用六款 RGB565 变换；无额外整帧缓冲。`camera_upgrade.test.cjs` 检查六款选择、竖列正方形布局及控件预算；`render_camera_upgrade.c` 通过真实触摸选择全部六款；`run_camera_controls.py` 校验纯色色块与实时合成，`run_camera_photo_zoom.py` 使用实际异步保存服务验证 48 组照片，`run_camera_video.py` 校验六款录像帧。

主界面保留纯黑取景区、红色快门、倍率与少量图标；移除贯穿取景区的粗线，只保留细角标。模式切换按钮旁用 PHOTO／VIDEO 标明当前模式；相册和色彩图标改为清晰的线形符号。待机仅显示小字提示，不增加中间按钮。LVGL 与实时 RGB565 预览共用图块和角标坐标。

取景角标按实际可见画面计算，不再固定围成 570×556 的近方形区域。共用布局函数同时考虑 CSI／USB 输入画幅、USB 解码画布中的黑边、居中倍率裁切及整数像素采样；待机、菜单和实时预览同步使用该边界。切换设备、分辨率或倍率时立即更新。`run_camera_controls.py` 用 CSI、4:3 USB 和 16:9 USB 的实像素替身检查四档倍率下边框外全黑、边框内有图像及快速预览分支；`render_camera_upgrade.c` 对实际 LVGL 截图像素断言 CSI／USB 四角位置，交互测试另检查模式与倍率参数同步。仍需实体摄像头核对实屏，宿主结果不代表已部署。

二级菜单改为右侧 176 像素宽的无字控件区，不再在取景区弹出卡片。设备使用板载相机／外接摄像头图标，当前设备用轻底圆形和红点标记；分页、分辨率与帧率用箭头、画幅、循环图标和档位圆点表达。单设备时不显示分页，固定档位降低图标亮度。刷新、收起均使用原生图标字体，无新增位图或整帧缓冲。

菜单展开时保留取景角标和原位红色大快门，分辨率、帧率与刷新排列在快门下方；原位收起后恢复当前模式和倍率。大快门在设备可用时保持明亮，轻点后收起设置并仅启动一次取景；无设备或保存中仍禁用。点选设备、收起箭头或轻点外部均可关闭。外部拖动、取消和内部空白点击不会关闭，也不会穿透到快门或倍率。菜单中的操作名称仅保留在透明点击控件内部，不呈现可见文字；异常回执在退出菜单后仍可显示。

进一步统一小图标的深色底座与描边，倍率使用圆形选中环。PHOTO／VIDEO 标识靠近模式切换按钮，STD／VIVID／B&W 显示实际色彩状态；待机顶栏补充分辨率与帧率，字符使用设备字体支持的 ASCII x。无设备或保存中入口快门降低不透明度，与实际可用状态一致；实时预览继续使用原纯红快门。无需额外帧缓冲。菜单的切换按钮与完成按钮使用相同关闭路径，从实时取景返回菜单后关闭可恢复一次原取景。

`camera_upgrade.test.cjs` 覆盖紧凑布局、操作隔离、倍率手势取消、错误提示恢复、关闭菜单恢复取景、快门可用状态、外部点击／拖动、右侧无字布局及实际模式／帧率选择；`render_camera_upgrade.c` 执行真实 LVGL／QuickJS 的两种主题截图及触摸交互，包含无设备／单设备菜单、原位收起和实际帧率切换，并断言可见标签仅使用图标字体且位于右侧；`run_camera_controls.py` 校验精简角标、VIDEO／色彩状态图块、共用触摸坐标、倍率边界和显示写回。异步照片轮询在合成器测试中使用隔离替身，不代替照片保存服务验收。均为宿主离线验证，未据此声明已烧录或完成实体屏幕验收。

### 输入法焦点、布局与按键对比度（2026-10-19）

输入法深色模式黑底白字，补齐的浅色模式白底黑字；候选栏、普通键、功能键和按下反馈采用相同中性色。禁用候选降低不透明度，语音入口及识别结束恢复均使用现有 `GLASS_ICON_MIC` 麦克风图标，录音中保持停止图标。输入框及宿主页面继续使用各自主题。

`render_ime_ui.c` 使用实际 LVGL 输入法和 QuickJS 弹窗，复现旧候选词串入新密码框、候选栏移动／缩放不同步，并验证修复后密码框显示 EN、禁止拼音候选与语音输入。Wi-Fi 共享键盘通过 `glass_ime_set_target` 即时清除旧字段组合；原始 LVGL 换目标路径也有候选点击保护。字段删除会解除键盘引用，语音换字段时停止并恢复原草稿。

两种主题各 5 张截图覆盖中文、密码、移动缩放、语音结束恢复和快应用输入；实际指针点击候选、禁用时拒绝点击，另检查 320／640／744／824／1024 宽度、40 次创建／销毁、先选词再提交的两次确认。普通键、功能键 CHECKED 与 PRESSED 状态分别检查文字和底色差异；快应用原先反置的按键底色已纠正。词库与语音后端为离线夹具，测试不发出录音或网络请求；ASan／UBSan 结果不替代实板触摸、词库或语音服务验收。

运行方式：`python3 tools/render_all_ui.py --apps <prepared-apps> --cache <lvgl-host-cache> --output <review-output> --harness-source app/espdl-quickapp/tests/render_ime_ui.c`。`render_wireless_ui.c` 同时回归实际 Wi-Fi 表单和切页清理。

### 原生 P4 JPEG（2026-10-26）

`tools/generate_p4_jpeg_fixtures.py --output <fixtures>` 使用 Pillow 生成真实 baseline、采样、优化 Huffman、灰度及拒绝用例。`tools/test_p4_jpeg_parser.py --fixtures <fixtures> --nuttx <prepared-nuttx> --toolchain <gcc14-bin> --output <output>` 对生产解析器执行截断、边界及 3 万组变异/随机输入，启用 ASan/UBSan，并交叉编译 P4 单元。

`tools/test_p4_dma2d.py --nuttx <prepared-nuttx> --toolchain <gcc14-bin> --output <output>` 验证实际共享管理器的并发租约、在途 IRQ、分配/IRQ 失败与残留配置复位，再编译真实 PPA/JPEG 驱动。`tools/test_p4_jpeg_adapter.py --output <output>` 检查应用私有 DMA 缓冲、补齐裁剪、失败不发布、软件回退条件及 FD 回收。

固件用 `--hardware-jpeg` 显式开启，默认关闭；`desktop jpeg-test` 执行真实 PPA → JPEG → PPA，比较内置已知 JPEG 的 RGB565/RGB888/灰度像素并测试空熵错误恢复。内置样本像素诊断不用于用户照片日志。宿主通过、整机构建、硬件像素、当前版本 Flash/CSI 与实体观察分开记；不能将前几项替代屏幕验收。专题见 `docs/NATIVE_JPEG_2026-10-26.md`。

### 有线热点共享（2026-10-26）

`tools/test_network_share.py --reference <pinned-network-reference> --output <out>` 编译真实内核共享控制器和应用适配，检查 eth0 固定出口、租约/DNS 门控、掉线、换 IP、冲突恢复和 NAT 所有权；再编译准备后的原生 NAT 分配/释放及开关函数体，检查 64 条上限、分配失败、完整释放与重复 enable 的锁平衡。此函数级测试不等同于端到端报文仿真。

`tools/test_network_recovery.py --sharing --output <out>` 验证上游冲突时保留 SSID/密码并重开到不冲突网段；不带 `--sharing` 回归原本的本地配置热点。`test_dhcpd_status.py` 同时覆盖选项关闭/开启，并检查真实 DHCP OFFER 的网关与 DNS。`test_portal_network.py` 包含新增网络状态字段。真实 LVGL 的 `render_network_recovery.c` 覆盖两主题的共享和冲突状态。

实板先执行 `desktop ethernet-dns`、`desktop ethernet-probe 223.5.5.5 53`，两个诊断均绑定 eth0 且有超时。热点开启后用 `desktop sharing` 记录往返计数；手机关闭移动数据再实际打开网站，才能确认端到端互联网访问。构建、读回、NAT active、计数、手机结果和当前版本屏幕/Flash 验收分别记录。详见 `docs/ETHERNET_HOTSPOT_2026-10-26.md`。

### 原生 JPEG 编码（2026-10-26）

`tools/test_p4_jpeg_emit.py --output <output>` 编译实际标记生成器与解析器，验证四种采样、六档质量、全部短容量与尾部守护，启用 ASan/UBSan。`tools/check_p4_jpeg_header_pillow.py --output <emit-output>` 使用独立 Pillow/libjpeg 核对生成头的量化/采样/尺寸；不把头检查当硬件编码验收。

`tools/test_p4_jpeg_encode_adapter.py --output <output>` 编译实际应用包装，对受控 ioctl 验证 30×24/非紧密 stride 输入的私有复制、边缘重复、128 字节输出对齐、失败清理/NULL 长度及 FD 回收。`test_photo_store.py` 分别运行 JPEG 开/关版本，覆盖常驻保存线程、裁切/风格、编码期间取消、ENOSPC 不回落、旧格式与回执清理；`test_photo_storage.py` 和 `test_assistant_platform.py` 同时运行。

实板 `desktop jpeg-encode-test` 执行 PPA → 19 组编码 → PPA，使用独立 TinyJpgDec 核对已知像素，包括约 89 KB 纹理结果和真实包装；短输出 ENOSPC 后继续有效作业。`desktop jpeg-test` 验证交替后的既有解码。`desktop jpeg-file-test /sdcard/photos/<filename>.jpg` 只读软件解码文件，仅记录尺寸/字节数/像素指纹，不打印图片或调用模型。真实 CSI 拍照保存与物理屏幕/内部 Flash 写擦分别验收，详见 `docs/NATIVE_JPEG_ENCODE_2026-10-26.md`。

### 语音页面全局唤醒（2026-10-19）

`render_grok_conversation.c` 验证大球页移除唤醒开关、仅显示默认开启状态；全局唤醒释放录音后交给大球的现有识别／AI／播报状态机，结束当前会话和退出页面不取消其他后台音频所有者。无内容时回到全局待机。包含慢连接、连续对话和两种主题，主机夹具不创建真实网络或录音。浮层与大球使用同一球体播放器、app 表情库及正视效果；浮层的空闲计时不截断大球会话。

`tools/test_wake_mini.py` 使用真实 LVGL 与 SD 小球素材，验证语音会话页面和本地语音页面空闲时可启动后台监听，前台录音／播报仍独占设备；已有 ASR 录音收到真实唤醒事件时显示浮层，连续 6 秒无输入或待处理任务时才收起，且不停止其他所有者的录音。覆盖事件过期、重复拒绝、切页、相机叠加和触摸穿透，启用 ASan／UBSan。

大球页例外：新唤醒与已有 ASR 录音中的唤醒均只使用页面内的大球，不显示小球。其他页面的浮层录音中切入大球页时立即隐藏浮层并移交原录音；不会重开 ASR，也不会在原浮层的空闲计时到期时停止大球会话。`wake_mini_host.c` 验证呈现与所有权，`render_grok_conversation.c` 验证生产大球的接管状态。

浮层从固定寿命改为连续空闲 6000ms 后收起。`wake_mini_host.c` 验证超过 6 秒的实际自有 ASR 输入不被截断、识别收尾、AI／工具任务、播报及本地设备核对期间持续显示；活动完成后重新计满 6 秒，新指令重置计时，过期指令不重置，后台唤醒采集本身不延长浮层寿命。浮层显示期间允许空闲麦克风恢复全局待机。

AI 结果反馈：`wake_mini_host.c` 验证真实 LVGL 结果卡片、已确认／未确认播报、不重复播放、语音失败保留结果、历史清除时停止自身播报，以及可变高度卡片的相机快照。`render_grok_conversation.c` 使用两种主题验证大球结果面板和对应语音，AI 文本声称成功但设备未确认时仍播报未确认。`tools/test_agent.py` 验证原生工具回执的确认标记覆盖、失败不发卡、紧凑结果投影的模式／轮次隔离、UTF-8 分块和确认状态持久化。完整 AI 历史不复制到小球状态中，也不以历史卡片触发开机播报。

`tools/test_voice_wake_observer.py --runtime <authenticated-model-runtime> --fixtures <pcm-fixtures> --output <output>` 使用生产唤醒观察器和真实认证模型，验证正例触发、负例不触发、原始 PCM 字节不变，以及模型缺失／坏分帧时仅关闭可选观察器。`tools/test_wake_handoff.py` 另检查共享录音发布唤醒事件时不改变前台任务、阶段或请求编号。主机输入夹具不能替代真人实板唤醒率验收。

### 小球待机素材缓存与元素复用（2026-10-23）

`tools/test_grok_cache.py --output <output>` 独立编译实际 `glass_grok_cache.h`，在 ASan／UBSan 下检查无损 RGB565A8 分平面 RLE、畸形数据、内存分配失败、八线程共享 512 KiB 预算和销毁归零。预算包含申请的帧数据及索引，不包含分配器管理开销；不足时继续原 SD 读取路径。

`tools/test_grok_reader.py --apps <prepared-apps> --cache <LVGL-host-cache> --assets <GBS-assets> --output <output>` 使用真实 LVGL 与五种原始素材，对照缓存关闭／开启时的 1643 帧顺序、原始像素与完整帧缓冲哈希；覆盖三轮待机循环、注视、动作、隐藏、减少动态效果，以及原图片控件和双缓冲指针复用。预热后的两个循环不再读取动画帧载荷；文件状态仍每秒检查，不能将其描述为 SD 总 I/O 为零。另有素材替换、缺卡、重试和清理用例，原 `test_grok_sd.py` 继续验证全部表情与在途销毁。测试素材路径应短于固件既有 96 字节路径缓冲。

只缓存表情 02 的最多 128 帧，压缩保存注视处理前的原始像素，命中时解压到既有后备缓冲。素材重开、错误及播放器退出清理缓存；FAT 时间精度无法保证识别同大小、同时间戳的原地替换。宿主帧读取减少与像素一致不能替代实板 CPU、首次预热和实体屏幕验收。

本机准备区的增量依赖未自动跟踪 `glass_grok_player.inc`；修改播放器或缓存头后应确保实际重编 `glass_grok.c`，或执行完整清理构建。不能只凭链接成功认定 `.inc` 的修改已进入镜像；候选需核对冻结源码与最终 ELF。

### 蓝牙连接状态、归属与配对保存（2026-10-25）

`tools/test_ble_identity.py --output <output>` 检查每构建新静态随机地址、广播名/路径边界、独立配对文件命名、同一镜像同步/重启不换身份，以及生产同步函数的设置/读回/类型检查失败均停止 HID 启动。标准 builder 自动生成候选头和 BLE-IDENTITY.json；生成头哈希进入构建元数据和清理指纹。每轮实板验收同时记录镜像、真实地址和广播名，新身份首配与旧配对兼容性分开。

`tools/test_ble_snapshots.py --output <output>` 检查普通设备和 HID 的四种连接组合、独立断连/代次及保存错误反馈。`tools/test_ble_hid_ownership.py --output <output>` 对生产访问函数验证外来/过期句柄不得读取或修改 HID 状态，重复配对仅允许当前所有者删除密钥。

`tools/test_ble_store.py --apps <prepared-apps> --output <output>` 使用固定 SDK 的真实密钥结构、RAM 存储实现和生产持久化代码。覆盖 54 组文件写入故障、RAM/文件回滚、错误保持、旧文件兼容、按实际 LTK 绑定用途、记录换序/删配对/换密钥、无变化免写和容量。原 SDK 删除记录的字节长度缺陷须先通过 `prepare_nimble_store.py` 在独立构建树修正；同一测试必须拒绝未修复的反例。测试仅在临时目录使用人工密钥。

`tools/test_ble_hid_bonding.py --output <output>` 检查用途标记延后至 SMP 完成密钥保存之后，仅由事件队列消费，且必须是当前加密、16 字节密钥的 HID 连接。另以连续邮箱轮询验证 50 ms watchdog 不被推迟，定时保存仍可执行。保留 `test_ble_central.py` 和 `test_ble_hid_mailbox.py`；`render_bluetooth_list.c` 检查两主题、独立控制/计数与保存失败提示。桌面头文件转引同一公共 BLE 头，禁止重新引入结构副本。以上宿主测试不代表实板双连接已通过。

`tools/test_ble_channel.py --output <output>` 编译完整生产共享 GATT 服务，以受控 NimBLE 传输运行 ASan/UBSan。覆盖加密所有者、句柄／代次、UUID、244 字节边界、四帧 FIFO／满队列、MTU 与订阅、发送副本／mbuf 所有权、忙碌／取消／超时不重放及 reset 唤醒。`test_interface_protocols.py` 编译实际 QPK 适配器验证严格参数及回执；`test_ble_platform.py` 检查应用清理共享数据不停止系统 HID。实机 Windows 收发和跨重启结果单独归档于 BLE_SHARED_GATT_2026-10-27.md，不能用传输替身代替。

`tools/test_nimble_callout.py --output <output>` 编译完整生产 NuttX NPL `os_callout.c` 与实际 `os_types.h`，用受控 POSIX 到期通知测试 100 批、每批 32 个独立定时器同时到期，包含两个事件队列与直接回调、重复同定时器合并、重新设定及常驻任务交付。ASan/UBSan 检查没有回调指针覆盖；真实无线连接另测。可用 `--source <旧生产 os_callout.c>` 做负控制：旧单指针实现首批只交付一个定时器，HCI 轮询定时器丢失，程序返回 42，测试必须失败。`test_ble_hid_bonding.py` 还实际编译加密回调，确认 ENOTCONN 清理不会再发送 terminate 或伪报鉴权失败，而弱密钥、外来句柄及真实加密错误仍被拒绝。
