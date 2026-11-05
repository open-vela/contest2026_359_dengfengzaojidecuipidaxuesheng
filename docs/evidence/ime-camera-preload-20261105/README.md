# 输入框预加载与 CSI 预览退出验收

输入框创建时即在后台准备拼音词库，覆盖桌面表单、本地 AI 输入框和 Quick App 输入对话框。加载期间保留拼音，完成后自动刷新候选；界面线程不等待词库初始化。相机预览中的返回操作会先停止采集，待显示页交接完成后退出应用。

2026-11-05 已在 ESP32-P4 实板烧录并独立回读校验镜像：

```text
SHA256 7fc37780eda29f108b9f6f5db7e626997ec68e8fa2fa7e7a41fa02175e6f754f
bytes  8299136
```

写入前后 4 MB 数据分区的校验值一致。详细结果见 [BOARD-VERIFICATION.json](BOARD-VERIFICATION.json)。

| 检查 | 实际结果 |
| --- | --- |
| 输入框先于键盘准备词库 | 页面返回后约 3.44 秒观察到加载完成，随后才打开键盘 |
| 首字输入 | 指针测试命令回执 425 毫秒，包含串口通信及派发开销 |
| 本轮输入渲染 | 最大 120 毫秒；预加载阶段渲染最大 430 毫秒 |
| 内存 | 首字输入后用户堆余量约 15.94 MiB，未复现内存占满 |
| 中文候选 | 输入 `ni` 显示“你”；候选确认路径通过 |
| CSI 实际预览 | SC2336，1024×600；采集与显示翻页正常 |
| 实体返回 | 用户点击右侧返回区域后一次退出，确认无卡顿、闪屏；设备回报采集线程结束并回到桌面，`pan_errors=0` |

首次词库准备仍需几秒；立即输入时会保留拼音并等待候选出现。初轮音乐页检查曾在词库尚未完成时采样，后续就绪与确认通过。只验证了 CSI 和实体返回点击；其他手势方向由宿主测试覆盖。已做硬件复位启动，未做断电冷启动或新蓝牙身份的配对验收。

## 源码与验证范围

实板结果绑定上面的冻结镜像。修复合入较新的主站代码时保留了独立的 JBox 更新，主站合并后的宿主回归另记在 [MERGED-HOST-VERIFIED.json](MERGED-HOST-VERIFIED.json)，不能把该宿主结果称为整棵新源码的实板烧录验收。

- [VERIFIED-IMAGE-SOURCES.json](VERIFIED-IMAGE-SOURCES.json)：冻结镜像构建输入的原始 SHA256。
- [PUBLICATION-SOURCE-MATCH.json](PUBLICATION-SOURCE-MATCH.json)：提交文件与实板源码的 LF 归一化摘要对照。
- [HOST-VERIFIED.json](HOST-VERIFIED.json)：原冻结候选的宿主验证摘要。

## 复跑宿主回归

在 Linux 上准备 GCC/G++、Python 3；界面回归还需匹配的 NuttX/apps 与已有原生 LVGL 宿主缓存。

```sh
python3 tools/test_ime_preload.py --output /tmp/ime-preload
python3 tools/test_camera_exit.py --output /tmp/camera-exit
python3 tools/test_camera_boundary.py --output /tmp/camera-boundary
python3 tools/test_camera_sync.py --output /tmp/camera-sync
python3 tools/render_all_ui.py \
  --apps /path/to/prepared/apps --cache /path/to/native-lvgl-cache \
  --output /tmp/ime-ui --safe-event-dispatch \
  --harness-source app/espdl-quickapp/tests/render_ime_ui.c
```

词库测试使用真实 Google Pinyin 和内置材料解码，并在初始化持锁期间注入存储延迟；覆盖非阻塞调用、单加载任务、真实候选、释放与重开。界面用例验证输入框先于键盘触发准备、下拉菜单不触发、加载后自动显示候选及对象反复创建/销毁。相机测试执行生产指针轮询和延后退出函数，覆盖返回点击、边缘手势、拖动取消与采集/重绘/录像清理门限。
