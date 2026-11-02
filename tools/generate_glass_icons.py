#!/usr/bin/env python3
"""Generate the authored Calm UI icon family as native LVGL 4-bit fonts.

Pillow is a build dependency; the device uses compiled fonts and A8 masks.
LV_SYMBOL codepoints stay compatible with existing labels and QuickJS apps.
"""
import argparse
import math
from pathlib import Path


class Shape(tuple):
    """Raster-compatible geometry with optional exact SVG construction."""
    def __new__(cls, mode, points, svg_path=None):
        result = super().__new__(cls, (mode, points))
        result.svg_path = svg_path
        return result


class CurvePoints(list):
    def __init__(self, points, commands):
        super().__init__(points)
        self.commands = commands


def line(*points):
    return Shape('line', points)


def circle(x, y, radius, start=0, end=360):
    count = max(12, round(abs(end - start) / 6))
    shape = line(*[(x + radius * math.cos(math.radians(start + (end - start) * i / count)),
                   y + radius * math.sin(math.radians(start + (end - start) * i / count)))
                 for i in range(count + 1)])
    first = shape[1][0]
    sweep = 1 if end > start else 0
    fmt = lambda p: f'{p[0]:.3f},{p[1]:.3f}'
    if abs(end-start) >= 359.999:
        opposite = (2*x-first[0], 2*y-first[1])
        shape.svg_path = f'M{fmt(first)} A{radius},{radius} 0 1,{sweep} {fmt(opposite)} A{radius},{radius} 0 1,{sweep} {fmt(first)} Z'
    else:
        shape.svg_path = f'M{fmt(first)} A{radius},{radius} 0 {int(abs(end-start)>180)},{sweep} {fmt(shape[1][-1])}'
    return shape


def bezier(start, first, second, end):
    return CurvePoints([((1-t)**3*start[0] + 3*(1-t)**2*t*first[0] + 3*(1-t)*t*t*second[0] + t**3*end[0],
             (1-t)**3*start[1] + 3*(1-t)**2*t*first[1] + 3*(1-t)*t*t*second[1] + t**3*end[1])
            for t in [i/24 for i in range(25)]], f'C{first[0]},{first[1]} {second[0]},{second[1]} {end[0]},{end[1]}')


def rect(x, y, w, h, r=2):
    points = []
    for cx, cy, angle in [(x+w-r,y+r,-90), (x+w-r,y+h-r,0),
                          (x+r,y+h-r,90), (x+r,y+r,180)]:
        points.extend((cx+r*math.cos(math.radians(angle+i*9)),
                       cy+r*math.sin(math.radians(angle+i*9))) for i in range(11))
    shape = line(*points, points[0])
    if r:
        shape.svg_path = (f'M{x+r},{y} H{x+w-r} A{r},{r} 0 0,1 {x+w},{y+r} '
                          f'V{y+h-r} A{r},{r} 0 0,1 {x+w-r},{y+h} '
                          f'H{x+r} A{r},{r} 0 0,1 {x},{y+h-r} '
                          f'V{y+r} A{r},{r} 0 0,1 {x+r},{y} Z')
    else:
        shape.svg_path = f'M{x},{y} H{x+w} V{y+h} H{x} Z'
    return shape


