#!/usr/bin/env python3
"""Attack table for one monster, read from the user's own disc file RST/<MONSTER>.CHR (read-only; nothing goes into the repo).

Usage:
    python tools/chr_dump.py disc/RST/MONKEY.CHR            # punch table (key 0xFCC)
    python tools/chr_dump.py disc/RST/MONKEY.CHR --keys     # list the keys in the file
    python tools/chr_dump.py disc/RST/MONKEY.CHR --csv      # the same table as CSV
    python tools/chr_dump.py disc/RST/*.CHR                 # several monsters
    python tools/chr_dump.py disc/RST/*.CHR --specials      # the monster's special, crowd control and the stamina gain

Format (docs/FORMATOS.md, "Character"): a 0xC-byte header followed by keys {u32 id; u32 size including the header;
data}, read by charreset::readKey. Key 0xFCC holds the StatePunch configs: 10 combo positions (1st hit, 2nd hit...)
x 6 attack types, each 0xA0 bytes starting at key + 0x10 + pos*0x3C0 + type*0xA0. The game copies them into the
Monster's StatePunch (Monster + 0x8470 + 0x20 + pos*0x420 + type*0xB0, the PunchConfig), which training mode reads from RAM.

--specials: the keys of the special states (SPECIAL_KEYS: offset in the key -> offset in the state, which lives at Monster + base)
and the shock wave of CrowdControl and RobotSpecial (ring in the key: range and the ring's fields +0x4..+0x18; what each one does
is in b4e810b:docs/investigacoes/2026-10-26/move_list_campos_especiais.md, "ShockWaveConfig"). The AttackState stamina gain
(state +0x14/+0x18, creditStaminaForAttack) is not in the file: no key writes it and the constructor zeroes it.
OGRE and SPIDER use a different key numbering (old format, without the 0x26B0-byte 0xFCC) and are left out.
"""
import argparse
import os
import struct
import sys

PUNCH_KEY = 0xFCC
PUNCH_DEFAULTS_KEY = 0xFA1   # +0x30: default fist radius (StatePunch +0x2980), used when the attack's own radius is 0
TIERS, TYPES = 10, 6

# locators (named bones): Monster::handleLocator, ids 0x802.. (treino_hitboxes.md)
LOCATORS = {0x802: 'L HAND', 0x803: 'R HAND', 0x804: 'L FOOT', 0x805: 'R FOOT'}

PUNCH_KEY_SIZE = 0x26B0   # size of 0xFCC in the format readKey reads (OGRE/SPIDER: old numbering)

