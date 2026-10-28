#!/usr/bin/env python3
"""Install pinned OpenH264 and direct-live sources into the v3 Apps tree."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tarfile

H264_SHA = '558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apps',type=Path,required=True)
    p.add_argument('--archive',type=Path,required=True)
    p.add_argument('--ca-bundle',type=Path,default=Path('/etc/ssl/certs/ca-certificates.crt'))
    a=p.parse_args()
    repo=Path(__file__).resolve().parent.parent
    dest=a.apps.resolve()/'system/bilibili_live'
    if not (a.apps/'system/desktop/glass_https.h').is_file():
        p.error('Apply the current v3 desktop overlay first')
    if hashlib.sha256(a.archive.read_bytes()).hexdigest()!=H264_SHA:
        p.error('OpenH264 archive checksum mismatch')
    ca=a.ca_bundle.read_text()
    if '-----BEGIN CERTIFICATE-----' not in ca: p.error('PEM CA bundle required')
    shutil.copytree(repo/'app/bilibili_live',dest,dirs_exist_ok=True)
    kconfig=dest/'Kconfig';kconfig.write_text(kconfig.read_text())
    vendor=dest/'vendor';vendor.mkdir(exist_ok=True)
    if not (vendor/'openh264/codec/api/wels/codec_api.h').exists():
        with tarfile.open(a.archive) as tar:
            members=[]
            for m in tar.getmembers():
                relative=Path(m.name).relative_to('openh264-2.6.0')
                if relative.parts and (relative.parts[0]=='codec' or relative.name=='LICENSE'):
                    m.name=str(Path('openh264')/relative);members.append(m)
            tar.extractall(vendor,members=members,filter='data')
    # Reapply the complete patch train to pinned originals; composed patches
    # must not match against intermediate edits left by an earlier install.
    patched_sources=[
        'common/src/WelsThreadLib.cpp','common/src/memory_align.cpp',
        'decoder/core/src/decoder_core.cpp','decoder/core/src/decoder.cpp',
        'decoder/core/src/pic_queue.cpp','decoder/core/src/decode_slice.cpp',
        'decoder/core/src/wels_decoder_thread.cpp',
        'common/src/mc.cpp','common/src/deblocking_common.cpp','decoder/core/src/decode_mb_aux.cpp',
        'decoder/core/src/parse_mb_syn_cavlc.cpp','decoder/core/src/cabac_decoder.cpp',
        'decoder/plus/src/welsDecoderExt.cpp','decoder/plus/inc/welsDecoderExt.h']
    with tarfile.open(a.archive) as tar:
        for relative in patched_sources:
            data=tar.extractfile('openh264-2.6.0/codec/'+relative).read()
            target=vendor/'openh264/codec'/relative
            if target.read_bytes()!=data: target.write_bytes(data)
    def patch(path,old,new):
        s=path.read_text()
        if new in s:return
        if s.count(old)!=1:raise RuntimeError('Unknown dependency baseline: '+str(path))
        path.write_text(s.replace(old,new,1))
    t=vendor/'openh264/codec/common/src/WelsThreadLib.cpp'
    patch(t,'#elif !defined(__Fuchsia__)\n','#elif !defined(__Fuchsia__) && !defined(__NuttX__)\n')
    # Migrate the existing single-core compatibility patch before matching.
    legacy=t.read_text(); legacy=legacy.replace('#ifdef __NuttX__\n  pInfo->ProcessorCount = 1;', '#ifdef __NuttX__\n  pInfo->ProcessorCount = 2;');t.write_text(legacy)
    patch(t,'WELS_THREAD_ERROR_CODE    WelsQueryLogicalProcessInfo (WelsLogicalProcessInfo* pInfo) {\n#ifdef ANDROID_NDK',
          'WELS_THREAD_ERROR_CODE    WelsQueryLogicalProcessInfo (WelsLogicalProcessInfo* pInfo) {\n#ifdef __NuttX__\n  pInfo->ProcessorCount = 2;\n  return WELS_THREAD_ERROR_OK;\n#else\n#ifdef ANDROID_NDK')
    patch(t,'#endif//__linux__\n}','#endif//__linux__\n#endif /* __NuttX__ */\n}')
    # This installer targets the dual-core ESP32-P4. Live keeps threads=0;
    # local timed playback can use both cores.
    patch(t, '#ifdef __NuttX__\n  pInfo->ProcessorCount = 1;',
          '#ifdef __NuttX__\n  pInfo->ProcessorCount = 2;')
    patch(t, '  err = pthread_attr_init (&at);\n',
          '  err = pthread_attr_init (&at);\n#ifdef __NuttX__\n  if (!err) err = pthread_attr_setstacksize (&at, 65536);\n#endif\n')
    core=vendor/'openh264/codec/decoder/core/src/decoder_core.cpp'
    for name,arg in [('ExpandBsBuffer','kiSrcLen'),('ExpandBsLenBuffer','kiCurrLen')]:
        patch(core,f'{name} (PWelsDecoderContext pCtx, const int {arg})',f'{name} (PWelsDecoderContext pCtx, const int32_t {arg})')
    # OpenH264 2.6.0 otherwise activates a worker after picture allocation
    # failed in ParseAccessUnit, dereferencing a NULL picture buffer.
    ext=vendor/'openh264/codec/decoder/plus/src/welsDecoderExt.cpp'
    patch(ext, '  ParseAccessUnit (m_pDecThrCtx[signal]);\n',
          '  int parseState = ParseAccessUnit (m_pDecThrCtx[signal]);\n'
          '  if (parseState != dsErrorFree) {\n'
          '    RELEASE_SEMAPHORE (&m_pDecThrCtx[signal].sThreadInfo.sIsIdle);\n'
          '    int error = m_pDecThrCtx[signal].pCtx->iErrorCode;\n'
          '    return error ? error : dsBitstreamError;\n'
          '  }\n')
    # Preserve the official threaded picture pool. Shrinking it can exhaust
    # pictures despite SPS bounds because of decoder pipeline ownership.
    memory=vendor/'openh264/codec/decoder/core/src/decoder.cpp'
    previous=memory.read_text()
    if '      int32_t dpb = MAX_DPB_COUNT;' in previous:
        begin=previous.index('      int32_t dpb = MAX_DPB_COUNT;')
        end=previous.index('\n',previous.index('      WelsLog (&pCtx->sLogCtx, WELS_LOG_INFO, "P4 threaded DPB=',begin))+1
        previous=previous[:begin]+'      iNumRefFrames = MAX_DPB_COUNT + iThreadCount;\n'+previous[end:]
        memory.write_text(previous)
    # Start compressed-AU buffers small; retain upstream growth on demand.
    patch(core, '  pCtx->iMaxBsBufferSizeInByte = MIN_ACCESS_UNIT_CAPACITY * MAX_BUFFERED_NUM;\n',
          '  pCtx->iMaxBsBufferSizeInByte = 65536 * MAX_BUFFERED_NUM;\n')
    align=vendor/'openh264/codec/common/src/memory_align.cpp'
    patch(align, '#include <stdlib.h>\n', '#include <stdlib.h>\n#include <stdio.h>\n')
    patch(align, '  if (NULL == pBuf)\n    return NULL;\n',
          '  if (NULL == pBuf) {\n'
          '    fprintf(stderr, "OpenH264 allocation failed: %s bytes=%u\\n", kpTag ? kpTag : "unknown", kiActualRequestedSize);\n'
          '    return NULL;\n'
          '  }\n')
    pictures=vendor/'openh264/codec/decoder/core/src/pic_queue.cpp'
    patch(pictures, '  if (pCtx->pThreadCtx != NULL) {\n',
          '  if (!pPic->pMbCorrectlyDecodedFlag || !pPic->pMbType ||\n'
          '      !pPic->pMv[LIST_0] || !pPic->pMv[LIST_1] ||\n'
          '      !pPic->pRefIndex[LIST_0] || !pPic->pRefIndex[LIST_1] ||\n'
          '      (GetThreadCount(pCtx) > 1 && !pPic->pNzc)) {\n'
          '    FreePicture(pPic, pMa); return NULL;\n'
          '  }\n'
          '  if (pCtx->pThreadCtx != NULL) {\n')
    patch(pictures, '    for (uint32_t i = 0; i < uiMbHeight; ++i) {\n      CREATE_EVENT',
          '    if (!pPic->pReadyEvent) { FreePicture(pPic, pMa); return NULL; }\n'
          '    for (uint32_t i = 0; i < uiMbHeight; ++i) {\n      CREATE_EVENT')
    # B prediction uses only scratch pixels, never reference-picture metadata.
    # Keep the official DPB untouched and avoid allocating unused MV/events twice.
    patch(pictures,
          'PPicture AllocPicture (PWelsDecoderContext pCtx, const int32_t kiPicWidth, const int32_t kiPicHeight) {',
          'static PPicture AllocPictureInternal (PWelsDecoderContext pCtx, const int32_t kiPicWidth, const int32_t kiPicHeight, bool scratch) {')
    patch(pictures, '  uint32_t uiMbWidth = (kiPicWidth + 15) >> 4;\n',
          '  if (scratch) return pPic; /* Only pData and strides are consumed. */\n'
          '  uint32_t uiMbWidth = (kiPicWidth + 15) >> 4;\n')
    # Remove duplicate wrappers left by the earlier scratch-height migration.
    text=pictures.read_text(encoding='utf-8')
    wrapper=('PPicture AllocPicture (PWelsDecoderContext pCtx, const int32_t w, const int32_t h) {\n'
             '  return AllocPictureInternal(pCtx, w, h, false);\n}\n'
             'PPicture AllocPredictionPicture (PWelsDecoderContext pCtx, const int32_t w, const int32_t h) {\n'
             '  (void)h; return AllocPictureInternal(pCtx, w, 16, true);\n}\n\n')
    text=text.replace(wrapper.replace('(void)h; return AllocPictureInternal(pCtx, w, 16, true);', 'return AllocPictureInternal(pCtx, w, h, true);'), '')
    while text.count(wrapper)>1: text=text.replace(wrapper,'',1)
    pictures.write_text(text,encoding='utf-8')
    patch(pictures, 'void FreePicture (PPicture pPic, CMemoryAlign* pMa) {\n',
          'PPicture AllocPicture (PWelsDecoderContext pCtx, const int32_t w, const int32_t h) {\n'
          '  return AllocPictureInternal(pCtx, w, h, false);\n}\n'
          'PPicture AllocPredictionPicture (PWelsDecoderContext pCtx, const int32_t w, const int32_t h) {\n'
          '  (void)h; return AllocPictureInternal(pCtx, w, 16, true);\n}\n\n'
          'void FreePicture (PPicture pPic, CMemoryAlign* pMa) {\n')
    for file in [memory, vendor/'openh264/codec/decoder/core/src/decode_slice.cpp']:
        text=file.read_text(encoding='utf-8')
        declaration='extern PPicture AllocPicture (PWelsDecoderContext pCtx, const int32_t kiPicWidth, const int32_t kiPicHeight);'
        new_declaration=declaration.replace('AllocPicture', 'AllocPredictionPicture')
        if new_declaration not in text:
            assert declaration in text
            text=text.replace(declaration, declaration+'\n'+new_declaration, 1)
        text=text.replace('pCtx->pTempDec = AllocPicture (', 'pCtx->pTempDec = AllocPredictionPicture (')
        file.write_text(text,encoding='utf-8')
    slices=vendor/'openh264/codec/decoder/core/src/decode_slice.cpp'
    text=slices.read_text(encoding='utf-8')
    allocation='      pCtx->pTempDec = AllocPredictionPicture (pCtx, pCtx->pSps->iMbWidth << 4, pCtx->pSps->iMbHeight << 4);\n'
    guarded=allocation+('    if (!pCtx->pTempDec) {\n'
                        '      pCtx->iErrorCode |= dsOutOfMemory;\n'
                        '      return ERR_INFO_MB_RECON_FAIL;\n'
                        '    }\n')
    if guarded not in text:
        assert text.count(allocation)==2
        text=text.replace(allocation,guarded)
        slices.write_text(text,encoding='utf-8')
    # Prediction scratch is consumed synchronously for a single macroblock.
    # Preserve the picture width/stride, reuse its 16 rows for every MB row.
    patch(pictures, '  return AllocPictureInternal(pCtx, w, h, true);\n',
          '  (void)h; return AllocPictureInternal(pCtx, w, 16, true);\n')
    text=slices.read_text(encoding='utf-8')
    for plane,stride,shift in [(0,'iLumaStride',4),(1,'iChromaStride',3),(2,'iChromaStride',3)]:
        old=f'pCtx->pTempDec->pData[{plane}] + ((iMbY * {stride} + iMbX) << {shift})'
        new=f'pCtx->pTempDec->pData[{plane}] + (iMbX << {shift})'
        if old in text:
            assert text.count(old)==2
            text=text.replace(old,new)
    slices.write_text(text,encoding='utf-8')
    patch(memory, 'pCtx->pTempDec->iHeightInPixel != kiPicHeight',
          'pCtx->pTempDec->iHeightInPixel != 16')
    # POSIX waits must consume the real semaphore even when already posted.
    # The upstream v + sem dual counters drift and race on two CPU cores.
    thread=vendor/'openh264/codec/decoder/core/src/wels_decoder_thread.cpp'
    text=thread.read_text(encoding='utf-8')
    if 'POSIX_SINGLE_SEMAPHORE_COUNT' not in text:
        begin=text.index('int SemCreate (',text.index('#else /* _WIN32 */'))
        end=text.index('void SemDestroy (',begin)
        original=text[begin:end]
        native=r"""
