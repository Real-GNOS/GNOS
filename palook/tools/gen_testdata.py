#!/usr/bin/env python3
"""Generate synthetic PAL-format test data for PALook.

Creates a minimal but realistic PAL data directory: MKF archives with
palettes, RLE sprites, map + tiles, backgrounds, RNG animations, WAV/VOC
sounds, MIDI, SSS script tables, DATA record tables and GBK text files.

Usage: gen_testdata.py [outdir]     (default: ./testdata)
"""

import math
import os
import struct
import sys
import wave


# --------------------------------------------------------------------------
# helpers

def u16(v):
    return struct.pack('<H', v & 0xFFFF)


def u32(v):
    return struct.pack('<I', v & 0xFFFFFFFF)


def mkf(chunks):
    """Build an MKF archive from raw chunks."""
    n = len(chunks)
    offs = [4 * (n + 1)]
    for c in chunks:
        offs.append(offs[-1] + len(c))
    return b''.join(u32(o) for o in offs) + b''.join(chunks)


# --------------------------------------------------------------------------
# palette (6-bit VGA entries, 256 colors x 3 bytes)

def build_palettes():
    day = [0] * 768

    def setc(i, r, g, b):
        day[i * 3:i * 3 + 3] = [max(0, min(63, r >> 2)),
                                max(0, min(63, g >> 2)),
                                max(0, min(63, b >> 2))]

    setc(0, 0, 0, 0)
    for i in range(1, 16):                      # grays
        setc(i, i * 17, i * 17, i * 17)
    for k in range(32):                         # 16..47 earth
        setc(16 + k, 96 + k * 4, 66 + k * 3, 36 + k * 2)
    for k in range(32):                         # 48..79 greens
        setc(48 + k, 26 + k * 3, 104 + k * 4, 34 + k * 2)
    for k in range(32):                         # 80..111 sky blues
        setc(80 + k, 36 + k * 2, 86 + k * 3, 136 + k * 3)
    for k in range(32):                         # 112..143 fire reds
        setc(112 + k, 156 + k * 3, 36 + k * 3, 28 + k)
    for k in range(32):                         # 144..175 purples
        setc(144 + k, 120 + k * 3, 40 + k * 2, 140 + k * 3)
    for k in range(32):                         # 176..207 skin/gold
        setc(176 + k, 210 + k, 150 + k * 2, 70 + k * 3)
    for k in range(48):                         # 208..255 misc
        setc(208 + k, (k * 5) % 256, (k * 11 + 60) % 256, (k * 23 + 120) % 256)

    # night: darker + blue shift
    night = []
    for i in range(256):
        r, g, b = day[i * 3:i * 3 + 3]
        night += [int(r * 0.5), int(g * 0.55), min(63, int(b * 0.75) + 7)]
    return bytes(day), bytes(night)


DAY_PAL, NIGHT_PAL = build_palettes()


# --------------------------------------------------------------------------
# RLE sprites (frame table + 02-header RLE frames)

def rle_encode(px, w, h):
    """Flat RLE: literal runs (<128, nonzero) and transparent runs
    (0x80+n, n <= min(w,127)) -- matches PAL_RLEBlitToSurface."""
    out = bytearray()
    i, total = 0, w * h
    max_skip = min(w, 127)
    while i < total:
        if px[i] == 0:
            n = 0
            while i + n < total and px[i + n] == 0 and n < max_skip:
                n += 1
            out.append(0x80 + n)
            i += n
        else:
            start = i
            while i < total and px[i] != 0 and i - start < 127:
                i += 1
            run = px[start:i]
            out.append(len(run))
            out += bytes(run)
    return bytes(out)


def rle_frame(w, h, px):
    return b'\x02\x00\x00\x00' + u16(w) + u16(h) + rle_encode(px, w, h)


