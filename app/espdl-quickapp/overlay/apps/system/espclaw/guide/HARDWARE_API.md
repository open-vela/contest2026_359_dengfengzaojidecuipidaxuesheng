# 快应用硬件 API 参考

这些接口属于本机 QuickJS 的 `system` 对象，不能用 Web Bluetooth、Web Audio、Node.js 或 ESP-IDF JavaScript API 代替。生成硬件应用时按本节的真实签名调用。用户没有要求硬件功能时，不自动录音、播放或连接设备。

## 1. 异步任务约定

下表中的设备操作均接收一个普通对象并立即返回正整数任务 ID；参数可省略时等同于 `{}`。启动失败可能抛出异常，运行中的失败由查询结果返回。不要把任务 ID 当作设备句柄或成功结果。

| API | 参数与返回 |
| --- | --- |
| `system.hardware.capabilities()` | 同步返回 version 3、GPIO／保留引脚、最终配置的总线引脚、PWM、音频／BLE，以及 boardInterfaces 官方板级接口目录 |
| `system.hardware.poll(id)` | 同步返回任务状态；`done:false` 时继续轮询。完成后这次调用取走结果并释放任务记录，不能再次读取同一结果 |
| `system.hardware.cancel(id)` | 请求取消排队中或运行中的任务；不阻塞。继续查询到结束。取消录音会放弃这次文件写入，保留同名旧录音 |
| `system.hardware.finish(id)` | 请求正常提前结束当前录音／播放；录音保存已经采集的内容 |
| `system.audio.stop(id)` | 与 `hardware.finish(id)` 相同，适用于录音、播放、提示音 |

状态结构：`{id,state,done,ok,bytes,elapsedMs,peak,result?,error?,message?}`。

`boardInterfaces.interfaces` 每项含 id、protocol、access、documented、compiled、available、endpoint（可选）和 note。compiled 来自固件配置；available 是节点／网卡注册或存储挂载检查，没有检查项时为 null，不证明实际传输或链路。先发现服务，再读真实完成回执。目录覆盖 DSI／GT911、CSI／UVC、ES8311、SD／Flash、C6 Wi-Fi／BLE、RMII Ethernet 和扩展总线。高速 USB Type-A Host 与高速 Type-C Device 共用控制器，此固件默认 HS Host（UVC／MSC／HID／UAC1），可互斥切换 HS CDC Device；独立 FS CDC 受屏幕 GPIO26/27 接线占用保护，当前接线返回 EBUSY。Serial/JTAG 仅用于可信本地控制台。

- `state` 为 `queued`、`running`、`done`，或已经取走／不存在时的 `unknown`。
- 只有 `done === true && ok === true` 才是成功。`result` 是相应 API 的结果对象。
- `error` 是负错误码，`message` 是对应说明。`EBUSY` 表示资源已占用，`EBADF` 表示没有打开，`EINVAL` 表示参数不合法，`ENODEV` 表示驱动／设备不可用，`ETIMEDOUT` 表示这次设备传输超时。不能将这些情况显示成“成功”。
- 录音进度的 `bytes` 是真实单声道 PCM 字节数，`elapsedMs` 由实际采样数计算，`peak` 是最近一块数据的绝对峰值（0～32768）。播放进度依据已归还的音频缓冲区计算。
- 音频有独立工作线程，录音／播放不阻止 GPIO、总线和 BLE 任务执行。各工作线程内按提交顺序执行，不要无节制地提交任务而不查询完成结果。
- 应用退出会取消未完成任务，关闭设备、释放引脚、停止自己启动的音频和 BLE 活动。关闭过程在后台完成，立即重开时遇到 `EBUSY` 可稍后重试。
- 用一个定时器每 100～250 ms 查询并更新状态，不在 JS 中用 `while` 等待完成。硬件请求不延长 JS 回调的运行预算。

```javascript
function watchHardware(id, done) {
  const timer = setInterval(function () {
    const state = system.hardware.poll(id);
    if (!state.done) return;
    clearInterval(timer);
    if (!state.ok) {
      prompt.showToast('硬件操作失败：' + (state.message || state.error));
      return;
    }
    if (done) done(state.result);
  }, 100);
}
```

## 2. GPIO