# Special-state keys (charreset::readKey, case = key - 0xFA1): (state name, state offset in Monster, required type
# (m_typeBits, Monster +0x14; None = any), fields (offset in key, offset in state, description; trailing 'i' = raw integer),
# wave (key offsets of the ring count, the timeScale, field +0 of ring 0 and the 1st ring; ShockWaveConfig offset in the state) or
# None, conditionals (key offsets copied only when != 0)). A description with '?' = role not confirmed in the state's asm
# (80b3802:docs/investigacoes/2026-10-25/move_list_especiais.md).
SPECIAL_KEYS = {
    0xFA9: ('SonicRoar', 0xF6F4, None, [
        (0x08, 0x1C, 'windup blend'), (0x0C, 0x20, 'windup (frames)'), (0x10, 0x24, 'loop blend?'),
        (0x14, 0x28, 'loop speed?'), (0x18, 0x2C, 'loop (frames)'), (0x1C, 0x30, 'range growth per frame'),
        (0x20, 0x34, 'max range'), (0x24, 0x38, 'cone (degrees, full width)'), (0x28, 0x3C, 'damage per target'),
        (0x2C, 0x40, 'damage to scenery (accumulated)'), (0x38, 0x44, '? (not read by the state)'), (0x3C, 0x48, '? (not read by the state)'),
        (0x30, 0x4C, 'winddown blend?'), (0x34, 0x50, 'winddown (frames)')], None, ()),
    0xFAA: ('BugAttack', 0xF9D8, None, [
        (0x08, 0x1C, '?'), (0x0C, 0x20, 'windup (frames)'), (0x10, 0x24, 'releases the bug (fraction of windup)'), (0x14, 0x28, '?'),
        (0x18, 0x2C, 'loop (frames)'), (0x1C, 0x30, 'launches the bug (fraction of loop)'), (0x20, 0x34, '?'),
        (0x24, 0x38, 'winddown (frames)')], None, ()),
    0xFAD: ('FireBreath', 0xF974, None, [
        (0x08, 0x38, 'windup blend'), (0x0C, 0x3C, 'windup (frames)'), (0x10, 0x40, 'loop blend'), (0x14, 0x44, '?'),
        (0x18, 0x1C, 'breath duration (frames)'), (0x1C, 0x20, 'range'), (0x20, 0x24, 'damage/s to monsters'),
        (0x24, 0x28, 'damage/s to scenery'), (0x28, 0x48, '?'), (0x2C, 0x4C, '?'), (0x30, 0x2C, 'fire damage (Raptros only)'),
        (0x34, 0x30, 'frames on fire (Raptros only)'), (0x3C, 0x34, 'victim slowdown')], None, (0x3C,)),
    0xFBC: ('RockSpecial', 0x102A8, None, [
        (0x08, 0x20, 'windup blend'), (0x0C, 0x24, 'windup (frames)'), (0x10, 0x28, 'loop blend'), (0x14, 0x2C, 'loop speed'),
        (0x18, 0x30, '?'), (0x1C, 0x34, 'winddown (frames)'), (0x20, 0x38, 'head fuse (s)'),
        (0x24, 0x3C, 'upward knockback'), (0x28, 0x40, 'outward knockback')], None, ()),
    0xFC4: ('ZapAttack', 0x1059C, None, [
        (0x08, 0x1C, 'windup blend'), (0x0C, 0x20, 'windup (frames)'), (0x10, 0x24, '?'), (0x14, 0x28, '?'),
        (0x18, 0x2C, 'loop (frames)'), (0x1C, 0x30, 'arc (+0x30)'), (0x20, 0x34, 'range (Manhattan)'), (0x24, 0x38, 'damage/s'),
        (0x30, 0x3C, 'outward knockback (end)'), (0x34, 0x40, 'upward knockback (end)'), (0x28, 0x44, '?'),
        (0x2C, 0x48, 'winddown (frames)'), (0x38, 0x4C, 'arc amplitude'), (0x3C, 0x50, '?')], None, ()),
    0xFC5: ('GrappleHook', 0x10600, None, [
        (0x08, 0x1C, '?'), (0x0C, 0x20, 'windup (frames)'), (0x10, 0x24, '?'), (0x14, 0x28, 'loop speed'), (0x18, 0x30, 'range'),
        (0x1C, 0x34, 'damage'), (0x30, 0x38, '?'), (0x28, 0x3C, '? (not read by the state)'), (0x2C, 0x40, '? (not read by the state)'),
        (0x20, 0x44, '? (not read by the state)'), (0x24, 0x48, 'winddown (frames)'), (0x34, 0x68, 'pull rate'),
        (0x38, 0x6C, 'pull rate (height)'), (0x3C, 0x70, 'ground push (+0x1E8)'), (0x40, 0x74, '?'),
        (0x44, 0x78, 'tip speed rate'), (0x48, 0x7C, 'homing rate'), (0x4C, 0x80, 'tip speed (mph)')],
        None, ()),
    0xFC7: ('AirStrike', 0x10790, None, [
        (0x08, 0x1C, '?'), (0x0C, 0x20, 'windup (frames)'), (0x10, 0x24, '?'), (0x14, 0x28, '?'), (0x18, 0x2C, 'loop (frames)'),
        (0x1C, 0x30, '?'), (0x20, 0x34, 'winddown (frames)'), (0x24, 0x174, 'damage per disc shot'),
        (0x28, 0x178, 'disc duration (fields)', 'i')], None, ()),
    0xFC8: ('LavaBlast', 0x10930, None, [
        (0x08, 0x1C, '?'), (0x0C, 0x20, 'windup (frames)'), (0x10, 0x24, '?'), (0x14, 0x28, '?'), (0x18, 0x2C, '?'),
        (0x2C, 0x30, '?'), (0x24, 0x34, '?'), (0x28, 0x38, '?'), (0x1C, 0x3C, '?'), (0x20, 0x40, 'winddown (frames)'),
        (0x30, 0x190, 'number of balls', 'i'), (0x34, 0x194, 'horizontal homing (game reads a float)'), (0x38, 0x198, 'vertical homing (game reads a float)'),
        (0x3C, 0x19C, 'damage per ball'), (0x40, 0x1A0, 'speed (mph)'), (0x44, 0x1A4, 'fire damage'),
        (0x48, 0x1A8, 'fields on fire')], None, ()),
    0xFCF: ('RobotSpecial', 0xFE5C, 0x40, [
        (0x08, 0x1C, '?'), (0x0C, 0x20, 'windup (frames)'), (0x10, 0x24, '?'), (0x14, 0x28, '?'), (0x18, 0x34, '?'),
        (0x1C, 0x38, 'winddown (frames)'), (0x20, 0x3C, 'min frames on target'), (0x24, 0x40, 'reticle lock (fields)'),
        (0x28, 0x44, '?'), (0x2C, 0x48, 'missile lifetime (s)'), (0x30, 0x4C, 'missile speed (mph)'), (0x34, 0x50, 'homing'),
        (0x38, 0x444, 'effect speed')], (0x3C, 0x40, 0x44, 0x48, 0x58, 7), ()),
    0xFE1: ('CrowdControl', 0xF3F0, None, [
        (0x08, 0x1C, 'windup blend?'), (0x0C, 0x20, 'windup (frames)'), (0x10, 0x24, '?'), (0x14, 0x28, '?'), (0x18, 0x2C, '?'),
        (0x1C, 0x30, 'winddown blend'), (0x20, 0x34, '?'), (0x24, 0x38, 'wave fires (fraction of loop)')],
        (0x28, 0x2C, 0x30, 0x34, 0x3C, 7), ()),
}
# Variants that no .CHR on the disc uses: the same fields, but the ring in the key has 5 floats (no +0x14/+0x18)
SPECIAL_KEYS[0xFE0] = SPECIAL_KEYS[0xFCF][:4] + (SPECIAL_KEYS[0xFCF][4][:5] + (5,), ())
SPECIAL_KEYS[0xFDF] = SPECIAL_KEYS[0xFE1][:4] + (SPECIAL_KEYS[0xFE1][4][:5] + (5,), ())

