"""Shows the save from both memory cards (the runtime's mc0/ and mc1/) as a single JSON with named keys.

Usage: python tools/mcsave.py <folder that contains mc0/ and mc1/>

Same format as the save.json the runtime writes (native/runtime/wotm_save.inc), so the two can be compared. Block layout
in docs/FORMATOS.md, "Memory card save". Read-only.
"""
import json
import os
import struct
import sys

MAGIC = 0x12345678

# monsterSelectMode: 12 groups of 4 SelectSlot. Groups 0..9 follow the order of Shell::RandomlySelectAI (aiMonsters), entry j of
# a group is costume j. Groups 10 and 11 are the stages (levelUnlocked); entry 47 is left over.
GROUP_MONSTERS = ["raptros", "togera", "preytor", "congar", "robo-47", "agamo", "ultra-v", "magmo", "kineticlops", "zorgulon"]
LEVEL_SLOTS = [(40, "volcano"), (41, "minisf"), (42, "capitol"), (43, "ufo"), (44, "dball"), (45, "bigshot"), (46, "crush")]
MONSTERS = ["null", "congar", "robo-47", "togera", "ultra-v", "preytor", "spider", "kineticlops", "magmo", "raptros",
            "agamo", "zorgulon", "ogre", "assboss", "jelly", "final", "temp16"]
LEVELS = ["default", "central", "vegas", "canyon2", "metro", "airport", "3-Mile", "sanfran", "island", "tokyo", "ufo",
          "capitol", "training", "canyon1", "minisf", "volcano", "tokyodm", "dylan", "david", "lars", "jenn", "yeon",
          "jacob", "steve", "pierre", "dball", "bigshot", "crush"]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def name(table, i):
    return table[i] if i < len(table) else i


def settings(b, j):
    """BASCUS-97197 (0x380 bytes): MonsterMc::getSettingsData."""
    flag = lambda i: u32(b, 0x28 + i * 4)
    j["options"] = {
        "difficulty": u32(b, 0x00),
        "sfxVolume": u32(b, 0x04),
        "musicVolume": u32(b, 0x08),
        "mono": u32(b, 0x10) != 0,
        "controllerConfig": [u32(b, 0x14), u32(b, 0x18)],
        "vibration": [u32(b, 0x1C), u32(b, 0x20)],
    }
    j["battleTokens"] = u32(b, 0x24)
    j["unlocks"] = {
        "monsters": {m: {"skins": [flag(g * 4 + k) != 0 for k in range(4)]} for g, m in enumerate(GROUP_MONSTERS)},
        "levels": {lv: flag(i) != 0 for i, lv in LEVEL_SLOTS},
        "mechaSweetTooth": flag(23) != 0,   # Agamo costume 4; in the runtime this key wins over agamo.skins[3]
    }
    # TheGame+0x1204B0..B8: endurance record per difficulty (8498ce0:docs/investigacoes/2026-10-24/save_campos_abertos.md)
    j["records"] = {"enduranceBest": {d: u32(b, 0xE8 + k * 4) for k, d in enumerate(("easy", "medium", "hard"))}}
    j["unknown"] = {"shell2A40": u32(b, 0x0C), "slot47": flag(47)}


def adventure(b, j, card):
    """savegame.dat (0x280 bytes): MonsterMc::getSaveData, 10 slots of 0x3C (only 0x14 used), into slots card*10.. of j.saves."""
    saves = j.setdefault("saves", [])
    base = card * 10
    saves.extend([None] * (base + 10 - len(saves)))
    for i in range(10):
        saves[base + i] = None
        if i >= min(u32(b, 0), 10):
            continue
        lvl, sel, lives, score, costume = struct.unpack_from("<5I", b, 0x10 + i * 0x3C)
        if lvl or sel or lives or score or costume:
            saves[base + i] = {"level": name(LEVELS, lvl), "monster": name(MONSTERS, sel >> 5), "variant": sel & 31,
                               "lives": lives, "score": score, "costume": costume}
    while saves and saves[-1] is None:
        saves.pop()


def read(path, size, magic_off):
    if not os.path.exists(path):
        return None
    b = open(path, "rb").read()
    if len(b) != size or u32(b, magic_off) != MAGIC:
        print("%s: wrong size or magic, skipped" % path, file=sys.stderr)
        return None
    return b


def main():
    root = sys.argv[1]   # the folder that has mc0/ (and mc1/, card 2)
    j = {"format": 1}
    cards = [os.path.join(root, mc, "BASCUS-97197") for mc in ("mc0", "mc1")]
    for d in reversed(cards):   # options: card 1's win (read last)
        b = read(os.path.join(d, "BASCUS-97197"), 0x380, 0x34C)
        if b:
            settings(b, j)
    for c, d in enumerate(cards):
        b = read(os.path.join(d, "savegame.dat"), 0x280, 0x268)
        if b:
            adventure(b, j, c)
    print(json.dumps(j, indent=2))


if __name__ == "__main__":
    main()
