#!/usr/bin/env python3
"""Record the final P4 direct-connect candidate and its exact evidence."""
import argparse,hashlib,json,re,shutil
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--score',type=int,help='only after visually reading the saved real-board framebuffer')
p.add_argument('--maximum',type=int)
a=p.parse_args()
if (a.score is None)!=(a.maximum is None):p.error('score and maximum must be supplied together')
if a.score is not None and not 0<=a.score<=a.maximum<=1000:p.error('invalid observed score')
root=Path(__file__).resolve().parents[1]
build=root/'build/openvela/direct-connect-retry';dest=root/'dist/candidates/direct-connect-retry'
digest=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
read=lambda n:(build/n).read_text(encoding='utf-8',errors='replace')
c=json.loads((dest/'candidate.json').read_text())
assert not (dest/'build-in-progress.json').exists()
for name,want in c['source_sha256'].items():
    assert digest(root/name)==want,name
    assert digest(dest/'source'/name)==want,name
assert digest(dest/'host-audit/Vela-NativeLoader-P4-UNTESTED.bin')==c['loader_sha256']
assert digest(dest/'app/com.netsurf.browser/native/browser.elf')==c['module_sha256']
assert 'Hash of data verified.' in read('flash-phase.log')
assert 'cpu=0 pool_end=48d10000' in read('boot-phase.log')
assert 'cpu=1 pool_end=48d10000' in read('boot-phase.log')
assert 'PASS: real bounded TCP connector' in read('tcp-test.log')
assert 'PASS: real bounded TLS wrapper' in read('tcp-test.log')
assert 'PASS: expired handshake budget reproduces legacy first-write timeout' in read('transport.log')
site=read('board-html5test-phase.log')
assert '[browser-loader] ready' in site
resource_sizes={'html':10624,'css':38337,'base.js':31227,'engine.js':152290,'data.js':97276}
for name,size in resource_sizes.items():assert 'decoded='+str(size) in site,name
assert re.search(r'host=api\.whichbrowser\.net attempt=\d+ result=0 status=200',site)
assert 'attempt=2 result=0 status=200' in site
assert '"loading":false,"errorPage":false' in site
assert (build/'board-html5test.png').is_file()
cancel=read('board-stall-cancel.log')
assert 'click 停止 -> 1' in cancel
assert 'result=-125 status=200' in cancel
assert '5473 /sdcard/qpk/com.netsurf.browser/normal.js' in read('board-normal-download.log')
normal=read('board-normal-state.log')
assert 'about:blank' in normal and 'WEBSITE_STATE' not in normal
assert '"type":"keyboard"' not in normal
report={'feature':'direct-connect-retry','firmware_sha256':c['loader_sha256'],
    'module_sha256':c['module_sha256'],'direct_network':True,'proxy_configured':False,
    'dns_or_origin_ip_overridden':False,'certificate_and_hostname_verification':True,
    'host_transport_sanitized':True,'production_tcp_and_phase_wrapper_sanitized':True,
    'cold_boot_both_core_execution_permissions_passed':True,'module_executed_on_p4':True,
    'board_main_html_css_and_three_scripts_http_200':True,'board_whichbrowser_http_200':True,
    'board_tcp_timeout_recovered_by_retry':True,
    'board_send_stage_timeout_seen':bool(re.search(r'result=-110[^\n]*stage=send',site)),
    'board_stalled_response_stop_passed':True,'normal_entry_restored':True,
    'html5test_score_obtained':a.score is not None,'html5test_score':a.score,'html5test_maximum':a.maximum,
    'score_source':'real LVGL framebuffer readback' if a.score is not None else None,
    'physical_display_or_finger_touch_observed':False,'arbitrary_modern_website_compatibility_verified':False,
    'limits':['System DNS resolution remains synchronous.',
        'Internet route can still drop connections; retries are bounded.',
        'No claim that all modern HTML5 APIs or websites are supported.'],
    'rollback_module':'/sdcard/qpk/com.netsurf.browser/native/pre-time.elf'}
(dest/'board-verification.json').write_text(json.dumps(report,indent=2)+'\n')
c['status']='P4 direct main resources and dependency HTTP 200; stall cancellation and normal entry verified; broader compatibility pending'
helpers=['tools/openvela_tcp_budget.py','tools/prepare_openvela_network_loader.py',
    'tools/check_openvela.py','tools/build_openvela_module.py','tools/verify_openvela_native.py',
    'tools/package_quickapp.py','tools/test_openvela_tcp_budget.py','tools/test_openvela_transport.py',
    'tools/openvela_board.py','tools/record_openvela_direct_verification.py',
    'tools/decode_openvela_frame.py','tests/openvela/host_tls.c','tests/openvela/transport.c']