I = {
 'audio': [line((9,18),(9,5),(19,3),(19,16)), circle(6.5,18,2.5),circle(16.5,16,2.5),line((9,8),(19,6))],
 'video': [rect(3,6,12,12),line((15,10),(21,7),(21,17),(15,14))],
 'list': [line((8,6),(21,6)),line((8,12),(21,12)),line((8,18),(21,18)),circle(3.5,6,.5),circle(3.5,12,.5),circle(3.5,18,.5)],
 'ok': [line((4,12),(9,17),(20,6))],
 'close': [line((6,6),(18,18)),line((18,6),(6,18))],
 'power': [circle(12,12,8,-50,230),line((12,2),(12,11))],
 'settings': [line((3,7),(7,7)),line((12,7),(21,7)),circle(9.5,7,2.5),line((3,17),(12,17)),line((17,17),(21,17)),circle(14.5,17,2.5)],
 'home': [line((3,10),(12,3),(21,10)),line((5,9),(5,21),(10,21),(10,14),(14,14),(14,21),(19,21),(19,9))],
 'download': [line((12,3),(12,15)),line((7,10),(12,15),(17,10)),line((4,15),(4,21),(20,21),(20,15))],
 'drive': [rect(3,14,18,7),line((3,16),(6,4),(18,4),(21,16)),circle(17,17.5,.5)],
 'refresh': [circle(12,12,8,30,180),circle(12,12,8,210,360),line((20,4),(20,9),(15,9)),line((4,20),(4,15),(9,15))],
 'mute': [line((10,5),(5,9),(2,9),(2,15),(5,15),(10,19),(10,5)),line((16,9),(22,15)),line((22,9),(16,15))],
 'volume_mid': [line((10,5),(5,9),(2,9),(2,15),(5,15),(10,19),(10,5)),circle(10,12,6,-45,45)],
 'volume_max': [line((10,5),(5,9),(2,9),(2,15),(5,15),(10,19),(10,5)),circle(10,12,6,-45,45),circle(10,12,10,-45,45)],
 'image': [rect(3,3,18,18),circle(8,8,2),line((3,17),(9,11),(15,17)),line((12,14),(16,10),(21,15))],
 'tint': [line((12,2),(5,11),(4,15),(5,19),(8,21),(12,22),(16,21),(19,19),(20,15),(19,11),(12,2))],
 'prev': [line((5,4),(5,20)),line((19,4),(9,12),(19,20),(19,4))],
 'play': [line((7,4),(20,12),(7,20),(7,4))],
 'pause': [rect(5,4,4,16,1),rect(15,4,4,16,1)],
 'stop': [rect(5,5,14,14,2)],
 'next': [line((19,4),(19,20)),line((5,4),(15,12),(5,20),(5,4))],
 'eject': [line((4,15),(12,4),(20,15),(4,15)),line((4,20),(20,20))],
 'left': [line((15,5),(8,12),(15,19))],
 'right': [line((9,5),(16,12),(9,19))],
 'plus': [line((12,4),(12,20)),line((4,12),(20,12))],
 'minus': [line((4,12),(20,12))],
 'eye_open': [line((2,12),(5,7),(9,5),(15,5),(19,7),(22,12),(19,17),(15,19),(9,19),(5,17),(2,12)),circle(12,12,3)],
 'eye_close': [line((2,9),(5,13),(9,15),(15,15),(19,13),(22,9)),line((4,12),(2,16)),line((8,15),(7,19)),line((16,15),(17,19)),line((20,12),(22,16))],
 'warning': [line((12,3),(22,20),(2,20),(12,3)),line((12,8),(12,13)),circle(12,17,.5)],
 'shuffle': [line((3,5),(6,5),(17,19),(21,19)),line((3,19),(6,19),(17,5),(21,5)),line((18,2),(21,5),(18,8)),line((18,16),(21,19),(18,22))],
 'up': [line((5,15),(12,8),(19,15))],
 'down': [line((5,9),(12,16),(19,9))],
 'loop': [line((4,10),(4,6),(20,6),(20,10)),line((17,3),(20,6),(17,9)),line((20,14),(20,18),(4,18),(4,14)),line((7,15),(4,18),(7,21))],
 'directory': [line((3,20),(3,4),(10,4),(12,7),(21,7),(21,20),(3,20))],
 'upload': [line((12,3),(12,15)),line((7,8),(12,3),(17,8)),line((4,15),(4,21),(20,21),(20,15))],
 'call': [line((5,3),(9,3),(10,7),(8,9),(11,13),(15,16),(17,14),(21,15),(21,19),(18,21),(14,20),(9,17),(5,12),(3,7),(5,3))],
 'cut': [circle(6,6,3),circle(6,18,3),line((8,8),(21,21)),line((8,16),(21,3))],
 'copy': [rect(8,8,13,13),line((16,4),(16,3),(3,3),(3,16),(4,16))],
 'save': [line((3,3),(17,3),(21,7),(21,21),(3,21),(3,3)),rect(7,3,9,6,0),rect(7,14,10,7,0)],
 'bars': [line((3,6),(21,6)),line((3,12),(21,12)),line((3,18),(21,18))],
 'envelope': [rect(3,5,18,14),line((3,7),(12,13),(21,7))],
 'charge': [line((13,2),(4,14),(11,14),(10,22),(20,9),(13,9),(13,2))],
 'paste': [rect(5,5,14,17),rect(9,2,6,5,1)],
 'bell': [line((4,18),(6,15),(6,8),(8,5),(12,4),(16,5),(18,8),(18,15),(20,18),(4,18)),circle(12,18,4,40,140)],
 'keyboard': [rect(2,5,20,14),line((6,9),(6.4,9)),line((10,9),(10.4,9)),line((14,9),(14.4,9)),line((18,9),(18.4,9)),line((6,13),(6.4,13)),line((10,13),(10.4,13)),line((14,13),(14.4,13)),line((18,13),(18.4,13)),line((7,16),(17,16))],
 'gps': [line((3,3),(10,21),(13,13),(21,10),(3,3))],
 'file': [line((4,2),(14,2),(20,8),(20,22),(4,22),(4,2)),line((14,2),(14,8),(20,8))],
 'wifi': [circle(12,23,19,235,305),circle(12,23,13,235,305),circle(12,23,7,235,305),circle(12,21,.8)],
 'usb': [line((12,21),(12,3)),line((9,6),(12,3),(15,6)),circle(12,21,1.5),line((12,16),(6,12),(6,8)),circle(6,7,1.3),line((12,13),(18,10),(18,6)),rect(16.5,3,3,3,.3)],
 'bluetooth': [line((12,2),(12,22),(19,16),(5,6)),line((5,18),(19,8),(12,2))],
 'trash': [line((3,6),(21,6)),line((9,6),(9,3),(15,3),(15,6)),line((5,6),(6,21),(18,21),(19,6)),line((10,10),(10,17)),line((14,10),(14,17))],
 'edit': [line((4,16),(4,21),(9,21),(21,9),(15,3),(4,16)),line((12,6),(18,12))],
 'backspace': [line((2,12),(8,5),(22,5),(22,19),(8,19),(2,12)),line((12,9),(18,15)),line((18,9),(12,15))],
 'sd_card': [line((5,2),(14,2),(19,7),(19,22),(5,22),(5,2)),line((8,2),(8,7)),line((11,2),(11,7)),line((14,2),(14,7))],
 'new_line': [line((20,4),(20,14),(4,14)),line((8,10),(4,14),(8,18))],
 'bullet': [circle(12,12,2)],
 'sparkles': [line((10,3),(12,9),(18,11),(12,13),(10,19),(8,13),(2,11),(8,9),(10,3)),line((19,2),(20,5),(23,6),(20,7),(19,10),(18,7),(15,6),(18,5),(19,2))],
 'camera': [line((3,7),(7,7),(9,4),(15,4),(17,7),(21,7),(21,20),(3,20),(3,7)),circle(12,13,4)],
 'grid': [rect(3,3,7,7,1.3),rect(14,3,7,7,1.3),rect(3,14,7,7,1.3),rect(14,14,7,7,1.3)],
 'mic': [rect(9,2,6,13,3),circle(12,12,7,0,180),line((12,19),(12,23)),line((8,23),(16,23))],
 'chat': [line((4,18),(3,21),(8,19),(17,19),(21,15),(21,7),(17,3),(7,3),(3,7),(3,15),(4,18))],
 'arrow_ne': [line((6,18),(18,6)),line((6,6),(18,6),(18,18))],
 'radio': [circle(12,12,2),circle(12,12,6,-60,60),circle(12,12,6,120,240),circle(12,12,10,-60,60),circle(12,12,10,120,240)],
 'fish': [line((3,12),(7,6),(13,4),(20,8),(23,4),(23,20),(20,16),(13,20),(7,18),(3,12)),circle(8,10,.7),line((15,5),(12,12),(15,19))],
 'qr': [rect(3,3,6,6,.5),rect(15,3,6,6,.5),rect(3,15,6,6,.5),line((15,15),(15,21),(21,21),(21,18),(18,18),(18,15),(21,15))],
 'face': [line((3,8),(3,3),(8,3)),line((16,3),(21,3),(21,8)),line((21,16),(21,21),(16,21)),line((8,21),(3,21),(3,16)),circle(8,10,.5),circle(16,10,.5),circle(12,12,5,35,145)],
 'headphones': [circle(12,12,9,180,360),rect(3,11,4,9,1.5),rect(17,11,4,9,1.5)],
 'ellipsis': [circle(5,12,1),circle(12,12,1),circle(19,12,1)],
 'sun': [circle(12,12,4)] + [line((12+7*math.cos(i*math.pi/4),12+7*math.sin(i*math.pi/4)),(12+10*math.cos(i*math.pi/4),12+10*math.sin(i*math.pi/4))) for i in range(8)],
 'heart': [line((12,21),(3,12),(2,7),(5,3),(9,3),(12,6),(15,3),(19,3),(22,7),(21,12),(12,21))],
 'mouse': [rect(6,2,12,20,6),line((12,2),(12,10)),line((6,10),(18,10))],
}
I['sliders'] = I['settings']
I['cloud_sun'] = [circle(8,8,4,170,320),line((8,1),(8,2)),line((1,8),(2,8)),line((3,3),(4,4)),line((13,3),(12,4)),line((2,13),(3,12)),
    line(*bezier((8,21),(2,21),(2,13),(8,13)),
         *bezier((8,13),(8,5),(20,5),(20,14)),
         *bezier((20,14),(25,14),(25,21),(20,21)),(8,21))]