| API | 参数 | 成功结果 |
| --- | --- | --- |
| `system.gpio.open(options)` | `pin` 必填；`mode` 为 `input`（默认）、`output`、`openDrain`；`pull` 为 `none`（默认）、`up`、`down`；`value` 为初始电平 0／1，默认 0 | `{}` |
| `system.gpio.read(options)` | `pin` | `{value:0或1}` |
| `system.gpio.write(options)` | `pin,value`；必须已经以输出方式打开 | `{}` |
| `system.gpio.close(options)` | `pin` | `{}`，引脚回到输入态并释放 |

当前固件开放的扩展 GPIO 是 2～6，实际列表以 `capabilities().gpioPins` 为准。出厂 P4X GPIO0/1 用于 XTAL32K；其他引脚被系统板载功能保留，不要猜测可用性。传感器电压、外部上拉和接线需要依据真实外设；没有外设时不能把浮空输入解释成传感器读数。

```javascript
watchHardware(system.gpio.open({pin: 6, mode: 'input', pull: 'up'}), function () {
  watchHardware(system.gpio.read({pin: 6}), function (r) {
    prompt.showToast('GPIO6 当前电平：' + r.value);
    watchHardware(system.gpio.close({pin: 6}));
  });
});
```

## 3. I²C

| API | 参数 | 成功结果 |
| --- | --- | --- |
| `system.i2c.open(options)` | `bus` 默认 1，当前只开放 I²C1 | `{}` |
| `system.i2c.transfer(options)` | `bus`；`address` 必填，7 位地址 8～119；`data` 是要写出的字节数组；`readLength` 默认 0；`frequency` 默认 100000 Hz，支持 10000～400000 | `{data:[读回字节],bytes:读回数量}` |
| `system.i2c.close(options)` | `bus` | `{}` |

出厂引脚配置的 I²C1 SDA=GPIO2，SCL=GPIO3，以 capabilities 为准。写入与读取同时指定时，用同一次总线传输完成寄存器地址写入与重复起始读取。`data` 与 `readLength` 不能同时为空，单个方向最多 65535 字节。`address` 已是 7 位值，不要再左移。总线没有设备应报告驱动错误，不能返回虚构读数。

```javascript
// 仅演示协议格式。0x48、寄存器0x00必须替换为实际外设手册中的值。
watchHardware(system.i2c.open({bus: 1}), function () {
  watchHardware(system.i2c.transfer({bus: 1, address: 0x48, data: [0x00], readLength: 2}), function (r) {
    console.log(JSON.stringify(r.data));
    watchHardware(system.i2c.close({bus: 1}));
  });
});
```

## 4. SPI

| API | 参数 | 成功结果 |
| --- | --- | --- |
| `system.spi.open(options)` | `bus` 默认 2，当前开放 SPI2 | `{}` |
| `system.spi.transfer(options)` | 非空 `data` 字节数组；`bus`；`mode` 0～3，默认 0；`frequency` 1000～20000000 Hz，默认 1000000 | `{data:[等长读回字节],bytes,frequency:实际时钟频率}` |
| `system.spi.close(options)` | `bus` | `{}` |

SPI2：CS=GPIO6、SCK=GPIO3、MOSI=GPIO4、MISO=GPIO5，以 capabilities 为准；8 位、MSB first、全双工。一次 `transfer` 期间保持片选有效。只读时由应用发送相应数量的 dummy 字节。模式、速度、命令和 dummy 值依据外设手册，不能假定所有设备一致。

## 5. UART

| API | 参数 | 成功结果 |
| --- | --- | --- |
| `system.uart.open(options)` | `port` 默认 1；`baudRate` 默认 115200，范围 1200～921600，驱动必须接受该值 | `{}` |
| `system.uart.read(options)` | `port`；`length` 默认 256，为最多读取字节数；`timeoutMs` 默认 1000，0 为立即读取 | `{data:[实际读回字节],bytes}`；没有数据时返回空数组 |
| `system.uart.write(options)` | `port,data` 字节数组；`timeoutMs` 默认 1000 | `{bytes:已发送字节数}`；未能在期限内完整提交时为错误 |
| `system.uart.close(options)` | `port` | `{}` |

UART1：TX=GPIO4、RX=GPIO5，8 数据位、无校验、1 停止位。SPI2 与 UART1 共用 GPIO4/5，不能同时打开。`read` 收到一批数据即可返回，不保证一次读完应用协议的一帧；应用应缓存半帧并按实际协议拆包。它不是 USB 控制台，也不允许读取系统日志串口。