c['build_and_test_source_sha256']={n:digest(root/n) for n in helpers}
for n in helpers:
    copy=dest/'source'/n;copy.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(root/n,copy)
(dest/'candidate.json').write_text(json.dumps(c,indent=2)+'\n')
ap=dest/'host-audit/verification.json';audit=json.loads(ap.read_text())
audit.update(board_tested=True,module_executed_on_p4=True,deployable=False,status=c['status'],
    board_acceptance='../board-verification.json')
ap.write_text(json.dumps(audit,indent=2)+'\n')
e=dest/'evidence';e.mkdir(exist_ok=True)
for n in ['transport.log','tcp-test.log','build-phase.log','frontend.log','module.log','audit.log',
    'package.log','network-loader-profile.json','tcp-budget.diff','flash-phase.log','boot-phase.log',
    'board-download.log','board-install.log','board-html5test-phase.log','board-html5test.png',
    'board-stall-download.log','board-stall-cancel.log','board-normal-download.log',
    'board-normal-open.log','board-normal-state.log','board-frame.log','board-frame-repair.log']:
    shutil.copy2(build/n,e/n)
for n in ['host-main-direct.json','host-dependency-direct.json','board-connect-cancel-final.log',
          'board-connect-deadline.log','board-recovery.log']:
    shutil.copy2(root/'build/openvela/direct-timeouts'/n,e/('earlier-'+n))
score_note=(f'屏幕显示 {a.score}/{a.maximum}；这只表示该测试的检测结果，不等于所有功能可用。' if a.score is not None
    else '未取得可确认的分数；主资源和检测依赖的 HTTP 200 不能替代评分或完整功能验收。')
(dest/'VERIFICATION.md').write_text(f'''# ESP32-P4 独立浏览器直连修复

最终固件 SHA-256：{c['loader_sha256']}。模块 SHA-256：{c['module_sha256']}。
App 和本目录 host-audit 内固件须配套，不能搭配缺少新导出的旧固件。App 版本字段仍为开发候选 0.5.0，以哈希区分。

## 修复与原因

- 明文 HTTP 使用现有非阻塞 webclient，等待时检查取消，截止/取消均在网络线程清理自身套接字。第一轮实板 TCP 截止约 15010 毫秒，恢复服务后刷新可重新加载。
- 浏览器专用 TLS 接口为 TCP 连接设置 5 秒预算，TLS 初始预算 15 秒；可重试的 pre-HTTP GET 最多三次，受 30 秒总连接预算限制，并有可取消退避。
- 实板对照中发现 TLS 握手完成后首写仍使用耗尽的握手预算，产生 stage=send 超时。最终接口在握手完成时切换到新的 60 秒 I/O 空闲预算，后续进展继续续期。
- 原 TLS 请求布局、接口、旧模块导出、系统运行时及配置保留；仅对显式选用新接口的浏览器生效。POST、证书错误、协议错误及响应阶段不重放，证书/主机名校验保持开启。同步 DNS 仍可能延迟取消。

## 验证

实际生产 TCP 函数及包装器通过 ASan/UBSan：SYN 悬挂、截止、取消、旧预算保持、无描述符泄漏、握手/首写预算交接。完整 HTTP/TLS 适配回归通过压缩、流式进展、证书错误、取消和重试边界；端到端回归可先复现旧首写超时，再用新接口取得 HTTP 200。

最终实板 https://html5test.co/：主 HTML、CSS、base.js、engine.js、data.js 及 api.whichbrowser.net 均 HTTP 200；TCP 超时由自动重试恢复。{score_note}

实板还验证了正文响应停滞时点击 App“停止”，正式入口已恢复为包内 app.js，打开 about:blank。UI 操作通过真实 LVGL 按钮事件，画面由设备 framebuffer 读取，不等于实体屏幕/手指触摸观察。现场截图见 evidence/board-html5test.png。

## 交付与边界

独立 App：app/NetSurfBrowser-0.5.0.zip。固件：host-audit/Vela-NativeLoader-P4-UNTESTED.bin（工具沿用候选命名，本轮有界实板结果见 board-verification.json）。source/ 保存对应 B/C 源码及关键构建/测试工具；evidence/ 保存最终日志和明确标注的早期对照。core archive 及原有 52 个通用补丁复用，冻结稳定版本未改。

本轮不使用代理、不硬编码域名 IP、不改网站或伪造检测 API。公网仍可丢包，重试有上限；不能据此声称任意现代网站兼容。原始 Flash/用户配置备份没有导出。板上 native/pre-time.elf、pre-time.js 及本地早期固件保留供回退。
''',encoding='utf-8')
print('DIRECT_CONNECT_EVIDENCE_RECORDED',dest)