I['degree'] = [circle(7,5,2.3)]
I['cloud'] = [line(*bezier((6,18),(0,18),(0,10),(7,10)),*bezier((7,10),(7,2),(19,2),(19,11)),*bezier((19,11),(25,11),(25,18),(19,18)),(6,18))]
I['cloud_rain'] = I['cloud'] + [line((7,20),(6,23)),line((13,20),(12,23)),line((19,20),(18,23))]
I['cloud_snow'] = I['cloud'] + [circle(7,22,.5),circle(13,22,.5),circle(19,22,.5)]
I['search'] = [circle(10,10,7),line((15,15),(22,22))]
I['toggle'] = [rect(2,6,20,12,6),circle(8,12,3)]
I['fan'] = [circle(12,12,2),line(*bezier((12,10),(4,0),(20,0),(14,11))),line(*bezier((14,12),(26,13),(19,26),(13,14))),line(*bezier((11,14),(1,24),(-2,9),(10,11)))]
I['thermometer'] = [line((9,14),(9,4),(10,2),(12,2),(14,4),(14,14)),circle(11.5,17,5,-140,140),line((11.5,7),(11.5,17))]
# Category thumbnails: authored front views inside the 168-square safe-area
# proportions, rendered at 64 px on the device. No invented product models.
I['device_lamp'] = [line((6,11),(8,3),(16,3),(18,11),(6,11)),line((12,11),(12,20)),rect(5,20,14,2,1)]
I['device_plug'] = [rect(6,3,12,18,3),line((9,8),(9,11)),line((15,8),(15,11)),circle(12,15,1.5),line((9,23),(15,23))]
I['device_fan'] = [circle(12,9,7),circle(12,9,1),line((12,16),(12,21)),line((7,22),(17,22)),
    line(*bezier((12,8),(7,0),(20,3),(13,8))),line(*bezier((13,9),(23,8),(15,20),(12,10))),line(*bezier((11,10),(3,18),(1,6),(11,8)))]
