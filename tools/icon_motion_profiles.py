"""Semantic gestures shared by the native mask compiler and SVG exporter.

Delay is in thousandths of the 520 ms timeline, never an extra repeat.
All gestures finish at the original geometry; they do not signal device state.
"""
from refined_icon_shapes import MOVING


def profile(name, count):
    from material_icon_shapes import PROFILES
    if name in PROFILES:
        return PROFILES[name]
    kind, pivot, direction = 1, (12, 12), (0, 0)
    moving = MOVING.get(name, set(range(count)))
    delays, reverse = {}, set()
    if name in ('fan', 'device_fan', 'refresh', 'loop', 'sun'):
        kind = 2
        if name in ('fan', 'device_fan'):
            pivot = (12, 9)
        if name in ('refresh', 'loop'):
            moving = set(range(count))
    elif name in ('settings', 'sliders', 'list', 'bars', 'toggle'):
        kind = 3
        if name in ('settings', 'sliders'):
            reverse = {5}
        if name == 'toggle':
            moving = {1}
    elif name in ('heart', 'camera', 'device_camera', 'bulb', 'device_lamp', 'power', 'device_washer'):
        kind = 4
        moving = {'bulb': {0}, 'device_lamp': {0}, 'power': {1}}.get(name, moving)
        pivot = {'camera': (12, 13), 'device_camera': (12, 9), 'device_washer': (12, 14)}.get(name, pivot)
    elif name in ('ok', 'play', 'plus', 'minus', 'stop'):
        kind = 5
    elif name in ('mic', 'bluetooth', 'qr', 'device_sensor', 'device_contact'):
        kind = 6
        moving = {'device_sensor': {1}, 'device_contact': {1}}.get(name, moving)
    elif name == 'device_curtain':
        kind, reverse = 7, {1, 3}
    elif name in ('bell', 'call'):
        kind, pivot = 8, (12, 4)
    elif name in ('download', 'upload', 'left', 'right', 'up', 'down', 'arrow_ne', 'prev', 'next'):
        kind = 9
        direction = {'download': (0, 1), 'upload': (0, -1), 'left': (-1, 0), 'right': (1, 0),
                     'up': (0, -1), 'down': (0, 1), 'arrow_ne': (1, -1), 'prev': (-1, 0), 'next': (1, 0)}[name]
        if name in ('prev', 'next'):
            moving = {1}
    elif name in ('wifi', 'radio', 'volume_mid', 'volume_max', 'grid', 'chat', 'ellipsis', 'sparkles') or name.startswith('battery_'):
        kind = 10
        moving = {'chat': {1, 2}, 'volume_mid': {1}, 'volume_max': {1, 2}}.get(name, set(range(count)))
        if name.startswith('battery_'):
            moving = set(range(2, count)) or {0}
        order = sorted(moving, reverse=name == 'wifi')
        delays = {index: min(rank * 60, 180) for rank, index in enumerate(order)}
    elif name in ('trash', 'envelope', 'device_lock'):
        kind = 11
        moving = {'trash': {0, 1}, 'envelope': {1}, 'device_lock': {1}}[name]
        pivot = (12, 6)
    elif name == 'device_plant':
        kind, moving, pivot, reverse = 12, {2, 3}, (12, 12), {3}
    elif name in ('device_humidifier', 'device_climate', 'device_heater', 'cloud_rain', 'cloud_snow'):
        kind = 13
        moving = {'device_humidifier': {1, 2}, 'device_climate': {2, 3, 4},
                  'device_heater': {3, 4, 5}, 'cloud_rain': {1, 2, 3}, 'cloud_snow': {1, 2, 3}}[name]
        direction = (0, 1 if name.startswith('cloud_') else -1)
        delays = {index: rank * 70 for rank, index in enumerate(sorted(moving))}
    return dict(kind=kind, pivot=pivot, direction=direction, moving=moving, delays=delays, reverse=reverse)


def svg_frames(p):
    """Equivalent SVG gestures, with the same per-layer sequencing as LVGL."""
    kind = p['kind']
    if kind == 14:
        return '0%,100%{transform:rotate(0deg)}44%{transform:rotate(24deg)}'
    if kind == 2:
        return 'from{transform:rotate(0deg)}to{transform:rotate(360deg)}'
    if kind in (4, 5):
        peak, pre = 'scale(1.09)', 'scale(.98)'
    elif kind in (3, 7):
        peak, pre = 'translateX(calc(var(--direction)*1.5px))', 'translateX(calc(var(--direction)*-.375px))'
    elif kind == 6:
        return '0%,100%{opacity:1}44%{opacity:.53}'
    elif kind == 8:
        return '0%,100%{transform:rotate(0deg)}20%{transform:rotate(-12deg)}45%{transform:rotate(12deg)}70%{transform:rotate(-5deg)}'
    elif kind == 9:
        x, y = p['direction']
        peak, pre = f'translate(calc(var(--direction)*{x*1.5}px),calc(var(--direction)*{y*1.5}px))', f'translate(calc(var(--direction)*{x*-.375}px),calc(var(--direction)*{y*-.375}px))'
    elif kind == 10:
        return '0%,100%{opacity:1;transform:translateY(0)}44%{opacity:.4;transform:translateY(-.5px)}'
    elif kind == 11:
        peak, pre = 'translateY(-1.5px) rotate(-6deg)', 'translateY(.375px) rotate(1.5deg)'
    elif kind == 12:
        peak, pre = 'rotate(calc(var(--direction)*8deg))', 'rotate(calc(var(--direction)*-2deg))'
    elif kind == 13:
        x,y = p['direction']
        return f'0%,100%{{opacity:1;transform:translate(0,0)}}44%{{opacity:.45;transform:translate({x*.75}px,{y*.75}px)}}'
    else:
        peak, pre = 'rotate(9deg) translateY(-1px)', 'rotate(-2.25deg) translateY(.25px)'
    return f'0%,100%{{transform:none}}16%{{transform:{pre}}}44%{{transform:{peak}}}72%{{transform:{pre}}}'
