# 锁屏背景

使用内置 `image_gen` 生成的四张原创背景，仅包含背景，不包含时间、文字或控件。

| 页面 | 背景 | 1024×600 成品 | 固件压缩输入 |
| --- | --- | --- | --- |
| 时钟 | 暖色晨光 | `01-morning.jpg` | `01-morning-compact.jpg` |
| 音乐 | 铜色暮光 | `02-dusk.jpg` | `02-dusk-compact.jpg` |
| 天气 | 晴空蓝 | `03-sky.jpg` | `03-sky-compact.jpg` |
| 照片默认页 | 青绿山丘 | `04-hills.jpg` | `04-hills-compact.jpg` |

完整生成提示词见 `PROMPTS.json`，资源摘要见 `MANIFEST.json`。原始 PNG 保存在工作区 `output/lockscreen-wallpapers-20261104/backgrounds/`。

固件使用 512×300 的普通 JPEG 压缩输入，进入页面时解码、放大至 1024×600 的单个 RGB565 画布；退出页面后释放。资源内置，不依赖 SD 卡或网络。照片页仍优先显示用户自己的 SD 照片。

更新素材后运行 `python tools/generate_lock_wallpapers.py`，同时交付生成的 `glass_lock_wallpapers.inc`。不要直接修改生成头文件。
