// gen_navicons.js — builds examples/16_navlab/nav_images.h
//
//   node tools/gen_navicons.js
//
// Eight 48x48 monochrome bitmaps in the OEM's own format: row-major, 6 bytes per row,
// MSB-first (bit 7 of byte 0 is the leftmost pixel of the row). 288 bytes each.
//
// The globe is not drawn — it is LIFTED, byte for byte, out of the OEM head unit's own
// 0x1F1 transfer in docs/captures/"aknowledge on on display.csv". That capture is complete:
// 43 consecutive frames, sequence 21..2F 20..2F 20..2B, zero gaps. Regenerating this file
// re-derives it from the CSV rather than trusting the checked-in bytes, so if the capture
// is ever re-cut the icon follows.
//
// The tryzub is TRACED from tools/assets/tryzub.png, because a hand-drawn one came out
// recognisably a trident and recognisably not the герб. The rest are generated geometry.

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const W = 48, H = 48, STRIDE = 6, BYTES = STRIDE * H;   // 288
const REPO = path.join(__dirname, '..');
const CSV = path.join(REPO, 'docs', 'captures', 'aknowledge on on display.csv');
const OUT = path.join(REPO, 'examples', '16_navlab', 'nav_images.h');

// ---------------------------------------------------------------------------
// The OEM globe, reassembled from the capture
// ---------------------------------------------------------------------------
function oemMessage() {
  const rows = fs.readFileSync(CSV, 'utf8').split(/\r?\n/).filter(l => l.trim()).slice(1)
                 .map(l => l.split(','));
  const frames = rows.filter(r => r[1].endsWith('1F1'));
  let payload = [], declared = 0, seq = [];
  for (const r of frames) {
    const d = r.slice(6, 14).filter(x => x !== '').map(x => parseInt(x, 16));
    const pci = d[0];
    if (pci >> 4 === 1)      { declared = ((pci & 0xF) << 8) | d[1]; payload.push(...d.slice(2)); }
    else if (pci >> 4 === 2) { seq.push(pci & 0xF); payload.push(...d.slice(1)); }
    // the lone 0x70 function registration is not part of the transfer
  }
  let expect = 1;
  for (const s of seq) {
    if (s !== expect) throw new Error(`capture has a sequence gap: expected 0x2${expect.toString(16)}, got 0x2${s.toString(16)}`);
    expect = (expect + 1) & 0xF;
  }
  if (declared !== 302) throw new Error(`expected a 302-byte message, capture declares ${declared}`);
  payload = payload.slice(0, declared);
  return { header: payload.slice(0, 14), bitmap: payload.slice(14) };
}

// ---------------------------------------------------------------------------
// Generated geometry
// ---------------------------------------------------------------------------
const blank = () => Array.from({ length: H }, () => new Array(W).fill(0));

// Renault: the losange — concentric diamond rings joined at the two vertical vertices.
function renault() {
  const g = blank(), cx = 23.5, cy = 23.5, a = 15, b = 22.5, k = 0.55, t1 = 3.2, t2 = 2.6;
  const d = (x, y, A, B) => Math.abs(x - cx) / A + Math.abs(y - cy) / B;
  const ia = a * k, ib = b * k;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const outer = d(x, y, a, b) <= 1;
    if (outer && d(x, y, a - t1, b - t1 * b / a) > 1) g[y][x] = 1;
    if (d(x, y, ia, ib) <= 1 && d(x, y, ia - t2, ib - t2 * ib / ia) > 1) g[y][x] = 1;
    if (Math.abs(x - cx) < 2 && outer && d(x, y, ia, ib) > 1) g[y][x] = 1;
  }
  return g;
}