# 0xFAA, after the BugAttack fields: the HomingBug statics (.sbss), one float each starting at key + 0x28
HOMING_BUG_STATICS = ['homingFactor', 'avoidanceFactor', 'maxForce', 'maxVel', 'initialVel', 'initialHealth', 'attachRange',
                      'attachMinAngle', 'attachFields', 'maxSeekFields', 'headRadius', 'feelerRadius', 'collisRadius',
                      'damageRadius', 'damagePerSecond', 'maxMonsterDamage', 'targetLockMaxRange', 'targetLockMinAngle']

# m_typeBits >> 5 (MonsterNewNames) by file name, for the type condition of 0xFCF/0xFE0
FILE_TYPE = {'MONKEY': 1, 'ROBOT': 2, 'LIZARD': 3, 'SHOGUN': 4, 'MANTIS': 5, 'SPIDER': 6, 'ENERGY': 7, 'LAVA': 8, 'DRAGON': 9,
             'ROCK': 10, 'ALIEN': 11, 'OGRE': 12, 'ASSBOSS': 13, 'JELLY': 14, 'FINAL': 15}


def read_keys(data):
    keys, off = [], 0xC
    while off + 8 <= len(data):
        kid, size = struct.unpack_from('<II', data, off)
        if kid == 0 or size < 8 or off + size > len(data):
            break
        keys.append((kid, off, size))
        off += size
    return keys


def f32(d, o):
    return struct.unpack_from('<f', d, o)[0]


