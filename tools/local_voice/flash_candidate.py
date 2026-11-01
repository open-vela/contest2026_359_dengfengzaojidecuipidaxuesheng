#!/usr/bin/env python3
"""Flash one audited app image; preserve data and a verified running rollback."""
import argparse,hashlib,io,json,re,shutil,subprocess,sys,time
from pathlib import Path
import serial
from serial.tools import list_ports
from esptool.reset import HardReset
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--candidate',type=Path,required=True);p.add_argument('--rollback',type=Path,required=True)
 p.add_argument('--output',type=Path,required=True);p.add_argument('--execute',action='store_true');p.add_argument('--evidence-root',type=Path);a=p.parse_args();assert a.execute
 candidate=a.candidate.resolve();image=candidate/'firmware/nuttx.bin';meta=json.loads((candidate/'firmware/BUILD-METADATA.json').read_text())
 assert sha(image)==meta['sha256']['nuttx.bin']
 assert meta['flash_offset']=='0x2000' and meta['data_offset']=='0xc00000' and meta['data_size']=='0x400000'
 assert image.stat().st_size+0x2000<=0xc00000 and int(meta['ebss'],16)<=0x4ffaefc0
 evidence_root=a.evidence_root.resolve() if a.evidence_root else candidate.parent
 for name in ('ELF-AUDIT.json','XIP-REVIEW.json'):
  v=json.loads((candidate/name).read_text());assert v['passed'] and v['image_sha256']==sha(image)
 reset=json.loads((candidate/'RESET-ELF-AUDIT.json').read_text());assert reset['passed'] and reset['elf_sha256']==sha(candidate/'firmware/nuttx.elf')
 for name,digest in meta['source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
 if meta.get('local_tts_clauses',{}).get('included'):
  cg=json.loads((candidate/'TTS-CLAUSE-VERIFICATION.json').read_text())
  assert cg['passed'] and cg['image_sha256']==sha(image) and cg['source_sha256']==meta['source_sha256']
  assert cg['default_enabled'] and cg['host_independent_parts'] and cg['host_first_play_before_next_synth'] and cg['host_audio_storage_cancel_cleanup'] and cg['legacy_complete_wave_exact']
 if meta.get('local_tts_quantization',{}).get('included'):
  qg=json.loads((candidate/'TTS-QUANT-VERIFICATION.json').read_text())
  assert qg['passed'] and qg['image_sha256']==sha(image) and qg['source_sha256']==meta['source_sha256']
  assert qg['host_all_waves_exact'] and qg['default_enabled'] and qg['guard_reference_cases']==1200272 and qg['target_common_path_no_int_to_float']
 if meta.get('local_tts_int8',{}).get('enabled'):
  i8=json.loads((candidate/'TTS-INT8-VERIFICATION.json').read_text())
  assert i8['passed'] and i8['image_sha256']==sha(image) and i8['source_sha256']==meta['source_sha256']
  assert i8['benchmark_only'] and not i8['default_enabled'] and not i8['quality_accepted']
  assert i8['actual_s8_mac_and_context_linked'] and i8['host_worker_bench_only_no_audio'] and i8['host_parallel_cleanup_faults']
  assert meta['local_tts_int8']['benchmark_only'] and not meta['local_tts_int8']['default_enabled']
 if meta.get('local_tts_int8',{}).get('full_row_reduction_diagnostic'):
  rg=json.loads((candidate/'TTS-REDUCE-VERIFICATION.json').read_text())
  assert rg['passed'] and rg['image_sha256']==sha(image) and rg['source_sha256']==meta['source_sha256']
  assert rg['benchmark_only'] and not rg['default_enabled'] and rg['host_disabled_exact'] and rg['host_integer_reference']
  assert rg['full_s8_mac_linked'] and rg['max_abs_integer_sum']==37486592 and rg['host_quality_passed']
  if meta['local_tts_int8'].get('affine_input_unroll'):
   assert rg['host_input4_exact'] and rg['host_input4_edge_reference'] and meta['local_tts_int8']['affine_input_unroll']==4
  if meta['local_tts_int8'].get('integer_input_rounding'):
   assert rg['host_integer_rounding_exact'] and rg['target_common_path_no_int_to_float'] and rg['guard_reference_cases']==1200608
 if meta.get('local_tts_pie',{}).get('included'):
  gate=json.loads((candidate/'TTS-PIE-VERIFICATION.json').read_text())
  assert gate['passed'] and gate['image_sha256']==sha(image)
  assert gate['actual_pie_mac_linked_and_called'] and gate['no_audio_worker_gate']
  if meta.get('local_sha_hw',{}).get('enabled'):
   hw=json.loads((candidate/'SHA-HW-VERIFICATION.json').read_text())
   assert hw['passed'] and hw['image_sha256']==sha(image)
   if gate['kernel_and_config_unchanged']:
    assert hw['config_delta']=={} and not gate.get('allowed_sha_only_config_change',False)
   else:
    assert hw['config_delta']=={'CONFIG_ESPRESSIF_SHA_ACCELERATOR':['n','y']}
    assert gate['allowed_sha_only_config_change']
   assert hw['kernel_transform_sources_unchanged'] and hw['reviewed_driver_sources_unmodified']
   assert hw['linked_driver_and_hal'] and hw['request_selftest_required'] and hw['host_api_mock_faults']
   assert hw['candidate_config_sha256']==sha(candidate/'firmware/resolved.config')
   assert hw['baseline_image_sha256']==gate['rollback_sha256']
   assert hw['source_sha256']==meta['local_sha_hw']['sources']
   for name,digest in hw['source_sha256'].items():assert sha(candidate/'kernel-sha-review'/name)==digest,name
  else:assert gate['kernel_and_config_unchanged']
  if meta.get('local_tts_parallel',{}).get('enabled'):
   par=json.loads((candidate/'PARALLEL-TTS-VERIFICATION.json').read_text())
   assert par['passed'] and par['image_sha256']==sha(image) and par['kernel_and_config_unchanged']
   assert par['allocation_io_outside_parallel_leaves'] and par['host_waveforms_and_retirement_faults'] and par['volatile_serial_control']
   assert par['stack_charge_bytes']==131072 and par['helper_stack_bytes']==65536 and par['cpu_affinity_linked']
   assert par['source_sha256']==meta['source_sha256']
  for name,digest in gate['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in gate['evidence_sha256'].items():assert sha(candidate.parent/name)==digest,name
 if meta.get('local_tts_fma',{}).get('enabled'):
  fg=json.loads((candidate/'FMA-TTS-VERIFICATION.json').read_text())
  assert fg['passed'] and fg['image_sha256']==sha(image) and not meta['local_tts_fma']['default_enabled']
  assert fg['explicit_fmadd_linked'] and fg['default_off_exact'] and fg['runtime_switch_idle_only']
  assert fg['host_boundary_cancel_rejection_passed'] and fg['source_sha256']==meta['source_sha256']
  assert len(fg['quality_cases'])==10 and all(x['snr_db']>=70 and x['cosine']>=0.9999999 for x in fg['quality_cases'])
 if meta.get('local_tts_input',{}).get('enabled'):
  inp=json.loads((candidate/'TTS-INPUT-VERIFICATION.json').read_text())
  assert inp['passed'] and inp['image_sha256']==sha(image) and inp['source_sha256']==meta['source_sha256']
  assert inp['ten_complete_waveforms_exact'] and inp['allocation_io_outside_leaves'] and inp['fault_cancel_rounding_tests']
 if meta.get('local_tts_windows',{}).get('enabled'):
  win=json.loads((candidate/'TTS-WINDOW-VERIFICATION.json').read_text())
  assert win['passed'] and win['image_sha256']==sha(image) and win['source_sha256']==meta['source_sha256']
  assert win['live_allocation_witness'] and win['ten_streamed_exact'] and win['faults_no_io_replay']
 if meta.get('local_tts_io',{}).get('q8_bulk_rows'):
  rows=json.loads((candidate/'TTS-IO-VERIFICATION.json').read_text())
  assert rows['passed'] and rows['image_sha256']==sha(image) and rows['source_sha256']==meta['source_sha256']
  assert rows['streamed_ten_exact'] and rows['in_place_canary_io_cancel'] and rows['service_busy_cancel_faults']
 for folder in ('engine-gates','llm-final-gates','tts-final-gates','frontend-gates','wake-adapter-gates'):
  assert json.loads((evidence_root/folder/'VERIFICATION.json').read_text())['passed']
 assert json.loads((evidence_root/'service-gates/VERIFICATION.json').read_text())['status']=='passed'
 if candidate.name in ('candidate2','candidate3'):
  assert json.loads((evidence_root/(candidate.name+'-engine-gates/VERIFICATION.json')).read_text())['passed']
  assert json.loads((evidence_root/(candidate.name+'-service-gates/VERIFICATION.json')).read_text())['status']=='passed'
  hot=json.loads((candidate/'HOTLOOP-AUDIT.json').read_text());assert hot['passed'] and hot['image_sha256']==sha(image)
 if candidate.name=='candidate3':
  assert json.loads((evidence_root/'candidate3-playback-gates/VERIFICATION.json').read_text())['status']=='passed'
  assert json.loads((evidence_root/'candidate3-engine-gates/GENERATION-EQUIVALENCE.json').read_text())['passed']
 if candidate.name=='candidate4':
  assert json.loads((evidence_root/'candidate3-engine-gates/VERIFICATION.json').read_text())['passed']
  for folder in ('candidate4-service-gates','candidate4-session-gates'):
   assert json.loads((evidence_root/folder/'VERIFICATION.json').read_text())['status']=='passed'
  for name in ('MODEL-EQUIVALENCE.json','RAM-PCM-VERIFICATION.json'):
   assert json.loads((candidate/name).read_text())['passed']
  hot=json.loads((candidate/'HOTLOOP-AUDIT.json').read_text());assert hot['passed'] and hot['image_sha256']==sha(image)
 if candidate.name in ('candidate5','candidate6','candidate7'):
  evidence=json.loads((candidate/'OPTIMIZATION-VERIFICATION.json').read_text())
  assert evidence['passed'] and evidence['image_sha256']==sha(image)
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(evidence_root/name)==digest,name
  gates=evidence_root/'optimization5'
  assert json.loads((gates/'fast-gates/VERIFICATION.json').read_text())['passed']
  ten=json.loads((gates/'ten-fast-exact/VERIFICATION.json').read_text());assert ten['passed'] and len(ten['cases'])==10
  assert all(row['bit_exact_pcm'] for row in ten['cases'])
  for folder in ('service-gates','session-gates'):assert json.loads((gates/folder/'VERIFICATION.json').read_text())['status']=='passed'
  ui_log=gates/'ui-review.log' if candidate.name=='candidate5' else evidence_root/'optimization6/ui-final.log'
  assert 'LOCAL_VOICE_UI_OK' in ui_log.read_text(encoding='utf-8')
  if candidate.name in ('candidate6','candidate7'):
   previous=json.loads((evidence_root/'device-tests5/BOARD-TESTS.json').read_text())
   assert previous['passed'] and previous['firmware_sha256']=='044cbcb5b2e1aa5fc058df5c64a977e9cd3fcdc788420727e6cb2e513b6e04c5'
  if candidate.name=='candidate7':
   assert evidence['foreground_service_prefix_unchanged']
   assert json.loads((evidence_root/'optimization7/wake-alignment-gates/VERIFICATION.json').read_text())['weight_start_skews']==16
   assert json.loads((evidence_root/'optimization7/service-gates/VERIFICATION.json').read_text())['status']=='passed'
  hot=json.loads((candidate/'HOTLOOP-AUDIT.json').read_text());assert hot['passed'] and hot['image_sha256']==sha(image) and hot['tts_kernels']
 assert json.loads((evidence_root/'device-install/DEPLOYMENT.json').read_text())['published']
 if candidate.name in ('candidate8','candidate9'):
  evidence=json.loads((candidate/'ASR-EVALUATION-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert meta['local_asr']['evaluation_enabled'] and not meta['local_asr']['microphone_ready']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(evidence_root/name)==digest,name
  assert evidence['foreground_inference_body_unchanged'] and evidence['kernel_config_identical_to_candidate7']
  assert json.loads((evidence_root/'device-asr-install8/DEPLOYMENT.json').read_text())['published']
  if candidate.name=='candidate9':assert json.loads((evidence_root/'asr-io-optimization1/VERIFICATION.json').read_text())['passed']
 stable=a.rollback.resolve();assert sha(stable) in ('1656244d09491a0ae947b8ef923c5527f29c06a2ef055e128f4cb3f8634b1bcd','142b40a00ff5f631e52271c80877daaa252dfdeab60f25cd267a8ac241fc9b0e','d6a098c4b27ff35cf159eb8d2fefec0528c83914e294e673de8ec8cde82add4f','41ec5f25355ada8e2f3e83e56640c103f74186a5ecb6e9e2be260e5503fe5af5','53be36bf54009b7f7aab085203f0f01e0e41ed96e111ebae041d73cd1010bd16','552f18a55a9e12387059a6eeeae3a8eb7b2e92a6d566c01888a56ee135ed4e09','9f83cbaa400013e62b4934fb7a61a796a6f605bbf5fc3ca973dcad71ad578ef8','d618a61cd4f5d7e798e0847233f2be162077872f2df84695a2d9692c0ae66f0f','bed5d672457dcec9335b1fed1fa5763523eba552e3bfcd68c42b05dac6fae85e','1a95421cb3406751f84ffaff4491a3426a4b99cf18f2466dbaf524ad7cbc4e26','9cab5455c4cb2b5f0fcc1ed09da63e4343d8d1b7fea77a54b52cffa8b9f377f7','fc010d3a48e21702089d04ea8ef5255ca7b22921f121e67e0d56d5fca81bd0db','4d75d42b98c9411b3b8b989ead4d1a8e3465e9dd70c3d94b28474cef210d7e2b','8c0a5af123f5a6186d107ddde6f189147e5c6468390b15a597285f6c0896158d','ca9a7902853a4e885e514f46e2f3851151453d7db5ae79fc46cbaa25db15fc26','0eba508d9f9e6f115882f872039680b4871c9bc90c8519b31ec99c6efc4b396e','d11dd5952b50cce01605895cc303d414659db311b6eebe5529a9107975a47532','87629eb8b0630652994d00acd3bb85d649a7e69e4225578d6d3ad85defd60e61','7542a0a0b044c2d2bf9f79a955b70d238344d15f75334f962e396e52f38e1fa9','8f5df2a2c4a5a8e7af30c0825fe107d39fe2ee6e763fefb3ac4a09a0a5f95200','a1608d185c2250e2efd4f5de26fe390ca3bbb00205e5b29d57ba8e8b3b7054af','80ce963097993e560d233c2ea25b0d883c7cae9f5c186b32322792d6bb04c814','8b100d2f148092c7cdb9ded6ec26c108b4937231175adfeba6f36da579bac5d4','7f2ff1899157a13cbd7aa570645d0fcae0aa069e3456d6c2d77e114c4fa68e6d','14441e9a1f9998b362e6c43d31f9bda07f8336905548fa7ddc9686f93884e5c3','113dcbe5cc17fb014005ffee37e53b184e1ed2d4c7ff3f376094c634f0a4233b','106d264380335933766b3441d6e0fdf857a35dd170e150e2780cee8dad9d0f38','170708f5bb6e0a57e56890bf9fb0911fb3b218b239d4bb6f40a0152234b6fee8','19fc210f0130d2330a199dad2026b3d8b8ab93950455d4287c78e26aa1c63f02','7a812bf803ec0ad60119d0e0066e64f3c1c6b90d49d32eb0cfa00f551b44973d','047be02f7f3bc751a84488444cd37dc5bce9db40b9d40358f5751399e6840dd4','43f660eb114cf4f79a7cd91e49c5001a49dad31c0e44fbdb272a19f9b5d04293','e32559039e1389c339ffe252edf3335c5ea8a3ca1f89c4f454dc46dfa4c9a80d','c3633017ff92da16d053801cc5dd436917c10b1b2ff7a14e8f73d17c9423b019','555c0d6a868b1a926b3a4c41bf0e6e0f002cd22502fb6bb3ca1acddc19ceca7b','1bfc7ea11342e8e43915f91092caa882372746c226692deb68775675d6307128','8fafeaa219f32b36b01841da14f49a37eda89e1bef02d25f8e5b10373839950d','1bce73f9814db758136a01b7a8a0335f82fe1d09319614b15f9e51bd20550da8','044cbcb5b2e1aa5fc058df5c64a977e9cd3fcdc788420727e6cb2e513b6e04c5','603846694a305ee970bf213cd584244af3fe8bd7c0c5731f23e7372b90018f88','6ce4d96bb560a5788337ca16711748a88e7674cbb1b34dcf26a4290c6aa38006','d9cad08f5d2827df91e62dacb99d389af04acd8ddc48158a36ea925b755c48d6','ab12e243cdc2616a9c4f99362fcebfb6211a4eac846727925d13c4745d8920d5','816cc314ec69bae717d5c9a0cd1ed61f3aff583cde4060e4c261eb415cb8b819','521479ceb44687a07ab2e70a9823922df6098cff95475fd13b28ab617151ccc5','b0eeb9435b7784532f8e444c9572c960c61ae467b0bbf4cc66ee7740f44bd238','a0c85c9f9e69a64847d2b61801cf46336953e4b9de1c8cd6460e1ada8084eae6','82a9e9022d2fa0f4f6ab07cbd9b78c3b991eae35d50c7c064b41845c4fcb8733')
 if meta.get('local_tts_pie',{}).get('included'):assert sha(stable)==gate['rollback_sha256']
 if candidate.name in ('candidate10','candidate11'):
  evidence=json.loads((candidate/'MICROPHONE-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert evidence['foreground_inference_body_unchanged'] and evidence['kernel_config_identical_to_candidate9']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(evidence_root/name)==digest,name
  assert meta['local_asr']['microphone_enabled'] and sha(stable)=='ab12e243cdc2616a9c4f99362fcebfb6211a4eac846727925d13c4745d8920d5'
 if candidate.name=='candidate12':
  evidence=json.loads((candidate/'RESOURCE-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert evidence['check_block_is_only_service_change'] and evidence['model_chain_unchanged'] and evidence['kernel_config_identical_to_candidate11']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(evidence_root/name)==digest,name
  assert meta['local_asr']['microphone_enabled'] and sha(stable)=='816cc314ec69bae717d5c9a0cd1ed61f3aff583cde4060e4c261eb415cb8b819'
 if candidate.name=='candidate13':
  evidence=json.loads((candidate/'ISOLATION-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert evidence['numeric_models_and_capture_bytes_unchanged'] and evidence['kernel_config_identical_to_candidate12'] and evidence['resource_check_block_unchanged']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(evidence_root/name)==digest,name
  assert sha(stable)=='521479ceb44687a07ab2e70a9823922df6098cff95475fd13b28ab617151ccc5'
 if candidate.name=='candidate14':
  evidence=json.loads((candidate/'SD-DIAGNOSTIC-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert evidence['compiled_overlay_inputs_identical_to_candidate13'] and evidence['kernel_config_identical_to_candidate13'] and evidence['receive_function_is_only_kernel_input_change'] and evidence['retries_clock_timeout_and_write_path_unchanged']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(evidence_root/name)==digest,name
  assert sha(stable)=='b0eeb9435b7784532f8e444c9572c960c61ae467b0bbf4cc66ee7740f44bd238'
 if candidate.name=='candidate22':
  evidence=json.loads((candidate/'CACHE-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  for key in ('kernel_and_config_unchanged','scalar_default','no_startup_model_job','text_only_default','dense_arithmetic_bit_exact','idle_media_invalidates_readiness'):
   assert evidence[key],key
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(candidate.parent/name)==digest,name
  assert sha(stable)=='170708f5bb6e0a57e56890bf9fb0911fb3b218b239d4bb6f40a0152234b6fee8'
 if candidate.name=='candidate23':
  evidence=json.loads((candidate/'SESSION-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  for key in ('kernel_config_unchanged','storage_guard_original_bodies_preserved','scalar_default','text_only_default','no_startup_model_job','dense_arithmetic_bit_exact','guarded_session_reuse','request_isolation_passed','audio_and_view_release_passed'):
   assert evidence[key],key
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(candidate.parent/name)==digest,name
  assert evidence['rollback_sha256']==sha(stable)=='106d264380335933766b3441d6e0fdf857a35dd170e150e2780cee8dad9d0f38'
 if candidate.name=='candidate24':
  evidence=json.loads((candidate/'PREFILL-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  for key in ('kernel_and_config_unchanged','scalar_default','text_only_default','no_startup_model_job','dense_arithmetic_bit_exact','fewer_prefill_weight_reads','rope_coefficients_bit_exact','request_and_session_isolation_passed'):
   assert evidence[key],key
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(candidate.parent/name)==digest,name
  assert evidence['rollback_sha256']==sha(stable)=='113dcbe5cc17fb014005ffee37e53b184e1ed2d4c7ff3f376094c634f0a4233b'
 if candidate.name=='candidate25':
  evidence=json.loads((candidate/'TTS-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  for key in ('kernel_and_config_unchanged','llm_arithmetic_unchanged','tts_convolution_arithmetic_unchanged','tts_ten_raw_float_exact','tts_dependency_proven','tts_low_memory_faults_passed','playback_and_gain_unchanged','no_startup_model_job','text_only_default'):
   assert evidence[key],key
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(candidate.parent/name)==digest,name
  assert evidence['rollback_sha256']==sha(stable)=='14441e9a1f9998b362e6c43d31f9bda07f8336905548fa7ddc9686f93884e5c3'
 if candidate.name in ('candidate20','candidate21'):
  evidence=json.loads((candidate/'LAZY-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert evidence['kernel_and_config_unchanged'] and evidence['scalar_default'] and evidence['no_startup_model_job'] and evidence['text_only_default']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(candidate.parent/name)==digest,name
  assert sha(stable)==({'candidate20':'7a812bf803ec0ad60119d0e0066e64f3c1c6b90d49d32eb0cfa00f551b44973d','candidate21':'19fc210f0130d2330a199dad2026b3d8b8ab93950455d4287c78e26aa1c63f02'}[candidate.name])
 if candidate.name=='candidate19':
  evidence=json.loads((candidate/'SPEED-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert evidence['kernel_and_config_unchanged'] and evidence['scalar_default'] and evidence['request_local_verified_stream_only']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(candidate.parent/name)==digest,name
  assert sha(stable)=='047be02f7f3bc751a84488444cd37dc5bce9db40b9d40358f5751399e6840dd4'
 if candidate.name=='candidate18':
  evidence=json.loads((candidate/'IME-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert evidence['only_compiled_change_is_local_voice_ui'] and evidence['kernel_and_config_unchanged'] and evidence['native_host_layout_passed']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  assert sha(stable)=='43f660eb114cf4f79a7cd91e49c5001a49dad31c0e44fbdb272a19f9b5d04293'
 if candidate.name=='candidate17':
  evidence=json.loads((candidate/'Q4-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert evidence['kernel_config_unchanged'] and evidence['scalar_default'] and evidence['weights_unchanged']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(evidence_root/name)==digest,name
  assert sha(stable)=='e32559039e1389c339ffe252edf3335c5ea8a3ca1f89c4f454dc46dfa4c9a80d'
 if candidate.name=='candidate16':
  evidence=json.loads((candidate/'DIAGNOSTICS-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert evidence['numerical_llm_body_unchanged'] and evidence['kernel_and_config_identical_to_candidate15']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(evidence_root/name)==digest,name
  assert sha(stable)=='82a9e9022d2fa0f4f6ab07cbd9b78c3b991eae35d50c7c064b41845c4fcb8733'
 if candidate.name=='candidate15':
  evidence=json.loads((candidate/'PREFILL-VERIFICATION.json').read_text());assert evidence['passed'] and evidence['image_sha256']==sha(image)
  assert evidence['per_dot_product_float_order_unchanged'] and evidence['allocator_four_mib_reserve_and_twelve_mib_cap_unchanged'] and evidence['kernel_and_config_identical_to_candidate14'] and evidence['model_resources_tokenizer_chat_decode_audio_service_unchanged']
  for name,digest in evidence['candidate_source_sha256'].items():assert sha(candidate/'source'/name)==digest,name
  for name,digest in evidence['evidence_sha256'].items():assert sha(evidence_root/name)==digest,name
  assert sha(stable)=='a0c85c9f9e69a64847d2b61801cf46336953e4b9de1c8cd6460e1ada8084eae6'
 if candidate.name=='candidate8':assert sha(stable)=='6ce4d96bb560a5788337ca16711748a88e7674cbb1b34dcf26a4290c6aa38006'
 if candidate.name=='candidate9':assert sha(stable)=='d9cad08f5d2827df91e62dacb99d389af04acd8ddc48158a36ea925b755c48d6'
 port=next(p for p in list_ports.comports() if p.device=='COM4');assert (port.vid,port.pid)==(0x303a,0x1001) and port.serial_number.lower()=='e8:f6:0a:e7:6f:f1'
 out=a.output.resolve();out.mkdir(exist_ok=False);private=out/'.private';private.mkdir();(private/'.gitignore').write_text('*\n')
 state=dict(image_sha256=sha(image),board_written=False,data_partition_preserved=False,rollback_sha256=sha(stable),ble_identity=meta['ble_identity'],physical_screen_accepted=False,speaker_accepted=False,real_wake_accepted=False)
 def save():(out/'DEPLOYMENT.json').write_text(json.dumps(state,indent=2)+'\n')
 def step(name,args,before='no-reset'):
  print('START',name,flush=True)
  with (out/(name+'.log')).open('xb') as log:
   r=subprocess.run([sys.executable,'-m','esptool','--chip','esp32p4','--port','COM4','--baud','460800','--before',before,'--after','no-reset',*map(str,args)],stdout=log,stderr=subprocess.STDOUT,timeout=420)
  if r.returncode:raise RuntimeError(name+' failed; see device log')
  print('PASS',name,flush=True)
 def boot(label):
  s=serial.Serial(port=None,baudrate=115200,timeout=.1,write_timeout=3);s.dtr=s.rts=False;s.port='COM4';s.open();data=bytearray()
  try:
   HardReset(s,uses_usb=True)();s.dtr=True;end=time.monotonic()+25
   while time.monotonic()<end:data.extend(s.read(s.in_waiting or 1))
  finally:s.close()
  (out/(label+'-boot.log')).write_bytes(data)
  if re.search(rb'PANIC|Assertion failed|Unhandled exception|Store access fault|Load access fault',data,re.I):raise RuntimeError('Board boot fault')
 save()
 with (out/'preflight.log').open('w',encoding='utf-8') as log:
  c=Console(log);c.serial.dtr=True
  try:
   assert 'active=0 ' in c.command('desktop ui recorder-status'),'Recording active; flash stopped'
   current=c.command('desktop local-voice status')
   if 'LOCAL_VOICE request=' in current:
    assert 'busy=0 ' in current and 'pending=0 ' in current,'Existing model/configuration task active; flash stopped without canceling it'
   for cmd in ('desktop ui local-stop','desktop ui music-stop','desktop speech stop'):c.command(cmd)
   state['backlight_before']=c.command('desktop ui backlight-status');assert 'percent=100 ' in state['backlight_before']
  finally:c.close()
 save();written=False
 try:
  step('identity',['flash-id'],'usb-reset')
  step('verify-running-rollback',['verify-flash','0x2000',stable])
  shutil.copyfile(stable,private/'rollback-app.bin')
  step('data-before',['read-flash','0xc00000','0x400000',private/'data-before.bin'])
  state['data_sha256_before']=sha(private/'data-before.bin');save()
  written=True;state['board_written']=True;save()
  step('write',['write-flash','--flash-mode','keep','--flash-size','keep','--flash-freq','keep','0x2000',image])
  step('verify',['verify-flash','0x2000',image])
  step('readback',['read-flash','0x2000',str(image.stat().st_size),private/'candidate-readback.bin'])
  assert sha(private/'candidate-readback.bin')==sha(image);state['independent_readback_verified']=True;save()
  step('data-after',['read-flash','0xc00000','0x400000',private/'data-after.bin'])
  assert sha(private/'data-after.bin')==state['data_sha256_before'];state['data_partition_preserved']=True;save()
  boot('candidate');state['warm_boot_captured']=True;save()
  with (out/'runtime.log').open('w',encoding='utf-8') as log:
   c=Console(log);c.serial.dtr=True
   try:
    state['xip']=c.command('desktop xip',20);expected=json.loads((candidate/'EXPECTED-XIP-HASHES.json').read_text())
    assert 'ready=1 protected_cpus=3' in state['xip'] and 'text_hash='+expected['text'] in state['xip'] and 'rodata_hash='+expected['rodata'] in state['xip']
    state['backlight_after']=c.command('desktop ui backlight-status');assert 'percent=100 ' in state['backlight_after'] and 'error=0' in state['backlight_after']
    state['local_voice']=c.command('desktop local-voice status');state['heap']=c.command('free')
   finally:c.close()
  state['runtime_checked']=True;save();print('LOCAL_VOICE_CANDIDATE_BOOTED',sha(image),flush=True)
 except BaseException as error:
  state['error']=type(error).__name__+': '+str(error)[:160];save()
  if written and not state.get('warm_boot_captured'):
   step('restore-app',['write-flash','--flash-mode','keep','--flash-size','keep','--flash-freq','keep','0x2000',private/'rollback-app.bin'],'usb-reset')
   step('verify-restored',['verify-flash','0x2000',private/'rollback-app.bin']);boot('restored');state['restored']=True;save()
  elif not written:boot('unchanged')
  raise
if __name__=='__main__':main()
