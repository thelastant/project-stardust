import json
p = 'config/extra_seeds.json'
d = json.load(open(p))
seeds = d.get('seeds', [])
addrs = {s['addr'] for s in seeds}
for a, reason in [
    ('0x0041778c', 'PPU function start missed by recovery inside a code span (boot log: unknown target 0x0041778c from 0x004174a4)'),
]:
    if a not in addrs:
        seeds.append({'addr': a, 'reason': reason})
        print('added', a)
json.dump(d, open(p, 'w'), indent=2)
print('seeds', len(seeds))