I['device_sensor'] = [rect(4,3,16,18,3),rect(7,6,10,8,1),line((9,9),(11,9)),line((13,11),(15,11)),circle(12,17,.7)]
I['device_contact'] = [rect(4,3,9,18,2),rect(16,5,4,14,1),line((7,7),(10,7)),circle(8.5,17,.6)]
I['device_climate'] = [rect(2,5,20,11,2),line((4,12),(20,12)),line((5,14),(19,14)),circle(18,8,.5),line((7,19),(6,22)),line((12,19),(12,23)),line((17,19),(18,22))]
I['device_media'] = [rect(2,3,20,15,2),line((9,18),(9,21),(15,21),(15,18)),line((6,22),(18,22)),line((10,7),(15,10),(10,13),(10,7))]
I['device_generic'] = I['home']
I['heart'] = [line(*bezier((12,21),(8,18),(2,13),(2,8)),
                   *bezier((2,8),(2,2),(9,1),(12,6)),
                   *bezier((12,6),(15,1),(22,2),(22,8)),
                   *bezier((22,8),(22,13),(16,18),(12,21)))]
I['eye_open'] = [line(*bezier((2,12),(7,3),(17,3),(22,12)),
                      *bezier((22,12),(17,21),(7,21),(2,12))),circle(12,12,3)]
I['tint'] = [line(*bezier((12,2),(9,7),(4,11),(4,15)),
                 *bezier((4,15),(4,25),(20,25),(20,15)),
                 *bezier((20,15),(20,11),(15,7),(12,2)))]
I['device_purifier'] = [rect(6,2,12,20,3),line((8,5),(16,5)),circle(12,7,.6)] + [line((8,y),(16,y)) for y in [10,12,14,16,18]]
def solid(shape):
    return Shape('fill',shape[1],getattr(shape,'svg_path',None))
I['body_lamp'] = [solid(line((6,11),(8,3),(16,3),(18,11),(6,11))),solid(rect(10.8,11,2.4,10,0)),solid(rect(5,20,14,2,1))]
I['body_plug'] = [solid(rect(6,3,12,18,3))]
I['body_fan'] = [solid(circle(12,9,7)),solid(rect(11,16,2,6,0)),solid(rect(7,21,10,1,0))]
I['body_sensor'] = [solid(rect(4,3,16,18,3))]
I['body_contact'] = [solid(rect(4,3,9,18,2)),solid(rect(16,5,4,14,1))]
I['body_climate'] = [solid(rect(2,5,20,11,2))]
I['body_media'] = [solid(rect(2,3,20,15,2)),solid(rect(9,18,6,4,0))]
I['body_generic'] = [solid(line((3,10),(12,3),(21,10),(3,10))),solid(rect(5,10,14,11,0))]
I['body_purifier'] = [solid(rect(6,2,12,20,3))]
def highlight(shape):
    return ('erase',shape[1])
