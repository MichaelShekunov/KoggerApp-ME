"""Rectangles of the guide's marks from items/<id>.txt (windowItems: path, class, objectName, x,y wxh, "text") and
shots.json in the format of the l412_adc project's tools/guide_shots.py; a contact sheet with the frames drawn, to check.
usage: python -E make_marks.py <run dir> <out shots.json> <sheet.png> <app build>"""
import json
import re
import sys

from PIL import Image, ImageDraw

run, out_json, sheet, build = sys.argv[1:5]
LINE = re.compile(r'^(\S+) (\S+) (\S+) ([\d.-]+),([\d.-]+) ([\d.]+)x([\d.]+)(?: "(.*)")?$')
PANEL_RIGHT = 600   # the settings panel's right edge at 1600x1000


def load(shot):
    items = []
    for ln in open('%s/items/%s.txt' % (run, shot), encoding='utf-8'):
        m = LINE.match(ln.strip())
        if m:
            items.append({'path': m.group(1), 'cls': m.group(2), 'name': m.group(3), 'x': float(m.group(4)),
                          'y': float(m.group(5)), 'w': float(m.group(6)), 'h': float(m.group(7)),
                          'text': re.sub('<[^>]+>', '', m.group(8)) if m.group(8) else None})
    # only the page on top: the device page (0.0.0.4.1.8...) over the settings page (0.0.0.4.1.3...)
    under = '0.0.0.4.1.8.' if shot == 'connect' else '0.0.0.4.1.3.' if shot.startswith(('settings_', 'upgrade')) else None
    items = [i for i in items if not (under and i['path'].startswith(under))]
    return items


def on_screen(it, top=150, bottom=930):
    return it['w'] > 0 and it['h'] > 0 and it['y'] >= top and it['y'] + it['h'] <= bottom


def by_text(items, text, **kw):
    hits = [i for i in items if i['text'] == text and i['cls'].startswith('QQuickText') and on_screen(i, **kw)]
    if not hits:
        hits = [i for i in items if i['text'] == text and on_screen(i, **kw)]
    if not hits:
        raise KeyError(text)
    return hits[0]


def union(rs, pad=4):
    x0 = min(r['x'] for r in rs) - pad
    y0 = min(r['y'] for r in rs) - pad
    x1 = max(r['x'] + r['w'] for r in rs) + pad
    y1 = max(r['y'] + r['h'] for r in rs) + pad
    return [round(x0), round(y0), round(x1 - x0), round(y1 - y0)]


def row(items, text, below=0):
    """A settings row: its label and whatever stands in the label's band to its right inside the panel; with below,
    also what is up to that many px under the label (a tab bar under its caption)."""
    lab = by_text(items, text)
    cy0, cy1 = lab['y'] - 6, lab['y'] + lab['h'] + 6 + below
    band = [i for i in items if i['x'] >= lab['x'] - 20 and i['x'] + i['w'] <= PANEL_RIGHT and i['w'] < 560
            and i['y'] >= cy0 and i['y'] + i['h'] <= cy1 and i['h'] < 70 + below and i['w'] > 2]
    return union([lab] + band)


def item(items, text=None, cls=None, name=None, index=0, top=0, bottom=1000):
    hits = [i for i in items if (text is None or i['text'] == text) and (cls is None or i['cls'].startswith(cls))
            and (name is None or i['name'] == name) and on_screen(i, top, bottom)]
    if len(hits) <= index:
        raise KeyError('%s %s %s' % (text, cls, name))
    return union([hits[index]], pad=3)


def biggest(items, cls, index=0):
    hits = [i for i in items if i['cls'].startswith(cls) and on_screen(i, 0, 1000)]
    hits.sort(key=lambda i: (i['y'], i['x']))
    return union([hits[index]], pad=3)


shots = []
skipped = []


def shot(sid, marks):
    shots.append({'id': sid, 'file': 'raw/%s.png' % sid, 'marks': marks})


def mk(n, rect, label):
    return {'rect': rect, 'n': n, 'label': label}


I = {s: load(s) for s in ('connect', 'settings_freq', 'settings_dsp', 'settings_chart', 'settings_period', 'settings_soundspeed',
                          'settings_dataset', 'settings_baud', 'settings_flash', 'upgrade', 'beam_menu', 'beam_save', 'beam_active')}
