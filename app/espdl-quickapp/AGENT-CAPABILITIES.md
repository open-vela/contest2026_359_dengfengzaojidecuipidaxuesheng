# ESPClaw Skills 与 MCP

ESPClaw 与 Grok 语音小球共用模型配置及对话服务，按模式隔离提示文件、工具、输入草稿和上下文。两种界面切到同一模式时，可以继续同一段对话。配置变更从下一次请求生效，当前请求固定模式及配置。

## ESP 工作区界面

工作区使用左侧模式与当前对话导航、中央消息区和底部输入框。新任务中的小球与标题、快捷卡片、输入框共用中心线；开始对话后，原有小球缩为 48 px，保留在 AI 输出文字旁边，表情由真实请求状态驱动。滚动时小球留在输出区左侧，输入文字或阅读完整回复时暂时隐藏，收起后恢复。点击小球或「语音」可进入同模式的语音对话。

创作模式右上角「草稿」展开预览与安装面板；助手模式显示工具能力入口。面板不增加独立定时器，隐藏时不参与绘制，也不会运行应用脚本。点击「预览草稿」才启动真实 QPK 应用，预览完成后才能安装。当前原生回归覆盖面板连续开关 30 次的控件／定时器数量稳定、小球实例不变及流式消息原位更新；这些检查不能代替 ESP 实机帧率和堆内存测量。

## 三种模式

| 模式 | 默认入口 | 能力 | 默认提示文件 |
| --- | --- | --- | --- |
| 聊天 | Grok 语音小球 | 自然交流，不开放工具 | `overlay/apps/system/desktop/agents/chat/agent.md` |
| 创作 | ESPClaw、开发者 CLI | QPK 草稿工具和创作技能，先预览再手动保存 | `overlay/apps/system/desktop/agents/create/agent.md` |
| 助手 | 两个界面的模式选择器 | 助手技能、已配置和获准的 MCP 工具 | `overlay/apps/system/desktop/agents/assistant/agent.md` |

点击当前模式名称选择切换；语音会话或 AI 请求进行中须先结束。模式选择不会自动推断用户意图，也不会由模型自行切换。创作控件仅在创作模式显示。模式选择在当前运行期间分别记忆，重启恢复上述默认入口。

三个 `agent.md` 是设备 Agent 的运行提示文件，不是 Codex 仓库开发指令。默认内容由 `tools/generate_agent_profiles.py` 生成内嵌头文件，无 SD 卡也可运行。可选覆盖文件为 `/sdcard/espclaw/agents/{chat,create,assistant}/agent.md`，UTF-8、最多 8 KiB，支持 BOM／CRLF；缺失、格式无效、超限或符号链接时使用内置默认值。每次请求加载一次，修改只影响下次请求。提示文件、技能和 MCP 返回内容均不能扩大模式权限；工具桥接层对越权调用直接返回 `mode_denied`。

历史仍使用现有双槽日志文件，总计保留最近 6 轮以控制内存，每轮保存稳定的模式标记。模型只收到当前模式的已完成历史，界面只显示当前模式的记录。「新对话」只清空当前模式；旧版未标模式的历史归入创作模式。重试保持原模式，其他模式正在执行时不能发送、重试或清空它的请求。

## 配置入口

在设备上打开「AI 设置／扫码配置」，使用已有配对流程连接设备配置页，在「Agent 能力」设置本地技能和 3 个 MCP 服务槽位。

服务默认关闭。填写显示名称、HTTPS MCP 地址及可选 Bearer 令牌，然后启用。允许工具列表支持逗号分隔的精确工具名，例如 `weather_get,scene_list`；空列表只允许发现工具，`*` 表示允许该服务的所有已发现工具。令牌留空保留原值；勾选「清除已保存的访问令牌」可改用无令牌服务。配置读取接口只返回令牌是否已设置。

