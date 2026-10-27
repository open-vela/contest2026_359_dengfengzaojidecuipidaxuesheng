from pathlib import Path
import json,re,hashlib
r=Path(__file__).resolve().parent;raw=r/'fixed-pure-01.log';result=json.loads((r/'fixed-pure-01.json').read_text())
assert not result['passed'] and result['error'].startswith('AssertionError: c6ble timers')
rows=result['cycles'];assert len(rows)==6
pattern=r'NPL_CALLOUT [^=\r\n]+=(\d+) [^=\r\n]+=(\d+) [^=\r\n]+=(\d+) [^=\r\n]+=(\d+) [^=\r\n]+=(\d+)'
stats=[dict(zip(['expirations','dispatched','coalesced','pending','highwater'],map(int,m.groups()))) for m in re.finditer(pattern,raw.read_text(errors='replace'))]
assert len(stats)==12
for i,row in enumerate(rows):
 assert not row['unexpected_epoch_change'] and row['final']['connected']==row['final']['encrypted']==row['final']['notify']==1 and not row['final']['store_error']
 assert stats[i*2]==row['timers_before']
 if 'timers_after' in row:assert stats[i*2+1]==row['timers_after']
 for value in stats[i*2:i*2+2]:assert value['expirations']==value['dispatched']+value['coalesced']+value['pending']
 assert stats[i*2+1]['dispatched']>stats[i*2]['dispatched']
sidecar={'reviewed_cycles':6,'hid_connection_checks_passed':True,'numeric_timer_accounting_passed':True,'raw_log_sha256':hashlib.sha256(raw.read_bytes()).hexdigest(),'original_runner_result':'failed parsing the label expiratons (missing i); original JSON retained unchanged','numeric_field_order':'immutable C19 CLI format, every numeric digit retained; exact conservation and before/after comparison checked','last_timer_snapshot':stats[-1],'radio_or_firmware_failure_observed':False}
(r/'fixed-pure-01-REVIEW.json').write_text(json.dumps(sidecar,indent=2)+'\n')
print('PURE01_REVIEW: six stable HID cycles, all 12 original numeric snapshots conserved; console-label parser failure retained')
