#!/usr/bin/env python3
"""Reader for WotM .NGP images and mesh extractor (formats in docs/FORMATOS.md).

A stage .NGP is a zip with the magic "IE\\3\\4" (instead of "PK\\3\\4") holding a single deflated file; a
monster .NGP is uncompressed. The image is prelinked to a base address (0xA00000); pointers inside it are absolute
PS2 RAM addresses, so `Image.u32(addr)` takes the address as the game sees it.
Writes nothing from the game: it reads the user's disc at run time.

Mesh (`HierObject::polyPkt`): a DMA sub-chain (QWC/ID=ret tag, 8 bytes + 8 bytes of padding) followed by a
VIF stream for the VU1 microcode. See `decode_object` and docs/FORMATOS.md ("Meshes").
"""
import struct
import sys
import zlib

LEVEL_BASE = 0xA00000


def inflate_ngp(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"IE\x03\x04":
        return raw  # monsters (.ngp in MON/) are not zipped (fileReadf)
    (_ver, _flags, method, _time, _date, _crc, csize, usize, nlen, xlen) = struct.unpack_from("<HHHHHIIIHH", raw, 4)
    if method != 8:
        raise ValueError("%s: method %d (only deflate is supported)" % (path, method))
    off = 4 + struct.calcsize("<HHHHHIIIHH") + nlen + xlen
    data = zlib.decompressobj(-15).decompress(raw[off:off + csize])
    if len(data) != usize:
        raise ValueError("%s: size %d != %d" % (path, len(data), usize))
    return data


class Image:
    def __init__(self, data, base=LEVEL_BASE):
        self.d = data
        self.base = base

    @classmethod
    def load(cls, path, base=LEVEL_BASE):
        return cls(inflate_ngp(path), base)

    def ok(self, addr, n=4):
        return self.base <= addr and addr - self.base + n <= len(self.d)

    def u8(self, a):
        return self.d[a - self.base]

    def s8(self, a):
        return struct.unpack_from("<b", self.d, a - self.base)[0]

    def u16(self, a):
        return struct.unpack_from("<H", self.d, a - self.base)[0]

    def s16(self, a):
        return struct.unpack_from("<h", self.d, a - self.base)[0]

    def u32(self, a):
        return struct.unpack_from("<I", self.d, a - self.base)[0]

    def f32(self, a):
        return struct.unpack_from("<f", self.d, a - self.base)[0]

    def roots(self):
        n = self.u32(self.base)
        return [self.u32(self.base + 4 + 4 * i) for i in range(n)]


OPNAMES = {0: "OBJECT", 1: "GROUP", 2: "LOD", 3: "TRANSLATE", 4: "ROTATE", 5: "LIGHT", 6: "SWITCH", 7: "ANIM_MGR",
           8: "CONTROL", 9: "NULL", 10: "COLL_VOL", 11: "COLL_TRIG", 12: "CHAR_ANIM", 13: "TERRAIN", 14: "TERRAIN_OBJ",
           15: "COLL_GRID", 16: "PART_MGR", 17: "ANIM_XFORM", 18: "BSP", 19: "PARTICLE", 20: "PART_FIELD",
           21: "PART_VISUAL", 22: "PART_EMITTER", 23: "ACTION_DATA", 24: "CHARACTER", 25: "CHAR_INSTANCE",
           30: "SCALE", 31: "DESTRUCTIBLE", 32: "DESTRUCT_TABLE", 33: "ANIM_CONTROL", 34: "ANIM_BLEND",
           35: "SKEL_BONE", 36: "ANIM_PLAYER", 37: "CAMERA", 38: "ANIM_SCALE", 39: "INTERACTIVE", 40: "ATTACHPT",
           41: "SCRIPT", 42: "POLY_COLL", 43: "ANIM_PROC"}

# ---------------------------------------------------------------- mesh (VIF)

# UNPACK: cmd = 0x60 | vn << 2 | vl ; bits per component
_VN_COMPS = (1, 2, 3, 4)
_VL_BITS = (32, 16, 8, 5)


def _unpack_words(vn, vl, num):
    if vl == 3:
        return (num * 16 + 31) // 32
    return (num * _VL_BITS[vl] * _VN_COMPS[vn] + 31) // 32


# VU1 data memory addresses used by the object microcode (found by inspecting the data)
VU_POS = 0xB5       # V3-32  positions (up to 190)
VU_NRM = 0x175      # V3-8   normals (signed, /127)
# strip section: GIF tag at H, then 3 qwords per vertex (ST, RGBAQ, XYZF)
#   H+1+3k  S-8    vertex index into VU_POS
#   H+2+3k  V4-8   RGBA (0x80 = 1.0)
#   H+3+3k  V2-16  UV (4096 = 1.0)


class Strip:
    def __init__(self):
        self.verts = []   # (px,py,pz, nx,ny,nz, r,g,b,a, u,v)


class Mesh:
    def __init__(self):
        self.strips = []
        self.prim = None   # the GIF tag's PRIM field (type, IIP, TME, FGE, ABE, ...)


TRACE = None   # optional list: the decoder records (cmd, imm) for each MSCAL/MSCALF/MSCNT


def decode_object(img, node):
    """Decodes a HierObject's `polyPkt` and returns a `Mesh` (strips of fully resolved vertices).
    Skinned objects are a chain of sub-packets (each with its own tag and up to 190 positions)."""
    mesh = Mesh()
    pp = img.u32(node + 4) & 0x0FFFFFFF
    for _ in range(256):
        nxt = _decode_packet(img, pp, mesh)
        if nxt is None:
            break
        pp = nxt
    return mesh


def _decode_packet(img, pp, mesh):
    """Decodes one sub-packet into `mesh`; returns the address of the next one (skinned) or None."""
    if not img.ok(pp, 16):
        return None
    skip = 0
    while skip < 0x400 and img.ok(pp + skip, 16) and img.u32(pp + skip) == 0 and img.u32(pp + skip + 4) == 0:
        skip += 16   # zeroed quadwords between sub-packets
    pp += skip
    tag = img.u32(pp)
    if ((tag >> 28) & 7) != 6:
        return None
    qwc = tag & 0xFFFF
    skinned = bool(tag & 0x10000)   # tag bit 16: V4-32 vertices with bone codes
    nwords = (qwc + 1) * 4 - 2
    if skinned:
        nwords = min(60000, (len(img.d) - (pp + 8 - img.base)) // 4)   # real length = up to two NOPs in a row (below)
    base = pp + 8
    if not img.ok(base, 16) or (not skinned and not img.ok(base, nwords * 4)):
        return None
    end_pp = None

    pos, nrm = [], []
    hdr = None            # (addr, nloop, prim)
    idx, rgba, uv, adc = [], [], [], set()
    mask = 0
    i = 0

    def flush():
        nonlocal hdr, idx, rgba, uv, adc
        if hdr is None or not idx:
            hdr, idx, rgba, uv, adc = None, [], [], [], set()
            return
        n = min(hdr[1], len(idx))
        cur = None
        for k in range(n):
            if cur is None or k in adc:
                cur = Strip()
                mesh.strips.append(cur)
            j = idx[k]
            p = pos[j] if j < len(pos) else (0.0, 0.0, 0.0)
            q = nrm[j] if j < len(nrm) else (0.0, 1.0, 0.0)
            c = rgba[k] if k < len(rgba) else (128, 128, 128, 128)
            t = uv[k] if k < len(uv) else (0, 0)
            cur.verts.append((p[0], p[1], p[2], q[0], q[1], q[2], c[0], c[1], c[2], c[3], t[0] / 4096.0, t[1] / 4096.0) + tuple(p[3:]))
        hdr, idx, rgba, uv, adc = None, [], [], [], set()

    while i < nwords:
        w = img.u32(base + 4 * i)
        cmd = (w >> 24) & 0x7F
        num = (w >> 16) & 0xFF
        imm = w & 0xFFFF
        a = base + 4 * (i + 1)
        if TRACE is not None and cmd in (0x14, 0x15, 0x17):
            TRACE.append((cmd, imm))   # diagnostics: MSCAL/MSCALF/MSCNT (microprogram entry point)
        if cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num or 256
            addr = imm & 0x3FF
            fmt = (vn, vl)
            if fmt == (2, 0) and addr == VU_POS:                       # V3-32 positions
                if hdr is not None:
                    flush()
                pos = [(img.f32(a + 12 * k), img.f32(a + 12 * k + 4), img.f32(a + 12 * k + 8)) for k in range(n)]
            elif fmt == (3, 0) and addr == VU_POS and n > 1:           # V4-32 (skinned): x,y,z with a bone code in the low 8 bits + weight in w
                if hdr is not None:
                    flush()
                pos = [(img.f32(a + 16 * k), img.f32(a + 16 * k + 4), img.f32(a + 16 * k + 8), img.f32(a + 16 * k + 12),
                        img.u32(a + 16 * k) & 0xFF, img.u32(a + 16 * k + 4) & 0xFF, img.u32(a + 16 * k + 8) & 0xFF) for k in range(n)]
            elif fmt == (2, 2) and addr == VU_NRM:                     # V3-8 normals
                # 3 bytes per normal, packed in consecutive bytes (no per-element padding)
                nrm = [tuple(img.s8(a + 3 * k + c) / 127.0 for c in range(3)) for k in range(n)]
            elif fmt == (3, 0) and n == 1:                             # V4-32: the section's GIF tag
                g0, g1 = img.u32(a), img.u32(a + 4)
                hdr = (addr, g0 & 0x7FFF, (g1 >> 15) & 0x7FF)
                mesh.prim = hdr[2]
            elif hdr is not None and fmt == (0, 2) and (mask & 0xFF) == 0xBF:   # masked S-8: marks ADC
                for k in range(n):
                    e = addr - hdr[0] - 3 + 3 * k
                    if e % 3 == 0:
                        adc.add(e // 3)
            elif hdr is not None and fmt == (0, 2) and addr == hdr[0] + 1:      # S-8 indexes
                idx = [img.u8(a + k) for k in range(n)]
            elif hdr is not None and fmt == (3, 2) and addr == hdr[0] + 2:      # V4-8 RGBA
                rgba = [(img.u8(a + 4 * k), img.u8(a + 4 * k + 1), img.u8(a + 4 * k + 2), img.u8(a + 4 * k + 3)) for k in range(n)]
            elif hdr is not None and fmt == (1, 1) and addr == hdr[0] + 3:      # V2-16 UV
                uv = [(img.s16(a + 4 * k), img.s16(a + 4 * k + 2)) for k in range(n)]
            i += 1 + _unpack_words(vn, vl, n)
            continue
        extra = 0
        if cmd == 0x20:                       # STMASK
            mask = img.u32(a)
            extra = 1
        elif cmd in (0x30, 0x31):             # STROW / STCOL
            extra = 4
        elif cmd == 0x4A:                     # MPG
            extra = (num or 256) * 2
        elif cmd in (0x50, 0x51):             # DIRECT
            extra = (imm or 65536) * 4
        elif cmd in (0x14, 0x15, 0x17):       # MSCAL/MSCALF/MSCNT: end of a strip section
            flush()
        elif cmd == 0 and skinned and i > 8 and img.u32(a) == 0:   # two NOPs in a row: end of the skinned sub-packet
            end_pp = (base + 4 * (i + 2) + 15) & ~15   # next sub-packet: quadword aligned
            break
        i += 1 + extra
    flush()
    return end_pp


# ---------------------------------------------------------------- full mesh (like native/wotm_native.hpp)

SLOT_NOTEX = 31   # section with PRIM.TME 0: the GS draws vertex color only


class Vtx:
    __slots__ = ("x", "y", "z", "w", "cx", "cy", "cz", "u", "v", "r", "g", "b", "a", "nx", "ny", "nz", "slot", "cull", "fge", "sub")


class MeshData:
    """Result of `decode_mesh`: `tris` (3 Vtx per triangle), `sprites` and what drawing needs to know about the object."""

    def __init__(self):
        self.tris = []
        self.sprites = []     # (x, y, z, size, u0, v0, u1, v1, r, g, b, a, slot, fge)
        self.skinned = False
        self.abe = False
        self.slot_mask = 0
        self.cur_slot = 0
        self.cur_tme = 1
        self.cur_fge = 0
        self.cur_sprite = 0
        self.cur_sub = 0
        self.drop_adc = 0
        self.drop_degen = 0


def _skip_zero_qwords(img, pp):
    skip = 0
    while skip < 0x400 and img.ok(pp + skip, 16) and img.u32(pp + skip) == 0 and img.u32(pp + skip + 4) == 0:
        skip += 16
    return pp + skip


def _decode_packet2(img, pp, md):
    """Port of the native layer's decodePacket: returns the next sub-packet (skinned chain) or 0."""
    pp = _skip_zero_qwords(img, pp)
    if not img.ok(pp, 16):
        return 0
    tag = img.u32(pp)
    if ((tag >> 28) & 7) != 6:
        return 0
    skinned = bool(tag & 0x10000)
    nwords = ((tag & 0xFFFF) + 1) * 4 - 2
    base = pp + 8
    if skinned:
        nwords = max(0, min(60000, (img.base + len(img.d) - base) // 4))
    if nwords == 0 or (not skinned and not img.ok(base, nwords * 4)):
        return 0
    if skinned:
        md.skinned = True
    end_pp = 0
    st = {"pos": [], "nrm": [], "hdr": False, "haddr": 0, "nloop": 0, "sel": 0, "idx": [], "rgba": [], "uv": [], "adc": [],
          "spos": []}

    def flush():
        if st["hdr"] and md.cur_sprite:
            sel = st["sel"] & 0x7FFF
            if 1 <= sel <= 13:
                md.cur_slot = (sel - 1) // 3
            slot = md.cur_slot if md.cur_tme else SLOT_NOTEX
            n = min(st["nloop"], len(st["spos"]), len(st["rgba"]), len(st["uv"]) // 2)
            for k in range(n):
                sx, sy, sz, ss = st["spos"][k]
                u0, v0 = st["uv"][2 * k]
                u1, v1 = st["uv"][2 * k + 1]
                r, g, b, a = st["rgba"][k]
                md.sprites.append((sx, sy, sz, ss, u0 / 4096.0, v0 / 4096.0, u1 / 4096.0, v1 / 4096.0, r, g, b, a, slot, md.cur_fge))
                md.slot_mask |= 1 << slot
        elif st["hdr"] and st["idx"]:
            n = min(st["nloop"], len(st["idx"]))
            restart = [False] * (n + 1)
            for k in st["adc"]:
                if k < n:
                    restart[k] = True
            strip = []
            pos, nrm, rgba, uv = st["pos"], st["nrm"], st["rgba"], st["uv"]
            for k in range(n):
                j = st["idx"][k]
                p = pos[j] if j < len(pos) else (0.0, 0.0, 0.0, 1.0, 0, 0, 0)
                v = Vtx()
                v.x, v.y, v.z, v.w, v.cx, v.cy, v.cz = p
                v.nx, v.ny, v.nz = nrm[j] if j < len(nrm) else (0, 0, 0)
                sel = st["sel"] & 0x7FFF
                if 1 <= sel <= 13:
                    md.cur_slot = (sel - 1) // 3
                v.slot = md.cur_slot if md.cur_tme else SLOT_NOTEX
                v.fge = md.cur_fge
                v.cull = (st["sel"] >> 15) & 1
                v.sub = md.cur_sub
                md.slot_mask |= 1 << v.slot
                v.r, v.g, v.b, v.a = rgba[k] if k < len(rgba) else (128, 128, 128, 128)
                if k < len(uv):
                    v.u, v.v = uv[k][0] / 4096.0, uv[k][1] / 4096.0
                else:
                    v.u = v.v = 0.0
                strip.append(v)
            # ADC on vertex k: the GS skips the triangle ending at k, but the strip continues
            for k in range(2, len(strip)):
                if k < len(restart) and restart[k]:
                    md.drop_adc += 1
                    continue
                a = strip[k - 1] if k & 1 else strip[k - 2]
                b = strip[k - 2] if k & 1 else strip[k - 1]
                c = strip[k]

                def same(p, q):
                    return p.x == q.x and p.y == q.y and p.z == q.z
                if same(a, b) or same(b, c) or same(a, c):
                    md.drop_degen += 1
                    continue
                md.tris += [a, b, c]
        st.update(hdr=False, idx=[], rgba=[], uv=[], adc=[], spos=[])

    mask = 0
    i = 0
    while i < nwords:
        w = img.u32(base + 4 * i)
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        a = base + 4 * (i + 1)
        if cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num or 256
            addr = imm & 0x3FF
            nw = _unpack_words(vn, vl, n)
            ha = st["haddr"]
            if vn == 2 and vl == 0 and addr == VU_POS:
                if st["hdr"]:
                    flush()
                st["pos"] = [(img.f32(a + 12 * k), img.f32(a + 12 * k + 4), img.f32(a + 12 * k + 8), 1.0, 0, 0, 0) for k in range(n)]
            elif vn == 3 and vl == 0 and addr == VU_POS and n > 1:
                if st["hdr"]:
                    flush()
                st["pos"] = [(img.f32(a + 16 * k), img.f32(a + 16 * k + 4), img.f32(a + 16 * k + 8), img.f32(a + 16 * k + 12),
                              img.u32(a + 16 * k) & 0xFF, img.u32(a + 16 * k + 4) & 0xFF, img.u32(a + 16 * k + 8) & 0xFF) for k in range(n)]
            elif vn == 2 and vl == 2 and addr == VU_NRM:
                st["nrm"] = [(img.s8(a + 3 * k), img.s8(a + 3 * k + 1), img.s8(a + 3 * k + 2)) for k in range(n)]
            elif st["hdr"] and md.cur_sprite and vn == 3 and vl == 0 and addr == ha + 3:
                st["spos"] = [(img.f32(a + 16 * k), img.f32(a + 16 * k + 4), img.f32(a + 16 * k + 8), img.f32(a + 16 * k + 12))
                              for k in range(n)]
            elif st["hdr"] and md.cur_sprite and vn == 1 and vl == 1 and addr == ha + 1:
                st["uv"] = [(img.s16(a + 4 * k), img.s16(a + 4 * k + 2)) for k in range(n)]
            elif vn == 3 and vl == 0 and n == 1:   # the section's GIF tag
                g1 = img.u32(a + 4)
                st.update(haddr=addr, nloop=img.u32(a) & 0x7FFF, hdr=True, sel=img.u32(a + 12))
                if (g1 >> 14) & 1:                  # PRE: PRIM in bits 47..57
                    md.cur_tme = (g1 >> 19) & 1
                    md.cur_fge = (g1 >> 20) & 1
                    md.cur_sprite = 1 if ((g1 >> 15) & 7) == 6 else 0
                if ((g1 >> 15) >> 6) & 1:
                    md.abe = True
            elif st["hdr"] and vn == 0 and vl == 2 and (mask & 0xFF) == 0xBF:   # masked S-8: ADC
                for k in range(n):
                    e = addr - ha - 3 + 3 * k
                    if e >= 0 and e % 3 == 0:
                        st["adc"].append(e // 3)
            elif st["hdr"] and vn == 0 and vl == 2 and addr == ha + 1:
                st["idx"] = [img.u8(a + k) for k in range(n)]
            elif st["hdr"] and vn == 3 and vl == 2 and addr == ha + 2:
                st["rgba"] = [(img.u8(a + 4 * k), img.u8(a + 4 * k + 1), img.u8(a + 4 * k + 2), img.u8(a + 4 * k + 3)) for k in range(n)]
            elif st["hdr"] and vn == 1 and vl == 1 and addr == ha + 3:
                st["uv"] = [(img.s16(a + 4 * k), img.s16(a + 4 * k + 2)) for k in range(n)]
            i += 1 + nw
            continue
        extra = 0
        if cmd == 0x20:
            mask = img.u32(a)
            extra = 1
        elif cmd in (0x30, 0x31):
            extra = 4
        elif cmd == 0x4A:
            extra = (num or 256) * 2
        elif cmd in (0x50, 0x51):
            extra = (imm or 65536) * 4
        elif cmd in (0x14, 0x15, 0x17):
            flush()
        elif cmd == 0 and skinned and i > 8 and img.u32(a) == 0:
            end_pp = (base + 4 * (i + 2) + 15) & ~15
            break
        i += 1 + extra
    flush()
    return end_pp


def decode_mesh(img, node):
    """Mesh of an OBJECT with each section's texture (`Vtx.slot` indexes the object's texId[]/gsTexCtx[]; SLOT_NOTEX = no
    texture), strip breaks (ADC) handled like the GS, and sprites. Port of decodeObject from native/wotm_native.hpp."""
    md = MeshData()
    pp = img.u32(node + 4) & 0x0FFFFFFF
    if not img.ok(pp, 16):
        return md
    for guard in range(256):
        md.cur_sub = guard
        pp = _decode_packet2(img, pp, md)
        if not pp:
            break
    return md


# ---------------------------------------------------------------- hierarchy

def ident():
    return [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]


def mul(a, b):
    """a*b with row vectors (p' = p*M): applies `a` first, then `b`."""
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def translate_m(t):
    m = ident()
    m[3][0], m[3][1], m[3][2] = t
    return m


def scale_m(s):
    m = ident()
    m[0][0], m[1][1], m[2][2] = s
    return m


def xform(m, p):
    return (p[0] * m[0][0] + p[1] * m[1][0] + p[2] * m[2][0] + m[3][0],
            p[0] * m[0][1] + p[1] * m[1][1] + p[2] * m[2][1] + m[3][1],
            p[0] * m[0][2] + p[1] * m[1][2] + p[2] * m[2][2] + m[3][2])


def walk(img, node, visit, m=None, seen=None, depth=0, lod=0):
    """Walks the hierarchy calling visit(node, matrix) on each OBJECT. `lod`: index into the LOD list (0 = the coarsest;
    a high number, like 99, picks the most detailed)."""
    if m is None:
        m = ident()
    if seen is None:
        seen = set()
    if depth > 64 or not img.ok(node, 4):
        return
    key = (node, tuple(map(tuple, m)))
    if key in seen:
        return
    seen.add(key)
    op = img.u32(node) & 63

    def kids(arr, n, mm):
        for k in range(min(n, 4096)):
            walk(img, img.u32(arr + 4 * k), visit, mm, seen, depth + 1, lod)

    if op == 0:
        visit(node, m)
    elif op == 1:
        if img.u16(node + 0xA) & 1:   # visual GROUPs only (the rest are collision etc.; Scene::walk)
            kids(node + 0x20, img.u16(node + 8), m)
    elif op == 3:
        t = (img.f32(node + 0x10), img.f32(node + 0x14), img.f32(node + 0x18))
        kids(node + 0x1C, img.u32(node + 8), mul(translate_m(t), m))
    elif op == 4:
        rm = [[img.f32(node + 0x10 + 16 * r + 4 * c) for c in range(4)] for r in range(4)]
        kids(node + 0x50, img.u32(node + 8), mul(rm, m))
    elif op == 30:
        t = (img.f32(node + 0x10), img.f32(node + 0x14), img.f32(node + 0x18))
        s = (img.f32(node + 0x20), img.f32(node + 0x24), img.f32(node + 0x28))
        kids(node + 0x2C, img.u32(node + 8), mul(mul(scale_m(s), translate_m(t)), m))
    elif op == 6:
        which, n = img.u8(node + 8), img.u8(node + 0xB)
        if which < n:
            walk(img, img.u32(node + 0xC + 4 * which), visit, m, seen, depth + 1, lod)
    elif op == 2:
        n = img.u32(node + 4)
        if 0 < n < 16:
            walk(img, img.u32(node + 0x20 + 16 * min(lod, n - 1) + 8), visit, m, seen, depth + 1, lod)
    # the nodes below follow the native layer's Scene::walk (native/wotm_native.hpp), minus what only exists with the game running
    elif op == 8:      # CONTROL: child1 (the solid building; child2 is the "ghost" version)
        walk(img, img.u32(node + 4), visit, m, seen, depth + 1, lod)
    elif op == 23:     # ACTION_DATA
        walk(img, img.u32(node + 4), visit, m, seen, depth + 1, lod)
    elif op == 35:     # ANIM_XFORM container (the monster's rigid parts)
        kids(node + 0xC, img.u32(node + 4), m)
    elif op == 25:     # CHAR_INSTANCE
        kids(node + 0x2C, img.u32(node + 0x28), m)
    elif op == 17:     # ANIM_XFORM: the matrix comes from the animation at run time; outside the game, identity
        kids(node + 0xCC, img.u32(node + 4), m)
    elif op == 38:     # ANIM_SCALE: (p + preScale) * scale + translate + postScale, with the values stored in the node
        sx, sy, sz = img.f32(node + 0x20), img.f32(node + 0x24), img.f32(node + 0x28)
        if sx or sy or sz:
            pre = translate_m((img.f32(node + 0x40), img.f32(node + 0x44), img.f32(node + 0x48)))
            post = translate_m((img.f32(node + 0x30) + img.f32(node + 0x50), img.f32(node + 0x34) + img.f32(node + 0x54),
                                img.f32(node + 0x38) + img.f32(node + 0x58)))
            kids(node + 0x60, img.u32(node + 4), mul(mul(mul(pre, scale_m((sx, sy, sz))), post), m))
    elif op == 39:     # INTERACTIVE_STATE: child at +0xC; if it is a DESTRUCTIBLE, state 0 (intact)
        c = img.u32(node + 0xC)
        if img.ok(c, 4) and (img.u32(c) & 63) == 31:
            if img.u16(c + 0xA):
                walk(img, img.u32(c + 0xC), visit, m, seen, depth + 1, lod)
        else:
            walk(img, c, visit, m, seen, depth + 1, lod)
    elif op == 31:     # DESTRUCTIBLE: state 0
        if img.u16(node + 0xA):
            walk(img, img.u32(node + 0xC), visit, m, seen, depth + 1, lod)


def main():
    img = Image.load(sys.argv[1])
    print("image: %d bytes, %d roots" % (len(img.d), len(img.roots())))
    for r in img.roots():
        w = img.u32(r)
        print("  root %06x op=%s id=%d" % (r, OPNAMES.get(w & 63, w & 63), w >> 18))


if __name__ == "__main__":
    main()
