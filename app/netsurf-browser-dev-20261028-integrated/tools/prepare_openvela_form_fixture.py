#!/usr/bin/env python3
"""Prepare a LAN fixture and temporary App entry for repeatable board input tests.

The real keyboard is shown; only its initial test text is supplied here.
Confirmation/cancellation still go through the real LVGL prompt buttons.
Restore the packaged app.js after testing. No storage writes or passwords.
"""
import argparse, json, shutil
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--candidate',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--base-url',required=True)
a=p.parse_args()
package=a.candidate/'app/com.netsurf.browser'
dest=a.output;dest.mkdir(parents=True,exist_ok=True)
shutil.copytree(package,dest/'com.netsurf.browser',dirs_exist_ok=True)
prefix='''// Temporary form fixture: constant test text, no credentials.
const fixtureInput = prompt.input;
prompt.input = function (options, callback) {
  if (options.title === '编辑网页输入框') options.value = 'P4test';
  console.log('FORM_PROMPT_OPEN', !!options.password);
  try { return fixtureInput(options, callback); }
  catch (error) { console.log('FORM_PROMPT_ERROR', String(error)); throw error; }
};
system.storage.get = function () { return FIXTURE_URL; };
console.log('FORM_CANCEL_CAPABILITY', prompt.cancelNotification === true);
var formProbe = setInterval(function () {
  if (!system.netsurf) return;
  var nativeTakeInput = system.netsurf.takeInput;
  console.log('FORM_NATIVE_INPUT_API', typeof nativeTakeInput);
  system.netsurf.takeInput = function () {
    var request = nativeTakeInput();
    if (request) console.log('FORM_REQUEST', request.id, request.maxLength, !!request.password);
    return request;
  };
  clearInterval(formProbe);
}, 100);
'''.replace('FIXTURE_URL',json.dumps(a.base_url.rstrip('/')+'/fixture/form.html'))
(dest/'t').write_text(prefix+ (package/'app.js').read_text(encoding='utf-8'),encoding='utf-8')
print('FORM_FIXTURE_READY',dest)
