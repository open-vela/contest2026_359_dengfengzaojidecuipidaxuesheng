# P1 实板验收

本文档定义 P1-06、P1-08、P1-10 和 P1-13 的可重复实板入口。源码与构建通过不代表 HIL 通过；只有脚本 exit code 为 0 且原始日志与当次 firmware SHA 一起归档时，才可标记对应实板项完成。

## Verification limits (2026-09-05)

The script only checks the measurements it can recognize. Exit 0 does not
establish leak freedom, touch accuracy, timing tolerances, simultaneous LVGL
animation/input load, or correct silicon-revision behavior. Camera-cycle logs
still require heap trend analysis. The new P1 images have not been flashed.

`desktop-v1-test` now includes `dsi_diag`, `ostest`, `smp`, and `timerjitter`.
Before the network scenarios, separately enable `CONFIG_SYSTEM_PING=y` and
`CONFIG_NETUTILS_IPERF=y`, resolve their Kconfig dependencies, rebuild, and
record the new firmware hash. The built-in NuttX application is `iperf`, not
`iperf3`; use a compatible server on the peer. DHCP/static IP configuration,
link-flap tests and no-cable startup are separate acceptance work.

Host parser regressions run without hardware:

```powershell
py -3.13 -m unittest discover -s tools/tests -v
```

使用 `desktop-v1-test` 配置构建 RTOS 测试固件：

```bash
cd nuttx
tools/configure.sh esp32p4-function-ev-board:desktop-v1-test
make CROSSDEV=/path/to/riscv32-esp-elf/bin/riscv32-esp-elf- -j16
```

在 Windows 上安装 `pyserial`后，依次执行：

```powershell
py -3.13 tools/p4_hil_acceptance.py --port COM7 --log evidence/rtos.log rtos
py -3.13 tools/p4_hil_acceptance.py --port COM7 --log evidence/camera-100.log camera-cycles --cycles 100
py -3.13 tools/p4_hil_acceptance.py --port COM7 --log evidence/soak-30m.log soak --minutes 30 --ping-host 192.168.1.1 --iperf-command "iperf -c 192.168.1.2 -t 1800"
py -3.13 tools/p4_hil_acceptance.py --port COM7 --log evidence/ethernet.log ethernet --ping-host 192.168.1.1 --iperf-command "iperf -c 192.168.1.2 -t 60"
py -3.13 tools/p4_hil_acceptance.py --port COM7 --log evidence/boot-100.log boot-cycles --cycles 100
```

`boot-cycles` 默认通过 CP210x EN 线做硬件复位，这不等于断电冷启动。要验收真正冷启动，必须通过 `--power-cycle-command` 指定继电器或可编程电源命令，且命令必须以无 shell 的参数形式可执行。

验收日志必须同时记录：contest commit、nuttx/apps/HAL SHA、defconfig SHA256、compiler version、`nuttx.bin` SHA256、PCB revision、silicon revision、eFuse block revision和 ROM revision。目前仓库中没有这些新场景的实板 PASS 日志，因此仍不应声称 100 次循环、30 分钟并发长稳、ostest/timer/SMP 或 Ethernet 吞吐已验收。