shot('connect', [mk(1, item(I['connect'], text='COM3', cls='QQuickText', top=250, bottom=350), 'COM3'),
                 mk(2, item(I['connect'], text='921600', cls='QQuickText', top=250, bottom=350), '921600'),
                 mk(3, item(I['connect'], text='Открыть', top=250, bottom=350), 'Открыть')])
shot('settings_freq', [mk(1, row(I['settings_freq'], 'Частота, кГц'), 'Частота, кГц'),
                       mk(2, row(I['settings_freq'], 'Количество импульсов'), 'Количество импульсов')])
shot('settings_dsp', [mk(1, row(I['settings_dsp'], 'Гор. сглаживание'), 'Гор. сглаживание')])
shot('settings_chart', [mk(1, row(I['settings_chart'], 'Количество отсчётов'), 'Количество отсчётов'),
                        mk(2, row(I['settings_chart'], 'Разрешение, мм'), 'Разрешение, мм'),
                        mk(3, row(I['settings_chart'], 'Оффсет отсчётов'), 'Оффсет отсчётов')])
shot('settings_period', [mk(1, row(I['settings_period'], 'Период, мс'), 'Период, мс')])
shot('settings_soundspeed', [mk(1, row(I['settings_soundspeed'], 'Скорость звука, м/с'), 'Скорость звука, м/с')])
shot('settings_dataset', [mk(1, row(I['settings_dataset'], 'Эхограмма', below=60), 'Эхограмма')])
shot('settings_baud', [mk(1, item(I['settings_baud'], text='921600', top=150, bottom=930), '921600'),
                       mk(2, item(I['settings_baud'], text='Задать', cls='QQuickText', top=150, bottom=930), 'Задать')])
flash = [mk(1, item(I['settings_flash'], text='Сохранить в устройство', cls='KButton', top=930, bottom=1000), 'Сохранить в устройство')]
try:
    flash.append(mk(2, item(I['settings_flash'], text='Сохранить в устройство', cls='FlashButton', top=150, bottom=930), 'Сохранить в устройство'))
except KeyError:
    pass
shot('settings_flash', flash)
up = I['upgrade']
path = [i for i in up if i['text'] == 'nanoSSS_v11.21.ufw' and on_screen(i)][0]
dots = [i for i in up if i['text'] == '...' and i['y'] > path['y'] - 20 and i['y'] < path['y'] + 30 and on_screen(i)][0]
shot('upgrade', [mk(1, union([path, dots]), 'файл прошивки и «...»'),
                 mk(2, item(up, text='ПРОШИТЬ', cls='DevButton', top=150, bottom=930), 'ПРОШИТЬ')])
shot('beam_menu', [mk(1, item(I['beam_menu'], name='devBadgeButton'), 'значок сонара'),
                   mk(2, biggest(I['beam_menu'], 'FreqPresetButton', 0), 'узкий луч'),
                   mk(3, biggest(I['beam_menu'], 'FreqPresetButton', 1), 'широкий луч')])
shot('beam_save', [mk(1, item(I['beam_save'], text='Записать'), 'Записать')])
act = [mk(1, item(I['beam_active'], name='presetNameText'), 'Узкий луч'), mk(2, biggest(I['beam_active'], 'FreqPresetButton', 0), 'узкий луч')]
tip = [i for i in I['beam_active'] if i['text'] and 'Удерживайте' in i['text'] and i['cls'].startswith('QQuickText')]
if tip:
    act.append(mk(3, union([tip[0]]), tip[0]['text']))
shot('beam_active', act)

json.dump({'app': build, 'lang': 'ru', 'shots': shots}, open(out_json, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)

thumbs = []
for s in shots:
    im = Image.open('%s/%s' % (run, s['file'])).convert('RGB')
    d = ImageDraw.Draw(im)
    for m in s['marks']:
        x, y, w, h = m['rect']
        d.rectangle([x, y, x + w, y + h], outline=(255, 40, 40), width=4)
        d.text((x + 4, y + 2), str(m['n']), fill=(255, 255, 0))
    d.text((10, 980), s['id'], fill=(255, 255, 0))
    thumbs.append(im.resize((800, 500)))
cols = 3
rows = (len(thumbs) + cols - 1) // cols
sh = Image.new('RGB', (cols * 800, rows * 500), (40, 40, 40))
for k, t in enumerate(thumbs):
    sh.paste(t, ((k % cols) * 800, (k // cols) * 500))
sh.save(sheet)
print('shots', len(shots))
for s in shots:
    print(s['id'], [(m['n'], m['rect']) for m in s['marks']])
