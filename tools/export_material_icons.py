#!/usr/bin/env python3
"""Export the installed Material artwork and Fluent-style state preview."""
import argparse
import html
import json
from pathlib import Path
import shutil
import zipfile
import generate_glass_icons as icons
from material_icon_shapes import ACCENTS, FILLED, MAPPING, ASSETS, RUNNING, raster_paths
from icon_motion_profiles import profile, svg_frames
from material_icon_design import DESIGNS

LABELS=dict(zip('camera mic chat settings bell heart audio headphones image directory wifi power device_lamp device_fan device_curtain device_washer device_camera device_lock search grid play pause download upload sliders cloud_sun device_plug device_sensor device_contact device_climate device_media device_generic device_purifier device_vacuum device_humidifier device_heater device_kitchen device_plant'.split(),
    '相机 语音 对话 设置 通知 收藏 音乐 耳机 相册 文件 无线网络 电源 台灯 风扇 窗帘 洗衣机 安防相机 门锁 搜索 应用 播放 暂停 下载 上传 调节 天气 插座 传感器 门窗传感器 空调 电视 家居 空气净化器 扫地机 加湿器 暖气 厨房 绿植'.split()))
LABELS.update({name:spec['label'] for name,spec in DESIGNS.items()})
LABELS.update({'body_'+name:LABELS['device_'+name]+'底色' for name in
              [n.removeprefix('body_') for n in MAPPING if n.startswith('body_')]})
LABELS['degree']='度数符号'


def running_frames(run):
    x,y=run['direction']
    move=lambda n:f'translate(calc(var(--direction)*{x*n}px),calc(var(--direction)*{y*n}px))'
    return {
        1:'0%,100%{opacity:.86}50%{opacity:1}',
        2:'from{transform:rotate(0deg)}to{transform:rotate(360deg)}',
        3:f'0%{{transform:translate(0,0);opacity:0}}20%{{opacity:1}}70%{{opacity:1}}100%{{transform:translate({x*1.5}px,{y*1.5}px);opacity:0}}',
        4:'0%,100%{opacity:.863}25%{opacity:1}75%{opacity:.725}',
        5:f'0%,100%{{transform:translate(0,0)}}25%{{transform:{move(1)}}}75%{{transform:{move(-1)}}}',
        6:'0%,100%{transform:rotate(0deg)}25%{transform:rotate(calc(var(--direction)*6deg))}75%{transform:rotate(calc(var(--direction)*-6deg))}',
        7:'0%,25%,50%,75%,100%{transform:scale(1)}12.5%,62.5%{transform:scale(1.06)}',
        8:'0%,100%{opacity:.706}25%{opacity:1}75%{opacity:.412}',
        9:'0%,76%,92%,100%{transform:scaleY(1)}84%{transform:scaleY(.25)}',
    }[run['kind']]