## 6. PWM 与舵机脉宽

| API | 参数 | 成功结果 |
| --- | --- | --- |
| `system.pwm.write(options)` | `channel` 0～3，默认 0；`frequency` 默认 1000 Hz；`duty` 必填，0～1 | `{pin}`；首次调用自动占用该通道，后续调用更新波形 |
| `system.pwm.stop(options)` | `channel` | `{}`；停止并释放通道／引脚 |
| `system.servo.write(options)` | `channel`；`frequency` 默认 50 Hz；`pulseUs` 默认 1500，允许 100～5000，但必须小于一个周期 | `{pin}` |
| `system.servo.stop(options)` | `channel` | `{}` |

通用配置通道 0／1／2／3 对应 GPIO2／3／4／6。完整 LCD 配置启用 `--lcd-backlight` 后，通道 0 改为屏幕专用 GPIO26，不向应用和 Agent 暴露；扩展 PWM 使用 capabilities 实际列出的通道 1／2／3。GPIO26 不在通用 GPIO 允许列表内。底层使用硬件 LEDC，不用 JS 定时器模拟 PWM。PWM 频率参数范围 1～100000 Hz，实际能否实现还受硬件时钟和分辨率约束，失败以任务错误为准。端点占空比按驱动精度处理。舵机 API 输出的是脉宽，不能保证机械角度；电源、共同地线、型号允许的脉宽区间必须由真实接线和器件决定。

```javascript
// 仅在用户确实连接了适合该脉宽的舵机后调用。
watchHardware(system.servo.write({channel: 3, frequency: 50, pulseUs: 1500}));
// 停止：watchHardware(system.servo.stop({channel: 3}));
```

## 7. 麦克风、扬声器与音频保存

| API | 参数 | 成功结果 |
| --- | --- | --- |
| `system.audio.record(options)` | `clip` 默认 `recording`；`durationMs` 默认 10000，0 表示录到用户停止；`sampleRate` 当前为 16000 | `{bytes,sampleRate:16000,channels:1}` |
| `system.audio.play(options)` | `clip`；`volume` 默认 30，范围 0～100 | `{playedMs}` |
| `system.audio.tone(options)` | `frequency` 默认 440 Hz，20～20000；`durationMs` 默认 300，正整数；`volume` 默认 30 | `{playedMs}` |
| `system.audio.save(options)` | `clip`；`data` 是 PCM16 小端字节数组；`sampleRate` 默认 16000、范围 8000～48000；`channels` 为 1（默认）或 2 | `{bytes}`；写成 WAV 文件，供 `play` 使用 |
| `system.audio.info(options)` | `clip` | `{fileBytes,bytes,sampleRate,channels,bitsPerSample:16,durationMs}`；校验 WAV 头与实际文件长度，`fileBytes` 包含文件头 |
| `system.audio.list()` | 无 | `{clips:[{clip,valid,fileBytes,bytes,sampleRate,channels,bitsPerSample,durationMs}]}`；列出本应用已保存的录音，不含写入中的临时文件；损坏文件的 `valid` 为 false，只保证有 `clip,fileBytes`，可供删除 |
| `system.audio.remove(options)` | `clip` | `{}` |
| `system.audio.stop(id)` | 进行中的录音／播放／提示音任务 ID | 立即返回；仍须轮询原任务到 `done` |

`clip` 是本应用的音频标识，只允许字母、数字、下划线、短横线，长度 1～58 字符，不是任意文件路径。预览和正式安装的应用身份不同，录音不自动跨身份复制。音频保存于本包的持久化数据空间，删除应用会一起删除。

录音列表应通过 `audio.list()` 读取实际文件，并自行排序、分页。创建新录音前先读取列表，选择未使用的 `clip`；同名录音在成功保存时会替换旧文件。内置“录音机”也使用这些接口，入口在“全部应用”。

录音直接使用板载 ES8311／I²S 输入，将真实立体声时隙中的左声道提取成单声道 PCM16 WAV，并分块写入存储。16 kHz 单声道每秒约 32 KB，空间不足会报告错误。取消或写入失败会保留同名旧音频；正常停止才提交新文件。空录音不算成功。