MCP 配置和技能索引在请求开始时读取，联网延迟到模型实际调用工具时。对模型开放 `mcp_list_tools`、`mcp_call_tool`；服务名称使用稳定别名 `mcp1`、`mcp2`、`mcp3`。模型先发现工具，获得原始 `inputSchema`，再按参数调用。调用同时受用户配置的工具允许列表约束，每个服务每轮最多保留 32 个已发现工具名。

## 自定义 Skill

通过配置页的「文件空间 → SD 卡」建立以下目录并上传 `SKILL.md`，也可以直接写入 SD 卡：

```text
/sdcard/espclaw/skills/assistant/weather/SKILL.md
/sdcard/espclaw/skills/create/my-app-style/SKILL.md
```

```markdown
---
name: weather
description: 用户询问天气时使用，优先调用已配置的天气工具。
---

1. 如果用户没有说明地点，先询问地点。
2. 使用 mcp_list_tools 查找可用天气工具及其 inputSchema。
3. 按实际参数调用 mcp_call_tool，依据工具结果回答。
4. 调用失败时明确说明，不能把猜测当成实时天气。
```

目录名是技能 ID，限 31 个 ASCII 字母、数字、短横线或下划线。文件使用 UTF-8，支持 BOM、LF 和 CRLF；每份最多 8 KiB。前言里的 `description` 使用一行文字，最多 160 个 UTF-8 字节。每轮最多索引 12 份自定义技能，目录扫描上限 64 项；符号链接、无效文件和超限文件不进入索引。全文通过 `skill_read` 按需读取，技能不执行脚本或系统命令。

助手模式内置 `conversation` 技能，创作模式内置 `quickapp` 技能，聊天模式不索引技能。自定义技能只能在所属模式目录内发现和读取。上一版 `/skills/技能名/SKILL.md` 应移到对应的 `create` 或 `assistant` 目录，避免自动把所有技能授予两个模式。QPK 工具仅在创作模式可用，已有大脚本参数上限不因扩展工具的上限而缩小。

## MCP 兼容范围

- Streamable HTTP：HTTPS POST、JSON-RPC、JSON 与 SSE 响应。
- 初始化版本首选 `2025-11-25`，接受 `2025-06-18`、`2025-03-26`；完成 `notifications/initialized` 后开始操作。
- 会话 ID 和协商版本用于后续请求，支持 `tools/list` 游标分页及 `tools/call`，保留 `content`、`structuredContent`、`isError` 等结果字段。
- 单次请求最多 15 秒，单个 JSON 消息／工具结果最多 16 KiB、嵌套深度最多 16。SSE 累计接收上限 128 KiB。
- 停止对话会中止当前请求，并尽力发送取消通知；服务端是否已完成操作取决于服务端，超时和会话过期不会自动重放工具调用。下次发现请求可建立新会话。
- 请求结束释放本轮扩展上下文，尽力发送有会话 ID 的 DELETE，单个关闭请求限 1 秒。
- 沿用固件的 TLS 信任链，手动配置 Bearer 令牌。当前未实现 OAuth 登录、stdio 子进程、旧版双端点 HTTP+SSE、资源／提示词能力，以及服务端 sampling／elicitation。连接这类服务需提供兼容的 MCP HTTP 网关。