I['bulb'] = [solid(circle(12,8,7.5)),solid(rect(9,11,6,7,1)),line((9,20),(15,20)),line((10,22),(14,22))]
I['body_lamp'].append(highlight(line((9,4),(11,4),(9,10),(7.5,10),(9,4))))
I['body_plug'].append(highlight(rect(7.5,5,1.5,10,.7)))
I['body_sensor'].append(highlight(rect(7,6,10,8,1)))
I['body_contact'].append(highlight(rect(5.5,5,1.5,12,.7)))
I['body_climate'].append(highlight(rect(4,7,13,3,1)))
I['body_media'].append(highlight(rect(4,5,16,11,1)))
I['body_generic'].append(highlight(rect(10,14,4,7,0)))
I['body_purifier'].append(highlight(rect(7.5,4,1.5,16,.7)))
# Additional quick-card category silhouettes; paired base/contour glyphs.
I['device_curtain'] = [line((2,3),(22,3)),rect(3,5,7,16,1),rect(14,5,7,16,1),line((6,7),(6,18)),line((18,7),(18,18))]
I['device_vacuum'] = [circle(12,12,10),circle(12,5,2),line((3,13),(21,13)),circle(12,17,.8)]
I['device_camera'] = [rect(5,2,14,15,5),circle(12,8,3),line((9,17),(9,21),(15,21),(15,17)),rect(5,21,14,1,0)]
I['device_humidifier'] = [rect(5,8,14,14,3),line((8,8),(8,6),(16,6),(16,8)),line(*bezier((9,4),(7,3),(11,2),(9,1))),line(*bezier((15,4),(13,3),(17,2),(15,1))),line((8,17),(16,17))]
I['device_heater'] = [rect(3,4,18,15,2),line((6,19),(5,22)),line((18,19),(19,22))]+[line((x,7),(x,16)) for x in [7,10,14,17]]
I['device_kitchen'] = [rect(4,6,16,15,3),line((7,6),(7,3),(17,3),(17,6)),rect(7,10,10,7,2),circle(12,19,.6)]
I['device_washer'] = [rect(3,2,18,20,2),line((3,7),(21,7)),circle(12,14,5),circle(17,4.5,.6),line((6,4.5),(10,4.5)),circle(12,14,3,10,150)]
I['device_plant'] = [line((6,14),(18,14),(16,22),(8,22),(6,14)),line((12,14),(12,5)),line(*bezier((12,9),(3,10),(3,1),(12,7))),line(*bezier((12,7),(21,8),(21,0),(12,5)))]
I['device_lock'] = [rect(5,10,14,12,2),circle(12,8,5,180,360),line((7,8),(7,10)),line((17,8),(17,10)),circle(12,15,1.5),line((12,16),(12,19))]
I['body_curtain'] = [solid(rect(3,5,7,16,1)),solid(rect(14,5,7,16,1))]
I['body_vacuum'] = [solid(circle(12,12,10)),highlight(circle(12,5,2))]
I['body_camera'] = [solid(rect(5,2,14,15,5)),solid(rect(9,17,6,5,0)),highlight(circle(12,8,3))]
I['body_humidifier'] = [solid(rect(5,8,14,14,3)),highlight(rect(7,10,2,9,1))]
I['body_heater'] = [solid(rect(3,4,18,15,2))]
I['body_kitchen'] = [solid(rect(4,6,16,15,3)),highlight(rect(7,10,10,7,2))]
I['body_washer'] = [solid(rect(3,2,18,20,2)),highlight(circle(12,14,5))]
I['body_plant'] = [solid(line((6,14),(18,14),(16,22),(8,22),(6,14)))]
I['body_lock'] = [solid(rect(5,10,14,12,2))]
for level, fill in [('empty',0),('1',1),('2',2),('3',3),('full',4)]:
    I['battery_'+level] = [rect(2,7,18,10,2),line((22,10),(22,14))]
    I['battery_'+level] += [line((5+i*3,10),(5+i*3,14)) for i in range(fill)]

SYMBOLS = dict(zip([
 'audio','video','list','ok','close','power','settings','home','download','drive',
 'refresh','mute','volume_mid','volume_max','image','tint','prev','play','pause','stop',
 'next','eject','left','right','plus','minus','eye_open','eye_close','warning','shuffle',
 'up','down','loop','directory','upload','call','cut','copy','save','bars','envelope',
 'charge','paste','bell','keyboard','gps','file','wifi','battery_full','battery_3',
 'battery_2','battery_1','battery_empty','usb','bluetooth','trash','edit','backspace','sd_card','new_line'],
 [61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,
  61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,
  61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,
  61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62189,
  62212,62810,63426,63650]))