`play` 播放本接口录制或 `save` 生成的 PCM16 WAV，当前支持单声道／双声道，双声道混为单声道输出。不能把 MP3、AAC、任意 WAV 变体或网络 URL 传给 `clip`。音乐播放器和录音／播放共享板载音频设备，正在占用时返回忙碌错误。

录音按钮与播放按钮都应由用户点击触发，提供状态、停止操作和失败提示。不要把麦克风没有返回数据时的超时显示为静音录制成功，不自动把录音发送给外部服务。

```javascript
// 可直接使用的录音／停止／播放交互片段。
let audioTask = 0;
const audioStatus = ui.text('尚未录音', 24, 24, 24, ui.primary);
function startAudio(operation) {
  if (audioTask) { prompt.showToast('请先停止当前音频'); return; }
  try { audioTask = operation(); }
  catch (e) { ui.setText(audioStatus, String(e)); }
}
ui.button('录音', 24, 88, 160, 54, function () {
  startAudio(function () { return system.audio.record({clip:'voice', durationMs:0}); });
});
ui.button('停止', 204, 88, 160, 54, function () {
  if (audioTask) system.audio.stop(audioTask);
});
ui.button('播放', 384, 88, 160, 54, function () {
  startAudio(function () { return system.audio.play({clip:'voice', volume:30}); });
});
setInterval(function () {
  if (!audioTask) return;
  const s = system.hardware.poll(audioTask);
  if (!s.done) {
    ui.setText(audioStatus, s.state + ' · ' + Math.floor(s.elapsedMs / 1000) + ' 秒');
    return;
  }
  audioTask = 0;
  ui.setText(audioStatus, s.ok ? '操作完成' : '失败：' + (s.message || s.error));
}, 150);
```

## 8. BLE 扫描与连接

| API | 参数 | 成功结果／后续状态 |
| --- | --- | --- |
| `system.ble.start()` | 无 | 启动蓝牙主机；随后查询 `status`，等待 `ready` |
| `system.ble.scan()` | 无 | 开始约 10 秒主动扫描；随后查询 `status` |
| `system.ble.status()` | 无 | `{ready,scanning,connecting,connected,error,devices:[...]}` |
| `system.ble.connect(options)` | `index` 为最近一次扫描列表中的索引 | 提交连接；轮询 `status()` 所返回的任务，从完成结果的 `result.connected` 确认连接状态 |
| `system.ble.cancel()` | 无 | 取消自己的扫描或待完成连接 |
| `system.ble.disconnect()` | 无 | 断开自己的连接 |

这里的 `status()` 同样返回任务 ID，要通过 `hardware.poll` 取得上表结果。扫描项包含 `index,name,address,addressType,rssi`。扫描完成后再让用户选择设备，扫描期间列表仍会变化。实际最多保存 32 个扫描项，当前蓝牙主机同时支持一个 BLE 连接；系统设置正在使用蓝牙时，不抢占它的连接。

ESP32-C6 提供 BLE，不提供经典蓝牙 A2DP。连接成功不等于已配对、可以播放蓝牙音频，或已经完成某设备的 GATT 协议。Central 与鼠标 Peripheral 共用一条连接且互斥。先读取 status 的 scanGeneration，再用 connect({index,scanGeneration}) 选择刚扫描的设备。连接完成后用 status.generation 作为下表 GATT 操作的必填 generation，断开后旧代次失效。

GATT 操作仍返回硬件任务 ID，轮询 hardware.poll：

| API | 参数／结果 |
| --- | --- |
| `system.ble.services({generation})` | 发现服务，返回 items 的 uuid、start、end |
| `system.ble.characteristics({generation,start,end})` | 返回 valueHandle、properties、uuid |
| `system.ble.descriptors({generation,start,end})` | 发现描述符，先找到真实 CCCD 0x2902；禁止猜句柄 |
| `system.ble.read({generation,handle})` | 返回 data 字节数组，最多512字节；超限报错 |
| `system.ble.write({generation,handle,data,response:true})` | 默认等 ATT Write Response；response:false 只表示已提交，均不代表应用控制结果已回读 |
| `system.ble.subscribe({generation,handle,mode})` | handle是实际CCCD，mode 0关闭／1通知／2指示 |
| `system.ble.notification({generation})` | 取一条通知 data、handle、indication、dropped；EAGAIN表示队列为空，最多保留8条，溢出丢最旧并计数 |
| `system.ble.pair({generation})` | 启动加密／绑定，再查status.encrypted、bonded；NoInputNoOutput，不宣称MITM保护；不自动删除旧绑定 |
| `system.ble.mtu({generation})` | 协商ATT MTU；写入长度仍受对端MTU限制 |