def svg(name, active=False, animated=False, size=32, running=False):
    if not name.startswith('home_'):
        animated=False
        running=False
    p=profile(name,len(icons.I[name]))
    run=RUNNING.get(name)
    parts=[]
    if run:
        for shape,flags in run['extra']:
            if flags&128:continue
            if flags&0x0f00:
                paint={256:'var(--icon-surface,#e6f0fb)',512:'var(--icon-recess,#c6ddf4)',1024:'#ffffff',2048:'var(--icon-warm,#f4e1ca)'}[flags&0x0f00]
                motion=f' gesture-{name} run-{name}' if flags&16 else ''
                if flags&8192:motion+=' work-warm'
                direction=-1 if flags&4 else 1
                parts.append(f'<path class="material-plane{motion}" d="{shape.svg_path}" style="fill:{paint};--direction:{direction};--delay:0ms;--duration:520ms;--run-delay:0ms"/>')
                continue
            if flags==32:
                parts.append(f'<defs><linearGradient id="beam-{name}" x1="0" y1="11" x2="0" y2="22" gradientUnits="userSpaceOnUse"><stop stop-color="#f3b84b"/><stop offset="1" stop-color="#f3b84b" stop-opacity="0"/></linearGradient></defs>')
            parts.append(f'<path class="run-light {"beam" if flags==32 else "floor"}" d="{shape.svg_path}" style="fill:{"url(#beam-"+name+")" if flags==32 else "#f3b84b"}"/>')
    parts.append(f'<path class="wash" d="{FILLED[name].svg_path}"/>')
    for i,shape in enumerate(icons.I[name]):
        delay=p['delays'].get(i,0)*.52
        classes=('accent ' if i in ACCENTS[name] else '')+('gesture-'+name if i in p['moving'] else '')
        if name.startswith('home_') and i not in p['moving']:classes+=' shell'
        if run and i in run.get('idle_hidden',set()):classes+=' work-signal'
        if run and run.get('natural_accent') and i in ACCENTS[name]:classes+=' natural-material'
        if run and i in run['moving']:classes+=' run-'+name
        run_delay=run['delays'].get(i,0)*run['period']/1000 if run else 0
        parts.append(f'<path class="{classes}" d="{shape.svg_path}" style="--direction:{-1 if i in p["reverse"] else 1};--delay:{delay}ms;--duration:{520-delay}ms;--run-delay:{run_delay}ms"/>')
    if run:
        for shape,flags in run['extra']:
            if flags&128:
                parts.append(f'<path class="local-ink accent gesture-{name} run-{name}" d="{shape.svg_path}" style="--direction:1;--delay:0ms;--duration:520ms;--run-delay:0ms"/>')
    style=f'''<style>
    @keyframes material-{name}{{{svg_frames(p)}}}
    .ms-icon .pose{{transform-origin:12px 12px;transition:transform 80ms ease-out}}
    .ms-icon path{{fill:currentColor}}
    .ms-icon .wash{{fill:var(--icon-accent,#3975db);opacity:0}}
    .ms-icon .local-ink{{opacity:0}}
    .ms-icon:is(.is-selected,.is-hover,.is-pressed,.play,.is-running) .local-ink{{opacity:1}}
    .ms-icon.is-selected .wash{{opacity:.235}}
    .ms-icon.is-hover .wash,.ms-icon.is-pressed .wash{{opacity:.118}}
    .ms-icon.is-selected.is-hover .wash,.ms-icon.is-selected.is-pressed .wash{{opacity:.235}}
    .ms-icon:is(.is-selected,.is-hover,.is-pressed,.play) .accent{{fill:var(--icon-accent,#3975db)}}
    .ms-icon.is-pressed .pose{{transform:scale(.9453125)}}
    .ms-icon.play .gesture-{name}{{transform-origin:{p['pivot'][0]}px {p['pivot'][1]}px;animation:material-{name} var(--duration) cubic-bezier(.4,0,.6,1) var(--delay) 1}}
    @media(prefers-reduced-motion:reduce){{.ms-icon .pose{{transition:none}}.ms-icon.play path{{animation:none!important}}}}
    .reduce .ms-icon .pose{{transition:none}}.reduce .ms-icon.play path{{animation:none!important}}
    </style>'''
    if run:
        frames=running_frames(run)
        style+=f'''<style>
        @keyframes household-touch{{0%,100%{{fill-opacity:1}}45%{{fill-opacity:.88}}}}
        .ms-icon.play:not(.is-running) .shell{{animation:household-touch 520ms ease-in-out 1}}
        @keyframes running-{name}{{{frames}}}
        @keyframes lamp-light{{0%,100%{{opacity:.35}}50%{{opacity:.42}}}}
        @keyframes lamp-floor{{0%,100%{{opacity:.21}}50%{{opacity:.28}}}}
        @keyframes boot-icon{{from{{opacity:.2}}to{{opacity:1}}}}
        @keyframes lamp-gesture{{0%,100%{{opacity:0}}44%{{opacity:.39}}}}
        .ms-icon .run-light{{opacity:0}}
        .ms-icon.is-running .wash{{opacity:.235}}
        .ms-icon.is-running .accent{{fill:var(--icon-accent,#3975db)}}
        .ms-icon.is-running .run-bulb{{fill:#f3b84b}}
        .ms-icon.play:not(.is-running) .run-light{{animation:lamp-gesture 520ms ease-in-out 1}}
        .ms-icon.is-running .run-{name}{{transform-origin:{run['pivot'][0]}px {run['pivot'][1]}px;animation:running-{name} {run['period']}ms linear calc(var(--run-delay) - {run['period']}ms) infinite,boot-icon 240ms ease-out 1}}
        .ms-icon.is-running .beam{{animation:lamp-light 2400ms ease-in-out infinite}}
        .ms-icon.is-running .floor{{animation:lamp-floor 2400ms ease-in-out infinite}}
        @media(prefers-reduced-motion:reduce){{.ms-icon.is-running .pose,.ms-icon.is-running path{{animation:none!important}}.ms-icon.is-running .beam{{opacity:.39}}.ms-icon.is-running .floor{{opacity:.25}}}}
        .reduce .ms-icon.is-running .pose,.reduce .ms-icon.is-running path{{animation:none!important}}.reduce .ms-icon.is-running .beam{{opacity:.39}}.reduce .ms-icon.is-running .floor{{opacity:.25}}
        </style>'''
    state=' is-selected' if active else ''
    if animated:state+=' play'
    if running:state+=' is-running'
    credit='Authored ESPClaw household artwork' if name.startswith('home_') else 'Adapted Google Material Symbols Rounded'
    if name.startswith('home_'):
        natural=f'.ms-icon .natural-material{{fill:#{run["color"]:06x}!important}}' if run.get('natural_accent') else ''
        style+=f'''<style>
        .ms-icon:not(.is-running) .accent{{fill:var(--icon-idle,#98a5b5)}}
        .ms-icon.is-running .accent{{fill:#{run['color']:06x}}}
        .ms-icon:not(.is-running) .work-signal,.ms-icon:not(.is-running) .run-light{{opacity:0!important;animation:none!important}}
        .ms-icon:not(.is-running) .work-warm{{fill:var(--icon-surface,#e6f0fb)!important}}
        {natural}</style>'''
    if not name.startswith('home_'):
        style+='<style>.ms-icon path,.ms-icon .pose{animation:none!important;transition:none!important;transform:none!important}.ms-icon .run-light{opacity:0!important}</style>'
    paint=' style="color:var(--icon-ink,#3f6ca2)"' if name.startswith('home_') else ''
    return f'<svg xmlns="http://www.w3.org/2000/svg" class="ms-icon{state}"{paint} viewBox="0 0 24 24" width="{size}" height="{size}" role="img"><title>{html.escape(LABELS.get(name,name))}</title><!-- {credit}; Apache-2.0. -->'+style+'<g class="pose">'+''.join(parts)+'</g></svg>'