SYMBOLS['bullet'] = 0x2022
SYMBOLS['degree'] = 0x00b0
SYMBOLS.update(search=0xf002,toggle=0xf205,fan=0xf2dc,thermometer=0xf2c9,bulb=0xf0eb)
EXTRA = ['sparkles','camera','grid','cloud_sun','mic','chat','arrow_ne','radio','sliders',
         'fish','qr','face','headphones','ellipsis','sun','heart','mouse','cloud','cloud_rain','cloud_snow',
         'device_lamp','device_plug','device_fan','device_sensor','device_contact','device_climate','device_media','device_generic','device_purifier',
         'body_lamp','body_plug','body_fan','body_sensor','body_contact','body_climate','body_media','body_generic','body_purifier',
         'device_curtain','device_vacuum','device_camera','device_humidifier','device_heater','device_kitchen','device_washer','device_plant','device_lock',
         'body_curtain','body_vacuum','body_camera','body_humidifier','body_heater','body_kitchen','body_washer','body_plant','body_lock']
SYMBOLS.update((name,0xe001+i) for i,name in enumerate(EXTRA))

# Re-authored rounded silhouettes with a distinct secondary action component.
# Shared by the static fallback and the native two-tone gesture renderer.
I['chat'] = [rect(3,3,18,15,4),line((7,18),(6,22),(12,18)),
             line((7,9),(17,9)),line((7,13),(13,13))]
I['camera'] = [rect(2.5,6,19,15,3),line((7,6),(9,3),(15,3),(17,6)),
               circle(12,13,4),circle(18,9,.5)]
I['home'] = [line((3,10),(12,3),(21,10)),rect(5,9,14,12,2),rect(10,14,4,7,1)]
I['mic'] = [rect(8,2,8,13,4),circle(12,12,7,0,180),line((12,19),(12,22)),line((8,22),(16,22))]
I['directory'] = [rect(3,7,18,14,2),line((3,9),(3,4),(9,4),(12,7)),line((7,12),(17,12))]
I['settings'] = [line((3,7),(6,7)),line((12,7),(21,7)),line((3,17),(12,17)),
                 line((18,17),(21,17)),circle(9,7,3),circle(15,17,3)]
I['sliders'] = I['settings']
FILE_KINDS = ['image','video','audio','text','pdf','document','spreadsheet',
              'presentation','archive','package','code','font','generic','folder']
file_page = [line((5,2),(14,2),(20,8),(20,22),(5,22),(5,2)),line((14,2),(14,8),(20,8))]
I['bulb'] = [circle(12,9,7,135,405),line((7,14),(9,17),(15,17),(17,14)),
             line((9,20),(15,20)),line((10,22),(14,22)),line((10,9),(12,12),(14,9)),line((12,12),(12,17))]
I['device_generic'] = I['home']
for kind in FILE_KINDS:
    name = 'file_'+kind
    if kind in ('image','video','audio','folder'):
        I[name] = I[{'folder':'directory'}.get(kind,kind)]
    else:
        inside = {
          'text':[line((8,11),(17,11)),line((8,15),(17,15)),line((8,19),(14,19))],
          'document':[line((8,11),(17,11)),line((8,15),(17,15)),line((8,19),(14,19))],
          'pdf':[line((9,19),(9,11),(14,11),(16,13),(14,15),(9,15))],
          'spreadsheet':[rect(8,11,9,8,.5),line((8,15),(17,15)),line((12,11),(12,19))],
          'presentation':[rect(8,11,9,7,1),line((12,18),(12,20)),line((11,13),(14,14.5),(11,16))],
          'archive':[line((11,9),(13,9),(13,11),(11,11),(11,13),(13,13),(13,15)),rect(10,16,5,4,1)],
          'package':[line((8,13),(12,11),(17,13),(17,18),(12,20),(8,18),(8,13),(12,15),(17,13)),line((12,15),(12,20))],
          'code':[line((10,12),(7,15),(10,18)),line((15,12),(18,15),(15,18)),line((13,11),(11,19))],
          'font':[line((8,19),(12,11),(16,19)),line((10,16),(14,16))],
          'generic':[line((8,12),(17,12)),line((8,17),(14,17))],
        }[kind]
        I[name] = file_page+inside
    SYMBOLS[name] = 0xe001+len(EXTRA)
    EXTRA.append(name)

from household_icon_design import CODEPOINTS
for name,code in CODEPOINTS.items():
    I[name]=I['device_generic']  # Installed from the explicit authored paths below.
    SYMBOLS[name]=code
    EXTRA.append(name)