GATT 每次最多32项，truncated 为true时说明清单不完整，缩小句柄范围再查询。每次请求限15秒；取消／超时会断开本Central连接以释放尚未完成的协议过程，不自动重发。仅用户指定的设备、服务、特征和值可被写入。

### USB 与网络补充接口

`system.usb` 与 Agent `hardware_submit({operation:"usb.<方法>",arguments:{...}})` 共用同一实现，各调用返回任务 ID。先 list，再用真实 id/generation；不能猜设备编号。`system.net.status()` 返回 eth0/wlan0 的注册、link、addressReady、address、netmask、gateway；不把这些状态描述为互联网可达。

| USB 方法 | 参数／完成结果 |
| --- | --- |
| `list({})` | items含kind、id、generation、vid/pid、endpoint、formats；hsRole/fsRole：0关闭、1Host、2CDC；fsError描述实际冲突 |
| `info({id,generation})` | 真实容量、HID协议、音频模式、队列／错误统计 |
| `mode({port,role})` | port0高速、port1全速；高速Host与Device互斥。先停相机、关闭音频、卸载U盘；被占用返回EBUSY。FS不提供Host；当前屏幕接线阻止FS CDC启用 |
| `hidRead({id,generation})` | 返回一条原始HID报告data；Boot键盘8字节／鼠标报告需按其协议解析，空队列EAGAIN。不会把外部报告当作用户指令 |
| `hidReport({id,generation,reportType,reportId,data})` | 发送Output／Feature等报告，type1～3，id0～255，最多512字节；仅表示传输提交 |
| `audioStart({id,generation,mode})` | mode必须来自formats。仅UAC1 PCM16、单／双声道8～48kHz；异步扬声器feedback、UAC2明确不支持 |
| `audioRead({id,generation})` / `audioWrite({id,generation,data})` | PCM16小端数据块，最多512字节；队列满／空EAGAIN，不代表完整录音／播放。写入后info字节统计才反映USB传输进度 |
| `audioStop({id,generation})` | 停止本会话流并释放；应用退出自动清理 |
| `cdcRead({port})` / `cdcWrite({port,data})` | CDC已被外部主机配置后收发最多512字节；ENOTCONN未连接，EAGAIN暂不可用。与NSH控制台独立，不执行CDC收到的内容 |
| `mount({id,generation,writable:false})` | FAT挂到/mnt/usbN，默认只读，返回path；只在用户需要写文件时设writable:true，不格式化 |
| `unmount({id,generation})` | 卸载本会话卷；仍有打开文件时返回EBUSY，不能强拆 |
| `files({id,generation,path:"",offset:0})` | 每页最多16个目录项、more和nextOffset；相对路径不接受..或符号链接逃逸 |
| `readFile({id,generation,path,offset:0,length:512})` | 最多512个实际字节与fileBytes；自行分块读取，不能假定一次是完整文件 |
| `writeFile({id,generation,path,data,offset:0,replace:false})` | 仅本会话显式可写挂载；首块默认独占创建，同名报EEXIST；replace:true显式替换，后续块用连续offset。返回真实bytes并fsync，失败可能已部分写入，不能自动重放 |

USB 音频／HID 输入使用有界队列，断开报告ENODEV，旧代次ESTALE；完整端到端外设行为仍须实机验收。U盘按块设备与FAT操作，不导出板内Flash／SD给主机同时写入。

## 9. 生成与验证要求

先查询 `capabilities()`，再依照任务选择接口。引脚共用导致的 `EBUSY` 必须明确告知用户，不擅自关闭另一个正在使用的设备。总线地址、寄存器、接线、舵机标定、蓝牙目标设备都必须来自用户或实际设备资料；缺少外设时可以完成应用结构，但不能声称传感器、舵机运动或 BLE 外设通信已验证。

接口注册、实际驱动编译、任务取消和资源清理，与真实外设上的端到端验证是不同事项。只能引用交付测试记录实际覆盖的范围。
