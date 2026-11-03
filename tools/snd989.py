#!/usr/bin/env python3
"""Reader and writer for War of the Monsters' sound formats (989snd): .BNK (SBlk v1), .VPK and .VAG. Documented in
docs/FORMATOS.md ("Sound").

  python tools/snd989.py bnk disc/SND/SHELL_S.BNK            # sounds and grains of a bank
  python tools/snd989.py stats disc/SND                       # grain types/flags used across all banks
  python tools/snd989.py wav disc/SND/SHELLMUS.BNK 0 out.wav  # sound N of the bank (TONE grains only, no envelope) as WAV
  python tools/snd989.py wav disc/VAG/CITYAMB.VPK - out.wav   # a whole .VPK/.VAG stream as WAV

The other way (8/16/24/32-bit PCM or float WAV to SPU2 ADPCM; the encoder simulates the game's decoder):
  python tools/snd989.py vpk music.wav new/CITYAMB.VPK --like disc/VAG/CITYAMB.VPK   # same rate and volumes as the original
  python tools/snd989.py vag effect.wav new/VICTORY.VAG [--loop]
  python tools/snd989.py bnk-replace disc/SND/SHELL_S.BNK 3 new.wav new/SHELL_S.BNK   # replace sound 3 in the bank
bnk-replace resamples to the rate at which the bank plays each TONE, keeps the original's loop and fixes up the offsets of the
other sounds. Resampling is linear: for music, resample beforehand in an audio editor.
"""
import collections
import os
import struct
import sys
import wave

GRAIN_NAMES = {0: "NULL", 1: "TONE", 2: "XREF_ID", 3: "XREF_NUM", 4: "LFO", 5: "STARTCHILD", 6: "STOPCHILD", 7: "PLUGIN",
               8: "BRANCH", 9: "TONE2", 20: "CONTROL_NULL", 21: "LOOP_START", 22: "LOOP_END", 23: "LOOP_CONTINUE", 24: "STOP",
               25: "RAND_PLAY", 26: "RAND_DELAY", 27: "RAND_PB", 28: "PB", 29: "ADD_PB", 30: "SET_REG", 31: "SET_REG_RAND",
               32: "INC_REG", 33: "DEC_REG", 34: "TEST_REG", 35: "MARKER", 36: "GOTO_MARKER", 37: "GOTO_RANDOM_MARKER",
               38: "WAIT_ALL_VOICES", 39: "PLAY_CYCLE", 40: "ADD_REG", 41: "KEY_OFF", 42: "KILL_VOICES", 43: "ON_STOP_MARKER",
               44: "COPY_REG"}


def read_bnk(path):
    """File: u32 type (3), u32 nChunks (2), {u32 offset, u32 size} x n; chunk 0 = SBlk, chunk 1 = ADPCM samples."""
    d = open(path, "rb").read()
    typ, n = struct.unpack_from("<II", d, 0)
    chunks = [struct.unpack_from("<II", d, 8 + 8 * i) for i in range(n)]
    blk = d[chunks[0][0]:chunks[0][0] + chunks[0][1]]
    smp = d[chunks[1][0]:chunks[1][0] + chunks[1][1]] if n > 1 else b""
    if blk[:4] != b"SBlk":
        raise ValueError("%s: no SBlk" % path)
    ver, flags, bank_id = struct.unpack_from("<II4s", blk, 4)
    n_sounds, n_grains, n_vags = struct.unpack_from("<hhh", blk, 0x16)
    first_sound, first_grain, vags_in_sr, vag_data_size, sram_alloc, next_block = struct.unpack_from("<6I", blk, 0x1c)
    block_names, sfx_ud = struct.unpack_from("<II", blk, 0x34)   # v1: no GrainData
    sounds = []
    for i in range(n_sounds):
        o = first_sound + 12 * i
        vol, vgroup, pan, ng, inst, sflags, fg = struct.unpack_from("<bbhbbHI", blk, o)
        grains = []
        for g in range(ng):
            go = first_grain + fg + 0x28 * g
            gtype, delay = struct.unpack_from("<Ii", blk, go)
            body = blk[go + 8:go + 0x28]
            gr = {"type": gtype, "delay": delay, "raw": body}
            if gtype in (1, 9):
                (pri, tvol, note, fine, tpan, mlo, mhi, pblo, pbhi, adsr1, adsr2, tflags, soff) = struct.unpack_from("<bbbbhbbbbHHHI", body, 0)
                gr.update(pri=pri, vol=tvol, note=note, fine=fine, pan=tpan, adsr1=adsr1, adsr2=adsr2, tflags=tflags, sample=soff)
            elif gtype in (5, 6, 8):
                gr["play"] = struct.unpack_from("<iibbbbi16s", body, 0)
            else:
                gr["params"] = struct.unpack_from("<hhhh", body, 0)
            grains.append(gr)
        sounds.append({"vol": vol, "group": vgroup, "pan": pan, "inst": inst, "flags": sflags, "grains": grains})
    names = {}
    if flags & 0x100 and block_names:
        name_tab = block_names + struct.unpack_from("<I", blk, block_names + 8)[0]
        for h in struct.unpack_from("<32h", blk, block_names + 0x18):
            o = name_tab + 0x14 * h
            while o + 0x14 <= len(blk) and blk[o] != 0:
                nm = blk[o:o + 16].split(b"\0")[0].decode("latin-1")
                names.setdefault(struct.unpack_from("<h", blk, o + 16)[0], nm)
                o += 0x14
    return {"type": typ, "version": ver, "flags": flags, "bank": bank_id[::-1].decode("latin-1"), "sounds": sounds,
            "n_grains": n_grains, "n_vags": n_vags, "samples": smp, "names": names}