协议参考：[MCP Streamable HTTP](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)、[生命周期](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)、[工具](https://modelcontextprotocol.io/specification/2025-11-25/server/tools)。

## JBOX-S3 专用工具（board · jbox_*）

JBOX-S3 另有28个 `jbox_*` 专用工具，直接随助手请求提供相同 schema，也可在 board MCP 分页发现后调用。
覆盖发现、已绑定连接、能力／状态、矩阵、GPIO／ADC／PWM、I²C／SPI／UART 和 S3 函数；
通过 `hardware_poll` 等待完成，取消／释放使用共享控制器的 `hardware_stop`／`hardware_release`。
没有配对、密钥读取或重置权限。完整映射、参数与只读调用示例见 [JBOX 连接文档](../../docs/JBOX_P4.md#jbox-专用-mcp-工具)。

## 内置智能家居工具（board · home_*）

- 链接共享 Home Assistant 服务（`CONFIG_SYSTEM_HASS=y`）时，board 服务额外提供
  `home_light`、`home_status`、`home_list`、`home_state`、`home_control`、`home_service` 六个进程内工具；
  未链接该服务的构建不提供 home_*，其他 board 工具仍按实际构建分页返回。
- 查询类工具只使用 `HASS_READ`；控制复用与原生页完全相同的白名单（`light`、`switch`、
  `input_boolean`、`fan`）和注册服务表（`hass_actions.h`）。AI 路径没有 `HASS_CONFIGURE`，
  不能读取地址、令牌或修改连接配置；`home_control` 的亮度仅对灯有效。
- 控制结果不乐观改写：HTTP 2xx 后在 8 秒内最多读取 4 次，开关一致且亮度误差不超过
  1 个百分点才算 `verified:true`；否则返回未确认说明，且不自动重发。
- 每次成功调用返回一个设备卡片（`kind` 图标域、`name`、`state`、`level`、`on`），由对话
  界面在消息流内渲染：每轮最多保留最近 4 张、同设备同轮去重，卡片随历史持久化。
  工具不打开智能家居应用、不调用任何界面函数，聊天里只会出现卡片和文字回复。
- 结果 JSON 不含地址与令牌，工具返回内容仅作为数据处理，不能提升权限。
- `home_light` 合并灯名匹配与查询/控制：仅在 `light.*` 中完整扫描唯一候选，重名时返回候选且不执行写入。
  只传 `query` 时实时读状态；`on`、`brightness`、`rgb_color` 可合并为一个控制请求。
  配置版本从匹配阶段绑定到控制提交，连接发生变化时拒绝沿用旧匹配。权限、服务白名单与颜色回读继续生效。
- 「Agent 能力 → 允许 AI 控制家居」默认关闭；查询可用，写入须显式启用。每次控制检查此配置，关闭后立即拒绝新控制。门锁、安防控制不向 AI 开放。
- `home_service` 仅在回读属性符合请求时标记 `verified:true`；未映射的服务保留未确认状态。场景、脚本和按钮的 HTTP 成功仅表示受理，卡片显示「已受理」，不宣称设备执行成功。

## 内置音乐工具（board · music_*）

桌面注册 `music_status`、`music_search`、`music_play`、`music_pause`、`music_resume`、
`music_stop`、`music_volume`、`music_skip` 八个进程内工具，仅助手模式可调用。
home_* 与 music_* 的完整 schema 已随助手首轮请求提供，优先直接调用，无需额外发现。
仅使用通用 `mcp_call_tool` 包装调用时，仍须先 `mcp_list_tools({server:"board"})`。
它们复用系统音乐服务与后台播放器；不调用界面入口，不打开音乐应用。歌曲、歌手、服务状态和进度
以 `kind:music` 小卡片显示在对话中，沿用每轮四张上限与历史保存。

播放参数 `query` 只写歌名，先核对搜索返回的歌名和用户指定歌手，不把歌手拼接到歌名；搜索只查找歌曲，不自动播放。
搜索返回 `result_id`；播放传回原 `query` 和此编号，在服务锁内校验当前搜索版本、搜索词、有效期与使用状态，
再复制服务内部的歌曲与地址。结果 60 秒内可使用一次，播放线程也检查期限；不做第二次网络搜索，避免
再次检索到不同演唱版本。新搜索、搜索失败、超期或已使用的结果会被拒绝，不自动回退到按歌名播放。已使用编号返回 `music_result_used_do_not_replay`，只能查状态，不能重新搜索后自动重发播放。
仅传 `query` 的旧调用仍兼容，原生页面、收藏切歌和手动重播仍重新解析地址。
音量限 0–100 的整数，切歌沿用收藏列表。
暂停与继续使用原生幂等接口，重复暂停不会切换为播放，停止后继续会返回无活动歌曲。
播放等控制返回 `accepted:true, verified:false` 表示受理，用 `music_status` 读取后续服务状态；不将受理
说成已听到声音。按搜索结果播放时，服务在受理锁内返回 `requested_track_id`；后续状态的 `track_id` 须与它一致，避免误认另一首正在播放的歌曲。
状态仍加载时本轮最多追加两次只读查询，仍未完成就报告加载中或未确认。音量返回值匹配目标即可结束。
超时／取消不自动重发副作用，已受理播放继续由系统服务托管。
地址、流 URL、封面 URL、API 密钥不进入工具结果，外部 MCP 返回内容不能伪造本机音乐卡片。

`tools/test_music_background.py --apps <prepared-apps>` 将真实音乐服务、MP3 解码、音乐 MCP 与
离线 HTTP／PCM 替身一起运行，验证搜索不播放、播放状态、幂等暂停／继续、音量、切歌、停止、
参数拒绝和取消；不链接 LVGL。真实扬声器和外部音乐服务仍须另行联调。

## 音乐与米家控制实例

以下是参数与结果处理参考，不代表已在用户设备执行。实体 ID、结果编号和歌曲信息必须取自真实工具返回。
米家设备需先接入用户的 Home Assistant，并在设备设置中完成连接；AI 控制还需启用「允许 AI 控制家居」。

### 搜索并播放指定歌曲

用户：“播放 Beyond 的海阔天空。”

1. 直接调用 `music_search({"query":"海阔天空"})`。
2. 核对返回歌名及歌手。若返回的是其他歌手，说明未找到所需版本，结束；当前接口一次只返回一首，不能编造候选列表。
3. 假设返回 `result_id:12` 且歌手符合要求，调用 `music_play({"query":"海阔天空","result_id":12})`。
4. 保留受理返回的 `requested_track_id`，调用 `music_status({})`。同一编号且 `state:"playing"` 时可说“已开始播放”。
   仍为 `loading` 时有限查询后说明正在加载；`error`、编号变化或未确认时如实说明，不重复发起播放。

完整流程通常为三次直接工具调用，网络检索只发生一次；查询播放状态不调用音乐检索接口。
聊天卡片自动显示歌曲和状态，助手用一句简短说明收尾。

| 用户请求 | 调用与处理 |
| --- | --- |
| 搜一下晴天 | `music_search({"query":"晴天"})`，展示结果后结束 |
| 播放刚才找到的那首 | 当前模式存在唯一、未使用且未超期的真实结果时，复用原 query 与 result_id；否则重新搜索核对 |
| 播放周杰伦的歌 | 先询问具体歌名，不能把歌手作为歌曲搜索词 |
| 暂停／继续音乐 | `music_pause({})`／`music_resume({})`，再查状态确认；停止后继续会返回无活动歌曲 |
| 停止音乐 | `music_stop({})` 后读取 `music_status({})` 确认 `stopped` |
| 音乐音量调到 30% | `music_volume({"level":30})`，检查返回 volume，无需搜索或再查状态 |
| 下一首 | `music_skip({"direction":"next"})`，核对受理编号与后续状态；没有收藏则说明原因 |
| 缺少音乐服务密钥 | 引导到扫码配置页的音乐服务保存密钥，不在聊天索取密钥 |

### 米家灯开关与亮度

用户：“打开客厅灯，亮度 40%。”

1. 一次调用 `home_light({"query":"客厅灯","on":true,"brightness":40})`；工具内部匹配唯一灯，多个匹配先询问具体设备，零匹配则说明未找到。
2. 工具内部提交一次并回读验证；`verified:true` 后可说“客厅灯已开启，亮度 40%”。无需额外 `home_list` 或 `home_state`。
   未确认则说“已发送请求，设备状态尚未确认”，不重复发送控制。

用户：“把书房灯调成粉色，亮度 30%。”

1. 一次调用 `home_light({"query":"书房灯","brightness":30,"rgb_color":[255,192,203]})`。
2. 依据工具对颜色与亮度的回读结果简短回复；不先开灯、再调亮度、再调色，也不沿用上一轮参数。未确认时不得宣称颜色已生效。

| 用户请求或异常 | 调用与处理 |
| --- | --- |
| 查看卧室温度 | `home_list({"query":"卧室温度"})` 获取唯一传感器，再用 `home_state` 读取；不需要启用控制权限 |
| 关闭风扇 | 查找唯一 fan 实体后 `home_control({"entity":"fan.bedroom","on":false})`，不传灯光亮度 |
| 阳台窗帘开到 60% | 找到真实 cover 实体后 `home_service({"entity":"cover.balcony","service":"set_cover_position","arguments":{"position":60}})`，根据回读报告 |
| 启用观影场景 | 找到真实 scene 实体后调用 `home_service` 的 `turn_on`；仅报告已受理，不能声称场景内所有设备已完成 |
| 设备离线／名称重名 | 说明离线或请用户选具体设备，不改用其他设备 |
| 尚未配置／控制未授权 | 引导到 Home Assistant 设置或「允许 AI 控制家居」，不在聊天收集令牌 |
| 控制超时／回执不明确 | 可只读查询当前状态，不自动再次提交动作 |

“开灯并播放音乐”等组合请求按用户明确的参数分别执行并报告每项结果；一项失败不将两项都记为成功，
也不撤销已经完成的另一项。所有示例均在对话内显示卡片，无需打开音乐或智能家居应用。

## 验证

新增本地工具、定时任务和精确别名的完整使用说明见 [本地助手工具](ASSISTANT-TOOLS.md)。

对话正文支持轻量 Markdown：标题、无序／有序列表、引用、分隔线、行内强调、删除线、行内代码与带语言／行号的围栏代码块。链接保留可读的目标地址；图片和 HTML 不执行、不自动下载。当前不是完整 CommonMark 实现：嵌套列表、表格布局、数学公式暂按文本保留。英文粗体使用 DejaVu ASCII 子集，中文沿用现有字库；斜体强调以下划线显示。字体共约 31 KiB，只创建一次，中文不重复打包。

思考／输出状态采用无底板的小行，展开后显示真实工具调用名称，最多保留最近 4 项和累计调用次数；不据调用推断执行成功，不显示模型内部思维。最近一轮完成后仍可查看这些本次会话的调用记录，重启后的持久历史不保存该临时过程。每条输出最多 24 个排版块、每块最多 64 个行内片段，超出部分保留原文。流式更新复用已有控件，代码横向滚动，状态动效复用现有刷新定时器并遵守减少动态效果设置。

`tools/test_agent.py --apps <prepared-apps> --output <test-output>` 使用 ASan／UBSan 执行真实技能、配置、JSON-RPC、字节分片 SSE 解析、ESPClaw 工具桥接、对话线程及双槽存储；覆盖跨模式工具拒绝、上下文隔离、旧历史迁移、模式标记持久化、局部清空、重试和请求模式固定。其中 `board-home-host` 用真实 home_* 工具、真实共享 HA 服务和脚本化 HTTP 传输验证卡片字段、状态缓存、确认／未确认控制、白名单与参数注入拒绝。HTTP 对端和模型调用由确定性测试替身提供。配置页另有 `portal_frontend.test.cjs`，原生 LVGL 界面和连续语音流程由 `render_grok_conversation.c` 验证。

尚未配置用户的实际 MCP 地址和令牌；本机测试不代表实机网络、第三方服务或模型端已验证。固件构建结果和烧录状态见本次输出目录的 `verification.json`。

## Harness 与多模态摄像头

工程入口见 [agent.md](agent.md)，执行边界、循环／调用预算、模式提示词和回执语义见 [HARNESS.md](HARNESS.md)。默认 CSI 与完整 USB 摄像头选择、拍照、本地分类、已授权云端问答／OCR，以及候选固件的构建与部署见 [MULTIMODAL-CAMERA.md](MULTIMODAL-CAMERA.md)。
