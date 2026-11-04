"""Original Calm UI artwork. 24-unit grid, 1.65-unit round strokes.

Shape indices are semantic: accents and animation never split a contour.
The older Duo artwork remains available in duotone_icon_shapes.py.
"""
import math

STROKE = 1.65


def stroke_width(size):
    """Optical compensation: stronger small marks, finer large illustrations."""
    return 1.8 if size <= 16 else 1.72 if size <= 20 else STROKE if size <= 24 else 1.55 if size <= 32 else 1.45


def optical_shapes(name, size, shapes, full):
    omit = {'device_camera': set(), 'device_washer': {3}, 'device_sensor': {2},
            'device_purifier': {3}, 'device_plug': {3}, 'camera': {2},
            'device_curtain': {3, 4}, 'image': {1}, 'device_vacuum': {3}}
    hidden = omit.get(name, set()) if size <= 20 else set()
    return [shape for shape in shapes if not any(shape is full[index] for index in hidden if index < len(full))]
ACCENTS = {
    'camera': {1}, 'mic': {0}, 'chat': {1, 2}, 'settings': {4, 5},
    'sliders': {4, 5}, 'audio': {1, 2}, 'bell': {1}, 'heart': {0},
    'wifi': {2}, 'bulb': {1}, 'device_lamp': {0}, 'device_plug': {1, 2},
    'device_fan': {4, 5, 6}, 'device_sensor': {1}, 'device_contact': {1},
    'device_climate': {1}, 'device_media': {3}, 'device_purifier': {1},
    'device_curtain': {1, 2}, 'device_vacuum': {1}, 'device_camera': {1},
    'device_humidifier': {1, 2}, 'device_heater': {3, 4, 5},
    'device_kitchen': {1}, 'device_washer': {2}, 'device_plant': {2, 3},
    'device_lock': {2}, 'cloud_sun': {0}, 'sun': {0}, 'tint': {0},
}
MOVING = {
    'camera': {1}, 'mic': {0}, 'settings': {4, 5}, 'sliders': {4, 5},
    'bell': {0, 1}, 'audio': {0, 1, 2}, 'heart': {0}, 'wifi': {0, 1, 2},
    'device_fan': {4, 5, 6}, 'device_washer': {2},
    'device_curtain': {1, 2, 3, 4}, 'device_camera': {1},
    'download': {0, 1}, 'upload': {0, 1}, 'refresh': {0, 1},
    'loop': {0, 1}, 'fan': {0, 1, 2, 3},
}


