"""Decode a saved, ELF-matched SD/WiFi timing record; no device access."""
import argparse,json,struct
from pathlib import Path


def decode(data,layout):
    types=layout['types'];rec=types['io_cause_s'];f=rec['fields']
    if len(data)!=2*rec['bytes']:
        raise ValueError('Dump length differs from this ELF record array')
    out=[]
    for kind in range(2):
        block=data[kind*rec['bytes']:(kind+1)*rec['bytes']]
        def scalar(name,fmt='<I'):
            return struct.unpack_from(fmt,block,f[name])[0]
        valid=scalar('valid')
        if valid==0:
            out.append({'kind':kind,'name':['sd_spi_token','wifi_sdmmc'][kind],
                        'frozen':False});continue
        if valid!=2 or scalar('magic')!=0x494f4331 or scalar('version')!=1 or scalar('bytes')!=rec['bytes'] or scalar('kind')!=kind:
            raise ValueError('Incomplete or mismatched I/O provenance record')
        wide={'start_us','end_us','off_since_us','off_total_us','off_max_us',
              'poll_gap_max_us','spi_call_max_us'}
        arrays={'events','irq_count','irq_max_us','irq_max_id','irq_max_pid','irq_at_end'}
        r={name:scalar(name,'<Q' if name in wide else '<i' if name in ['owner_pid','error'] else '<I') for name in f if name not in arrays}
        if r['end_us']<r['start_us']:
            raise ValueError('Invalid monotonic timing interval')
        r.update(name=['sd_spi_token','wifi_sdmmc'][kind],frozen=True,
                 elapsed_us=r['end_us']-r['start_us'],
                 elapsed_ticks=(r['end_tick']-r['start_tick'])&0xffffffff)
        r['off_open_us']=r['end_us']-r['off_since_us'] if r['suspended'] else 0
        r['off_including_open_us']=r['off_total_us']+r['off_open_us']
        ev=types['io_cause_event_s'];events=[]
        for seq in range(max(0,r['event_count']-16),r['event_count']):
            base=f['events']+(seq%16)*ev['bytes']
            e={'sequence':seq}
            for name,offset in ev['fields'].items():
                fmt='<Q' if name=='us' else '<i' if name in ['pid','irq'] else '<B'
                e[name]=struct.unpack_from(fmt,block,base+offset)[0]
            events.append(e)
        r['scheduler_events']=events;r['irq_cpus']=[]
        irq=types['io_irq_context_s']
        for cpu in range(2):
            c={name:struct.unpack_from(fmt,block,f[name]+cpu*width)[0] for name,fmt,width in [('irq_count','<I',4),('irq_max_us','<Q',8),('irq_max_id','<i',4),('irq_max_pid','<i',4)]}
            base=f['irq_at_end']+cpu*irq['bytes']
            c['at_freeze']={name:struct.unpack_from('<Q' if name=='since_us' else '<I' if name=='depth' else '<i',block,base+offset)[0] for name,offset in irq['fields'].items()}
            current=c['at_freeze']
            c['open_irq_in_window_us']=(r['end_us']-max(r['start_us'],current['since_us'])) if current['depth'] else 0
            r['irq_cpus'].append(c)
        out.append(r)
    if not any(r['frozen'] for r in out):
        raise ValueError('No complete first-fault record in this saved dump')
    return {'records':out,'notes':[
        'IRQ times bracket software dispatch, not hardware assertion or IRQ-disabled intervals.',
        'Suspend/resume intervals include IRQ handling, blocking and preemption.',
        'SPI call and poll gaps include interruption; they are not hardware-only latency.',
        'speed_requested is configured SD speed, not a measured bus frequency.',
        'No SD command/LBA is captured by this diagnostic window.',
        'Frozen SD and WiFi windows may be different transactions and times.',
        'A saved dump is not a new board observation.']}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--layout',type=Path,required=True)
    parser.add_argument('--dump',type=Path,required=True)
    parser.add_argument('--output',type=Path)
    a=parser.parse_args();result=decode(a.dump.read_bytes(),json.loads(a.layout.read_text()))
    text=json.dumps(result,indent=2,ensure_ascii=False)+'\n'
    if a.output:
        with a.output.open('x',encoding='utf-8') as f:f.write(text)
    else:print(text,end='')