def i32(d, o):
    return struct.unpack_from('<i', d, o)[0]


def loc_name(v):
    if v < 0x7D0:
        return '-'
    return LOCATORS.get(v, '0x%X' % v)


def legacy(keys):
    """Old key numbering (OGRE, SPIDER): the current readKey would read them as something else."""
    return not any(k == PUNCH_KEY and s == PUNCH_KEY_SIZE for k, _, s in keys)


def specials(data, keys, name):
    """Text lines for --specials: the special and crowd control keys, in file order."""
    if legacy(keys):
        return ['%s: old key numbering (no 0x%X of 0x%X bytes); not in the table' % (name, PUNCH_KEY, PUNCH_KEY_SIZE)]
    out = ['%s:' % name]
    for kid, o, size in keys:
        spec = SPECIAL_KEYS.get(kid)
        if not spec:
            continue
        st_name, base, need_type, fields, wave, cond = spec
        out.append('  key 0x%X -> %s (Monster +0x%X)%s' % (kid, st_name, base,
                   '' if need_type is None else ', only if m_typeBits == 0x%X' % need_type))
        if need_type is not None and FILE_TYPE.get(name, 0) << 5 != need_type:
            out.append('    (ignored for this monster: readKey requires type 0x%X)' % need_type)
            continue
        for fld in fields:
            ko, so, desc = fld[:3]
            if ko + 4 > size:
                continue
            if len(fld) > 3 and fld[3] == 'i':
                val = '%d' % i32(data, o + ko)
            else:
                v = f32(data, o + ko)
                if ko in cond and v == 0:
                    out.append('    state +0x%03X  (key +0x%02X = 0: keeps the constructor value)  %s' % (so, ko, desc))
                    continue
                val = '%g' % v
                if v != 0 and abs(v) < 1e-30:   # integer in the file where the field is a float: the game ends up with ~0
                    desc += ' [in file: integer %d; as a float it is ~0]' % i32(data, o + ko)
                    val = '~0'
            out.append('    state +0x%03X = %-10s %s' % (so, val, desc))
        if kid == 0xFAA:
            for i, s in enumerate(HOMING_BUG_STATICS):
                if 0x28 + 4 * i + 4 <= size:
                    out.append('    HomingBug::s_%-18s = %g' % (s, f32(data, o + 0x28 + 4 * i)))
        if wave:
            kn, kts, k0, kring, cfg, nf = wave
            stride = nf * 4   # in the key: range and the ring's fields +0x4.. (7 floats in the disc's keys, 5 in the variants)
            count = f32(data, o + kn)
            n = 0
            while n < count and n < 20 and kring + stride * (n + 1) <= size:   # readKey loops while i+1 < n (float)
                n += 1
            out.append('    wave (ShockWaveConfig at state +0x%X): %d rings (int at state +0x%X), timeScale %g (state +0x%X), vertical tolerance %g '
                       '(+0x0 of ring 0, state +0x%X; 0 = off)' % (cfg, n, cfg, f32(data, o + kts), cfg + 0x2A0, f32(data, o + k0), cfg + 0x20))
            out.append('      ring    range    dmg   dead  force C  force 10%s   (ring i at state +0x%X + i*0x1C; range at state +0x%X + 4i)' % (
                '     stun   recoil' if nf > 5 else '', cfg + 0x20, cfg + 0x250))
            for i in range(n):
                r = o + kring + stride * i
                vals = [f32(data, r + 4 * j) for j in range(nf)]
                out.append('      %4d  %7g  %5g  %5g  %7g  %8g' % tuple([i] + vals[:5]) + ''.join('  %7g' % v for v in vals[5:]))
            out.append('      (ShockWave::update: forces C/10 -> HitEvent +0x38/+0x34, both 0 = recoil only; stun -> HitEvent +0x3C, '
                       '> 100 stuns; recoil -> Recoil +0x4C of the victim)')
    if len(out) == 1:
        out.append('  no special key')
    out.append('  AttackState stamina gain (state +0x14/+0x18): not from the .CHR (the constructor zeroes it; no key writes it)')
    return out


