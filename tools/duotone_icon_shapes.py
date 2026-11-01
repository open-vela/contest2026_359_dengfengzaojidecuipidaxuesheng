"""Authored Pine/Coral UI icon silhouettes on a 24 px grid.

The public LV_SYMBOL codepoints are owned by generate_glass_icons.py. This
module only replaces artwork, keeping navigation and application contracts
unchanged. Primary contours come first; accent geometry is deliberately small.
"""

def apply(I, line, rect, circle, bezier, solid):
    def closed(points):
        return line(*points, points[0])
    def arc(x, y, r, start, end):
        return circle(x, y, r, start, end)

    # Navigation and communication: broad silhouettes, no hairline details.
    I['grid'] = [rect(3, 3, 7, 7, 2), rect(14, 3, 7, 7, 2),
                 rect(3, 14, 7, 7, 2), rect(14, 14, 7, 7, 2)]
    I['home'] = [line((3, 11), (12, 3), (21, 11)),
                 line((5, 10), (5, 21), (19, 21), (19, 10)),
                 line((10, 21), (10, 15), (14, 15), (14, 21))]
    I['camera'] = [rect(2.5, 6, 19, 15, 4), line((7, 6), (9, 3), (15, 3), (17, 6)),
                   circle(12, 13, 4.2), circle(18.2, 9.5, .65)]
    I['sparkles'] = [line((9, 2), (10.5, 7.5), (16, 9), (10.5, 10.5), (9, 16), (7.5, 10.5), (2, 9), (7.5, 7.5), (9, 2)),
                     line((18, 14), (19, 18), (23, 19), (19, 20), (18, 24), (17, 20), (13, 19), (17, 18), (18, 14))]
    I['chat'] = [rect(2.5, 3, 19, 16, 5), line((7, 19), (6, 23), (12, 19)),
                 line((7, 9), (17, 9)), line((7, 14), (14, 14))]
    I['mic'] = [rect(8, 2, 8, 13, 4), arc(12, 11, 7, 0, 180),
                line((12, 18), (12, 22)), line((8, 22), (16, 22))]
    I['headphones'] = [arc(12, 12, 9, 180, 360), line((3, 12), (3, 17)),
                       rect(2, 12, 5, 9, 2), rect(17, 12, 5, 9, 2)]
    I['audio'] = [line((9, 19), (9, 5), (20, 3), (20, 16)),
                  circle(6, 19, 3), circle(17, 16, 3), line((9, 9), (20, 7))]
    I['video'] = [rect(2, 5, 14, 14, 3), line((16, 10), (22, 7), (22, 17), (16, 14))]
    I['image'] = [rect(2.5, 3, 19, 18, 3), circle(8, 8.5, 2),
                  line((4, 18), (10, 12), (15, 17)), line((13, 15), (17, 11), (21, 15))]
    I['heart'] = [line(*bezier((12, 21), (8, 18), (2, 14), (2, 8)),
                       *bezier((2, 8), (2, 2), (9, 1), (12, 6)),
                       *bezier((12, 6), (15, 1), (22, 2), (22, 8)),
                       *bezier((22, 8), (22, 14), (16, 18), (12, 21)))]
    I['ellipsis'] = [circle(4, 12, 1), circle(12, 12, 1), circle(20, 12, 1)]
    I['bullet'] = [circle(12, 12, 2.5)]
    I['list'] = [line((9, 6), (21, 6)), line((9, 12), (21, 12)), line((9, 18), (21, 18)),
                 circle(4, 6, .8), circle(4, 12, .8), circle(4, 18, .8)]
    I['bars'] = [line((3, 6), (21, 6)), line((3, 12), (21, 12)), line((3, 18), (21, 18))]

    # Actions: geometric, optically centered and comfortable at 14–24 px.
    I['search'] = [circle(10, 10, 7), line((15, 15), (22, 22))]
    I['plus'] = [line((12, 4), (12, 20)), line((4, 12), (20, 12))]
    I['minus'] = [line((4, 12), (20, 12))]
    I['close'] = [line((5, 5), (19, 19)), line((19, 5), (5, 19))]
    I['ok'] = [line((3, 12), (9, 18), (21, 5))]
    I['arrow_ne'] = [line((6, 18), (18, 6)), line((8, 6), (18, 6), (18, 16))]
    I['left'] = [line((15, 4), (7, 12), (15, 20))]
    I['right'] = [line((9, 4), (17, 12), (9, 20))]
    I['up'] = [line((4, 15), (12, 7), (20, 15))]
    I['down'] = [line((4, 9), (12, 17), (20, 9))]
    I['new_line'] = [line((19, 4), (5, 4), (5, 20)), line((1, 15), (5, 20), (10, 15))]
    I['refresh'] = [arc(12, 12, 8, 35, 190), line((20, 4), (20, 10), (14, 10)),
                    arc(12, 12, 8, 215, 370), line((4, 20), (4, 14), (10, 14))]
    I['loop'] = [line((4, 10), (4, 6), (20, 6), (20, 10)), line((16, 2), (20, 6), (16, 10)),
                 line((20, 14), (20, 18), (4, 18), (4, 14)), line((8, 14), (4, 18), (8, 22))]
    I['shuffle'] = [line((3, 6), (7, 6), (17, 18), (21, 18)), line((17, 14), (21, 18), (17, 22)),
                    line((3, 18), (7, 18), (17, 6), (21, 6)), line((17, 2), (21, 6), (17, 10))]
    I['download'] = [line((12, 3), (12, 16)), line((6, 11), (12, 17), (18, 11)),
                     line((4, 16), (4, 21), (20, 21), (20, 16))]
    I['upload'] = [line((12, 21), (12, 8)), line((6, 13), (12, 7), (18, 13)),
                   line((4, 3), (20, 3))]
    I['power'] = [arc(12, 13, 8, -48, 228), line((12, 2), (12, 12))]
    I['settings'] = [line((3, 7), (6, 7)), line((13, 7), (21, 7)), circle(9.5, 7, 3),
                     line((3, 17), (11, 17)), line((18, 17), (21, 17)), circle(14.5, 17, 3)]
    I['sliders'] = I['settings']
    I['toggle'] = [rect(2, 6, 20, 12, 6), circle(8, 12, 3.5)]
    I['trash'] = [line((4, 6), (20, 6)), line((9, 3), (15, 3)),
                  line((6, 6), (7, 22), (17, 22), (18, 6)), line((10, 10), (10.5, 18)), line((14, 10), (13.5, 18))]
    I['edit'] = [line((4, 20), (5, 15), (17, 3), (21, 7), (9, 19), (4, 20)), line((14, 6), (18, 10))]
    I['backspace'] = [line((9, 4), (21, 4), (21, 20), (9, 20), (2, 12), (9, 4)),
                      line((11, 9), (17, 15)), line((17, 9), (11, 15))]
    I['copy'] = [rect(8, 8, 13, 13, 2), line((16, 4), (16, 3), (3, 3), (3, 16), (7, 16))]
    I['save'] = [line((3, 3), (17, 3), (21, 7), (21, 21), (3, 21), (3, 3)),
                 rect(7, 3, 10, 6, 1), rect(7, 14, 10, 7, 1)]
    I['paste'] = [rect(5, 5, 14, 17, 2), rect(9, 2, 6, 5, 1)]
    I['cut'] = [circle(5, 6, 3), circle(5, 18, 3), line((7.5, 7.5), (21, 21)), line((7.5, 16.5), (21, 3))]
    I['warning'] = [line((12, 2), (23, 21), (1, 21), (12, 2)), line((12, 8), (12, 14)), circle(12, 18, .6)]
    I['eye_open'] = [line(*bezier((2, 12), (7, 3), (17, 3), (22, 12)),
                          *bezier((22, 12), (17, 21), (7, 21), (2, 12))), circle(12, 12, 3.5)]
    I['eye_close'] = [line(*bezier((2, 10), (7, 18), (17, 18), (22, 10))),
                      line((5, 15), (3, 19)), line((12, 17), (12, 21)), line((19, 15), (21, 19))]
    I['bell'] = [line((4, 18), (6, 15), (6, 9), (8, 5), (12, 3), (16, 5), (18, 9), (18, 15), (20, 18), (4, 18)),
                 arc(12, 18, 4, 35, 145)]
    I['envelope'] = [rect(2, 4, 20, 16, 3), line((3, 6), (12, 13), (21, 6))]
    I['call'] = [line((5, 3), (9, 3), (11, 8), (8, 10), (11, 14), (14, 17), (16, 14), (21, 16), (21, 20), (18, 22), (12, 20), (6, 15), (3, 9), (3, 5), (5, 3))]
    I['keyboard'] = [rect(2, 5, 20, 14, 3), line((6, 9), (6, 9)), line((10, 9), (10, 9)), line((14, 9), (14, 9)), line((18, 9), (18, 9)), line((7, 15), (17, 15))]
    I['gps'] = [line((19, 3), (4, 10), (10, 13), (13, 21), (19, 3)), line((10, 13), (19, 3))]

    # Media controls retain familiar silhouettes but share the same radius.
    I['play'] = [line((7, 3), (21, 12), (7, 21), (7, 3))]
    I['pause'] = [rect(5, 3, 4, 18, 1.5), rect(15, 3, 4, 18, 1.5)]
    I['stop'] = [rect(5, 5, 14, 14, 3)]
    I['prev'] = [line((5, 3), (5, 21)), line((20, 4), (9, 12), (20, 20), (20, 4))]
    I['next'] = [line((19, 3), (19, 21)), line((4, 4), (15, 12), (4, 20), (4, 4))]
    I['eject'] = [line((12, 3), (21, 15), (3, 15), (12, 3)), line((4, 20), (20, 20))]
    I['mute'] = [line((11, 4), (5, 9), (2, 9), (2, 15), (5, 15), (11, 20), (11, 4)), line((16, 9), (22, 15)), line((22, 9), (16, 15))]
    I['volume_mid'] = [line((11, 4), (5, 9), (2, 9), (2, 15), (5, 15), (11, 20), (11, 4)), arc(11, 12, 7, -48, 48)]
    I['volume_max'] = [line((11, 4), (5, 9), (2, 9), (2, 15), (5, 15), (11, 20), (11, 4)), arc(11, 12, 7, -48, 48), arc(11, 12, 11, -48, 48)]

    # Files: one strong page silhouette and one legible category mark.
    file_page = [line((5, 2), (14, 2), (20, 8), (20, 22), (5, 22), (5, 2)), line((14, 2), (14, 8), (20, 8))]
    I['file'] = file_page
    I['directory'] = [line((2, 7), (2, 4), (9, 4), (12, 7), (22, 7), (22, 21), (2, 21), (2, 7)),
                      line((3, 10), (21, 10))]
    I['drive'] = [rect(2, 14, 20, 7, 2), line((3, 15), (6, 4), (18, 4), (21, 15)), circle(17, 17.5, .8)]
    I['sd_card'] = [line((7, 2), (19, 2), (19, 22), (5, 22), (5, 7), (7, 2)), line((9, 5), (9, 9)), line((13, 5), (13, 9)), line((16, 5), (16, 9))]
    for kind in ['text', 'document', 'pdf', 'spreadsheet', 'presentation', 'archive',
                 'package', 'code', 'font', 'generic']:
        inside = {
          'text': [line((8, 11), (17, 11)), line((8, 15), (17, 15)), line((8, 19), (14, 19))],
          'document': [line((8, 11), (17, 11)), line((8, 15), (17, 15)), line((8, 19), (14, 19))],
          'pdf': [line((9, 19), (9, 11), (14, 11), (16, 13), (14, 15), (9, 15))],
          'spreadsheet': [rect(8, 11, 9, 8, 1), line((8, 15), (17, 15)), line((12, 11), (12, 19))],
          'presentation': [rect(8, 11, 9, 7, 1), line((12, 18), (12, 20)), line((11, 13), (14, 14.5), (11, 16))],
          'archive': [line((11, 9), (13, 9), (13, 11), (11, 11), (11, 13), (13, 13), (13, 15)), rect(10, 16, 5, 4, 1)],
          'package': [line((8, 13), (12, 11), (17, 13), (17, 18), (12, 20), (8, 18), (8, 13)), line((12, 15), (17, 13)), line((12, 15), (12, 20))],
          'code': [line((10, 12), (7, 15), (10, 18)), line((15, 12), (18, 15), (15, 18)), line((13, 11), (11, 19))],
          'font': [line((8, 19), (12, 11), (16, 19)), line((10, 16), (14, 16))],
          'generic': [line((8, 12), (17, 12)), line((8, 17), (14, 17))],
        }[kind]
        I['file_' + kind] = file_page + inside
    I['file_image'] = I['image']
    I['file_video'] = I['video']
    I['file_audio'] = I['audio']
    I['file_folder'] = I['directory']

    # Connectivity and power: bold status marks with no tiny internal bars.
    I['wifi'] = [arc(12, 17, 13, 215, 325), arc(12, 17, 8, 215, 325), circle(12, 19, 1.5)]
    I['bluetooth'] = [line((12, 2), (18, 7), (6, 17), (18, 12), (12, 2)), line((12, 2), (12, 22), (18, 17), (6, 7))]
    I['radio'] = [circle(12, 12, 2), arc(12, 12, 6, -45, 45), arc(12, 12, 10, -45, 45), arc(12, 12, 6, 135, 225), arc(12, 12, 10, 135, 225)]
    I['usb'] = [line((12, 21), (12, 3)), line((9, 6), (12, 3), (15, 6)), line((12, 12), (6, 12), (6, 8)), circle(6, 6, 2), line((12, 16), (18, 16), (18, 11)), rect(16, 7, 4, 4, .5)]
    I['charge'] = [line((13, 2), (4, 14), (11, 14), (10, 22), (20, 9), (13, 9), (13, 2))]
    for level, count in [('empty', 0), ('1', 1), ('2', 2), ('3', 3), ('full', 4)]:
        I['battery_' + level] = [rect(2, 7, 18, 10, 2), line((22, 10), (22, 14))]
        I['battery_' + level] += [rect(5 + i * 3.5, 10, 2, 4, .6) for i in range(count)]

    # Weather and ambient icons use a single accent cue instead of many rays.
    I['sun'] = [circle(12, 12, 5)] + [line((12 + 9*__import__('math').cos(a*__import__('math').pi/4), 12 + 9*__import__('math').sin(a*__import__('math').pi/4)),
                                               (12 + 11*__import__('math').cos(a*__import__('math').pi/4), 12 + 11*__import__('math').sin(a*__import__('math').pi/4))) for a in range(0, 8, 2)]
    I['cloud'] = [line(*bezier((6, 19), (0, 19), (0, 10), (7, 10)),
                       *bezier((7, 10), (7, 2), (19, 2), (19, 11)),
                       *bezier((19, 11), (25, 11), (25, 19), (18, 19)), (6, 19))]
    I['cloud_sun'] = [I['cloud'][0], circle(18, 6, 4), line((18, 0), (18, 1)), line((23, 3), (24, 2))]
    I['cloud_rain'] = I['cloud'] + [line((7, 21), (6, 23)), line((13, 21), (12, 23)), line((19, 21), (18, 23))]
    I['cloud_snow'] = I['cloud'] + [circle(7, 22, .8), circle(13, 22, .8), circle(19, 22, .8)]
    I['tint'] = [line(*bezier((12, 2), (9, 7), (4, 11), (4, 15)),
                      *bezier((4, 15), (4, 25), (20, 25), (20, 15)),
                      *bezier((20, 15), (20, 11), (15, 7), (12, 2)))]
    I['thermometer'] = [line((9, 14), (9, 4), (10, 2), (14, 2), (15, 4), (15, 14)), arc(12, 17, 5, -140, 140), line((12, 7), (12, 17))]
    I['bulb'] = [arc(12, 10, 7, 135, 405), line((7, 14), (9, 17), (15, 17), (17, 14)), line((9, 21), (15, 21)), line((10, 23), (14, 23)), line((12, 7), (12, 14))]
    I['mouse'] = [rect(6, 2, 12, 20, 6), line((12, 3), (12, 9))]
    I['fish'] = [line(*bezier((2, 12), (7, 3), (16, 3), (21, 12)), *bezier((21, 12), (16, 21), (7, 21), (2, 12))), line((21, 12), (17, 6), (17, 18), (21, 12)), circle(8, 10, .8)]
    I['qr'] = [rect(2, 2, 8, 8, 1), rect(14, 2, 8, 8, 1), rect(2, 14, 8, 8, 1), line((14, 14), (18, 14), (18, 18), (22, 18)), line((14, 22), (14, 18)), line((22, 22), (22, 20))]
    I['face'] = [line((3, 8), (3, 3), (8, 3)), line((16, 3), (21, 3), (21, 8)), line((21, 16), (21, 21), (16, 21)), line((8, 21), (3, 21), (3, 16)), circle(9, 11, .6), circle(15, 11, .6), arc(12, 12, 5, 35, 145)]

    # Device family: simplified front views; contours and accents stay paired.
    I['device_lamp'] = [line((5, 11), (8, 3), (16, 3), (19, 11), (5, 11)), line((12, 11), (12, 20)), line((6, 21), (18, 21))]
    I['device_plug'] = [rect(6, 3, 12, 18, 4), line((9, 8), (9, 11)), line((15, 8), (15, 11)), line((12, 21), (12, 23))]
    I['device_fan'] = [circle(12, 9, 8), circle(12, 9, 1.5), line((12, 17), (12, 21)), line((7, 22), (17, 22)),
                       line(*bezier((12, 8), (7, 1), (19, 3), (13, 8))), line(*bezier((13, 10), (21, 9), (16, 18), (12, 11))), line(*bezier((10, 10), (4, 17), (2, 7), (11, 8)))]
    I['device_sensor'] = [rect(4, 2, 16, 20, 4), circle(12, 9, 4), line((9, 17), (15, 17))]
    I['device_contact'] = [rect(3, 3, 10, 18, 2), rect(17, 6, 4, 12, 1), line((6, 7), (10, 7))]
    I['device_climate'] = [rect(2, 5, 20, 11, 3), line((4, 12), (20, 12)), line((6, 19), (5, 22)), line((12, 19), (12, 23)), line((18, 19), (19, 22))]
    I['device_media'] = [rect(2, 3, 20, 15, 3), line((9, 22), (15, 22)), line((12, 18), (12, 22)), line((10, 7), (16, 10.5), (10, 14), (10, 7))]
    I['device_generic'] = I['home']
    I['device_purifier'] = [rect(6, 2, 12, 20, 4), line((9, 6), (15, 6)), line((9, 11), (15, 11)), line((9, 15), (15, 15)), line((9, 19), (15, 19))]
    I['device_curtain'] = [line((2, 3), (22, 3)), rect(3, 5, 7, 16, 1), rect(14, 5, 7, 16, 1), line((7, 7), (7, 18)), line((17, 7), (17, 18))]
    I['device_vacuum'] = [circle(12, 12, 10), circle(12, 5, 2.5), line((4, 14), (20, 14)), circle(12, 18, .8)]
    I['device_camera'] = [rect(5, 2, 14, 15, 5), circle(12, 8, 3.5), line((9, 17), (9, 21), (15, 21), (15, 17)), line((5, 22), (19, 22))]
    I['device_humidifier'] = [rect(5, 8, 14, 14, 4), line((8, 8), (8, 6), (16, 6), (16, 8)), arc(10, 3, 2, 90, 270), arc(16, 3, 2, -90, 90), line((8, 17), (16, 17))]
    I['device_heater'] = [rect(3, 4, 18, 15, 3), line((6, 19), (5, 22)), line((18, 19), (19, 22)), line((8, 8), (8, 15)), line((12, 8), (12, 15)), line((16, 8), (16, 15))]
    I['device_kitchen'] = [rect(4, 6, 16, 15, 3), line((7, 6), (7, 3), (17, 3), (17, 6)), rect(7, 10, 10, 7, 2), circle(12, 19, .7)]
    I['device_washer'] = [rect(3, 2, 18, 20, 3), line((3, 7), (21, 7)), circle(12, 14, 5), circle(17, 4.5, .8), line((6, 4.5), (10, 4.5))]
    I['device_plant'] = [line((6, 14), (18, 14), (16, 22), (8, 22), (6, 14)), line((12, 14), (12, 5)), line(*bezier((12, 10), (3, 11), (3, 2), (12, 8))), line(*bezier((12, 8), (21, 9), (21, 1), (12, 6)))]
    I['device_lock'] = [rect(4, 10, 16, 12, 3), arc(12, 9, 5, 180, 360), line((7, 9), (7, 10)), line((17, 9), (17, 10)), circle(12, 15, 1.5), line((12, 16), (12, 19))]