#if !defined(__APPLE__)
// POSIX_SINGLE_SEMAPHORE_COUNT: exactly one native token per release/wait.
int SemCreate (SWelsDecSemphore* s, long value, long max) {
  s->v = 0; s->max = max;
  if (pthread_mutex_init(&s->m, NULL)) return 1;
  if (WelsEventOpen(&s->e, "")) { pthread_mutex_destroy(&s->m); return 2; }
  for (long i=0; i<value; ++i) sem_post(s->e);
  return 0;
}
int SemWait (SWelsDecSemphore* s, int32_t timeout) {
  int rc;
  struct timespec ts;
  if (timeout>0) getTimespecFromTimeout(&ts, timeout);
  do {
    if (timeout==0) rc=sem_trywait(s->e);
    else if (timeout==WELS_DEC_THREAD_WAIT_INFINITE || timeout<0) rc=sem_wait(s->e);
    else rc=sem_timedwait(s->e,&ts);
  } while (rc!=0 && errno==EINTR);
  return rc==0 ? WELS_DEC_THREAD_WAIT_SIGNALED : WELS_DEC_THREAD_WAIT_TIMEDOUT;
}
void SemRelease (SWelsDecSemphore* s, long* previous) {
  pthread_mutex_lock(&s->m);
  int count=0; sem_getvalue(s->e,&count);
  if (previous) *previous=count>0?count:0;
  if (count<s->max) sem_post(s->e);
  pthread_mutex_unlock(&s->m);
}
#else
"""
        text=text[:begin]+native+original+'#endif\n\n'+text[end:]
        thread.write_text(text,encoding='utf-8')
    # A condition-variable wakeup is not itself proof the event is signaled.
    patch(thread,
          '    rc = pthread_cond_wait (& (e->c), & (e->m));\n',
          '    while (!e->isSignaled && rc == 0)\n'
          '      rc = pthread_cond_wait (& (e->c), & (e->m));\n')
    # Both workers enqueue output, and the caller dequeues on another core.
    # Protect only queue metadata, never the actual picture decoding work.
    patch(ext, 'static DECODING_STATE  ConstructAccessUnit (',
          'class DecoderQueueLock {\n'
          '  WELS_MUTEX* mutex;\n'
          ' public:\n'
          '  explicit DecoderQueueLock(WELS_MUTEX* p): mutex(p) { if (mutex) WelsMutexLock(mutex); }\n'
          '  ~DecoderQueueLock() { if (mutex) WelsMutexUnlock(mutex); }\n'
          '};\n\n'
          'static DECODING_STATE  ConstructAccessUnit (')
    text=ext.read_text(encoding='utf-8')
    for function in ['BufferingReadyPicture','ReleaseBufferedReadyPictureReorder','ReleaseBufferedReadyPictureNoReorder']:
        begin=text.index('void CWelsDecoder::'+function+' (') if function!='ReleaseBufferedReadyPictureNoReorder' else text.index('void CWelsDecoder::'+function+'(')
        brace=text.index('{',begin)
        guard='\n  DecoderQueueLock queueLock(m_iThreadCount >= 1 ? &m_csDecoder : NULL);'
        if not text[brace+1:].startswith(guard):text=text[:brace+1]+guard+text[brace+1:]
    ext.write_text(text,encoding='utf-8')
    # The shared last-decoded picture can already belong to the other worker.
    # Queue the picture owned by this thread so dequeue decrements the right slot.
    patch(ext,
          '      if (pCtx->pLastDecPicInfo->pPreviousDecodedPictureInDpb != NULL) {\n'
          '        m_sPictInfoList[i].iPicBuffIdx = pCtx->pLastDecPicInfo->pPreviousDecodedPictureInDpb->iPicBuffIdx;\n'
          '        if (GetThreadCount (pCtx) <= 1) ++pCtx->pLastDecPicInfo->pPreviousDecodedPictureInDpb->iRefCount;\n'
          '      }\n',
          '      PPicture output = GetThreadCount(pCtx) > 1\n'
          '        ? static_cast<PWelsDecoderThreadCTX>(pCtx->pThreadCtx)->pDec\n'
          '        : pCtx->pLastDecPicInfo->pPreviousDecodedPictureInDpb;\n'
          '      if (output != NULL) {\n'
          '        m_sPictInfoList[i].iPicBuffIdx = output->iPicBuffIdx;\n'
          '        if (GetThreadCount(pCtx) <= 1) ++output->iRefCount;\n'
          '      }\n')
    # A worker must never join/reset its own decoder or free its peer's pool.
    patch(ext, '    if (pDecContext->iErrorCode & dsOutOfMemory) {\n',
          '    if (GetThreadCount(pDecContext) >= 1 &&\n'
          '        (pDecContext->iErrorCode & (dsOutOfMemory | dsRefListNullPtrs))) {\n'
          '      SET_EVENT(&m_sBufferingEvent);\n'
          '      return (DECODING_STATE)pDecContext->iErrorCode;\n'
          '    }\n'
          '    if (pDecContext->iErrorCode & dsOutOfMemory) {\n')
    patch(ext,
          '      ConstructAccessUnit (pWelsDecoder, pThrCtx);\n',
          '      int state = ConstructAccessUnit (pWelsDecoder, pThrCtx);\n'
          '      if (state != dsErrorFree) {\n'
          '        pThrCtx->pCtx->iErrorCode |= state;\n'
          '        SET_EVENT(&pThrCtx->sSliceDecodeStart);\n'
          '        SET_EVENT(&pThrCtx->sSliceDecodeFinish);\n'
          '        SET_EVENT(&pThrCtx->sImageReady);\n'
          '        if (pThrCtx->pDec && pThrCtx->pDec->pReadyEvent) {\n'
          '          uint32_t rows = (pThrCtx->pDec->iHeightInPixel + 15) >> 4;\n'
          '          for (uint32_t row=0; row<rows; ++row) SET_EVENT(&pThrCtx->pDec->pReadyEvent[row]);\n'
          '        }\n'
          '      }\n')
    patch(ext,
          '  WAIT_SEMAPHORE (&m_pDecThrCtx[signal].sThreadInfo.sIsIdle, WELS_DEC_THREAD_WAIT_INFINITE);\n',
          '  WAIT_SEMAPHORE (&m_pDecThrCtx[signal].sThreadInfo.sIsIdle, WELS_DEC_THREAD_WAIT_INFINITE);\n'
          '  int previousError = m_pDecThrCtx[signal].pCtx->iErrorCode;\n'
          '  if (previousError & (dsOutOfMemory | dsRefListNullPtrs)) {\n'
          '    RELEASE_SEMAPHORE(&m_pDecThrCtx[signal].sThreadInfo.sIsIdle);\n'
          '    return previousError;\n'
          '  }\n')
    patch(ext,
          '    RELEASE_SEMAPHORE (&m_pDecThrCtxActive[0]->sThreadInfo.sIsIdle);\n',
          '    RELEASE_SEMAPHORE (&m_pDecThrCtxActive[0]->sThreadInfo.sIsIdle);\n'
          '    state |= m_pDecThrCtxActive[0]->pCtx->iErrorCode;\n')
    # A returned picture remains owned by the API caller until its next call.
    # Otherwise a second worker can recycle a non-reference B picture while
    # the caller is still reading its pixels for conversion/display.
    header=vendor/'openh264/codec/decoder/plus/inc/welsDecoderExt.h'
    patch(header, '  int32_t                 m_iLastBufferedIdx;\n',
          '  int32_t                 m_iLastBufferedIdx;\n'
          '  PPicture                m_pHeldOutput;\n'
          '  void ReleaseHeldOutput();\n')
    patch(ext, '    m_iLastBufferedIdx (0),\n',
          '    m_iLastBufferedIdx (0),\n    m_pHeldOutput (NULL),\n')
    patch(ext, 'DECODING_STATE CWelsDecoder::DecodeFrameNoDelay (',
          'void CWelsDecoder::ReleaseHeldOutput() {\n'
          '  if (m_pHeldOutput) {\n'
          '    PPicture picture=m_pHeldOutput; m_pHeldOutput=NULL;\n'
          '    --picture->iRefCount;\n'
          '    if (picture->iRefCount <= 0 && picture->pSetUnRef) picture->pSetUnRef(picture);\n'
          '  }\n}\n\n'
          'DECODING_STATE CWelsDecoder::DecodeFrameNoDelay (')
    text=ext.read_text(encoding='utf-8')
    for function in ['DecodeFrameNoDelay','FlushFrame']:
        begin=text.index('DECODING_STATE CWelsDecoder::'+function+' (')
        brace=text.index('{',begin)
        statement='\n  ReleaseHeldOutput();'
        if not text[brace+1:].startswith(statement):text=text[:brace+1]+statement+text[brace+1:]
    for indent in ['            ','      ']:
        old=(indent+'--pPic->iRefCount;\n'+indent+'if (pPic->iRefCount <= 0 && pPic->pSetUnRef)\n'+indent+'  pPic->pSetUnRef(pPic);')
        new=(indent+'if (m_iThreadCount >= 1) m_pHeldOutput=pPic;\n'+indent+'else {\n'+old+ '\n'+indent+'}')
        if new not in text:
            assert text.count(old)==1
            text=text.replace(old,new,1)
    ext.write_text(text,encoding='utf-8')
    patch(ext, 'int32_t CWelsDecoder::ThreadResetDecoder (PWelsDecoderContext& pCtx) {\n',
          'int32_t CWelsDecoder::ThreadResetDecoder (PWelsDecoderContext& pCtx) {\n'
          '  m_pHeldOutput=NULL; /* The following reset releases the entire old pool. */\n')
    # Use the same short mutex for claiming/marking pictures and retiring output.
    # SetUnRef modifies several fields after making a slot look reusable; without
    # this lock the peer can claim the slot before retirement has finished.
    patch(core, 'int32_t DecodeCurrentAccessUnit (PWelsDecoderContext pCtx, uint8_t** ppDst, SBufferInfo* pDstInfo) {\n',
          'class PicturePoolLock {\n'
          '  WELS_MUTEX* mutex;\n'
          ' public:\n'
          '  explicit PicturePoolLock(WELS_MUTEX* p): mutex(p) { if (mutex) WelsMutexLock(mutex); }\n'
          '  ~PicturePoolLock() { if (mutex) WelsMutexUnlock(mutex); }\n'
          '};\n\n'
          'int32_t DecodeCurrentAccessUnit (PWelsDecoderContext pCtx, uint8_t** ppDst, SBufferInfo* pDstInfo) {\n')
    patch(core, '    if (pCtx->pDec == NULL) {\n      //make call PrefetchPic',
          '    if (pCtx->pDec == NULL) {\n'
          '      PicturePoolLock poolLock(iThreadCount > 1 ? pCtx->pCsDecoder : NULL);\n'
          '      //make call PrefetchPic')
    patch(ext, 'void CWelsDecoder::ReleaseHeldOutput() {\n',
          'void CWelsDecoder::ReleaseHeldOutput() {\n'
          '  DecoderQueueLock queueLock(m_iThreadCount >= 1 ? &m_csDecoder : NULL);\n')
    patch(core, '      if (NULL == pCtx->pDec) {\n        WelsLog',
          '      if (NULL == pCtx->pDec) {\n'
          '        fprintf(stderr, "OpenH264 pool exhausted capacity=%d slots=", (int)pCtx->pPicBuff->iCapacity);\n'
          '        for (int slot=0; slot<pCtx->pPicBuff->iCapacity; ++slot) {\n'
          '          PPicture picture=pCtx->pPicBuff->ppPic[slot];\n'
          '          if (picture) fprintf(stderr, "%d:%d/%d ", slot, (int)picture->iRefCount, (int)picture->bUsedAsRef);\n'
          '        }\n'
          '        fprintf(stderr, "\\n");\n'
          '        WelsLog')
    # Keep each worker's prediction references alive until its frame finishes.
    # Output ownership alone does not cover a peer still using an evicted DPB ref.
    patch(header, '  CWelsDecoder (void);\n',
          '  CWelsDecoder (void);\n'
          '  void PinFrameReferences(PWelsDecoderContext pCtx);\n'
          '  void UnpinFrameReferences(PWelsDecoderContext pCtx);\n')
    patch(header, '  PPicture                m_pHeldOutput;\n',
          '  PPicture                m_pHeldOutput;\n'
          '  PPicture m_pPinnedRefs[WELS_DEC_MAX_NUM_CPU][MAX_REF_PIC_COUNT * LIST_A];\n'
          '  unsigned m_pinnedCount[WELS_DEC_MAX_NUM_CPU];\n')
    patch(ext, '  ResetReorderingPictureBuffers (&m_sReoderingStatus, m_sPictInfoList, true);\n\n  m_iCpuCount',
          '  memset(m_pinnedCount, 0, sizeof(m_pinnedCount));\n'
          '  ResetReorderingPictureBuffers (&m_sReoderingStatus, m_sPictInfoList, true);\n\n  m_iCpuCount')
    methods=r"""