def punch_table(data, keys):
    """Punch table rows (dicts), ordered by position, then type; [] without key 0xFCC."""
    by_id = {k: (o, s) for k, o, s in keys}
    if PUNCH_KEY not in by_id or by_id[PUNCH_KEY][1] != PUNCH_KEY_SIZE:
        return [], None
    o, _ = by_id[PUNCH_KEY]
    base = o + 0x10
    default_r = None
    if PUNCH_DEFAULTS_KEY in by_id:
        fo, fs = by_id[PUNCH_DEFAULTS_KEY]
        if fs >= 0x34:
            default_r = f32(data, fo + 0x30)
    rows = []
    for t in range(TIERS):
        for k in range(TYPES):
            b = base + t * 0x3C0 + k * 0xA0
            anim = i32(data, b + 0x28)
            dmg = f32(data, b + 0x20)
            locs = [i32(data, b + 0x10), i32(data, b + 0x14)]
            if anim <= 0 and dmg == 0:
                continue
            r = f32(data, b + 0xA8)
            rows.append({
                'pos': t, 'tipo': k, 'anim': anim,
                'loc': '+'.join(loc_name(v) for v in locs if v >= 0x7D0) or '-',
                'ini': f32(data, b + 0x2C), 'fim': f32(data, b + 0x30),
                'cont_ini': f32(data, b + 0x34), 'cont_fim': f32(data, b + 0x38),
                'dano': dmg, 'raio': r if r > 0 or default_r is None else default_r, 'raio_padrao': r <= 0,
                'kb_h': f32(data, b + 0x8C), 'kb_v': f32(data, b + 0x90),
                'atord': f32(data, b + 0x94), 'pausa': f32(data, b + 0x98),
            })
    return rows, default_r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+')
    ap.add_argument('--keys', action='store_true', help='list the keys in the file')
    ap.add_argument('--csv', action='store_true', help='table as CSV')
    ap.add_argument('--specials', action='store_true', help='special and crowd control keys (and the stamina gain)')
    a = ap.parse_args()
    if a.csv:
        print('monstro,pos,tipo,anim,loc,ini,fim,cont_ini,cont_fim,dano,raio,kb_h,kb_v,atord,pausa')
    for path in a.files:
        data = open(path, 'rb').read()
        keys = read_keys(data)
        name = os.path.splitext(os.path.basename(path))[0]
        if a.keys:
            print('%s: %s' % (name, ' '.join('%X(%X)' % (k, s) for k, _, s in keys)))
            continue
        if a.specials:
            print('\n'.join(specials(data, keys, name)))
            continue
        rows, default_r = punch_table(data, keys)
        if a.csv:
            for r in rows:
                print('%s,%d,%d,%d,%s,%.3f,%.3f,%.3f,%.3f,%.1f,%.2f,%.0f,%.0f,%.0f,%.0f' % (
                    name, r['pos'], r['tipo'], r['anim'], r['loc'], r['ini'], r['fim'], r['cont_ini'], r['cont_fim'],
                    r['dano'], r['raio'], r['kb_h'], r['kb_v'], r['atord'], r['pausa']))
            continue
        if not rows:
            print('%s: no key 0x%X (punches)' % (name, PUNCH_KEY))
            continue
        print('%s: punches (key 0x%X), default radius %s' % (name, PUNCH_KEY, '%.2f' % default_r if default_r is not None else '?'))
        print('  pos type anim  window       dmg  radius stun   knockback   pause  locators')
        for r in rows:
            print('  %3d %4d %4d  %4.0f-%3.0f%%  %5.1f  %5.1f%s %5.0f  %4.0f/%-4.0f  %5.0f  %s' % (
                r['pos'] + 1, r['tipo'], r['anim'], r['ini'] * 100, r['fim'] * 100, r['dano'], r['raio'],
                '*' if r['raio_padrao'] else ' ', r['atord'], r['kb_h'], r['kb_v'], r['pausa'], r['loc']))
        print('  (pos: hit in the combo, 1 = first; window: % duration of the animation with hit testing; * default radius; stun > 100 stuns)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
