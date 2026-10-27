# 热点网页刷新超时修复

候选 3 镜像 `b8f95c77534649db77d490ab32346ea49b698ca8d785c584e5bf03642aa0ae38`
已确认手机取得 `192.168.4.2` 并进入 AP 的 ARP 表，STA 也有真实 DHCP
和 TCP 收发。用户报告首页显示异常，刷新后连接超时。串口记录为
`listener=-1 running=1 accepts=14 closes=13 active=1 errors=1309 error=98`。
因此 DHCP 已修复，但这一版网页配网不能验收通过。

源码核查确认三个关联问题：

1. HTTP 先启动、DHCP 后以 `task_create()` 创建。目标 NuttX 克隆未设置
   CLOEXEC 的文件描述符，子任务可保留监听绑定和客户端连接。监听与接纳
   连接改用原子 `SOCK_CLOEXEC`，不改变全局文件表策略。
2. 目标 `poll()` 将单项 setup 失败也报告为 `POLLERR`；例如 TCP 的 poll
   资源不足，并不等同于监听套接字失效。现在通过 `SO_ACCEPTCONN` 核查，
   健康或状态暂不可判定时保留监听并退避；客户端错误不再误关监听。
3. 旧 worker 重绑失败直接继续循环，跳过 pending 超时清理。目标 TCP
   端口查找仅排除 TIME_WAIT，未关闭的本地连接还可阻止重绑。现在先处理
   超时，重绑失败仍处理客户端；确认监听失效时释放本 worker 的 pending。

监听错误仅保存首个故障的阶段、errno、poll 标志、SO_ACCEPTCONN 与
SO_ERROR，以及故障/重建计数，不保存请求、配对码、令牌或口令。候选 3
没有这些首错字段，不能声称已经唯一证明其最初是哪一次 poll/accept
事件触发了关监听。

`tools/test_portal_worker.py` 编译真实 HTTP worker、路由及嵌入网页资源，
以真实 TCP 检查首页/样式/脚本完整字节、空预连接、配对、并行状态/配置
读取、连续刷新、响应截断及故障恢复。它还模拟目标 poll 错误和 NuttX
活跃连接阻止重绑的语义；不是以 Linux SO_REUSEADDR 行为替代目标行为。
冻结候选 3 是负向对照。文件继承另由
`tools/test_portal_fd_inheritance.py` 执行目标的真实 `fdlist_copy()`。

宿主通过不代表新镜像已经通过手机配网、真实配置试连/回退、冷启动、
新 BLE 身份配对或显示/Flash 实体验收。证据须绑定最终候选源码和镜像。