# SPU2 ADPCM: 16-byte blocks (filter/shift, flags, 14 bytes = 28 4-bit samples)
FILTERS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def adpcm_decode(data, off=0, stop_at_end=True, hist=None):
    out = []
    h1, h2 = hist or (0, 0)
    while off + 16 <= len(data):
        sf, fl = data[off], data[off + 1]
        shift, filt = sf & 0xF, min(sf >> 4, 4)
        k0, k1 = FILTERS[filt]
        for i in range(28):
            b = data[off + 2 + i // 2]
            nib = (b >> 4) if i & 1 else (b & 0xF)
            s = (nib << 12) & 0xFFFF
            s = (s - 0x10000 if s & 0x8000 else s) >> shift
            s += (h1 * k0 + h2 * k1) >> 6
            s = max(-32768, min(32767, s))
            out.append(s)
            h2, h1 = h1, s
        off += 16
        if stop_at_end and fl & 1:   # end (with or without a loop)
            break
    return out, off


def _block_error(samples, i0, h1, h2, filt, shift):
    """Encodes 28 samples with (filter, shift), simulating the decoder; returns (error, nibbles, h1, h2)."""
    k0, k1 = FILTERS[filt]
    err, nibs = 0, []
    for i in range(28):
        s = samples[i0 + i] if i0 + i < len(samples) else 0
        pred = (h1 * k0 + h2 * k1) >> 6
        # nibble n reconstructs (n << 12 >> shift) + pred; pick the closest n, in -8..7
        q = ((s - pred) << shift) / 4096.0
        n = max(-8, min(7, int(q + 0.5) if q >= 0 else -int(-q + 0.5)))
        out = max(-32768, min(32767, ((n << 12) >> shift) + pred))
        err += (out - s) * (out - s)
        nibs.append(n & 15)
        h2, h1 = h1, out
    return err, nibs, h1, h2


def adpcm_encode(samples, loop_start=None, end_flags=True):
    """16-bit PCM (list of int) -> SPU2 ADPCM, the exact inverse of `adpcm_decode` (the encoder simulates the decoder, so
    what comes out is what the game plays). loop_start: index of the sample where the loop restarts (None = no loop).
    end_flags: write the end/loop flags (banks and .VAG); .VPK files have no flags. Per block, picks the filter and shift with
    the smallest error, trying the estimated shift and its neighbors."""
    out = bytearray()
    h1 = h2 = 0
    nblk = max(1, (len(samples) + 27) // 28)
    lblk = None if loop_start is None else min(nblk - 1, loop_start // 28)
    for b in range(nblk):
        i0 = b * 28
        best = None
        for filt in range(5):
            k0, k1 = FILTERS[filt]
            # maximum residual when predicting from the original samples: estimates the shift
            p1, p2 = (samples[i0 - 1] if i0 >= 1 else 0), (samples[i0 - 2] if i0 >= 2 else 0)
            mx = 0
            for i in range(28):
                s = samples[i0 + i] if i0 + i < len(samples) else 0
                mx = max(mx, abs(s - ((p1 * k0 + p2 * k1) >> 6)))
                p2, p1 = p1, s
            shift = 12
            while shift > 0 and (mx << shift) > 7 * 4096:
                shift -= 1
            for sh in {shift, max(0, shift - 1), min(12, shift + 1)}:
                r = _block_error(samples, i0, h1, h2, filt, sh)
                if best is None or r[0] < best[0][0]:
                    best = (r, filt, sh)
        (err, nibs, h1, h2), filt, sh = best
        flags = 0
        if end_flags:
            if lblk is not None and b == lblk:
                flags |= 4 | 2
            if b == nblk - 1:
                flags |= 1 | (2 if lblk is not None else 0)
        out += bytes((filt << 4 | sh, flags)) + bytes(nibs[2 * k] | nibs[2 * k + 1] << 4 for k in range(14))
    return bytes(out)


def read_wav_any(path):
    """PCM WAV (8/16/24/32 bits), 32-bit float or WAVE_FORMAT_EXTENSIBLE -> (Hz, [int16 channel])."""
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise ValueError("%s: not a WAV" % path)
    o, fmt, data = 12, None, None
    while o + 8 <= len(d):
        cid, n = struct.unpack_from("<4sI", d, o)
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", d, o + 8)
            if fmt[0] == 0xFFFE:
                fmt = (struct.unpack_from("<H", d, o + 8 + 24)[0],) + fmt[1:]
        elif cid == b"data":
            data = d[o + 8:o + 8 + n]
        o += 8 + n + (n & 1)
    if not fmt or data is None:
        raise ValueError("%s: WAV without fmt or data" % path)
    tag, nch, rate, _, _, bits = fmt
    bpp = bits // 8
    n = len(data) // (bpp * nch)
    chans = [[] for _ in range(nch)]
    for i in range(n):
        for c in range(nch):
            p = (i * nch + c) * bpp
            if tag == 3 and bits == 32:
                v = int(max(-1.0, min(1.0, struct.unpack_from("<f", data, p)[0])) * 32767)
            elif tag == 1 and bits == 8:
                v = (data[p] - 128) << 8
            elif tag == 1 and bits == 16:
                v = struct.unpack_from("<h", data, p)[0]
            elif tag == 1 and bits == 24:
                v = int.from_bytes(data[p:p + 3], "little", signed=True) >> 8
            elif tag == 1 and bits == 32:
                v = struct.unpack_from("<i", data, p)[0] >> 16
            else:
                raise ValueError("%s: format %d with %d bits not supported" % (path, tag, bits))
            chans[c].append(v)
    return rate, chans


def resample(x, src, dst):
    """Linear resampling (good enough for effects; for music, resample beforehand in an editor)."""
    if src == dst or not x:
        return list(x)
    n = max(1, int(round(len(x) * dst / src)))
    out = []
    for i in range(n):
        t = i * src / dst
        k = int(t)
        f = t - k
        a = x[min(k, len(x) - 1)]
        b = x[min(k + 1, len(x) - 1)]
        out.append(int(round(a + (b - a) * f)))
    return out


def to_channels(chans, n):
    """Adjusts the channel count: mono becomes n copies; with more channels than n, the extra ones are mixed into the last."""
    if len(chans) == n:
        return chans
    if len(chans) == 1:
        return [list(chans[0]) for _ in range(n)]
    if n == 1:
        return [[sum(v) // len(v) for v in zip(*chans)]]
    return chans[:n]


def write_vag(path, rate, samples, name="WOTM", loop=False):
    """Single-channel VAGp: 0x30-byte big-endian header (version 0x20, data size, Hz, name) followed by the ADPCM."""
    body = bytes(16) + adpcm_encode(samples, 0 if loop else None)
    hdr = b"VAGp" + struct.pack(">III", 0x20, 0, len(body), ) + struct.pack(">I", rate) + bytes(12) + name.encode()[:16].ljust(16, b"\0")
    with open(path, "wb") as f:
        f.write(hdr + body)


VPK_HALF = 0x5000   # bytes per channel per block (the header field holds the pair: 0xA000)


def write_vpk(path, rate, chans, vols=None):
    """.VPK like the ones on the disc: ' KPV', bytes per channel, start 0x800, block pair (0x5000 per channel), Hz, channels,
    {u16 vol, u16 pan} per channel; data in alternating 0x5000-byte blocks per channel, ADPCM without flags."""
    enc = [adpcm_encode(c, None, end_flags=False) for c in chans]
    per = max(len(e) for e in enc)
    nblk = (per + VPK_HALF - 1) // VPK_HALF
    vols = vols or ([(127, 270), (127, 90)] if len(chans) == 2 else [(127, 0)] * len(chans))
    # bytes per channel rounded up to whole blocks: the native reader (wotm_snd.hpp) only reads start + bytes per channel x
    # channels from the file, and the right channel's last block would be cut off; the cost is up to 0.8 s of silence at the end
    hdr = bytearray(b" KPV" + struct.pack("<5I", nblk * VPK_HALF, 0x800, VPK_HALF * len(chans), rate, len(chans)))
    for v, p in vols:
        hdr += struct.pack("<HH", v, p)
    hdr += bytes(0x800 - len(hdr))
    out = bytearray(hdr)
    for b in range(nblk):
        for e in enc:
            blk = e[b * VPK_HALF:(b + 1) * VPK_HALF]
            out += blk + bytes(VPK_HALF - len(blk))
    with open(path, "wb") as f:
        f.write(out)


def sample_end(smp, off):
    """End (exclusive) of the ADPCM sample starting at off: the block with the end flag (bit 0) is included."""
    o = off
    while o + 16 <= len(smp):
        o += 16
        if smp[o - 15] & 1:
            break
    return o


def sample_loop(smp, off, end):
    """Index of the sample where the loop restarts, or None (no flag 2 in the last block)."""
    if end - off < 16 or not smp[end - 15] & 2:
        return None
    for o in range(off, end, 16):
        if smp[o + 1] & 4:
            return (o - off) // 16 * 28
    return 0


def bnk_replace(path, sound, wav, out):
    """Replaces the samples of sound `sound` in the bank with the WAV (resampled to the rate at which the bank plays each TONE).
    The other sounds stay the same; sample offsets, the chunk size and the SBlk size fields are fixed up."""
    raw = bytearray(open(path, "rb").read())
    b = read_bnk(path)
    typ, nchunks = struct.unpack_from("<II", raw, 0)
    chunks = [list(struct.unpack_from("<II", raw, 8 + 8 * i)) for i in range(nchunks)]
    if nchunks < 2:
        raise SystemExit("bank has no sample chunk")
    blk = bytearray(raw[chunks[0][0]:chunks[0][0] + chunks[0][1]])
    smp = bytes(raw[chunks[1][0]:chunks[1][0] + chunks[1][1]])
    first_sound, first_grain = struct.unpack_from("<II", blk, 0x1C)
    # every reference to a sample: (position of the field in the SBlk, offset)
    refs = []
    for i in range(len(b["sounds"])):
        o = first_sound + 12 * i
        ng, fg = struct.unpack_from("<b", blk, o + 4)[0], struct.unpack_from("<I", blk, o + 8)[0]
        for g in range(ng):
            go = first_grain + fg + 0x28 * g
            if struct.unpack_from("<I", blk, go)[0] in (1, 9):
                refs.append((i, go + 8 + 16, struct.unpack_from("<I", blk, go + 24)[0], go))
    targets = [r for r in refs if r[0] == sound]
    if not targets:
        raise SystemExit("sound %d has no TONE" % sound)
    rate_in, chans = read_wav_any(wav)
    chans = to_channels(chans, len(targets)) if len(chans) > 1 else [chans[0]] * len(targets)
    new, extra = {}, {}   # extra: a sample another sound also uses -> a new copy at the end of the chunk, for this sound only
    for k, (_, _, off, go) in enumerate(targets):
        if off in new or off in extra:
            continue
        note, fine = struct.unpack_from("<bb", blk, go + 8 + 2)
        rate = round(tone_rate(note, fine))
        end = sample_end(smp, off)
        others = [r for r in refs if r[0] != sound and off <= r[2] < end]
        loop = sample_loop(smp, off, end)
        pcm = resample(chans[k], rate_in, rate)
        data = adpcm_encode(pcm, 0 if loop is not None else None)
        if others:
            print("sound %d: the sample at %#x also belongs to sound %d, which keeps the original" % (sound, off, others[0][0]))
            extra[off] = data
        else:
            new[off] = (end, data)
    # rebuild the sample chunk with the offset map
    pieces, cur, omap = [], 0, []
    for off in sorted(new):
        end, data = new[off]
        pieces.append(smp[cur:off])
        omap.append((off, end, sum(len(p) for p in pieces), len(data)))
        pieces.append(data)
        cur = end
    pieces.append(smp[cur:])
    smp2 = b"".join(pieces)

    def remap(o):
        delta = 0
        for (a, e, na, nl) in omap:
            if o == a:
                return na
            if o >= e:
                delta += nl - (e - a)
        return o + delta
    for (_, pos, off, _) in refs:
        struct.pack_into("<I", blk, pos, remap(off))
    if extra:
        if len(smp2) % 16:
            smp2 += bytes(16 - len(smp2) % 16)
        at = {}
        for off, data in extra.items():
            at[off] = len(smp2)
            smp2 += data
        for (i, pos, off, _) in refs:
            if i == sound and off in at:
                struct.pack_into("<I", blk, pos, at[off])
    grow = len(smp2) - len(smp)
    vag_size, sram = struct.unpack_from("<II", blk, 0x28)
    if vag_size == len(smp):
        struct.pack_into("<I", blk, 0x28, len(smp2))
    if sram >= len(smp):
        struct.pack_into("<I", blk, 0x2C, sram + grow)
    out_b = bytearray(raw[:8 + 8 * nchunks])
    pos = chunks[0][0]
    out_b += bytes(max(0, pos - len(out_b)))
    bodies = [bytes(blk), smp2] + [bytes(raw[c[0]:c[0] + c[1]]) for c in chunks[2:]]
    for i, body in enumerate(bodies):
        gap = chunks[i][0] - (chunks[i - 1][0] + chunks[i - 1][1]) if i else 0
        out_b += bytes(max(0, gap))
        struct.pack_into("<II", out_b, 8 + 8 * i, len(out_b), len(body))
        out_b += body
    with open(out, "wb") as f:
        f.write(out_b)
    total = len(smp2)
    print("wrote %s (samples: %d -> %d bytes%s)" % (out, len(smp), total,
                                                       "; over 2 MB, the PS2 sound RAM limit" if total > 2 * 1024 * 1024 else ""))


def write_wav(path, rate, channels):
    with wave.open(path, "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(2)
        w.setframerate(rate)
        n = min(len(c) for c in channels)
        frames = bytearray()
        for i in range(n):
            for c in channels:
                frames += struct.pack("<h", c[i])
        w.writeframes(bytes(frames))


def read_vpk(path):
    """' KPV': u32 bytes per channel, u32 data start, u32 block pair size (0xA000 = 0x5000 per channel), u32 Hz,
    u32 channels."""
    d = open(path, "rb").read()
    if d[:4] == b" KPV":
        per_ch, start, inter, rate, nch = struct.unpack_from("<5I", d, 4)
        inter //= nch
        chans = [[] for _ in range(nch)]
        hist = [(0, 0)] * nch
        o = start
        while o < len(d):
            for c in range(nch):
                blk = d[o:o + inter]
                s, _ = adpcm_decode(blk, 0, stop_at_end=False, hist=hist[c])
                chans[c] += s
                if len(s) >= 2:
                    hist[c] = (s[-1], s[-2])
                o += inter
        return rate, chans
    if d[:4] == b"VAGp":
        size, rate = struct.unpack_from(">II", d, 0xc)   # standard VAG: size at 0x0C, Hz at 0x10
        s, _ = adpcm_decode(d, 0x30, stop_at_end=False)
        return rate, [s]
    raise ValueError("unknown format: %s" % path)


def tone_rate(center_note, center_fine, note=60, fine=0):
    """Rate at which 989snd plays a TONE's sample (effects are played at note 60). A negative CenterNote = a PS2 note
    (48 kHz base); positive = PS1 (44.1 kHz base). As in sceSdNote2Pitch, CenterFine is added to fine."""
    base = 48000 if center_note < 0 else 44100
    return base * 2 ** ((note - abs(center_note) + (fine + center_fine) / 128) / 12)


def main():
    cmd = sys.argv[1]
    if cmd == "bnk":
        b = read_bnk(sys.argv[2])
        print("bank %s v%d flags=%#x sounds=%d grains=%d vags=%d samples=%d bytes" % (
            b["bank"], b["version"], b["flags"], len(b["sounds"]), b["n_grains"], b["n_vags"], len(b["samples"])))
        for i, s in enumerate(b["sounds"]):
            print("sound %3d %-16s vol=%d group=%d pan=%d inst=%d flags=%#x" % (i, b["names"].get(i, ""), s["vol"], s["group"], s["pan"],
                                                                             s["inst"], s["flags"]))
            for g in s["grains"]:
                extra = ""
                if g["type"] in (1, 9):
                    extra = "vol=%d note=%d.%d pan=%d adsr=%04x/%04x tflags=%#x sample=%#x" % (
                        g["vol"], g["note"], g["fine"], g["pan"], g["adsr1"], g["adsr2"], g["tflags"], g["sample"])
                elif "play" in g:
                    extra = str(g["play"][:6])
                else:
                    extra = str(g["params"])
                print("    %-14s delay=%-5d %s" % (GRAIN_NAMES.get(g["type"], g["type"]), g["delay"], extra))
    elif cmd == "stats":
        types = collections.Counter()
        sflags = collections.Counter()
        bflags = collections.Counter()
        groups = collections.Counter()
        for fn in sorted(os.listdir(sys.argv[2])):
            if not fn.upper().endswith(".BNK"):
                continue
            b = read_bnk(os.path.join(sys.argv[2], fn))
            bflags[hex(b["flags"])] += 1
            print("%-14s %s v%d sounds=%4d grains=%4d vags=%4d samples=%8d names=%d" % (
                fn, b["bank"], b["version"], len(b["sounds"]), b["n_grains"], b["n_vags"], len(b["samples"]), len(b["names"])))
            for s in b["sounds"]:
                sflags[hex(s["flags"])] += 1
                groups[s["group"]] += 1
                for g in s["grains"]:
                    types[GRAIN_NAMES.get(g["type"], str(g["type"]))] += 1
        print("grain types:", dict(types.most_common()))
        print("sound flags:", dict(sflags.most_common()))
        print("bank flags:", dict(bflags))
        print("volume groups:", dict(sorted(groups.items())))
    elif cmd == "wav":
        path, which, out = sys.argv[2], sys.argv[3], sys.argv[4]
        if path.upper().endswith(".BNK"):
            b = read_bnk(path)
            tones = [g for g in b["sounds"][int(which)]["grains"] if g["type"] in (1, 9)]
            chans = [adpcm_decode(b["samples"], g["sample"])[0] for g in tones]
            rate = round(tone_rate(tones[0]["note"], tones[0]["fine"]))
            write_wav(out, rate, chans[:2] if len(chans) >= 2 else chans)
        else:
            rate, chans = read_vpk(path)
            write_wav(out, rate, chans)
        print("wrote", out)
    elif cmd == "vpk":
        src, out = sys.argv[2], sys.argv[3]
        rate, chans = read_wav_any(src)
        vols = None
        if "--like" in sys.argv:
            like = open(sys.argv[sys.argv.index("--like") + 1], "rb").read()
            _, _, _, orate, onch = struct.unpack_from("<5I", like, 4)
            vols = [struct.unpack_from("<HH", like, 0x18 + 4 * c) for c in range(onch)]
            chans = [resample(c, rate, orate) for c in to_channels(chans, onch)]
            rate = orate
        elif len(chans) == 1:
            chans = to_channels(chans, 2)   # every .VPK on the disc is stereo
        write_vpk(out, rate, chans, vols)
        print("wrote %s (%d Hz, %d channels, %.1f s)" % (out, rate, len(chans), len(chans[0]) / rate))
    elif cmd == "vag":
        src, out = sys.argv[2], sys.argv[3]
        rate, chans = read_wav_any(src)
        write_vag(out, rate, to_channels(chans, 1)[0], loop="--loop" in sys.argv)
        print("wrote", out)
    elif cmd == "bnk-replace":
        bnk_replace(sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
