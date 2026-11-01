"""Pinned Google Material Symbols Rounded, adapted for the LVGL icon ABI.

SVG paths stay editable. Only the build step rasterizes them; the ESP does not
need a font service, SVG parser, Lottie runtime or network connection.
"""
import io
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ASSETS = Path(__file__).resolve().parents[1] / 'materials/material-symbols'
MAPPING = dict(zip(
    'audio video list ok close power settings home download drive refresh mute volume_mid volume_max image tint prev play pause stop next eject left right plus minus eye_open eye_close warning shuffle up down loop directory upload call cut copy save bars envelope charge paste bell keyboard gps file wifi battery_full battery_3 battery_2 battery_1 battery_empty usb bluetooth trash edit backspace sd_card new_line bullet degree search toggle fan thermometer bulb sparkles camera grid cloud_sun mic chat arrow_ne radio sliders fish qr face headphones ellipsis sun heart mouse cloud cloud_rain cloud_snow'.split(),
    'music_note videocam format_list_bulleted check close power_settings_new settings home download hard_drive refresh volume_off volume_down volume_up image water_drop skip_previous play_arrow pause stop skip_next eject chevron_left chevron_right add remove visibility visibility_off warning shuffle keyboard_arrow_up keyboard_arrow_down repeat folder upload call content_cut content_copy save menu mail bolt content_paste notifications keyboard near_me description wifi battery_full battery_5_bar battery_3_bar battery_1_bar battery_0_bar usb bluetooth delete edit backspace sd_card keyboard_return fiber_manual_record radio_button_unchecked search toggle_on mode_fan device_thermostat lightbulb auto_awesome photo_camera grid_view partly_cloudy_day mic chat north_east radio tune set_meal qr_code sentiment_satisfied headphones more_horiz sunny favorite mouse cloud rainy weather_snowy'.split()))
DEVICES = dict(zip(
    'lamp plug fan sensor contact climate media generic purifier curtain vacuum camera humidifier heater kitchen washer plant lock'.split(),
    'table_lamp outlet mode_fan sensors sensor_door climate_mini_split tv home air_purifier curtains vacuum videocam humidity_indoor heat cooking local_laundry_service potted_plant lock'.split()))
for _name, _symbol in DEVICES.items():
    MAPPING['device_' + _name] = _symbol
    MAPPING['body_' + _name] = _symbol
MAPPING.update({'file_' + k: v for k, v in zip(
    'image video audio text pdf document spreadsheet presentation archive package code font generic folder'.split(),
    'image movie audio_file text_snippet picture_as_pdf description table_chart slideshow folder_zip deployed_code code text_fields draft folder'.split())})
from household_icon_design import BY_SYMBOL
MAPPING.update((name,'authored/'+name) for name in BY_SYMBOL)


def fetch():
    """Explicit refresh only. Normal generation is offline and hash checked."""
    import concurrent.futures
    import hashlib
    import urllib.request
    ASSETS.mkdir(parents=True, exist_ok=True)
    def read(url):
        for attempt in range(3):
            try:
                with urllib.request.urlopen(url, timeout=25) as response:
                    return response.read()
            except Exception:
                if attempt == 2: raise
    lock = ASSETS / 'sources.json'
    revision = json.loads(lock.read_text())['revision'] if lock.exists() else json.loads(read(
        'https://api.github.com/repos/google/material-design-icons/commits/master'))['sha']
    root = f'https://raw.githubusercontent.com/google/material-design-icons/{revision}'
    records = []
    def download(item):
        name, filled = item
        filename = f'{name}{"_fill1" if filled else ""}_24px.svg'
        url = f'{root}/symbols/web/{name}/materialsymbolsrounded/{filename}'
        path = ASSETS / filename
        data = path.read_bytes() if path.exists() else read(url)
        ET.fromstring(data)
        path.write_bytes(data)
        return dict(file=filename, url=url, sha256=hashlib.sha256(data).hexdigest())
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        futures = {pool.submit(download, (name, fill)): (name, fill)
                   for name in sorted(set(MAPPING.values())) if not name.startswith('authored/') for fill in (False, True)}
        failures=[]
        for future in concurrent.futures.as_completed(futures):
            try: records.append(future.result())
            except Exception as error: failures.append((futures[future], str(error)))
    if failures:
        raise RuntimeError(failures)
    (ASSETS / 'LICENSE').write_bytes(read(root + '/LICENSE'))
    lock.write_text(json.dumps(dict(repository='google/material-design-icons', revision=revision,
        style='Material Symbols Rounded', weight=400, optical_size=24,
        files=sorted(records, key=lambda r:r['file'])), indent=2) + '\n')
    print(f'Pinned {len(records)} SVGs at {revision}')