// ---------------------------------------------------------------------------
// Tracing a PNG
// ---------------------------------------------------------------------------
// The tryzub is NOT drawn here. Drawing one by hand produced something that was
// recognisably a trident and recognisably not the герб — the proportions of the real
// emblem are not something you land by tuning arc radii. So the actual artwork goes in
// tools/assets/ and this traces it, which means any future logo takes the same path
// instead of another evening of parametric guessing.
//
// Minimal decoder: 8-bit, non-interlaced, any colour type. That is what the asset is, and
// a general PNG library is not worth a dependency for one file.
function decodePNG(buf) {
  if (buf.readUInt32BE(0) !== 0x89504E47) throw new Error("not a png");
  let p = 8, w = 0, h = 0, bd = 0, ct = 0, il = 0, idat = [], plte = null, trns = null;
  while (p < buf.length) {
    const len = buf.readUInt32BE(p), type = buf.toString("ascii", p + 4, p + 8);
    const data = buf.slice(p + 8, p + 8 + len);
    if (type === "IHDR") { w = data.readUInt32BE(0); h = data.readUInt32BE(4); bd = data[8]; ct = data[9]; il = data[12]; }
    else if (type === "IDAT") idat.push(data);
    else if (type === "PLTE") plte = data;
    else if (type === "tRNS") trns = data;
    else if (type === "IEND") break;
    p += 12 + len;
  }
  if (il) throw new Error("interlaced png unsupported");
  if (bd !== 8) throw new Error("bit depth " + bd + " unsupported");
  const ch = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[ct];
  if (!ch) throw new Error("colour type " + ct);
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = w * ch, out = Buffer.alloc(h * stride);
  let q = 0;
  for (let y = 0; y < h; y++) {
    const f = raw[q++], line = raw.slice(q, q + stride); q += stride;
    const cur = out.slice(y * stride, (y + 1) * stride);
    const prev = y ? out.slice((y - 1) * stride, y * stride) : null;
    for (let i = 0; i < stride; i++) {
      const A = i >= ch ? cur[i - ch] : 0, B = prev ? prev[i] : 0;
      const C = (prev && i >= ch) ? prev[i - ch] : 0;
      let v = line[i];
      if (f === 1) v += A; else if (f === 2) v += B; else if (f === 3) v += (A + B) >> 1;
      else if (f === 4) {
        const pp = A + B - C, pa = Math.abs(pp - A), pb = Math.abs(pp - B), pc = Math.abs(pp - C);
        v += (pa <= pb && pa <= pc) ? A : (pb <= pc ? B : C);
      }
      cur[i] = v & 0xFF;
    }
  }
  const lum = new Float32Array(w * h), alpha = new Float32Array(w * h);
  for (let i = 0; i < w * h; i++) {
    let r, gg, b, al = 255;
    if (ct === 0) { r = gg = b = out[i]; }
    else if (ct === 2) { r = out[i*3]; gg = out[i*3+1]; b = out[i*3+2]; }
    else if (ct === 3) { const ix = out[i]; r = plte[ix*3]; gg = plte[ix*3+1]; b = plte[ix*3+2]; if (trns && ix < trns.length) al = trns[ix]; }
    else if (ct === 4) { r = gg = b = out[i*2]; al = out[i*2+1]; }
    else { r = out[i*4]; gg = out[i*4+1]; b = out[i*4+2]; al = out[i*4+3]; }
    lum[i] = 0.299*r + 0.587*gg + 0.114*b; alpha[i] = al;
  }
  return { w, h, lum, alpha };
}