def sprite(frames):
    """frames: [(w, h, pixels)] -> sprite blob with word offset table."""
    encoded = [rle_frame(w, h, px) for (w, h, px) in frames]
    blobs = []
    off = 2 + 2 * len(encoded)
    for f in encoded:
        blobs.append((off, f))
        off += len(f)
        if off % 2:
            off += 1
    out = bytearray(u16(len(encoded)))
    for o, _ in blobs:
        out += u16(o // 2)
    assert len(out) == 2 + 2 * len(frames)
    for o, f in blobs:
        while len(out) < o:
            out += b'\x00'
        out += f
        if len(out) % 2:
            out += b'\x00'
    return bytes(out)


def blank(w, h):
    return [0] * (w * h)


def rect(px, w, x0, y0, x1, y1, color):
    for y in range(max(0, y0), y1):
        for x in range(max(0, x0), x1):
            px[y * w + x] = color


# --------------------------------------------------------------------------
# content generators

def make_character_sprites():
    """mgo.mkf: 4-frame 32x48 walking figure, 3 lumps."""
    lumps = []
    for lump in range(3):
        frames = []
        robe = [96, 104, 88][lump % 3]
        for f in range(4):
            px = blank(32, 48)
            rect(px, 32, 10, 2, 22, 6, 40)          # hair
            rect(px, 32, 10, 6, 22, 16, 186)         # face
            rect(px, 32, 13, 9, 15, 11, 4)           # eyes
            rect(px, 32, 18, 9, 20, 11, 4)
            rect(px, 32, 8, 16, 24, 34, robe)        # robe
            rect(px, 32, 6, 18, 8, 30, robe - 4)     # sleeves
            rect(px, 32, 24, 18, 26, 30, robe - 4)
            rect(px, 32, 8, 34, 24, 36, 34)          # belt
            step = [0, 2, 0, -2][f]                  # walk cycle
            rect(px, 32, 12 + step, 36, 16 + step, 46, 26)
            rect(px, 32, 17 - step, 36, 21 - step, 46, 26)
            frames.append((32, 48, px))
        lumps.append(sprite(frames))
    return lumps


def make_ball_sprites():
    """ball.mkf: round item icons."""
    frames = []
    for f in range(6):
        px = blank(24, 24)
        for y in range(24):
            for x in range(24):
                d = math.hypot(x - 11.5, y - 11.5)
                if d <= 10:
                    px[y * 24 + x] = 176 + (f * 4) % 24
                if d <= 10 and d > 8.5:
                    px[y * 24 + x] = 168
        rect(px, 24, 8 - f, 7 - f // 2, 12 - f, 11 - f // 2, 250)
        frames.append((24, 24, px))
    return [sprite(frames)]


def make_ui_sprites():
    """data.mkf chunks 9/10: small icons."""
    def icons(base):
        frames = []
        for f in range(4):
            px = blank(16, 16)
            rect(px, 16, 0, 0, 16, 16, base)
            rect(px, 16, 1, 1, 15, 15, base + 8)
            rect(px, 16, 3 + f, 3, 9 + f, 13 - f, base + 16)
            frames.append((16, 16, px))
        return sprite(frames)
    return icons(120), icons(64)


def make_tile_sprite():
    """gop.mkf: diamond tiles 32x16 (ground 0..3, top 4..7)."""
    frames = []
    for idx in range(8):
        px = blank(32, 16)
        if idx < 4:
            c1, c2 = 50 + idx * 6, 53 + idx * 6    # greens
        else:
            c1, c2 = 116 + (idx - 4) * 6, 120 + (idx - 4) * 6
        for y in range(16):
            for x in range(32):
                if abs(x - 15.5) / 16 + abs(y - 7.5) / 8 <= 1.0:
                    edge = abs(x - 15.5) / 16 + abs(y - 7.5) / 8
                    shade = c2 if ((x // 4) + (y // 2)) % 2 else c1
                    if edge > 0.82:
                        shade = max(16, c1 - 6)
                    px[y * 32 + x] = shade
        frames.append((32, 16, px))
    return sprite(frames)


def make_map():
    """map.mkf: 64 cols x 128 rows x 2 halves, 4 bytes per cell."""
    rows, cols = 128, 64
    data = bytearray()
    for y in range(rows):
        for x in range(cols):
            for h in range(2):
                d = 0
                if h == 0:
                    # ground: grass with a winding path and a pond
                    on_path = abs(x - (20 + int(6 * math.sin(y / 9.0)))) < 2
                    pond = ((x - 44) ** 2) / 64 + ((y - 96) ** 2) / 900 < 1
                    if pond:
                        idx = 84 + (x + y) % 8
                    elif on_path:
                        idx = 16 + (x + y) % 8
                    else:
                        idx = 48 + (x * 7 + y * 3) % 8
                    d = idx
                else:
                    # sparse decorations on the top layer
                    if (x * 13 + y * 17) % 11 == 0 and 4 <= y < rows - 4:
                        d = (4 + (x + y) % 4) + 1   # stored = idx + 1
                data += u32(d)
    assert len(data) == rows * cols * 2 * 4
    return bytes(data)


def make_background():
    """fbp.mkf: 320x200 indexed scene."""
    w, h = 320, 200
    px = blank(w, h)
    for y in range(h):
        if y < 120:
            idx = 80 + min(24, y * 24 // 120)
        else:
            idx = 48 + (y - 120) // 6
        rect(px, w, 0, y, w, y + 1, idx)
    # clouds
    for cx, cy in ((40, 30), (160, 18), (250, 44)):
        rect(px, w, cx, cy, cx + 52, cy + 9, 9)
        rect(px, w, cx + 8, cy - 6, cx + 40, cy, 11)
    # hills with wavy top
    for x in range(w):
        top = 116 + int(14 * math.sin(x / 37.0)) + int(8 * math.sin(x / 13.0))
        for y in range(top, 150):
            px[y * w + x] = 52 + (x + y) % 10
    # ground with texture
    for y in range(146, h):
        for x in range(w):
            px[y * w + x] = 18 + (x * 3 + y * 7) % 24
    return bytes(px)


def make_rng():
    """rng.mkf: nested frame table, full-screen delta frames."""
    total_pairs = 320 * 200 // 2

    def frame_delta(f):
        ops = bytearray()
        done = 0
        while done < total_pairs:
            chunk = min(256, total_pairs - done)
            ops.append(0x0b)
            ops.append(chunk - 1)               # writes chunk pairs
            for k in range(chunk):
                i = (done + k) * 2
                for px_i in (i, i + 1):
                    x, y = px_i % 320, px_i // 320
                    v = 1 + ((x * x) // 512 + (y * y) // 256 + f * 37) % 254
                    ops.append(v)
            done += chunk
        ops.append(0x00)
        assert len(ops) <= 65000, len(ops)
        return bytes(ops)

    frames = [frame_delta(f) for f in range(4)]
    n = len(frames)
    offs = [4 * (n + 1)]
    for fr in frames:
        offs.append(offs[-1] + len(fr))
    return b''.join(u32(o) for o in offs) + b''.join(frames)


# --------------------------------------------------------------------------
# audio / midi

def make_wav_sine():
    rate, secs, freq = 44100, 0.7, 440
    import io
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(rate)
        frames = bytearray()
        for i in range(int(rate * secs)):
            t = i / rate
            env = min(1.0, i / 1000.0, (rate * secs - i) / 3000.0)
            v = int(22000 * env * math.sin(2 * math.pi * freq * t))
            frames += struct.pack('<h', v)
        f.writeframes(bytes(frames))
    return buf.getvalue()


def make_wav_square():
    rate, secs = 11025, 0.5
    import io
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as f:
        f.setnchannels(1)
        f.setsampwidth(1)
        f.setframerate(rate)
        frames = bytearray()
        for i in range(int(rate * secs)):
            on = (i * 440 * 2 // rate) % 2 == 0
            frames.append(200 if on else 56)
        f.writeframes(bytes(frames))
    return buf.getvalue()


def make_voc():
    """Creative Voice File with one type-01 block (~11000 Hz, 8-bit)."""
    sig = b'Creative Voice File\x1A'
    hdr = sig + u16(0x1A) + u16(0x010A) + u16(0x1135)   # offset, ver, csum
    assert len(hdr) == 0x1A
    tc = 166                                           # 256-166=90 -> ~11.2k
    n = 6000
    pcm = bytearray()
    for i in range(n):
        v = int(128 + 90 * math.sin(2 * math.pi * 660 * i / 11200.0))
        pcm.append(max(0, min(255, v)))
    body = bytes([tc, 0]) + bytes(pcm)
    block = bytes([0x01]) + bytes([len(body) & 0xFF, (len(body) >> 8) & 0xFF,
                                   (len(body) >> 16) & 0xFF]) + body
    return hdr + block + b'\x00'


def make_midi():
    track = (b'\x00\x90\x3c\x64\x60\x80\x3c\x00'
             b'\x00\x90\x40\x64\x60\x80\x40\x00'
             b'\x00\xff\x2f\x00')
    return (b'MThd' + u32(6) + u16(0) + u16(1) + u16(96) +
            b'MTrk' + u32(len(track)) + track)


# --------------------------------------------------------------------------
# tables

def make_sss(msg_bytes):
    """Returns (chunks, msg_offsets_chunk)."""
    # 0: event objects, 16 x u16
    ev = []
    for i in range(4):
        ev += [0, 8 + i * 5, 40 + i * 3, 0,       # vanish, x, y, layer
               100 + i, 200 + i,                   # trigger, auto script
               0, 1,                               # state, trigger mode
               i, 4,                               # sprite, frames
               0, 0, 0,                            # dir, current, idle frame
               0, 0, 0]                            # ptr, frames auto, idle auto
    ev_chunk = b''.join(u16(v) for v in ev)

    # 1: scenes, 4 x u16
    sc = []
    for i in range(8):
        sc += [i * 7 % 64, 300 + i, 400 + i, i * 12]
    sc_chunk = b''.join(u16(v) for v in sc)

    # 2: object definitions, 7 x u16 (WIN95 = 14 bytes)
    ob = []
    for i in range(10):
        ob += [0, 0, i * 4, 10 + i * 5, 500 + i, 600 + i, 0x0007]
    ob_chunk = b''.join(u16(v) for v in ob)

    # 4: scripts, 4 x u16 (op displayed as hex)
    scripts = []
    for i in range(16):
        scripts += [(i * 7) & 0xFF, 0x0100 + i, i * 3, 0xFFFF]
    sc4 = b''.join(u16(v) for v in scripts)

    # 3: message offsets into m.msg
    offsets = [0]
    acc = 0
    for _ in range(10):
        acc += _msg_len(_) + 1
        offsets.append(acc)
    assert acc == len(msg_bytes), (acc, len(msg_bytes))
    off_chunk = b''.join(u32(o) for o in offsets)
    return [ev_chunk, sc_chunk, ob_chunk, off_chunk, sc4]


MSGS = [
    '少侠留步，此去前路凶险。',
    '店家，来一壶上好的女儿红！',
    '这把剑乃是祖传之物。',
    '前方客栈可有打尖住店？',
    '在下李逍遥，途经此地。',
    '灵儿，我们一定能找到娘亲。',
    '妖魔休走，看剑！',
    '此物价值纹银十两。',
    '夜深了，早些歇息吧。',
    '缘之一字，最是难解。',
]


def _msg_len(i):
    return len(MSGS[i].encode('gbk'))


def make_msg():
    out = b''
    for m in MSGS:
        out += m.encode('gbk') + b'\x00'
    return out


# --------------------------------------------------------------------------
# codepage detection emulation (mirrors pal::detectCodepage in textdec.cpp:
# score both candidates over the NUL-stripped buffer, strictly fewer invalid
# characters wins, ties keep Big5 which comes first)

_VALID_RANGES = ((0x4E00, 0x9FFF), (0x3400, 0x4DBF), (0xF900, 0xFAFF),
                 (0x0020, 0x007E), (0x3000, 0x301E), (0xFF01, 0xFF5E))


def predict_codepage(data):
    """Return (name, confidence) exactly like pal::detectCodepage would."""
    raw = data.replace(b'\x00', b' ')
    min_invalids = None
    best = 'Big5'
    for name, codec in (('Big5', 'big5'), ('GBK', 'gb18030')):
        s = raw.decode(codec, errors='replace')
        invalids = sum(1 for ch in s
                       if not any(a <= ord(ch) <= b for a, b in _VALID_RANGES))
        if min_invalids is None or invalids < min_invalids:
            min_invalids = invalids
            best = name
    text_len = len(raw)
    if text_len and min_invalids < text_len // 2:
        conf = (text_len // 2 - min_invalids) * 200 // text_len
    else:
        conf = 0
    return best, max(0, min(100, conf))


def gbk_only_chars(limit=2):
    """CJK characters whose GBK bytes no Big5 decoder can read.

    GBK trail bytes 0x80-0xA0 are outside the Big5 trail ranges (0x40-0x7E,
    0xA1-0xFE), so a word containing one forces Big5 to score invalid
    characters while GBK scores zero -- exactly how real simplified-Chinese
    game data tips the detector off.
    """
    found = []
    for cp in range(0x4E00, 0x9FFF):
        c = chr(cp)
        try:
            b = c.encode('gbk')
        except UnicodeEncodeError:
            continue
        if len(b) != 2:
            continue
        try:
            b.decode('big5')
        except UnicodeDecodeError:
            try:
                b.decode('gb18030')
            except UnicodeDecodeError:
                continue
            found.append(c)
            if len(found) >= limit:
                break
    return found


def make_words():
    words = ['金创药', '还魂丹', '灵山仙', '紫菁玉', '珍珠',
             '女娲石', '试心果1', '无影神偷1', '醉仙酿', '御剑术1',
             'MP', 'HP']
    # GBK-extension words so the detector has to choose GBK over Big5.
    words += [c + '剑' for c in gbk_only_chars(2)]
    out = bytearray()
    for w in words:
        b = w.encode('gbk')
        if len(b) > 10:
            raise SystemExit('word too long: %s' % w)
        out += b + b' ' * (10 - len(b))
    return bytes(out)


def make_desc():
    lines = []
    texts = ['攻击力上升', '防御力上升', '身法提升', '恢复全部体力',
             '解百毒', '复活同伴', '投掷攻击', '增加金钱']
    for i, t in enumerate(texts):
        lines.append('%04x=%s' % (i + 1, t))
    return ('\n'.join(lines) + '\n').encode('gbk')


def make_data(ui_sprite, fx_sprite):
    chunks = [None] * 11

    # 0: stores, 9 x u16 per shop
    store = b''
    for shop in range(5):
        store += b''.join(u16((shop * 9 + i) % 90 + 1) for i in range(9))
    chunks[0] = store

    # 1: enemies, 35 x u16
    en = b''
    for i in range(6):
        fields = [2, 2, 2, 4, 12, 0,          # idle/magic/attack frames etc
                  40, 41, 42, 43, 44,          # sounds
                  100 + i * 35, 20 + i * 5,   # hp, exp
                  15 + i * 4, 3 + i,          # cash, level
                  5 + i, 30,                  # magic, magic rate
                  0, 0,                       # attack equiv item
                  0, 0,                       # steal item, count
                  10 + i * 3, 6 + i,          # attack, magic strength
                  5 + i, 8 + i, 5,            # defense, dexterity, flee
                  20,                         # poison resist
                  10, 10, 10, 10, 10,         # elemental resist
                  10, 0, 0]                   # physical, dual, collect
        assert len(fields) == 35, len(fields)
        en += b''.join(u16(v) for v in fields)
    chunks[1] = en

    # 2: enemy teams, 5 x u16
    team = b''
    for i in range(4):
        team += b''.join(u16((i + j) % 6 + 1 if j < 3 else 0)
                         for j in range(5))
    chunks[2] = team

    # 3: player roles (unstructured -> hex view)
    chunks[3] = bytes((i * 37 + 11) % 256 for i in range(200))

    # 4: magic, 16 x u16
    mg = b''
    for i in range(8):
        fields = [i, 1, 0, 0, 20 + i * 5, 8, 0, 4,
                  3, 1, 0, 0, 4 + i, 30 + i * 10, i % 5, 30 + i]
        assert len(fields) == 16
        mg += b''.join(u16(v) for v in fields)
    chunks[4] = mg

    # 5: battlefields, 6 x u16
    bf = b''
    for i in range(4):
        bf += b''.join(u16((i + j) * 10 % 256) for j in range(6))
    chunks[5] = bf

    # 6: level-up magic, 10 x u16
    lv = b''
    for p in range(2):
        lv += b''.join(u16(v) for v in (10 + p, 5 + p, 12 + p, 9 + p,
                                        14 + p, 13 + p, 16 + p, 17 + p,
                                        18 + p, 21 + p))
    chunks[6] = lv

    # 7/8: unstructured
    chunks[7] = bytes(range(32))
    chunks[8] = bytes(range(16, 32))

    # 9/10: sprites
    chunks[9] = ui_sprite
    chunks[10] = fx_sprite
    return mkf(chunks)


# --------------------------------------------------------------------------
# main

def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else 'testdata'
    os.makedirs(outdir, exist_ok=True)

    def save(name, data):
        with open(os.path.join(outdir, name), 'wb') as f:
            f.write(data)
        print('  %-12s %8d bytes' % (name, len(data)))

    print('generating PAL test data in', outdir)

    save('pat.mkf', mkf([DAY_PAL + NIGHT_PAL, DAY_PAL]))
    save('map.mkf', mkf([make_map()]))
    save('gop.mkf', mkf([make_tile_sprite()]))
    save('fbp.mkf', mkf([make_background()]))
    save('mgo.mkf', mkf(make_character_sprites()))
    save('ball.mkf', mkf(make_ball_sprites()))
    save('rng.mkf', mkf([make_rng()]))
    save('sounds.mkf', mkf([make_wav_sine(), make_wav_square()]))
    save('voc.mkf', mkf([make_voc()]))
    save('midi.mkf', mkf([make_midi()]))
    save('mus.mkf', mkf([b'RIX\x01\x00' + bytes(range(64))]))
    ui_spr, fx_spr = make_ui_sprites()
    save('data.mkf', make_data(ui_spr, fx_spr))

    msg = make_msg()
    save('sss.mkf', mkf(make_sss(msg)))
    save('m.msg', msg)
    save('word.dat', make_words())
    save('desc.dat', make_desc())

    # Codepage sanity: emulate the detector and require GBK to win for every
    # text file, otherwise the fixture exercises the wrong decoder.
    ok = True
    for name in ('word.dat', 'm.msg', 'desc.dat'):
        with open(os.path.join(outdir, name), 'rb') as f:
            data = f.read()
        cp, conf = predict_codepage(data)
        print('  codepage %-9s -> %-4s (confidence %d%%)'
              % (name, cp, conf))
        if cp != 'GBK':
            ok = False
    if not ok:
        raise SystemExit('codepage prediction failed: text fixtures must '
                         'decode as GBK')
    print('done.')


if __name__ == '__main__':
    main()