# Final art direction: locally authored Duo silhouettes. Keep this after all
# legacy declarations so every public codepoint receives the new artwork.
from duotone_icon_shapes import apply as apply_duotone_artwork
apply_duotone_artwork(I, line, rect, circle, bezier, solid)
from refined_icon_shapes import apply as apply_refined_artwork, STROKE, optical_shapes, stroke_width
apply_refined_artwork(I, line, rect, circle, bezier, solid)
from material_icon_shapes import apply as apply_material_artwork, raster_paths
apply_material_artwork(I)

def rounded_points(points):
    """Fillet authored corners; keep circles and cubic samples continuous."""
    closed=points[0]==points[-1]
    vertices=list(points[:-1] if closed else points)
    out=[]
    for i,p in enumerate(vertices):
        if not closed and i in (0,len(vertices)-1):out.append(p);continue
        a=vertices[(i-1)%len(vertices)];b=vertices[(i+1)%len(vertices)]
        da=math.dist(a,p);db=math.dist(b,p)
        if not da or not db:out.append(p);continue
        radius=min(.75,da*.23,db*.23)
        start=(p[0]+(a[0]-p[0])*radius/da,p[1]+(a[1]-p[1])*radius/da)
        end=(p[0]+(b[0]-p[0])*radius/db,p[1]+(b[1]-p[1])*radius/db)
        out.extend(((1-t)**2*start[0]+2*(1-t)*t*p[0]+t*t*end[0],
                    (1-t)**2*start[1]+2*(1-t)*t*p[1]+t*t*end[1]) for t in [j/6 for j in range(7)])
    if closed:out.append(out[0])
    return out


def raster(name, size, shapes=None):
    source = I[name] if shapes is None else shapes
    if source and source[0][0] == 'svgfill':
        return raster_paths([shape.svg_path for shape in source], size)
    from PIL import Image, ImageDraw
    scale = 8
    image = Image.new('L', (size*scale,size*scale))
    draw = ImageDraw.Draw(image)
    factor = size*scale/24
    stroke = max(1, round(stroke_width(size)*factor))
    source = I[name] if shapes is None else shapes
    for mode, points in optical_shapes(name, size, source, I[name]):
        if mode=='line':points=rounded_points(points)
        xy = [(round(x*factor),round(y*factor)) for x,y in points]
        if mode in ('fill','erase'):
            draw.polygon(xy,fill=255 if mode=='fill' else 0)
            continue
        draw.line(xy, fill=255, width=stroke, joint='curve')
        radius = stroke/2
        for x,y in xy:
            draw.ellipse((x-radius,y-radius,x+radius,y+radius),fill=255)
    return image.resize((size,size),Image.Resampling.LANCZOS)