// Crop to the artwork, fit it into 48x48 keeping the aspect, and threshold on how much of
// each destination cell the ink covers. COVERAGE, not nearest-neighbour: at this scale one
// destination pixel is ~15 source pixels across, and point-sampling an outline drawing
// drops whole strokes depending on where the grid lands. 0.30 keeps the strokes one pixel
// wide and the enclosed shapes open; lower fills the gaps in, higher breaks the outline.
function traceImage(file, coverage) {
  const img = decodePNG(fs.readFileSync(file));
  const ink = (x, y) => { const i = y*img.w + x; return img.alpha[i] > 128 && img.lum[i] < 140; };
  let x0 = img.w, y0 = img.h, x1 = -1, y1 = -1;
  for (let y = 0; y < img.h; y++) for (let x = 0; x < img.w; x++) if (ink(x, y)) {
    if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
  }
  const cw = x1 - x0 + 1, chh = y1 - y0 + 1;
  const scale = Math.min(W / cw, H / chh);
  const dw = Math.round(cw * scale), dh = Math.round(chh * scale);
  const ox = Math.floor((W - dw) / 2), oy = Math.floor((H - dh) / 2);
  const g = blank();
  for (let dy = 0; dy < dh; dy++) for (let dx = 0; dx < dw; dx++) {
    const sx0 = x0 + Math.floor(dx * cw / dw), sx1 = Math.max(x0 + Math.floor((dx+1) * cw / dw), sx0 + 1);
    const sy0 = y0 + Math.floor(dy * chh / dh), sy1 = Math.max(y0 + Math.floor((dy+1) * chh / dh), sy0 + 1);
    let n = 0, t = 0;
    for (let sy = sy0; sy < sy1; sy++) for (let sx = sx0; sx < sx1; sx++) { t++; if (ink(sx, sy)) n++; }
    if (t && n / t >= coverage) g[oy + dy][ox + dx] = 1;
  }
  return g;
}

const tryzub = () => traceImage(path.join(__dirname, "assets", "tryzub.png"), 0.30);

// ---------------------------------------------------------------------------
// A 5x7 font, and the three "instrument" images built from it
//
// These exist to answer a different question from the logo images. A logo tells you the
// panel accepted a bitmap; FINE DETAIL tells you what it did with it. If a 5x7 glyph comes
// back legible, the pane is a true 48x48 1:1 bitmap. If it comes back smeared, doubled or
// half-height, the pane is being scaled and the geometry below is wrong.
// ---------------------------------------------------------------------------
const FONT = {
  '0': ['.###.', '#...#', '#..##', '#.#.#', '##..#', '#...#', '.###.'],
  '1': ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'],
  '2': ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'],
  '3': ['.###.', '#...#', '....#', '..##.', '....#', '#...#', '.###.'],
  '4': ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'],
  '5': ['#####', '#....', '####.', '....#', '....#', '#...#', '.###.'],
  '6': ['..##.', '.#...', '#....', '####.', '#...#', '#...#', '.###.'],
  '7': ['#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'],
  '8': ['.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'],
  '9': ['.###.', '#...#', '#...#', '.####', '....#', '...#.', '.##..'],
  ':': ['.....', '..#..', '..#..', '.....', '..#..', '..#..', '.....'],
  '.': ['.....', '.....', '.....', '.....', '.....', '.##..', '.##..'],
  '-': ['.....', '.....', '.....', '#####', '.....', '.....', '.....'],
  'C': ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'],
  'V': ['#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'],
  '°': ['.##..', '#..#.', '#..#.', '.##..', '.....', '.....', '.....'],
  ' ': ['.....', '.....', '.....', '.....', '.....', '.....', '.....'],
};

// Centred single line of 5x7 text with a one-pixel gap; unknown characters become spaces.
function text(g, s, y0) {
  const w = s.length * 6 - 1, x0 = Math.round((W - w) / 2);
  for (let i = 0; i < s.length; i++) {
    const glyph = FONT[s[i]] || FONT[' '];
    for (let r = 0; r < 7; r++) for (let c = 0; c < 5; c++)
      if (glyph[r][c] === '#') {
        const x = x0 + i * 6 + c, y = y0 + r;
        if (x >= 0 && x < W && y >= 0 && y < H) g[y][x] = 1;
      }
  }
}