def apply(I, line, rect, circle, bezier, solid):
    def path(*segments):
        points = []
        commands = []
        for segment in segments:
            if not points:
                commands.append(f'M{segment[0][0]},{segment[0][1]}')
            elif points[-1] != segment[0]:
                commands.append(f'L{segment[0][0]},{segment[0][1]}')
            if hasattr(segment, 'commands'):
                commands.append(segment.commands)
            else:
                commands.extend(f'L{x},{y}' for x,y in segment[1:])
            points.extend(segment)
        result = line(*points)
        result.svg_path = ' '.join(commands)
        return result

    def dot(x, y, r=.75):
        return solid(circle(x, y, r))

    # Open contours meet at their actual endpoints; no doubled roof or tail.
    I['home'] = [line((3, 10), (12, 3), (21, 10)),
                 path([(5, 9), (5, 19)], bezier((5, 19), (5, 20.1), (5.9, 21), (7, 21)),
                      [(10, 21), (10, 14), (14, 14), (14, 21), (17, 21)],
                      bezier((17, 21), (18.1, 21), (19, 20.1), (19, 19)), [(19, 9)])]
    I['camera'] = [path([(5, 7), (7.5, 7)],
                       bezier((7.5, 7), (8.2, 7), (8.3, 4.5), (9.5, 4.5)), [(14.5, 4.5)],
                       bezier((14.5, 4.5), (15.7, 4.5), (15.8, 7), (16.5, 7)), [(19, 7)],
                       bezier((19, 7), (20.7, 7), (21, 7.8), (21, 9.5)), [(21, 17.5)],
                       bezier((21, 17.5), (21, 19.2), (20.2, 20), (18.5, 20)), [(5.5, 20)],
                       bezier((5.5, 20), (3.8, 20), (3, 19.2), (3, 17.5)), [(3, 9.5)],
                       bezier((3, 9.5), (3, 7.8), (3.3, 7), (5, 7))), circle(12, 13.5, 3.6), dot(18, 10, .5)]
    I['chat'] = [path([(8, 19), (4, 21), (4.8, 16.7)],
                     bezier((4.8, 16.7), (3.5, 15.3), (3, 13.8), (3, 11.5)),
                     bezier((3, 11.5), (3, .8), (21, .8), (21, 11.5)),
                     bezier((21, 11.5), (21, 17.2), (16, 21.5), (8, 19))),
                 line((8, 10), (16, 10)), line((8, 14), (13, 14))]
    I['mic'] = [rect(9, 3, 6, 12, 3), circle(12, 11.5, 6.5, 0, 180),
                line((12, 18), (12, 21)), line((9, 21), (15, 21))]
    I['settings'] = [line((3, 7), (6, 7)), line((12, 7), (21, 7)),
                     line((3, 17), (12, 17)), line((18, 17), (21, 17)),
                     circle(9, 7, 3), circle(15, 17, 3)]
    I['sliders'] = I['settings']
    I['grid'] = [rect(x, y, 6.5, 6.5, 1.7) for y in (3.5, 14) for x in (3.5, 14)]
    I['bell'] = [path([(5, 17.5), (6.5, 15), (6.5, 10)],
                     bezier((6.5, 10), (6.5, 2), (17.5, 2), (17.5, 10)),
                     [(17.5, 15), (19, 17.5), (5, 17.5)]), circle(12, 18.5, 2.5, 25, 155)]
    I['heart'] = [path(bezier((12, 20), (10, 18.8), (3, 14.5), (3, 8.5)),
                      bezier((3, 8.5), (3, 3.2), (8.8, 2), (12, 6.5)),
                      bezier((12, 6.5), (15.2, 2), (21, 3.2), (21, 8.5)),
                      bezier((21, 8.5), (21, 14.5), (14, 18.8), (12, 20)))]
    I['sparkles'] = [path(bezier((10, 3), (11, 9), (11, 9), (17, 10)),
                         bezier((17, 10), (11, 11), (11, 11), (10, 17)),
                         bezier((10, 17), (9, 11), (9, 11), (3, 10)),
                         bezier((3, 10), (9, 9), (9, 9), (10, 3))),
                     line((19, 15), (19, 21)), line((16, 18), (22, 18))]
    I['audio'] = [line((9, 18), (9, 6), (19, 4), (19, 16)),
                  circle(6.5, 18, 2.5), circle(16.5, 16, 2.5)]
    I['video'] = [rect(3, 6, 12, 12, 2.5), line((15, 10), (21, 7.5), (21, 16.5), (15, 14))]
    I['headphones'] = [circle(12, 12, 8, 180, 360), rect(4, 11, 4, 9, 1.7), rect(16, 11, 4, 9, 1.7)]
    I['image'] = [rect(3, 3, 18, 18, 3), circle(8.5, 8.5, 1.5),
                  line((3.5, 17.5), (9, 12), (13, 16), (16, 13), (20.5, 17.5))]
    I['play'] = [line((8, 4.5), (20, 12), (8, 19.5), (8, 4.5))]
    I['pause'] = [line((8, 5), (8, 19)), line((16, 5), (16, 19))]
    I['stop'] = [rect(5, 5, 14, 14, 2)]
    I['prev'] = [line((5, 5), (5, 19)), line((19, 5), (9, 12), (19, 19), (19, 5))]
    I['next'] = [line((19, 5), (19, 19)), line((5, 5), (15, 12), (5, 19), (5, 5))]
    speaker = line((11, 4.5), (6, 9), (3, 9), (3, 15), (6, 15), (11, 19.5), (11, 4.5))
    I['mute'] = [speaker, line((16, 9), (21, 15)), line((21, 9), (16, 15))]
    I['volume_mid'] = [speaker, circle(11, 12, 6, -45, 45)]
    I['volume_max'] = I['volume_mid'] + [circle(11, 12, 10, -45, 45)]
    I['wifi'] = [path(bezier((3, 8.5), (8, 3.7), (16, 3.7), (21, 8.5))),
                 path(bezier((6.5, 12), (9.5, 9), (14.5, 9), (17.5, 12))),
                 line((10.5, 16), (12, 17.5), (13.5, 16))]
    I['bluetooth'] = [line((12, 3), (12, 21), (18, 16), (6, 6)), line((6, 18), (18, 8), (12, 3))]
    I['power'] = [circle(12, 12.5, 8, -48, 228), line((12, 3), (12, 11))]
    I['charge'] = [line((13.5, 3), (5, 13), (11, 13), (10.5, 21), (19, 10), (13, 10), (13.5, 3))]
    for level, count in [('empty', 0), ('1', 1), ('2', 2), ('3', 3), ('full', 4)]:
        I['battery_' + level] = [rect(2.5, 7, 17, 10, 2), line((22, 10), (22, 14))]
        I['battery_' + level] += [line((5.5 + i*3.5, 10), (5.5 + i*3.5, 14)) for i in range(count)]
    I['search'] = [circle(10.5, 10.5, 6.5), line((15.5, 15.5), (21, 21))]
    I['ok'] = [line((4, 12), (9.5, 17.5), (20, 6.5))]
    I['close'] = [line((6, 6), (18, 18)), line((18, 6), (6, 18))]
    I['plus'] = [line((12, 5), (12, 19)), line((5, 12), (19, 12))]
    I['minus'] = [line((5, 12), (19, 12))]
    for name, points in {'left': [(15, 5), (8, 12), (15, 19)],
                         'right': [(9, 5), (16, 12), (9, 19)],
                         'up': [(5, 15), (12, 8), (19, 15)],
                         'down': [(5, 9), (12, 16), (19, 9)]}.items():
        I[name] = [line(*points)]
    I['download'] = [line((12, 3), (12, 15)), line((7, 10), (12, 15), (17, 10)),
                     line((4, 16), (4, 20), (20, 20), (20, 16))]
    I['upload'] = [line((12, 15), (12, 3)), line((7, 8), (12, 3), (17, 8)),
                   line((4, 16), (4, 20), (20, 20), (20, 16))]
    I['new_line'] = [line((20, 5), (20, 14), (4, 14)), line((9, 9), (4, 14), (9, 19))]
    I['refresh'] = [circle(12, 12, 8, 35, 190), circle(12, 12, 8, 215, 370),
                    line((20, 4), (20, 9), (15, 9)), line((4, 20), (4, 15), (9, 15))]
    I['trash'] = [line((4, 6), (20, 6)), line((9, 3), (15, 3)),
                  line((6, 6), (7, 21), (17, 21), (18, 6)), line((10, 10), (10, 17)), line((14, 10), (14, 17))]
    I['envelope'] = [rect(3, 5, 18, 14, 2.5), line((3.5, 7), (12, 13), (20.5, 7))]
    I['directory'] = [path([(3, 8), (3, 5)], bezier((3, 5), (3, 3.8), (3.8, 3), (5, 3)),
                           [(9, 3), (12, 6), (19, 6)], bezier((19, 6), (20.2, 6), (21, 6.8), (21, 8)),
                           [(21, 18)], bezier((21, 18), (21, 19.2), (20.2, 20), (19, 20)),
                           [(5, 20)], bezier((5, 20), (3.8, 20), (3, 19.2), (3, 18)), [(3, 8)]),
                      line((3, 9), (21, 9))]
    I['file'] = [line((5, 3), (14, 3), (20, 9), (20, 21), (5, 21), (5, 3)),
                 line((14, 3), (14, 9), (20, 9))]
    # Smooth weather silhouettes with rays safely inside the canvas.
    I['sun'] = [circle(12, 12, 4)] + [line((12+7*math.cos(a), 12+7*math.sin(a)),
                                         (12+9.5*math.cos(a), 12+9.5*math.sin(a)))
                                        for a in [i*math.pi/4 for i in range(8)]]
    I['cloud'] = [path(bezier((7, 18), (1, 18), (1, 10), (7, 10)),
                       bezier((7, 10), (7, 3), (18, 3), (18, 11)),
                       bezier((18, 11), (24, 11), (24, 18), (18, 18)), [(7, 18)])]
    I['cloud_sun'] = [circle(16.5, 7.5, 4, 190, 390), I['cloud'][0]]
    I['cloud_rain'] = I['cloud'] + [line((8, 20.5), (7, 22)), line((13, 20.5), (12, 22)), line((18, 20.5), (17, 22))]
    I['cloud_snow'] = I['cloud'] + [dot(7, 21.5, .65), dot(12, 21.5, .65), dot(17, 21.5, .65)]
    I['tint'] = [path(bezier((12, 3), (10, 6), (5, 11), (5, 15)),
                      bezier((5, 15), (5, 23), (19, 23), (19, 15)),
                      bezier((19, 15), (19, 11), (14, 6), (12, 3)))]
    I['bulb'] = [path(bezier((9, 17), (9, 15), (5, 13), (5, 9)),
                      bezier((5, 9), (5, .3), (19, .3), (19, 9)),
                      bezier((19, 9), (19, 13), (15, 15), (15, 17)), [(9, 17)]),
                 line((9, 20), (15, 20)), line((10.5, 22), (13.5, 22))]
    I['thermometer'] = [path([(9, 14.5), (9, 5)], bezier((9, 5), (9, 1), (15, 1), (15, 5)),
                             [(15, 14.5)], bezier((15, 14.5), (21, 23), (3, 23), (9, 14.5))),
                        line((12, 8), (12, 17.5))]
    # Devices share a calm front elevation and a single functional detail.
    I['device_lamp'] = [line((6, 11), (8, 4), (16, 4), (18, 11), (6, 11)),
                        line((12, 11), (12, 20)), line((7, 21), (17, 21))]
    I['device_plug'] = [rect(6, 3, 12, 18, 3), line((9, 8), (9, 11)), line((15, 8), (15, 11)), dot(12, 16)]
    def fan_blade(angle):
        def turn(p):
            return (12+p[0]*math.cos(angle)-p[1]*math.sin(angle),
                    9+p[0]*math.sin(angle)+p[1]*math.cos(angle))
        return path(bezier(*map(turn,[(-.45,-1.6),(-3.8,-3),(-2.3,-6.3),(.6,-5.6)])),
                    bezier(*map(turn,[(.6,-5.6),(3.5,-4.9),(2.8,-1.8),(.45,-1.6)])),
                    bezier(*map(turn,[(.45,-1.6),(.1,-1.7),(-.1,-1.7),(-.45,-1.6)])))
    I['device_fan'] = [circle(12, 9, 7), dot(12, 9, .65), line((12, 16), (12, 21)), line((8, 21), (16, 21))]
    I['device_fan'] += [fan_blade(i*math.tau/3) for i in range(3)]
    I['fan'] = I['device_fan'][1:2] + I['device_fan'][4:]
    MOVING['fan'] = {1, 2, 3}
    I['device_sensor'] = [rect(5, 3, 14, 18, 3), rect(8, 6, 8, 7, 1.2), line((10, 17), (14, 17))]
    I['device_contact'] = [rect(4, 3, 9, 18, 2), rect(17, 6, 3, 12, 1), line((7, 7), (10, 7))]
    I['device_climate'] = [rect(3, 5, 18, 10, 2.5), line((6, 11), (18, 11)),
                           path(bezier((7, 18), (5, 21), (9, 20), (7, 22))), line((12, 18), (12, 21)),
                           path(bezier((17, 18), (19, 21), (15, 20), (17, 22)))]
    I['device_media'] = [rect(3, 4, 18, 13, 2), line((12, 17), (12, 21)), line((8, 21), (16, 21)),
                         line((10, 8), (15, 10.5), (10, 13), (10, 8))]
    I['device_generic'] = I['home']
    I['device_purifier'] = [rect(6, 3, 12, 18, 3), line((9, 7), (15, 7)),
                            line((9, 12), (15, 12)), line((9, 16), (15, 16))]
    I['device_curtain'] = [line((2.5, 4), (21.5, 4)),
                           path([(4, 5), (4, 20)], bezier((4, 20), (7, 20), (9.5, 18), (10, 15)), [(10, 5)]),
                           path([(14, 5), (14, 15)], bezier((14, 15), (14.5, 18), (17, 20), (20, 20)), [(20, 5)]),
                           path(bezier((7, 6), (7, 12), (7, 15), (6, 17))),
                           path(bezier((17, 6), (17, 12), (17, 15), (18, 17)))]
    I['device_vacuum'] = [circle(12, 12, 9), rect(9, 4, 6, 4, 2), line((4, 14), (20, 14)), line((10, 18), (14, 18))]
    I['device_camera'] = [rect(6, 3, 12, 14, 5), circle(12, 9, 3), line((12, 17), (12, 21)), line((7, 21), (17, 21))]
    I['device_humidifier'] = [rect(5, 9, 14, 12, 3),
                             path(bezier((9, 6), (6, 4), (12, 3), (9, 1.5))),
                             path(bezier((15, 6), (12, 4), (18, 3), (15, 1.5))), line((9, 17), (15, 17))]
    I['device_heater'] = [rect(3, 5, 18, 13, 2), line((7, 18), (6, 21)), line((17, 18), (18, 21)),
                          line((8, 9), (8, 14)), line((12, 9), (12, 14)), line((16, 9), (16, 14))]
    I['device_kitchen'] = [rect(4, 6, 16, 15, 3), rect(7, 10, 10, 7, 2), line((8, 6), (8, 3), (16, 3), (16, 6))]
    I['device_washer'] = [rect(4, 3, 16, 18, 2), line((4, 8), (20, 8)), circle(12, 14, 4), line((7, 5.5), (10, 5.5))]
    I['device_plant'] = [line((6, 14), (18, 14), (16, 21), (8, 21), (6, 14)), line((12, 14), (12, 6)),
                         path(bezier((12, 11), (3, 12), (4, 3), (12, 9))),
                         path(bezier((12, 8), (21, 9), (20, 1), (12, 6)))]
    I['device_lock'] = [rect(5, 10, 14, 11, 2.5),
                        path([(8, 10), (8, 7)], bezier((8, 7), (8, 1.5), (16, 1.5), (16, 7)), [(16, 10)]),
                        line((12, 14), (12, 17))]
    # Rebind aliases after the final artwork is installed.
    for kind in ('text','document','pdf','spreadsheet','presentation','archive','package','code','font','generic'):
        I['file_'+kind] = I['file'] + I['file_'+kind][2:]
    for kind in ('image', 'video', 'audio', 'folder'):
        I['file_' + kind] = I[{'folder': 'directory'}.get(kind, kind)]
    for name in ('bullet', 'ellipsis'):
        I[name] = [dot(12, 12, 1.6)] if name == 'bullet' else [dot(x, 12, 1) for x in (5, 12, 19)]
    # Category fills must use the same silhouette as their foreground contour.
    for kind in ('lamp', 'plug', 'fan', 'sensor', 'contact', 'climate', 'media',
                 'purifier', 'curtain', 'vacuum', 'camera', 'humidifier',
                 'heater', 'kitchen', 'washer', 'plant', 'lock'):
        shapes = I['device_' + kind]
        closed = [shape for shape in shapes if len(shape[1]) > 3 and shape[1][0] == shape[1][-1]]
        I['body_' + kind] = [solid(shape) for shape in closed[:1]]
        if kind in ('sensor', 'media', 'washer', 'camera', 'kitchen'):
            hole = shapes[{'sensor': 1, 'media': 0, 'washer': 2, 'camera': 1, 'kitchen': 1}[kind]]
            if kind == 'media':
                hole = rect(4.5, 5.5, 15, 10, 1)
            I['body_' + kind].append(('erase', hole[1]))
    I['body_contact'] = [solid(I['device_contact'][0]), solid(I['device_contact'][1])]
    I['body_curtain'] = [solid(I['device_curtain'][1]), solid(I['device_curtain'][2])]
    I['body_generic'] = [solid(line((3, 10), (12, 3), (21, 10), (3, 10))),
                         solid(rect(5, 10, 14, 11, 1)), ('erase', rect(10, 14, 4, 7, 0)[1])]