def preview(output):
    from PIL import Image,ImageDraw,ImageFont
    canvas=Image.new('RGB',(1440,1120),'#f4f6fa');d=ImageDraw.Draw(canvas)
    font_path=next(p for p in [Path('C:/Windows/Fonts/msyh.ttc'),Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')] if p.exists())
    def font(size):return ImageFont.truetype(str(font_path),size)
    def icon(name,x,y,size,color,selected=False,accent='#3975db'):
        if selected:
            mask=raster_paths([FILLED[name].svg_path],size).point(lambda p:round(p*.235))
            canvas.paste(accent,(x,y),mask)
        for i,shape in enumerate(icons.I[name]):
            canvas.paste(accent if selected and i in ACCENTS[name] else color,(x,y),icons.raster(name,size,[shape]))
    d.text((64,38),'ESPCLAW  /  MATERIAL SYMBOLS × FLUENT MOTION',font=font(16),fill='#61718a')
    d.text((60,80),'熟悉的轮廓，清晰的状态。',font=font(42),fill='#172c48')
    d.text((64,150),'圆角矢量  ·  双色激活  ·  交互触发动效',font=font(19),fill='#61718a')
    names=list(LABELS)[:18]
    for i,name in enumerate(names):
        x,y=64+(i%6)*222,218+(i//6)*176
        d.rounded_rectangle((x,y,x+202,y+154),18,fill='white')
        icon(name,x+68,y+19,64,'#25364b',True)
        d.text((x+101,y+113),LABELS[name],font=font(17),fill='#52657d',anchor='mt')
    d.rounded_rectangle((64,776,1376,1048),22,fill='#18273b')
    d.text((96,803),'默认 → 选中',font=font(22),fill='#e9eff8')
    d.text((96,842),'保持完整轮廓，以填充与功能部件表达激活。',font=font(16),fill='#9db0cb')
    for i,name in enumerate(['camera','settings','heart','mic','device_washer','device_lock']):
        x=100+i*144
        icon(name,x,911,36,'#e4edf7')
        icon(name,x+49,911,36,'#e4edf7',True,'#93b8ff')
    d.line((1000,812,1000,1014),fill='#354961')
    d.text((1032,817),'实际像素尺寸',font=font(17),fill='#b0c0d6')
    for i,size in enumerate([14,16,20,24,32]):
        x=1032+i*62
        icon('camera',x+(32-size)//2,910+(32-size)//2,size,'#e4edf7',True,'#93b8ff')
        d.text((x+16,969),str(size),font=font(14),fill='#9db0cb',anchor='mt')
    d.text((64,1074),'设计预览 · 原生 LVGL 页面与动画关键帧另附',font=font(15),fill='#6b7d93')
    canvas.save(output/'preview.png')


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    out=parser.parse_args().output;out.mkdir(parents=True,exist_ok=True)
    for folder in ('svg','selected','animated','running'):(out/folder).mkdir(exist_ok=True)
    cards=[];manifest=[]
    for name,code in sorted(icons.SYMBOLS.items(),key=lambda p:p[1]):
        for folder,active,animated in [('svg',False,False),('selected',True,False),('animated',True,True)]:
            (out/folder/f'{name}.svg').write_text(svg(name,active,animated),encoding='utf-8')
        if name in RUNNING:(out/'running'/f'{name}.svg').write_text(svg(name,True,size=64,running=True),encoding='utf-8')
        group='aux' if name.startswith('body_') or name=='degree' else 'files' if name.startswith('file_') else 'home' if name.startswith('device_') else 'system'
        label=LABELS.get(name,name)
        detail=RUNNING[name]['detail'] if name in RUNNING else '配套静态资源，与主体保持对齐'
        cards.append(f'<button class="tile" data-name="{name}" data-label="{label}" data-group="{group}" aria-pressed="false"><span class="glyph">{svg(name)}</span><strong>{label}</strong><small>{detail}</small><small>{MAPPING[name]}</small></button>')
        run=RUNNING.get(name)
        manifest.append(dict(name=name,label=label,codepoint=f'U+{code:04X}',material_symbol=MAPPING[name],group=group,
            motion=profile(name,len(icons.I[name]))['kind'],operating=bool(run),detail=detail,
            mechanism=sorted(run['moving']) if run else [],fixed_parts=sorted(set(range(len(icons.I[name])))-run['moving']) if run else list(range(len(icons.I[name]))),
            operating_kind=run['kind'] if run else 0,period_ms=run['period'] if run else 0,
            operating_direction=run['direction'] if run else [0,0],auxiliary_of=name.replace('body_','device_') if name.startswith('body_') else None))
    page='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESPClaw · Material × Fluent 图标</title><style>
    *{box-sizing:border-box}body{margin:0;color:#243750;background:#f4f6fa;font:15px system-ui,'Microsoft YaHei',sans-serif;--icon-accent:#3975db}main{max-width:1320px;margin:auto;padding:48px 32px}header{display:grid;grid-template-columns:1fr 220px;gap:20px;align-items:center;margin-bottom:32px}.eyebrow{letter-spacing:2px;font-size:11px;color:#617694}h1{font-size:42px;letter-spacing:-1.8px;line-height:1.3;margin:16px 0}p{color:#697d94;line-height:1.9}a{color:var(--icon-accent)}.hero{display:grid;place-items:center;width:180px;height:180px;border-radius:40px;background:#e7effe}.hero svg{width:96px;height:96px}.meta{display:flex;gap:12px;flex-wrap:wrap}.pill{padding:5px 10px;border-radius:20px;background:#e7edf6;font-size:12px}nav{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:24px 0}input,select,nav button{font:inherit;color:inherit;padding:11px 14px;border-radius:11px;border:1px solid #d8e1ed;background:#fff}input{min-width:240px}button{cursor:pointer}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(152px,1fr));gap:12px}.tile{min-height:154px;padding:20px 10px;display:flex;flex-direction:column;align-items:center;gap:12px;border:1px solid transparent;border-radius:17px;background:#fff;color:inherit}.tile:hover,.tile:focus-visible{border-color:#a5bee6;outline:2px solid transparent}.tile[aria-pressed=true]{background:#e9f0ff;border-color:#92b2eb}.glyph{height:68px;display:grid;place-items:center}.glyph svg{width:var(--size,40px);height:var(--size,40px)}strong{font-size:13px;font-weight:600}small{font-size:10px;color:#8492a7}.tile[hidden]{display:none}footer{margin-top:40px;font-size:12px;color:#71849b;line-height:1.9}.status{margin-left:auto;color:#71849b;font-size:13px}.dark{background:#122032;color:#e4edf8;--icon-accent:#93b8ff}.dark .tile,.dark input,.dark select,.dark nav button{background:#1d3048;color:#e4edf8}.dark .tile[aria-pressed=true],.dark .hero{background:#2a4163}.dark .pill{background:#263a55}.dark p{color:#a1b4ce}nav button[aria-pressed=true]{border-color:#3975db}.states{display:flex;gap:12px;overflow:auto;padding:18px 22px;background:#fff;border-radius:18px}.states div{min-width:150px;text-align:center}.states svg{display:block;margin:0 auto 12px;width:40px;height:40px}.dark .states{background:#1d3048}@media(max-width:650px){main{padding:28px 18px}header{grid-template-columns:1fr}.hero{display:none}h1{font-size:33px}.status{margin-left:0}.grid{grid-template-columns:repeat(2,1fr)}}
    </style><main><header><div><div class="eyebrow">ESPCLAW / MATERIAL × FLUENT</div><h1>熟悉的轮廓，<br>清晰的状态。</h1><p>Google Material Symbols 圆角图形，搭配微软 AnimatedIcon 状态设计。<br>移入、按下或点击图标体验反馈；点击会保留选中状态。</p><div class="meta"><span class="pill">137 个映射资源</span><span class="pill">118 个交互图标</span><span class="pill">6 档原生尺寸</span></div></div><div class="hero">__HERO__</div></header><div class="states">__STATES__</div>
    <nav><input id="search" type="search" aria-label="搜索图标" placeholder="搜索名称、用途或 Google 图标名"><select id="group" aria-label="图标类别"><option value="all">交互图标</option><option value="system">系统与媒体</option><option value="home">家居设备</option><option value="files">文件类型</option><option value="aux">辅助遮罩</option></select><select id="size" aria-label="图标尺寸"><option>14</option><option>16</option><option>20</option><option>24</option><option selected>32</option><option>64</option></select><button id="theme">深色预览</button><button id="reduced" aria-pressed="false">减少动态效果</button><a href="material-fluent-icons.zip">下载 SVG</a><span id="count" class="status"></span></nav><div class="grid">__CARDS__</div><footer>此处为离线 SVG 交互预览；native/ 与 pages/ 保存实际 LVGL 宿主渲染。尚未烧录设备。<br>图形：<a href="https://fonts.google.com/icons">Google Material Symbols</a> · Apache-2.0；动效参考：<a href="https://learn.microsoft.com/zh-cn/windows/apps/develop/ui/controls/animated-icon">Microsoft AnimatedIcon</a>。SVG 已为分层动画和双色状态适配。</footer></main><script>
    const tiles=[...document.querySelectorAll('.tile')],reduce=document.querySelector('#reduced');
    function play(s){s.classList.remove('play');void s.getBoundingClientRect();s.classList.add('play')}
    for(const tile of tiles){const s=tile.querySelector('svg');tile.onpointerenter=()=>{s.classList.add('is-hover');play(s)};tile.onpointerleave=()=>{s.classList.remove('is-hover','is-pressed')};tile.onpointerdown=()=>s.classList.add('is-pressed');tile.onpointerup=tile.onpointercancel=()=>s.classList.remove('is-pressed');tile.onfocus=()=>s.classList.add('is-hover');tile.onblur=()=>s.classList.remove('is-hover','is-pressed');tile.onkeydown=e=>{if(e.key===' '||e.key==='Enter')s.classList.add('is-pressed')};tile.onkeyup=()=>s.classList.remove('is-pressed');tile.onclick=()=>{const selected=tile.getAttribute('aria-pressed')!=='true';tile.setAttribute('aria-pressed',selected);s.classList.toggle('is-selected',selected);play(s)}}
    function filter(){const q=document.querySelector('#search').value.toLowerCase(),g=document.querySelector('#group').value;let n=0;for(const t of tiles){const visible=(g==='all'?t.dataset.group!=='aux':g===t.dataset.group)&&t.textContent.toLowerCase().concat(t.dataset.name).includes(q);t.hidden=!visible;if(visible)n++}document.querySelector('#count').textContent=n+' 个图标'}
    document.querySelector('#search').oninput=filter;document.querySelector('#group').onchange=filter;filter();document.body.style.setProperty('--size','32px');document.querySelector('#size').onchange=e=>document.body.style.setProperty('--size',e.target.value+'px');document.querySelector('#theme').onclick=e=>{const dark=document.body.classList.toggle('dark');e.target.textContent=dark?'浅色预览':'深色预览'};reduce.onclick=()=>{const r=document.body.classList.toggle('reduce');reduce.setAttribute('aria-pressed',r)};
    </script></html>'''
    states=[]
    for title,cls in [('默认',''),('移入','is-hover'),('按下','is-pressed'),('选中','is-selected'),('选中 · 移入','is-selected is-hover'),('选中 · 按下','is-selected is-pressed')]:
        states.append('<div>'+svg('camera').replace('class="ms-icon"',f'class="ms-icon {cls}"')+'<small>'+title+'</small></div>')
    demos=[]
    for name in ['device_lamp','device_washer','device_fan','device_humidifier','device_purifier','device_climate','device_media','device_camera','mic','wifi','download','settings']:
        running=name in ('device_lamp','device_washer')
        detail=RUNNING[name]['detail']
        demos.append(f'<article class="run-card"><div class="run-glyph">{svg(name,running,size=64,running=running)}</div><strong>{LABELS.get(name,name)}</strong><p>{detail}</p><button class="run-toggle" aria-pressed="{str(running).lower()}">{"关闭" if running else "开机"}</button><small class="run-status">{"运行中" if running else "已关闭"}</small></article>')
    run_section='<section class="run-section"><h2>让图标表达工作状态</h2><p>开机 → 运行 → 关闭。下方开关只用于预览，不连接真实家居设备。</p><div class="run-grid">'+''.join(demos)+'</div></section>'
    run_css='.run-section{margin:36px 0}.run-section h2{font-size:27px}.run-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(175px,1fr));gap:14px}.run-card{padding:24px 16px;text-align:center;border-radius:20px;background:white}.run-glyph{height:92px;display:grid;place-items:center}.run-glyph svg{width:72px;height:72px}.run-card strong{font-size:16px}.run-card p{font-size:12px;margin:7px 0 14px}.run-toggle{border:0;border-radius:18px;padding:8px 24px;background:#e9eef5;color:#526780}.run-toggle[aria-pressed=true]{background:#e2edff;color:#2865c2}.run-status{display:block;margin-top:10px}.dark .run-card{background:#1d3048}.dark .run-toggle{background:#314761;color:#c4d5ed}'
    page=page.replace('</style><main>',run_css+'</style><main>',1).replace('<div class="states">',run_section+'<div class="states">',1)
    run_js="document.querySelectorAll('.run-toggle').forEach(b=>b.onclick=()=>{const on=b.getAttribute('aria-pressed')!=='true',card=b.closest('.run-card'),s=card.querySelector('svg');b.setAttribute('aria-pressed',on);b.textContent=on?'关闭':'开机';card.querySelector('.run-status').textContent=on?'运行中':'已关闭';s.classList.toggle('is-running',on);s.classList.toggle('is-selected',on)});"
    page=page.replace('</script>',run_js+'</script>')
    page=page.replace('native/ 与 pages/','native/ 与 appliances/')
    page=page.replace('<button id="theme">','<button id="run-all" aria-pressed="false">运行当前分类</button><button id="theme">',1)
    page=page.replace('6 档原生尺寸</span>','6 档原生尺寸</span><span class="pill">118 种功能动效</span>',1)
    page=page.replace('移入、按下或点击图标体验反馈；点击会保留选中状态。','逐项定义功能部件的动作；移入、按下体验反馈，按类别预览运行效果。',1)
    page=page.replace('</style><main>', '.tile[hidden] path{animation-play-state:paused!important}.tile.is-outside path{animation-play-state:paused!important}</style><main>',1)
    page=page.replace('</script>', '''
    const runAll=document.querySelector('#run-all');let batchRunning=false;
    function syncRun(){for(const t of tiles){const on=batchRunning&&!t.hidden&&t.dataset.group!=='aux',s=t.querySelector('svg');s.classList.toggle('is-running',on);s.classList.toggle('is-selected',on||t.getAttribute('aria-pressed')==='true')}runAll.setAttribute('aria-pressed',batchRunning);runAll.textContent=batchRunning?'停止运行预览':'运行当前分类'}
    runAll.onclick=()=>{batchRunning=!batchRunning;syncRun()};
    document.querySelector('#search').addEventListener('input',syncRun);document.querySelector('#group').addEventListener('change',syncRun);
    const observer=new IntersectionObserver(entries=>entries.forEach(e=>e.target.classList.toggle('is-outside',!e.isIntersecting)),{rootMargin:'100px'});tiles.forEach(t=>observer.observe(t));
    document.querySelectorAll('.ms-icon').forEach(s=>s.addEventListener('animationend',e=>{if(e.animationName.startsWith('material-'))s.classList.remove('play')}));
    </script>''')
    page=page.replace('__HERO__',svg('camera',True)).replace('__STATES__',''.join(states)).replace('__CARDS__',''.join(cards))
    (out/'index.html').write_text(page,encoding='utf-8')
    (out/'manifest.json').write_text(json.dumps(dict(family='Material Symbols Rounded × Fluent state motion',sizes=[14,16,20,24,32,64],icons=manifest),ensure_ascii=False,indent=2),encoding='utf-8')
    shutil.copy2(ASSETS/'LICENSE',out/'LICENSE')
    shutil.copy2(ASSETS/'sources.json',out/'sources.json')
    (out/'NOTICE.txt').write_text('Google Material Symbols Rounded, Apache License 2.0.\nAdapted: normalized paths, separated contours, duotone states, state transitions and operating animations.\nMicrosoft documentation is a design reference; no WinUI or Lottie runtime is bundled.\n',encoding='utf-8')
    with zipfile.ZipFile(out/'material-fluent-icons.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for folder in ('svg','selected','animated','running'):
            for path in sorted((out/folder).glob('*.svg')):archive.write(path,path.relative_to(out))
        for name in ('manifest.json','sources.json','LICENSE','NOTICE.txt'):archive.write(out/name,name)
    preview(out)
    print(f'Exported {len(manifest)} mappings and {len(manifest)*3+len(RUNNING)} SVG assets to {out}')


if __name__=='__main__':main()
