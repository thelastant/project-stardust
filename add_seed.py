import json
p = 'config/spu_images.json'
d = json.load(open(p))
imgs = d.get('images', d)
x = [i for i in imgs if isinstance(i, dict) and i.get('name') == 'eboot_edgezlib_inflate'][0]
ee = x['extra_entries']
addrs = {e['ls'] for e in ee}
for ls, reason in [
    ('0x5d08', 'jump-table target of bi $2 at LS 0x5cc0 (runtime trace; unowned code range 0x5d08..0x5dd4)'),
]:
    if ls not in addrs:
        ee.append({'ls': ls, 'reason': reason})
        print('added', ls)
json.dump(d, open(p, 'w'), indent=2)
print('extra count', len(ee))
