#!/usr/bin/env python3
"""Build the "police" mod pack: each city's Free Roam cruise .rac with police
cars as its only AI opponents (src/games/remix/police.c spawns them).

  police_pack.py <tune dir> <out pack dir>

<tune dir> holds the disc archives' tune/ files (tune/race/<city>/cruise/easy/
<city>_cruise_easy_cruise_race01.rac, as Free Roam's cruise events name them in
tune/race/<city>.loc). <out> gets tune/race/<city>/cruise/easy/<same name>.rac
and mod.json.

The cruise .rac lists 7 vp_eclipse_04 racers first, then (San Diego, Detroit)
2-3 police cars; the race takes the first min(count, 8, setup +0x40)
opponents, so a cruise with opponents on would get Eclipses. Here the list is
police only, 3 of them (the game takes 2 on low memory: police.c): one parked
by the cruise's start (the farthest Eclipse spot), then the city's own police
blocks (patrol routes kept), then more Eclipse spots. Every police car is
enabled at the start (DisabledAtStart 0) and respawns near the player after a
lost chase (RespawnCop 1, as the career's races do). The rest of the file
(ambient cars, bangers, triggers) is the original's."""

import json
import os
import re
import sys

CITIES = {   # folder, file prefix, the city's police car (vehicle.lst Class COPCAR)
    'sd': ('sd', 'vp_impalass_cop_s_96'),
    'atlanta': ('atl', 'vp_impalass_cop_a_96'),
    'detroit': ('detroit', 'vp_impalass_cop_d_96'),
    'tokyo': ('tokyo', 'vp_d_tokyo_cop_01'),
}
COPS = 3

MOD = {"id": "police", "name": "Police", "author": "mc3-web", "credit": "",
       "description": "Police cars patrol Free Roam and chase you when you speed or crash",
       "version": "1.0", "kind": "gameplay"}


def blocks(text):
    """(head, [Opponent blocks], tail): blocks are 'Opponent' + its balanced braces."""
    out, pos, head_end = [], 0, None
    for m in re.finditer(r'^Opponent\s*$', text, re.M):
        if m.start() < pos:
            continue
        if head_end is None:
            head_end = m.start()
        i = text.index('{', m.end())
        depth = 0
        for j in range(i, len(text)):
            depth += {'{': 1, '}': -1}.get(text[j], 0)
            if depth == 0:
                break
        end = j + 1
        while end < len(text) and text[end] in '\r\n':
            end += 1
        out.append(text[m.start():end])
        pos = end
    if head_end is None:
        raise SystemExit('no Opponent blocks')
    return text[:head_end], out, text[pos:]


def field(block, name, value):
    return re.sub(r'(\t%s )\S+' % name, lambda m: m.group(1) + value, block, count=1)


def cop_from(block, car, keep_route):
    b = re.sub(r'(\tCarType )\S+', lambda m: m.group(1) + car, block, count=1)
    b = field(b, 'DisabledAtStart', '0')
    b = field(b, 'RespawnCop', '1')
    if not keep_route:   # an Eclipse's spot: no route of its own
        b = re.sub(r'\tRoute\s*\{.*?\n\t\}', '\tRoute\n\t{\n\t\tRedoStartCutOnLap 1 \n\t\tNumOfLaps 1 \n'
                   '\t\tnumWaypoints 0 \n\t\tnumEdges 0 \n\t}', b, count=1, flags=re.S)
    return b


def build(src, dst, city, prefix, car):
    rel = 'race/%s/cruise/easy/%s_cruise_easy_cruise_race01.rac' % (city, prefix)
    with open(os.path.join(src, rel), 'rb') as f:
        text = f.read().decode('latin-1')
    head, opps, tail = blocks(text)
    police = [b for b in opps if re.search(r'\tCarType \S*cop', b)]
    others = [b for b in opps if b not in police]
    if not others:
        raise SystemExit('%s: no racer spots' % rel)
    cops = [cop_from(others[-1], car, False)] + [cop_from(b, car, True) for b in police]
    cops += [cop_from(b, car, False) for b in reversed(others[:-1])]
    cops = cops[:COPS]
    head = re.sub(r'numOpponents \d+', 'numOpponents %d' % len(cops), head, count=1)
    out = os.path.join(dst, 'tune', rel)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'wb') as f:
        f.write((head + ''.join(cops) + tail).encode('latin-1'))
    print('%s: %d police (%s), %d of the city\'s own' % (rel, len(cops), car, min(len(police), COPS - 1)))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    if os.path.isdir(os.path.join(src, 'tune')):
        src = os.path.join(src, 'tune')
    for city, (prefix, car) in CITIES.items():
        build(src, dst, city, prefix, car)
    with open(os.path.join(dst, 'mod.json'), 'w', newline='\n') as f:
        json.dump(MOD, f, indent=2)
        f.write('\n')


if __name__ == '__main__':
    main()