class Shape(tuple):
    def __new__(cls, path):
        obj = super().__new__(cls, ('svgfill', []))
        obj.svg_path = path
        return obj


def normalized(name, filled=False):
    if name in BY_SYMBOL:return ''.join(BY_SYMBOL[name]['paths'])
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.svgLib.path import parse_path
    filename = f'{MAPPING[name]}{"_fill1" if filled else ""}_24px.svg'
    svg = ET.parse(ASSETS / filename).getroot()
    x,y,w,h = map(float, svg.attrib.get('viewBox', '0 0 24 24').split())
    pen=SVGPathPen(None)
    transform=TransformPen(pen, (24/w,0,0,24/h,-x*24/w,-y*24/h))
    for path in svg.findall('{http://www.w3.org/2000/svg}path'):
        if path.attrib.get('fill') == 'none': continue
        parse_path(path.attrib['d'], transform)
    return pen.getCommands()


def raster_paths(paths, size):
    from PIL import Image
    import resvg_py
    svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">' + ''.join(
        f'<path d="{p}" fill="white"/>' for p in paths) + '</svg>'
    png = resvg_py.svg_to_bytes(svg_string=svg, width=size*4, height=size*4,
                              skip_system_fonts=True)
    return Image.open(io.BytesIO(png)).getchannel('A').resize((size,size),Image.Resampling.LANCZOS)


def components(path):
    """Keep each exterior together with its holes, preserving nonzero winding."""
    from PIL import ImageChops
    contours=re.findall(r'M[^M]+',path)
    masks=[raster_paths([p],96) for p in contours]
    mass=[sum(m.getdata()) for m in masks]
    parent={}
    for i, mask in enumerate(masks):
        if mass[i]<20: continue
        containers=[j for j, other in enumerate(masks) if mass[j]>mass[i] and
                    sum(ImageChops.multiply(mask,other).getdata()) / mass[i] > .96]
        if containers: parent[i]=min(containers,key=lambda j:mass[j])
    groups={}
    for i,p in enumerate(contours):
        if mass[i]<20: continue
        # An island inside a counterform is a separate functional component
        # (e.g. a camera lens inside its hollow body), not another body hole.
        depth=0;ancestor=i
        while ancestor in parent:
            ancestor=parent[ancestor];depth+=1
        root=parent[i] if depth%2 else i
        groups.setdefault(root,[]).append(p)
    return [Shape(''.join(parts)) for parts in groups.values()]


ACCENTS={}
PROFILES={}
FILLED={}
RUNNING={}


def apply(icons):
    import hashlib
    from material_icon_design import DESIGNS, resolve
    lock=json.loads((ASSETS/'sources.json').read_text())
    for record in lock['files']:
        assert hashlib.sha256((ASSETS/record['file']).read_bytes()).hexdigest()==record['sha256'], record['file']
    assert set(icons)==set(MAPPING), (set(icons)-set(MAPPING), set(MAPPING)-set(icons))
    assert set(DESIGNS)=={n for n in icons if not n.startswith('body_') and n!='degree'}
    cache={}
    for name in icons:
        filled=name.startswith('body_')
        key=(MAPPING[name],filled)
        if key not in cache: cache[key]=[Shape(path) for path in BY_SYMBOL[name]['paths']] if name in BY_SYMBOL else components(normalized(name,filled))
        shapes=cache[key]
        assert shapes, name
        icons[name]=shapes
        FILLED[name]=Shape(normalized(name,True))
        # Small disconnected functional parts receive the second color. Keep
        # a complete exterior, including counterforms, within each layer.
        areas=[sum(raster_paths([s.svg_path],48).getdata()) for s in shapes]
        body=max(range(len(shapes)),key=lambda i:areas[i])
        accents=set(range(len(shapes)))-{body}
        if name in DESIGNS:
            PROFILES[name], RUNNING[name] = resolve(name, shapes, accents, Shape)
            if name in BY_SYMBOL:
                FILLED[name]=Shape(''.join(path for i,path in enumerate(BY_SYMBOL[name]['paths']) if i not in BY_SYMBOL[name]['idle_hidden']))
            # Accent and movement refer to the same reviewed functional parts.
            # A local indicator keeps the entire enclosing frame stationary.
            ACCENTS[name] = set(RUNNING[name]['moving'])
        else:
            PROFILES[name]=dict(kind=0,pivot=(12,12),direction=(0,0),moving=set(),reverse=set(),delays={})
            ACCENTS[name]=set()


if __name__=='__main__':
    fetch()
