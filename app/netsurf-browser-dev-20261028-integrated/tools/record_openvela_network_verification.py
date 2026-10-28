#!/usr/bin/env python3
"""Record exact candidate hashes and bounded direct-network board evidence."""
import hashlib
import json
import shutil
from pathlib import Path

root = Path(__file__).resolve().parents[1]
build = root / 'build/openvela/network-connectivity'
dest = root / 'dist/candidates/network-connectivity'
read = lambda name: (build / name).read_text(encoding='utf-8', errors='replace')
candidate = json.loads((dest / 'candidate.json').read_text())
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
for name, expected in candidate['source_sha256'].items():
    assert digest(root / name) == expected, name
    assert digest(dest / 'source' / name) == expected, name
assert digest(dest / 'app/com.netsurf.browser/native/browser.elf') == candidate['module_sha256']
assert 'PASS: transient pre-send TLS retry bounded to one' in read('transport.log')
assert 'PASS: timeout page refresh uses existing core retry handler' in read('host-final.log')
trace = read('board-final-continuous.log')
assert 'attempt=2 result=0 status=200 wire=6996 decoded=38337' in trace
for size in (10624, 31227, 152290, 97276):
    assert 'decoded=' + str(size) in trace
assert 'api.whichbrowser.net attempt=2 result=-111' in trace
assert '"loading":false,"errorPage":true' in read('board-recovery-error.log')
recovery = read('board-recovery-refresh.log')
assert '网页加载失败：Connection timed' in recovery
assert 'click 刷新 -> 1' in recovery
assert '"title":"P4 next page"' in recovery
assert '"loading":false,"errorPage":false' in recovery
assert 'GET /fixture/next.html HTTP/1.1" 200' in read('recovery-server-errors.log')
assert '5473 /sdcard/qpk/com.netsurf.browser/normal.js' in read('board-normal-download.log')
normal = read('board-normal-state.log')
assert 'about:blank' in normal
assert '"type":"keyboard"' not in normal
assert 'WEBSITE_STATE' not in normal
report = {
    'feature': 'network-connectivity',
    'firmware_sha256': candidate['loader_sha256'],
    'module_sha256': candidate['module_sha256'],
    'direct_network': True, 'proxy_used': False,
    'tls_certificate_and_hostname_verification': True,
    'host_transport_sanitized': True, 'host_real_core_refresh_passed': True,
    'board_html5test_main_html_css_and_three_scripts_http_200': True,
    'board_transient_tcp_timeout_recovered_on_second_attempt': True,
    'board_error_page_refresh_after_endpoint_recovery_passed': True,
    'normal_entry_restored': True,
    'html5test_score_obtained': False,
    'html5test_remaining_dependency': 'api.whichbrowser.net: two direct TCP attempts refused (ECONNREFUSED)',
    'arbitrary_modern_website_compatibility_verified': False,
    'remaining_limits': [
        'Direct Internet route remains intermittent; this candidate does not change router/ISP routing.',
        'Plain HTTP blocking TCP connect can exceed the configured socket I/O timeout.',
        'Optional favicon TLS handshakes returned EPROTO/net-send failure on some requests.',
        'No HTML5test score or broad HTML5 compatibility claim.',
    ],
    'rollback_module': '/sdcard/qpk/com.netsurf.browser/native/pre-net.elf',
}
(dest / 'board-verification.json').write_text(json.dumps(report, indent=2) + '\n')
candidate['status'] = 'bounded direct HTTPS and error-page recovery verified on P4; HTML5test score and broad compatibility pending'
(dest / 'candidate.json').write_text(json.dumps(candidate, indent=2) + '\n')
audit_path = dest / 'host-audit/verification.json'
audit = json.loads(audit_path.read_text())
audit.update(board_tested=True, module_executed_on_p4=True, deployable=False,
             status=candidate['status'], board_acceptance='../board-verification.json')
audit_path.write_text(json.dumps(audit, indent=2) + '\n')
evidence = dest / 'evidence'
evidence.mkdir(exist_ok=True)
for name in ['transport.log', 'host-final.log', 'build-final.log', 'frontend.log',
             'module.log', 'audit.log', 'package.log', 'boot-fixed.log',
             'board-final-continuous.log', 'board-recovery-install.log',
             'board-recovery-failed.log', 'board-recovery-error.log',
             'board-recovery-refresh.log', 'board-normal-download.log',
             'board-normal-open.log', 'board-normal-state.log']:
    shutil.copy2(build / name, evidence / name)
for name in ['host-direct-network.json', 'host-lan-direct-network.json',
             'host-whichbrowser-network.json', 'public-dns.json', 'board-network.log']:
    shutil.copy2(root / 'build/openvela/html5test' / name, evidence / name)
(evidence / 'recovery-requests.log').write_text('\n'.join(
    line for line in read('recovery-server-errors.log').splitlines()
    if 'GET /fixture/' in line) + '\n', encoding='utf-8')
(dest / 'VERIFICATION.md').write_text('''# ESP32-P4 独立浏览器直连候选验证

复用现有 52 个通用补丁和已验证核心库，未修改核心归档；本候选修改 OpenVela 网络适配、错误页刷新和 App 状态显示。没有配置代理。

## 修复

- HTTPS 保留初始 30 秒连接预算，使用既有连接器的 60 秒无进展超时，避免正常传输因总耗时被中断。
- 仅 GET 在未发送 HTTP、未收到响应的 TCP/握手瞬态错误上重试一次。POST、证书错误、取消和部分响应不重放；证书和主机名校验保持启用。
- 超时/获取失败页面的“刷新”调用既有 NetSurf 重试处理器，重新请求原始网址；App 显示失败标题。

## 验证结果

- 宿主真实 NetSurf/LVGL/QuickJS 回归通过；生产网络适配 ASan/UBSan 测试通过，覆盖重试边界、进展超时、压缩、取消和证书错误。
- ESP 直接访问 https://html5test.co/：主 HTML、CSS、base.js、engine.js、data.js 均 HTTP 200。CSS 首次 TCP 超时，第二次自动重试成功。
- 实板在本地目的端不可达时出现超时错误页；恢复服务后点击真实 LVGL“刷新”按钮，原 URL 返回 HTTP 200，标题恢复为 P4 next page。
- 已恢复包内正式 app.js（5473 字节），打开 about:blank，无临时网址覆盖或状态监控。

## 未通过与限制

HTML5test 的 api.whichbrowser.net 依赖两次直连均被拒绝，网页检测脚本等待该依赖，因此没有得到分数。未修改网站或伪造功能检测。直连链路仍有间歇超时；明文 HTTP 的底层阻塞连接可能超过读写超时，部分可选图标 TLS 握手也失败。不能声称所有网络故障已解决或任意现代网站兼容。

## 产物与回退

本目录 app/NetSurfBrowser-0.5.0.zip 是开发候选，版本字段沿用 0.5.0，以 candidate.json 和 board-verification.json 的哈希区分；须配套 form-input 候选内 SHA-256 为 2c7790e2156dd46ee144894ec9d290531e71925b9511df0689eb28377b0aa08a 的装载固件。不要混用旧装载固件。板上保留 native/pre-net.elf 供回退。冻结稳定归档和用户数据未改动；原始 Flash 备份不导出。
''', encoding='utf-8')
print('NETWORK_EVIDENCE_RECORDED', dest)