const disc = (g, cx, cy, r, th) => {
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const d = Math.hypot(x - cx, y - cy);
    if (d <= r && d >= r - th) g[y][x] = 1;
  }
};
const line = (g, x0, y0, x1, y1, th = 1) => {
  const n = Math.ceil(Math.hypot(x1 - x0, y1 - y0) * 3);
  for (let i = 0; i <= n; i++) {
    const x = x0 + (x1 - x0) * i / n, y = y0 + (y1 - y0) * i / n;
    for (let dx = -th / 2; dx <= th / 2; dx += 0.5) for (let dy = -th / 2; dy <= th / 2; dy += 0.5) {
      const px = Math.round(x + dx), py = Math.round(y + dy);
      if (px >= 0 && px < W && py >= 0 && py < H) g[py][px] = 1;
    }
  }
};
const box = (g, x0, y0, x1, y1, fill) => {
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++)
    if (fill || y === y0 || y === y1 || x === x0 || x === x1)
      if (x >= 0 && x < W && y >= 0 && y < H) g[y][x] = 1;
};

// A clock face over the value. Hands are drawn for the value shown, not for "now" — this
// file is generated once and checked in, so a real clock would be a lie by the second build.
function clockFace(hhmm) {
  const g = blank(), cx = 23.5, cy = 13, r = 11.5;
  disc(g, cx, cy, r, 2);
  // FOUR ticks, not twelve. At r = 11.5 a twelve-tick face and two hands land on the same
  // pixels and the whole face reads as noise — which is itself the lesson this image is
  // here to teach about how little detail 48x48 holds.
  for (let k = 0; k < 4; k++) {
    const a = k * Math.PI / 2;
    line(g, cx + (r - 4) * Math.sin(a), cy - (r - 4) * Math.cos(a),
            cx + (r - 2.5) * Math.sin(a), cy - (r - 2.5) * Math.cos(a), 1);
  }
  const hh = parseInt(hhmm.slice(0, 2), 10), mm = parseInt(hhmm.slice(3), 10);
  const ha = ((hh % 12) + mm / 60) * Math.PI / 6, ma = mm * Math.PI / 30;
  line(g, cx, cy, cx + 5 * Math.sin(ha), cy - 5 * Math.cos(ha), 1);
  line(g, cx, cy, cx + 7.5 * Math.sin(ma), cy - 7.5 * Math.cos(ma), 1);
  box(g, 23, 12, 24, 13, true);                        // hub
  text(g, hhmm, 29);
  return g;
}

// A thermometer over the value.
function thermo(value) {
  const g = blank();
  box(g, 21, 3, 26, 17, false);            // tube
  disc(g, 23.5, 20.5, 4.5, 1.5);           // bulb outline
  box(g, 22, 20, 25, 22, true);            // bulb fill
  box(g, 23, 9, 24, 19, true);             // mercury column
  for (let y = 6; y <= 15; y += 3) box(g, 27, y, 29, y, true);   // scale marks
  text(g, value, 29);
  return g;
}

// A battery over the value.
function battery(value) {
  const g = blank();
  box(g, 10, 6, 37, 21, false);            // case
  box(g, 38, 11, 40, 16, true);            // terminal nub
  for (let i = 0; i < 3; i++) box(g, 13 + i * 8, 9, 17 + i * 8, 18, true);   // charge bars
  text(g, value, 29);
  return g;
}

// Vertically resample a grid into rows [y0..y1], OR-ing every source row that lands in a
// destination row. OR rather than nearest-neighbour because dropping a row of a 1bpp
// outline breaks the outline; over-inking a squashed glyph merely thickens it.
function squashY(src, y0, y1) {
  const g = blank(), hDst = y1 - y0 + 1;
  for (let y = 0; y < H; y++) {
    const dy = y0 + Math.floor(y * hDst / H);
    for (let x = 0; x < W; x++) if (src[y][x] && dy >= 0 && dy < H) g[dy][x] = 1;
  }
  return g;
}

// The tryzub with the clock under it — the one image here that is two things at once, and
// therefore the best single probe of how much detail the pane really resolves.
function tryzubClock(hhmm) {
  const g = squashY(tryzub(), 0, 30);
  text(g, hhmm, 34);
  return g;
}