def generate(output):
    items = sorted(SYMBOLS.items(), key=lambda x:x[1])
    chunks = ['/* SPDX-License-Identifier: Apache-2.0\n * Generated by tools/generate_glass_icons.py.\n * Google Material Symbols Rounded; adapted into LVGL masks and motion layers.\n * Source/license: materials/material-symbols/.\n */\n#include "glass_icons.h"\n']
    # Unicode spellings in AI answers share the existing authored monochrome
    # glyphs. Keep the original text; this is font fallback, never text rewrite.
    aliases={0x2705:'ok',0x2611:'ok',0x2714:'ok',0x274c:'close',0x2715:'close',
             0x274e:'close',0x26a0:'warning',0x1f4a1:'bulb',0x1f50a:'volume_max',
             0x1f509:'volume_mid',0x1f507:'mute',0x1f3b5:'audio',0x1f3b6:'audio',
             0x1f50d:'search',0x1f50e:'search',0x23f0:'bell',0x1f514:'bell',
             0x1f4f7:'camera',0x1f3a4:'mic',0x1f3a7:'headphones',0x2728:'sparkles',
             0x1f4ac:'chat'}
    chunks.append('''static bool icon_unicode_glyph(const lv_font_t *font,lv_font_glyph_dsc_t *out,uint32_t letter,uint32_t next)
{
  /* Presentation selectors and joiners have no standalone visible glyph. */
  if(letter==0xfe0f||letter==0xfe0e||letter==0x200d){
    *out=(lv_font_glyph_dsc_t){0};return true;
  }
  if(letter==0x3000||letter==0xa0||letter==0x2002||letter==0x2003||letter==0x2009||letter==0x202f){
    unsigned em=font->line_height-4;
    *out=(lv_font_glyph_dsc_t){0};
    out->adv_w=letter==0x3000||letter==0x2003?em:letter==0x2009||letter==0x202f?em/4:em/2;
    return true;
  }
  switch(letter){
'''+''.join(f'    case 0x{cp:x}:letter=0x{SYMBOLS[name]:x};break;\n' for cp,name in aliases.items())+'''
    default:break;
  }
  return lv_font_get_glyph_dsc_fmt_txt(font,out,letter,next);
}
''')
    for size in [14,16,20,24,32,64]:
        glyphs = [(name,code) for name,code in items if not name.startswith(('device_','body_'))] if size != 64 else [(name,code) for name,code in items if name.startswith(('device_','body_','home_')) or name in ('cloud_sun','sun','cloud','cloud_rain','cloud_snow','degree')]
        if size == 24:
            glyphs = [(name,code) for name,code in items if not name.startswith('body_')]
        pixels = bytearray()
        descriptors = ['    {0},']
        for name, code in glyphs:
            index = len(pixels)
            values = [(p*15+127)//255 for p in raster(name,size).tobytes()]
            pixels.extend((values[i]<<4)|values[i+1] for i in range(0,len(values),2))
            descriptors.append(f'    {{.bitmap_index={index}, .adv_w={size*16}, .box_w={size}, .box_h={size}, .ofs_y=1}}, /* {name} */')
        bitmap = '\n'.join('    '+','.join(f'0x{v:02x}' for v in pixels[i:i+24])+',' for i in range(0,len(pixels),24))
        start=glyphs[0][1];end=glyphs[-1][1]
        unicode_list=','.join(str(code-start) for _,code in glyphs)
        chunks.append(f'''static const uint8_t bitmap_{size}[] = {{
{bitmap}
}};
static const lv_font_fmt_txt_glyph_dsc_t glyphs_{size}[] = {{
{chr(10).join(descriptors)}
}};
static const uint16_t unicode_{size}[] = {{{unicode_list}}};
static const lv_font_fmt_txt_cmap_t cmap_{size}[] = {{
    {{.range_start={start}, .range_length={end-start+1}, .glyph_id_start=1,
      .unicode_list=unicode_{size}, .list_length={len(glyphs)}, .type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}}
}};
static const lv_font_fmt_txt_dsc_t dsc_{size} = {{
    .glyph_bitmap=bitmap_{size}, .glyph_dsc=glyphs_{size}, .cmaps=cmap_{size},
    .cmap_num=1, .bpp=4, .bitmap_format=0
}};
static const lv_font_t font_{size} = {{
    .get_glyph_dsc=icon_unicode_glyph,
    .get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,
    .line_height={size+4}, .base_line=3, .dsc=&dsc_{size},
    .fallback=&lv_font_montserrat_{14 if size <= 16 else 24}
}};
''')
    chunks.append('''const lv_font_t *glass_icon_font(int pixels)
{
  if (pixels <= 14) return &font_14;
  if (pixels <= 16) return &font_16;
  if (pixels <= 20) return &font_20;
  if (pixels <= 24) return &font_24;
  if (pixels <= 32) return &font_32;
  return &font_64;
}
''')
    chunks.append('#include "glass_icon_motion.inc"\n')
    (output/'glass_icons.c').write_text('\n'.join(chunks),encoding='utf-8')
    header=['/* SPDX-License-Identifier: Apache-2.0 */','/* Generated icon API; LV_SYMBOL names retain their existing codepoints. */',
            '#ifndef GLASS_ICONS_H','#define GLASS_ICONS_H','#include <lvgl/lvgl.h>',
            '#ifdef __cplusplus','extern "C" {','#endif',
            'const lv_font_t *glass_icon_font(int pixels);',
            'lv_obj_t *glass_icon_create(lv_obj_t *parent);',
            'void glass_icon_play(lv_obj_t *object);',
            'void glass_icon_play_children(lv_obj_t *parent);',
            'void glass_icon_set_reduced(bool reduced);']
    header.append('void glass_icon_set_active(lv_obj_t *object, bool active);')
    header.append('void glass_icon_set_running(lv_obj_t *object, bool running);')
    header.append('int glass_icon_diagnose(lv_obj_t *object, char *buffer, size_t size);')
    for name in EXTRA:
        encoded=''.join(f'\\x{x:02x}' for x in chr(SYMBOLS[name]).encode())
        header.append(f'#define GLASS_ICON_{name.upper()} "{encoded}"')
    header.extend(['#ifdef __cplusplus','}','#endif','#endif',''])
    (output/'glass_icons.h').write_text('\n'.join(header),encoding='utf-8')
    print(f'Generated {len(items)} symbols, six native LVGL sizes: {output}')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'app/espdl-quickapp/overlay/apps/system/desktop')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True);generate(args.output)