void CWelsDecoder::PinFrameReferences(PWelsDecoderContext ctx) {
  DecoderQueueLock lock(&m_csDecoder);
  unsigned thread=static_cast<PWelsDecoderThreadCTX>(ctx->pThreadCtx)->sThreadInfo.uiThrNum;
  for (unsigned list=0; list<LIST_A; ++list) {
    for (unsigned index=0; index<MAX_REF_PIC_COUNT; ++index) {
      PPicture ref=ctx->sRefPic.pRefList[list][index];
      if (!ref) continue;
      bool found=false;
      for (unsigned n=0;n<m_pinnedCount[thread];++n) if(m_pPinnedRefs[thread][n]==ref) found=true;
      if (!found && m_pinnedCount[thread]<MAX_REF_PIC_COUNT*LIST_A) {
        m_pPinnedRefs[thread][m_pinnedCount[thread]++]=ref; ++ref->iRefCount;
      }
    }
  }
}
void CWelsDecoder::UnpinFrameReferences(PWelsDecoderContext ctx) {
  DecoderQueueLock lock(&m_csDecoder);
  unsigned thread=static_cast<PWelsDecoderThreadCTX>(ctx->pThreadCtx)->sThreadInfo.uiThrNum;
  while (m_pinnedCount[thread]) {
    PPicture ref=m_pPinnedRefs[thread][--m_pinnedCount[thread]];
    --ref->iRefCount;
    if (ref->iRefCount<=0 && ref->pSetUnRef) ref->pSetUnRef(ref);
  }
}
void PinWorkerReferences(PWelsDecoderContext ctx) {
  PWelsDecoderThreadCTX thread=static_cast<PWelsDecoderThreadCTX>(ctx->pThreadCtx);
  static_cast<CWelsDecoder*>(thread->threadCtxOwner)->PinFrameReferences(ctx);
}