// A 6x6 checkerboard — not decoration. It is the ORIENTATION PROBE: it is the only image
// here whose top-left corner is distinguishable from its bottom-right, so it is what tells
// you whether the panel reads rows top-down and bits MSB-first the way this file assumes.
function checker() {
  const g = blank();
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++)
    g[y][x] = ((Math.floor(x / 6) + Math.floor(y / 6)) & 1) ? 1 : 0;
  for (let x = 0; x < 12; x++) g[0][x] = 1;   // a bar along the top edge only
  for (let y = 0; y < 12; y++) g[y][0] = 1;   // and down the left edge only
  return g;
}

// ---------------------------------------------------------------------------
const pack = g => {
  const out = new Array(BYTES).fill(0);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++)
    if (g[y][x]) out[y * STRIDE + (x >> 3)] |= 0x80 >> (x & 7);
  return out;
};
const art = g => g.map(r => r.map(v => v ? '#' : '.').join('')).join('\n');

function emit(name, bytes) {
  let s = `inline constexpr uint8_t ${name}[kBitmapBytes] = {\n`;
  for (let i = 0; i < bytes.length; i += 12)
    s += '  ' + bytes.slice(i, i + 12).map(b => '0x' + b.toString(16).toUpperCase().padStart(2, '0')).join(', ') + ',\n';
  return s + '};\n';
}

const oem = oemMessage();
const images = [
  ['kBmpGlobe',       oem.bitmap,                    'the OEM globe, lifted from the capture'],
  ['kBmpRenault',     pack(renault()),               'the Renault losange'],
  ['kBmpTryzub',      pack(tryzub()),                'the Ukrainian tryzub'],
  ['kBmpTryzubClock', pack(tryzubClock('10:56')),    'tryzub with the clock under it'],
  ['kBmpClock',       pack(clockFace('10:56')),      'clock face + digits'],
  ['kBmpTemp',        pack(thermo('23.5°C')),        'thermometer + temperature'],
  ['kBmpVolts',       pack(battery('12.4 V')),       'battery + accumulator voltage'],
  ['kBmpChecker',     pack(checker()),               'orientation probe'],
];

let h = `// nav_images.h - GENERATED by tools/gen_navicons.js. Do not edit; edit the generator.
//
// Four 48x48 monochrome bitmaps in the OEM's 0x1F1 format: row-major, 6 bytes per row,
// MSB-first, 288 bytes each. See examples/16_navlab/main.cpp for what the 14 header bytes
// in front of them mean, and docs/PROTOCOL-NOTES.md for how the format was derived.
#pragma once
#include <stdint.h>

namespace navlab {

inline constexpr uint16_t kBitmapW     = 48;
inline constexpr uint16_t kBitmapH     = 48;
inline constexpr uint16_t kBitmapBytes = 288;   // 48 rows x 6 bytes

// The OEM's own 14-byte header, verbatim. Bytes 12 and 13 are 0x30 0x30 = 48, 48 - the
// only two fields in it whose meaning is better than a guess, and they agree with the
// bitmap being exactly 288 bytes. Bytes 4..10 are "ABCDEF\\0": a seven-byte string slot,
// sitting at a placeholder because the captured car had no navigation CD.
inline constexpr uint8_t kOemHeader[14] = {
  ${oem.header.map(b => '0x' + b.toString(16).toUpperCase().padStart(2, '0')).join(', ')}
};

`;
for (const [name, bytes, what] of images) {
  if (bytes.length !== BYTES) throw new Error(`${name} is ${bytes.length} bytes, expected ${BYTES}`);
  h += `// ${what}\n${emit(name, bytes)}\n`;
}
h += `}  // namespace navlab\n`;

fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, h);
console.log('wrote ' + path.relative(REPO, OUT));
for (const [name, bytes] of images) {
  const g = blank();
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++)
    g[y][x] = (bytes[y * STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1;
  console.log('\n' + name + '\n' + art(g));
}
