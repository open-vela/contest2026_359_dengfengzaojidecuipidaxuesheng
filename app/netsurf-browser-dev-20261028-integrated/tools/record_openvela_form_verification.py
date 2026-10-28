#!/usr/bin/env python3
"""Record the bounded form candidate's exact host/hardware evidence."""
import hashlib,json,shutil
from pathlib import Path
root=Path(__file__).resolve().parents[1]
build=root/'build/openvela/form-input'; dest=root/'dist/candidates/form-input'
read=lambda name:(build/name).read_text(encoding='utf-8',errors='replace')
assert 'PASS: form text/Chinese/empty/password' in read('host.log')
assert 'four close/reopen cycles' in read('host.log')
assert 'PASS: real QPK finish:' in read('prompt-test.log')
assert 'Hash of data verified.' in read('flash-final.log')
assert 'FORM_CANCEL_CAPABILITY true' in read('board-final-open.log')
assert '"text":"编辑网页输入框"' in read('board-final-cancel.log')
assert 'click 取消 -> 1' in read('board-final-cancel.log')
assert '"text":"打开网页或搜索"' in read('board-final-reedit.log')
assert 'FORM_REQUEST 2 32 false' in read('board-final-reedit.log')
assert 'click 确定 -> 1' in read('board-final-confirm.log')
assert 'GET /fixture/form-result?q=P4test HTTP/1.1" 200' in read('server-errors.log')
assert 'form-result?q=P4test' in read('board-final-result-visible.log')
assert 'about:blank' in read('board-normal-state.log')
assert '"type":"keyboard"' not in read('board-normal-state.log')
candidate=json.loads((dest/'candidate.json').read_text())
audit=json.loads((dest/'host-audit/verification.json').read_text())
assert candidate['loader_sha256']==audit['sha256']
for name,digest in candidate['source_sha256'].items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,name
report={
    'feature':'form-input','firmware_sha256':candidate['loader_sha256'],
    'module_sha256':candidate['module_sha256'],
    'host_form_tests':True,'host_real_qpk_cancel_sanitized':True,
    'flash_hash_verified':True,'cold_boot_psram_both_core_checks':True,
    'native_module_executed':True,'real_lvgl_prompt_open_cancel_reopen_confirm':True,
    'get_form_received':'q=P4test','normal_entry_restored':True,
    'test_text_preinitialized':True,'physical_key_typing_verified':False,
    'physical_chinese_ime_verified':False,'textarea_supported':False,
    'arbitrary_modern_website_compatibility_verified':False,
    'firmware_bytes':audit['image_bytes'],'module_bytes':audit['module_bytes'],
    'rollback_app':'/sdcard/qpk/com.netsurf.browser/pre-input.js',
    'rollback_module':'/sdcard/qpk/com.netsurf.browser/native/pre-input.elf',
    'initial_attempt_rejected':'copied libapps.a selected legacy runtime; rebuilt archive and verified final ELF',
}
(dest/'board-verification.json').write_text(json.dumps(report,indent=2)+'\n')
candidate['status']='basic single-line form board checks passed; physical typing/IME and broader compatibility pending'
(dest/'candidate.json').write_text(json.dumps(candidate,indent=2)+'\n')
evidence=dest/'evidence';evidence.mkdir(exist_ok=True)
for name in ['host.log','prompt-test.log','flash-final.log','boot-final.log',
             'board-final-open.log','board-final-tap.log','board-final-cancel.log',
             'board-final-reedit.log','board-final-confirm.log','board-final-submit.log',
             'board-final-result-visible.log','board-normal-open.log','board-normal-state.log',
             'rejected-attempt-1.json','loader/loader-profile.json','loader/prompt-cancel.diff']:
    shutil.copy2(build/name,evidence/Path(name).name)
# Keep only fixture requests in the export, not a generic server log.
(evidence/'fixture-requests.log').write_text('\n'.join(line for line in read('server-errors.log').splitlines()
    if 'GET /fixture/' in line)+'\n',encoding='utf-8')
print('FORM_EVIDENCE_RECORDED',dest)