"""
    patch(ext, 'void CWelsDecoder::ReleaseHeldOutput() {\n',
          methods+'void CWelsDecoder::ReleaseHeldOutput() {\n')
    patch(ext, '      int state = ConstructAccessUnit (pWelsDecoder, pThrCtx);\n',
          '      int state = ConstructAccessUnit (pWelsDecoder, pThrCtx);\n'
          '      pWelsDecoder->UnpinFrameReferences(pThrCtx->pCtx);\n')
    patch(core, 'class PicturePoolLock {\n',
          'extern void PinWorkerReferences(PWelsDecoderContext ctx);\n'
          'class PicturePoolLock {\n')
    patch(core, '            SET_EVENT (&pThreadCtx->sSliceDecodeStart);\n',
          '            PinWorkerReferences(pCtx);\n'
          '            SET_EVENT (&pThreadCtx->sSliceDecodeStart);\n')
    # Workers must never write the caller's returned plane array after API return.
    patch(header, '  PPicture                m_pHeldOutput;\n',
          '  PPicture                m_pHeldOutput;\n'
          '  unsigned char* m_workerOutput[WELS_DEC_MAX_NUM_CPU][3];\n')
    patch(ext, '  m_pDecThrCtx[signal].ppDst = ppDst;\n',
          '  memset(m_workerOutput[signal], 0, sizeof(m_workerOutput[signal]));\n'
          '  m_pDecThrCtx[signal].ppDst = m_workerOutput[signal];\n')
    # Every PrefetchPic output reservation must transfer to a queue or retire.
    patch(header, '  void UnpinFrameReferences(PWelsDecoderContext pCtx);\n',
          '  void UnpinFrameReferences(PWelsDecoderContext pCtx);\n'
          '  void RetireUnqueuedPicture(PWelsDecoderThreadCTX thread);\n'
          '  bool m_workerQueued[WELS_DEC_MAX_NUM_CPU];\n')
    patch(ext, '  memset(m_pinnedCount, 0, sizeof(m_pinnedCount));\n',
          '  memset(m_workerQueued, 0, sizeof(m_workerQueued));\n'
          '  memset(m_pinnedCount, 0, sizeof(m_pinnedCount));\n')
    patch(ext, '  memset(m_workerOutput[signal], 0, sizeof(m_workerOutput[signal]));\n',
          '  m_workerQueued[signal] = false;\n'
          '  memset(m_workerOutput[signal], 0, sizeof(m_workerOutput[signal]));\n')
    patch(ext, '      m_iLastBufferedIdx = i;\n',
          '      if (GetThreadCount(pCtx) > 1)\n'
          '        m_workerQueued[static_cast<PWelsDecoderThreadCTX>(pCtx->pThreadCtx)->sThreadInfo.uiThrNum] = true;\n'
          '      m_iLastBufferedIdx = i;\n')
    patch(ext, 'void CWelsDecoder::ReleaseHeldOutput() {\n',
          'void CWelsDecoder::RetireUnqueuedPicture(PWelsDecoderThreadCTX thread) {\n'
          '  DecoderQueueLock lock(&m_csDecoder);\n'
          '  if (GetThreadCount(thread->pCtx)>1 && thread->pDec && !m_workerQueued[thread->sThreadInfo.uiThrNum]) {\n'
          '    PPicture picture=thread->pDec;\n'
          '    --picture->iRefCount;\n'
          '    if (picture->iRefCount<=0 && picture->pSetUnRef) picture->pSetUnRef(picture);\n'
          '    fprintf(stderr, "OpenH264 retired unqueued picture slot=%d state=%d\\n", picture->iPicBuffIdx, thread->pCtx->iErrorCode);\n'
          '  }\n'
          '}\n\n'
          'void CWelsDecoder::ReleaseHeldOutput() {\n')
    patch(ext, '      pWelsDecoder->UnpinFrameReferences(pThrCtx->pCtx);\n',
          '      pWelsDecoder->UnpinFrameReferences(pThrCtx->pCtx);\n'
          '      pWelsDecoder->RetireUnqueuedPicture(pThrCtx);\n')
    mc=vendor/'openh264/codec/common/src/mc.cpp'
    shutil.copy2(repo/'app/bilibili_live/p4_fused_mc.inc',mc.parent/'p4_fused_mc.inc')
    patch(mc, 'static inline void PixelAvg_c (',
          '#include "p4_fused_mc.inc"\n\nstatic inline void PixelAvg_c (')
    quarter = {'01':(-1,0,0,0), '03':(-1,0,1,0),
               '10':(0,-1,0,0), '30':(0,-1,0,1),
               '11':(0,0,0,0), '13':(1,0,0,0),
               '31':(0,1,0,0), '33':(1,1,0,0)}
    text=mc.read_text()
    for position,args in quarter.items():
        start=text.index('static inline void McHorVer'+position+'_c (')
        brace=text.index('{',start)+1
        call=','.join(map(str,args))
        fast=('\n#ifdef BILI_P4_FUSED_MC\n'
              '  if (BiliMcEnabled(1) && BiliMcDisjoint(pSrc,iSrcStride,pDst,iDstStride,iWidth,iHeight)) {\n'
              '    BiliMcQuarter<'+call+'>(pSrc,iSrcStride,pDst,iDstStride,iWidth,iHeight);\n'
              '    return;\n  }\n#endif\n')
        text=text[:brace]+fast+text[brace:]
    central = {'12':(-1,0), '32':(-1,1), '21':(0,-1), '23':(1,-1)}
    for position,args in central.items():
        start=text.index('static inline void McHorVer'+position+'_c (')
        brace=text.index('{',start)+1
        call=','.join(map(str,args))
        fast=('\n#ifdef BILI_P4_FUSED_CENTRAL\n'
              '  if (BiliMcEnabled(2) && BiliMcDisjoint(pSrc,iSrcStride,pDst,iDstStride,iWidth,iHeight)) {\n'
              '    BiliMcCentral<'+call+'>(pSrc,iSrcStride,pDst,iDstStride,iWidth,iHeight);\n'
              '    return;\n  }\n#endif\n')
        text=text[:brace]+fast+text[brace:]
    text += ('\nextern "C" int bili_mc_set_features(unsigned features) {\n'
             '#if defined(BILI_P4_FUSED_MC) || defined(BILI_P4_FUSED_CENTRAL)\n'
             '  if (features > 3) return -1;\n'
             '  unsigned old = __atomic_load_n(&g_bili_mc_features, __ATOMIC_RELAXED);\n'
             '  __atomic_store_n(&g_bili_mc_features, features, __ATOMIC_RELAXED);\n'
             '  return static_cast<int>(old);\n'
             '#else\n  return features ? -1 : 0;\n#endif\n}\n')
    mc.write_text(text)
    # Keep the bounded scalar pixel/entropy kernels in internal instruction
    # RAM on P4. Other platforms and disabled builds retain the upstream
    # placement; arithmetic and bitstream handling are unchanged.
    import re
    hot = {
        'common/src/mc.cpp': ['McCopy_c','McLuma_c','McChroma_c'] +
            ['McHorVer'+str(x)+str(y)+'_c' for x in range(4) for y in range(4) if x or y],
        'decoder/core/src/decode_mb_aux.cpp': ['IdctResAddPred_c','IdctResAddPred8x8_c'],
        'decoder/core/src/parse_mb_syn_cavlc.cpp': ['CavlcGetTrailingOnesAndTotalCoeff',
            'CavlcGetTotalZeros','CavlcGetRunBefore','CavlcGetLevelVal',
            'WelsResidualBlockCavlc','WelsResidualBlockCavlc8x8'],
        'decoder/core/src/cabac_decoder.cpp': ['DecodeBinCabac','DecodeBypassCabac'],
    }
    placement = ('#if defined(__NuttX__) && defined(__riscv) && defined(BILI_P4_HOT_IRAM)\n'
                 '#define BILI_P4_HOT __attribute__((section(".iram1.bili_h264")))\n'
                 '#else\n#define BILI_P4_HOT\n#endif\n')
    for relative,names in hot.items():
        target=vendor/'openh264/codec'/relative
        text=target.read_text()
        for name in names:
            pattern=r'^(\s*(?:static\s+)?(?:inline\s+)?)(void|int32_t)(\s+'+name+r'\s*\()'
            text,count=re.subn(pattern,r'\1BILI_P4_HOT \2\3',text,flags=re.M)
            if count!=1:raise RuntimeError('Expected one hot-kernel definition: '+name)
        target.write_text(placement+text)
    import json
    (dest/'bili_roots.h').write_text('/* Generated public CA trust; no credentials. */\nstatic const char bili_roots[] =\n'+
        '\n'.join(json.dumps(line) for line in ca.splitlines(keepends=True))+';\n')
    (dest/'ca-sha256.txt').write_text(hashlib.sha256(ca.encode()).hexdigest()+'\n')
    # NuttX Application.mk does not invalidate objects on a flag-only change.
    flags_hash=hashlib.sha256((dest/'Makefile').read_bytes()).hexdigest()
    stamp=dest/'build-flags.sha256'
    if not stamp.exists() or stamp.read_text().strip()!=flags_hash:
        for source in dest.rglob('*'):
            if source.suffix in ('.c','.cpp'):source.touch()
        stamp.write_text(flags_hash+'\n')
    print('Installed direct-live: existing desktop TLS, pinned OpenH264')
if __name__=='__main__':main()
