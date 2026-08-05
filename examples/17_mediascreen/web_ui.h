// web_ui.h — the console for 17_mediascreen.
//
// SIX TABS, ONE COMMAND FUNCTION. Everything goes through cmd(op, params) and every press
// reports its own result in the header, because the previous console had buttons that
// silently did nothing for a day and no way to tell which.
//
// Three rules this file follows, each learned the hard way on this board:
//   * anything an inline handler calls is a `function` DECLARATION, never a const arrow.
//     Only the hoisted form is reachable from onclick here; a const that is not reachable
//     fails silently and takes every control that used it with it.
//   * no escape sequences produced indirectly. One lost backslash is a SyntaxError, and a
//     SyntaxError anywhere in the block means the whole script never runs — so every button
//     on the page dies at once while the HTML still renders perfectly.
//   * tools/check_web_ui.js parses this file and catches both in about a second. The build
//     cannot: a console page is a C++ raw string literal, so broken JS compiles and flashes.
#pragma once

static const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AffaDisplay console</title>
<style>
:root{--bg:#0e1014;--pn:#161a21;--fg:#e9ecf2;--dim:#7f889b;--ln:#242a35;--ac:#5ac8fa;--ok:#4ade80;--wn:#ffb020;--bd:#ff5f5f}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.5 ui-monospace,Menlo,Consolas,monospace}
header{padding:10px 14px;border-bottom:1px solid var(--ln);display:flex;gap:14px;align-items:center;flex-wrap:wrap;position:sticky;top:0;background:var(--bg);z-index:5}
h1{font-size:13px;margin:0;letter-spacing:.14em;white-space:nowrap}
#st{color:var(--dim);font-size:12px;flex:1;min-width:200px}
#res{font-size:12px;min-width:140px;text-align:right}
nav{display:flex;gap:2px;padding:0 14px;border-bottom:1px solid var(--ln);flex-wrap:wrap;background:var(--bg)}
nav button{background:none;border:none;border-bottom:2px solid transparent;color:var(--dim);padding:9px 14px;font:inherit;cursor:pointer}
nav button.on{color:var(--fg);border-bottom-color:var(--ac)}
main{padding:14px;display:flex;gap:14px;flex-wrap:wrap;align-items:flex-start}
section{background:var(--pn);border:1px solid var(--ln);border-radius:8px;padding:13px;min-width:280px}
h2{font-size:10px;margin:0 0 11px;color:var(--dim);letter-spacing:.16em;text-transform:uppercase}
button{background:#1c212b;color:var(--fg);border:1px solid var(--ln);border-radius:5px;padding:6px 10px;font:inherit;cursor:pointer}
button:hover{border-color:var(--ac)}
button.p{background:#123c52;border-color:var(--ac);color:#d8f2ff}
button.d{border-color:var(--bd);color:#ffd5d5}
input,select{background:#0a0c11;color:var(--fg);border:1px solid var(--ln);border-radius:5px;padding:5px 7px;font:inherit}
.r{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin-bottom:8px}
label{display:flex;gap:5px;align-items:center;color:var(--dim)}
canvas{image-rendering:pixelated;background:#000;border:1px solid var(--ln);display:block;cursor:crosshair;touch-action:none}
pre{background:#07090d;border:1px solid var(--ln);border-radius:5px;padding:8px;margin:0;max-height:340px;overflow:auto;font-size:11px;color:var(--dim);white-space:pre}
small{color:var(--dim)}
.k{color:var(--ac)}.g{color:var(--ok)}.w{color:var(--wn)}.b{color:var(--bd)}
table{border-collapse:collapse;width:100%;font-size:11px}
td,th{padding:3px 6px;border-bottom:1px solid var(--ln);text-align:left}
th{color:var(--dim);font-weight:normal}
.hide{display:none}
</style></head><body>

<header>
  <h1>AFFA<span class="k">DISPLAY</span></h1>
  <div id="st">connecting&hellip;</div>
  <div id="res">&nbsp;</div>
  <button class="d" onclick="cmd('panic')">PANIC</button>
</header>

<nav>
  <button id="t-bitmap" class="on" onclick="tab('bitmap')">Bitmap</button>
  <button id="t-text"   onclick="tab('text')">Text</button>
  <button id="t-menus"  onclick="tab('menus')">Menus</button>
  <button id="t-keys"   onclick="tab('keys')">Keys</button>
  <button id="t-wire"   onclick="tab('wire')">Wire</button>
  <button id="t-set"    onclick="tab('set')">Settings</button>
</nav>

<main id="p-bitmap">
  <section>
    <h2>Editor &mdash; 48 &times; 48</h2>
    <canvas id="cv" width="48" height="48" style="width:288px;height:288px"></canvas>
    <div class="r" style="margin-top:9px">
      <button onclick="draw(1)">pen</button>
      <button onclick="draw(0)">eraser</button>
      <button onclick="inv()">invert</button>
      <button onclick="clr()">clear</button>
      <small id="pen">pen</small>
    </div>
    <div class="r">
      <small>shift</small>
      <button onclick="sh(0,-1)">&uarr;</button><button onclick="sh(0,1)">&darr;</button>
      <button onclick="sh(-1,0)">&larr;</button><button onclick="sh(1,0)">&rarr;</button>
    </div>
    <div class="r">
      <input id="btx" size="8" value="AFFA"><input id="bsz" type="number" value="20" style="width:56px">
      <button onclick="drawText()">render text</button>
    </div>
    <div class="r"><button class="p" onclick="sendBmp()">SEND TO PANEL</button></div>
  </section>

  <section>
    <h2>Images</h2>
    <div class="r">
      <button onclick="load('globe')">globe</button>
      <button onclick="load('tryzub')">tryzub</button>
      <button onclick="load('tryzubclock')">tryzub+clock</button>
      <button onclick="load('renault')">renault</button>
    </div>
    <div class="r">
      <button onclick="load('dash')">dash</button>
      <button onclick="load('gauges')">gauges</button>
      <button onclick="load('combo')">combo</button>
      <button onclick="load('fontsheet')">font</button>
      <button onclick="load('checker')">checker</button>
    </div>
    <p><small>Loads into the editor. <b>SEND TO PANEL</b> is what puts it on the glass.</small></p>

    <h2 style="margin-top:15px">Animations &mdash; drawn on the device</h2>
    <div class="r">
      <button onclick="scene('spectrum')">spectrum</button>
      <button onclick="scene('vu')">VU needle</button>
      <button onclick="scene('wave')">waveform</button>
      <button onclick="scene('clock')">clock</button>
    </div>
    <div class="r">
      <button onclick="scene('stars')">starfield</button>
      <button onclick="scene('bounce')">bounce</button>
      <button onclick="scene('rings')">rings</button>
    </div>
    <div class="r">
      <small>frame ms</small><input id="per" type="number" value="250" style="width:70px">
      <button onclick="setPeriod()">set</button>
      <button onclick="cmd('pane',{on:0})">stop pane</button>
      <button onclick="scene('blank')">blank</button>
    </div>
    <p><small>
      An image is 44 CAN frames at ISO-TP BlockSize 1 &mdash; 47&ndash;64 ms measured. 250 ms
      is about 23% of the link; 120 ms is the floor. Identical frames are never resent, so a
      still image costs one transfer and then nothing. <b>stop pane</b> does not clear the
      glass &mdash; no command to erase this pane is known; <b>blank</b> sends 288 zero bytes.
    </small></p>
  </section>
</main>

<main id="p-text" class="hide">
  <section>
    <h2>Main line &mdash; 8 characters</h2>
    <div class="r"><input id="mt" size="26" placeholder="text"></div>
    <div class="r">
      <label><input type="checkbox" id="msc"> scroll it</label>
      <small>ms</small><input id="mms" type="number" value="700" style="width:66px">
      <button class="p" onclick="sendText()">SET TEXT</button>
    </div>
    <div class="r">
      <small>icon</small>
      <select id="mic"><option value="0x55">none 55</option><option value="0x45">AF-RDS 45</option><option value="0x09">capture 09</option></select>
      <small>source</small>
      <select id="msr"><option value="0xFF">none FF</option><option value="0xDF">MANU DF</option><option value="0xFD">PRESET FD</option></select>
    </div>
    <div class="r">
      <small>format</small>
      <select id="mfm"><option value="0x60">plain ASCII 60</option><option value="0x31">radio 5+point+1  31</option><option value="0x19">radio +tick 19</option></select>
    </div>
    <p><small>
      The field is <b>8 characters</b>, measured with a column ruler rather than assumed.
      Longer than that has to scroll or be cut. Format <span class="k">0x31</span> is radio
      style &mdash; the panel draws a point between the digits, which is why the OEM's
      <span class="k">"   1056 "</span> reads as 105.6 FM and not as the number 1056.
    </small></p>
  </section>

  <section>
    <h2>Clock</h2>
    <div class="r">
      <input id="hh" size="4" maxlength="4" value="1056" style="width:70px">
      <button class="p" onclick="setTimeNow()">setTime</button>
      <button onclick="useNow()">browser clock</button>
    </div>
    <p><small>On this family the radio owns the clock; it only stays right if something keeps
      sending it.</small></p>

    <h2 style="margin-top:15px">Power</h2>
    <div class="r">
      <button onclick="cmd('power',{on:1})">display ON</button>
      <button onclick="cmd('power',{on:0})">display OFF</button>
      <button onclick="cmd('opening')">replay OEM opening</button>
    </div>
    <p><small><span class="k">52 09 00</span> / <span class="k">52 00 00</span>. Nothing draws
      at all while off. The opening is what makes the panel accept a nav image.</small></p>

    <h2 style="margin-top:15px">Popup overlay</h2>
    <div class="r">
      <input id="pt" size="10" value="VOL 28">
      <button class="p" onclick="sendPopup()">show</button>
      <button onclick="cmd('pophide')">hide</button>
    </div>
    <div class="r">
      <small>icon</small>
      <select id="pic"><option value="0x09">volume 09</option><option value="0x55">none 55</option><option value="0x45">AF-RDS 45</option></select>
      <small>fmt</small>
      <select id="pfm"><option value="0x60">plain 60</option><option value="0x31">radio 31</option></select>
    </div>
    <p><small>The one true overlay: the screen underneath keeps redrawing and reappears when
      it is hidden. Its lifetime is the application's &mdash; nothing auto-closes it.</small></p>

    <h2 style="margin-top:15px">Fullscreen</h2>
    <div class="r">
      <button class="p" onclick="randomFull()">random full-width</button>
      <button onclick="cmd('fshide')">hide</button>
    </div>
  </section>
</main>

<main id="p-menus" class="hide">
  <section>
    <h2>Two-row menu</h2>
    <div class="r">
      <input id="mh" size="9" value="MENU"><input id="ma" size="9" value="ROW ONE"><input id="mb" size="9" value="ROW TWO">
    </div>
    <div class="r">
      <small>scroll mask</small>
      <select id="msk"><option value="0">none</option><option value="7">up</option><option value="11">down</option><option value="3">both</option></select>
      <button class="p" onclick="sendMenu()">showMenu</button>
    </div>
    <div class="r">
      <button onclick="cmd('hilite',{n:0})">highlight row 0</button>
      <button onclick="cmd('hilite',{n:1})">highlight row 1</button>
    </div>

    <h2 style="margin-top:15px">N-item list</h2>
    <div class="r"><input id="nh" size="10" value="NAVIGATION"></div>
    <div class="r"><input id="ni" style="flex:1;min-width:220px" value="DESTINATION|ROUTE|MAP|TRAFFIC|SETTINGS|BACK"></div>
    <div class="r">
      <button class="p" onclick="sendMenuN()">showMenuN</button>
      <small>index</small><input id="nsel" type="number" value="0" style="width:52px">
      <button onclick="selectItem()">selectMenuItem</button>
    </div>
    <p><small>
      The panel <b>tracks six items and draws two</b> &mdash; the glass is a two-row viewport.
      Send the list once, then move the selection with <b>selectMenuItem</b> (eight bytes
      instead of two hundred) and the panel scrolls itself.
    </small></p>
  </section>

  <section>
    <h2>Info menu &mdash; three rows, 8 chars each</h2>
    <div class="r"><input id="r0" size="18"><label><input type="checkbox" id="s0"> scroll</label></div>
    <div class="r"><input id="r1" size="18"><label><input type="checkbox" id="s1"> scroll</label></div>
    <div class="r"><input id="r2" size="18"><label><input type="checkbox" id="s2"> scroll</label></div>
    <div class="r">
      <button class="p" onclick="sendRows()">showInfoMenu</button>
      <label><input type="checkbox" id="rlive" onchange="sendRows()"> keep repainting</label>
    </div>
    <p><small>
      This is the screen that <b>coexists with the bitmap</b> &mdash; both on the glass at
      once. Repainting three rows on a timer is a lot of traffic next to one main line, so it
      is opt-in.
    </small></p>

    <h2 style="margin-top:15px">Info popup</h2>
    <div class="r">
      <button class="p" onclick="cmd('infopopup',{a:'LINE ONE',b:'LINE TWO',c:'LINE THREE'})">show</button>
      <button onclick="cmd('infohide')">hide</button>
    </div>

    <h2 style="margin-top:15px">Message box</h2>
    <div class="r">
      <input id="ch" size="9" value="CONFIRM"><input id="ca" size="12" value="DELETE ENTRY?">
    </div>
    <div class="r">
      <button class="p" onclick="sendConfirm()">showConfirmBox</button>
      <button onclick="cmd('fshide')">close box</button>
      <button onclick="cmd('boxsel',{n:0})">select 0</button>
      <button onclick="cmd('boxsel',{n:1})">select 1</button>
    </div>
    <p><small>
      <span class="w">showConfirmBox is a ONE-button OK box.</span> It emits
      <span class="k">21 05 00 00 01 49</span> and byte 4 is the button count, confirmed on
      four OEM captures with 0, 1 and 2 buttons. The real Yes/No form is
      <span class="k">21 05 01 00 02 49</span> with two 6-byte labels, and the library has no
      builder for it yet. The select buttons send <span class="k">29 05 &lt;n&gt;</span>,
      which is how a box moves its selection &mdash; on a one-button box only index 0 exists,
      so index 1 doing nothing is the expected answer, not a fault.
      <br><br><b>close box</b> is <span class="k">54 03</span> &mdash; the same command that
      closes the full window, because the box IS a mode <span class="k">0x05</span> screen.
      It had no close button at all until somebody was left staring at a delete prompt.
    </small></p>
  </section>
</main>

<main id="p-keys" class="hide">
  <section style="flex:1;min-width:340px">
    <h2>Press a key on the stalk</h2>
    <div class="r">
      <button onclick="cmd('keysclear')">clear</button>
      <small id="kc">0 seen</small>
    </div>
    <table id="kt"><thead><tr><th>ms</th><th>key</th><th>code</th><th>edge</th></tr></thead><tbody></tbody></table>
    <p><small>
      What the library <b>decoded</b>, not what arrived on the wire &mdash; the raw frames are
      on the Wire tab. The <span class="k">03 89</span> guard is why this shows real keys:
      <span class="k">0x1C1</span> also carries registration and status frames, and a decoder
      without the guard reports those as keys 0x640F and 0x3030.
      <br><br>Nothing here is emulated. If a press does not appear, the panel did not send it
      or the wiring does not carry it &mdash; the joystick is on the panel, not the radio.
    </small></p>
  </section>
</main>

<main id="p-wire" class="hide">
  <section style="flex:1;min-width:360px">
    <h2>Every frame, both directions</h2>
    <div class="r">
      <button onclick="cmd('tap',{all:0})">hide nav continuations</button>
      <button onclick="cmd('tap',{all:1})">show everything</button>
      <button class="p" onclick="exportWire()">export .tsv</button>
      <label><input type="checkbox" id="wauto" checked> follow</label>
    </div>
    <pre id="fr">&nbsp;</pre>
    <p><small>
      Coalesced against the newest row, so a repeat shows as a count. <b>This is the only
      thing here that is evidence</b> &mdash; a log line prints whether or not the call was
      accepted; a frame either went out or it did not.
      <br>Nav continuations are hidden by default: an image is 1 + 43 CFs + 43 flow controls,
      and at 4 fps that flushes the ring ten times a second. The <b>first</b> frame of each
      transfer is always kept &mdash; that is where the header is.
    </small></p>
  </section>
  <section style="flex:1;min-width:300px">
    <h2>Log</h2>
    <pre id="log">&nbsp;</pre>
  </section>
</main>

<main id="p-set" class="hide">
  <section>
    <h2>Panel family &mdash; a BOOT choice</h2>
    <div class="r">
      <span id="fam"><small>&nbsp;</small></span>
      <button onclick="famSet('carminat')">Carminat</button>
      <button onclick="famSet('updatelist')">UpdateList</button>
    </div>
    <p><small>
      Carminat syncs on <span class="k">0x3AF</span>, UpdateList on <span class="k">0x3DF</span>,
      and the handshake, ids and text encoding all differ. There is no honest runtime switch,
      so the choice is stored and applied on the next boot. The nav pane and the N-item list
      are <b>Carminat only</b>.
    </small></p>

    <h2 style="margin-top:15px">Main line hold</h2>
    <div class="r">
      <small>auto-return after</small><input id="hold" type="number" value="0" style="width:80px">
      <small>ms &mdash; 0 = never</small>
      <button onclick="setHold()">set</button>
    </div>
    <p><small>
      While a menu or a box is on the glass the scrolling line <b>shuts up</b>, so a repaint
      cannot close a screen somebody is reading. The library reports which screen was last
      acknowledged (<span class="k">lastRendered()</span>); deciding what to do about it is
      the application's job. Auto-return is off by default, because a menu that closes itself
      while you are reading it is the bug this prevents.
    </small></p>

    <h2 style="margin-top:15px">Board</h2>
    <div class="r">
      <button onclick="location='/update'">OTA update</button>
      <button class="d" onclick="cmd('reboot')">reboot</button>
    </div>
    <pre id="diag">&nbsp;</pre>
  </section>
</main>

<script>
// ONE COMMAND FUNCTION. Every action goes through it, every press reports its own result.
// FUNCTION DECLARATIONS THROUGHOUT: only the hoisted form is reachable from the inline
// onclick handlers above, and a const arrow that is not reachable fails silently.
function cmd(op, p) {
  var q = 'op=' + encodeURIComponent(op);
  if (p) for (var k in p) q += '&' + k + '=' + encodeURIComponent(p[k]);
  return fetch('/api/cmd?' + q).then(function (r) { return r.json(); }).then(function (j) {
    say(j.ok, (j.op || op) + ': ' + j.msg);
    poll();
    return j;
  }).catch(function (e) { say(false, String(e)); });
}
function el(id) { return document.getElementById(id); }
function say(ok, m) { var e = el('res'); e.className = ok ? 'g' : 'b'; e.textContent = m; }

var cur = 'bitmap';
function tab(n) {
  var all = ['bitmap', 'text', 'menus', 'keys', 'wire', 'set'];
  for (var i = 0; i < all.length; i++) {
    el('p-' + all[i]).className = (all[i] === n) ? '' : 'hide';
    el('t-' + all[i]).className = (all[i] === n) ? 'on' : '';
  }
  cur = n;
  poll();
}

// ---- bitmap editor ---------------------------------------------------------
var W = 48, H = 48, ST = 6, NB = 288;
var cv = el('cv'), cx = cv.getContext('2d');
var px = new Uint8Array(W * H), penVal = 1, drawing = false;

function paint() {
  var im = cx.createImageData(W, H);
  for (var i = 0; i < W * H; i++) {
    var v = px[i] ? 255 : 0;
    im.data[i * 4] = v; im.data[i * 4 + 1] = v; im.data[i * 4 + 2] = v; im.data[i * 4 + 3] = 255;
  }
  cx.putImageData(im, 0, 0);
}
function draw(v) { penVal = v; el('pen').textContent = v ? 'pen' : 'eraser'; }
function inv() { for (var i = 0; i < px.length; i++) px[i] ^= 1; paint(); }
function clr() { px.fill(0); paint(); }
function sh(dx, dy) {
  var n = new Uint8Array(W * H);
  for (var y = 0; y < H; y++) for (var x = 0; x < W; x++) {
    var sx = x - dx, sy = y - dy;
    if (sx >= 0 && sx < W && sy >= 0 && sy < H) n[y * W + x] = px[sy * W + sx];
  }
  px = n; paint();
}
function put(e) {
  var r = cv.getBoundingClientRect();
  var x = Math.floor((e.clientX - r.left) / r.width * W);
  var y = Math.floor((e.clientY - r.top) / r.height * H);
  if (x >= 0 && x < W && y >= 0 && y < H && px[y * W + x] !== penVal) {
    px[y * W + x] = penVal; paint();
  }
}
cv.addEventListener('pointerdown', function (e) { drawing = true; cv.setPointerCapture(e.pointerId); put(e); });
cv.addEventListener('pointermove', function (e) { if (drawing) put(e); });
cv.addEventListener('pointerup', function () { drawing = false; });

// The browser already has a font renderer, so any string becomes a bitmap without a single
// glyph crossing the flash.
function drawText() {
  var s = el('btx').value || '';
  var size = +el('bsz').value || 20;
  var o = document.createElement('canvas'); o.width = W; o.height = H;
  var c = o.getContext('2d');
  c.fillStyle = '#000'; c.fillRect(0, 0, W, H);
  c.fillStyle = '#fff'; c.textAlign = 'center'; c.textBaseline = 'middle';
  c.font = 'bold ' + size + 'px monospace';
  c.fillText(s, W / 2, H / 2, W);
  var d = c.getImageData(0, 0, W, H).data;
  for (var i = 0; i < W * H; i++) px[i] = d[i * 4] > 110 ? 1 : 0;
  paint();
}
function hex(a) {
  var s = '';
  for (var i = 0; i < a.length; i++) s += ('0' + a[i].toString(16).toUpperCase()).slice(-2);
  return s;
}
// The OEM packing: row-major, 6 bytes a row, MSB-first.
function pack() {
  var b = new Uint8Array(NB);
  for (var y = 0; y < H; y++) for (var x = 0; x < W; x++)
    if (px[y * W + x]) b[y * ST + (x >> 3)] |= 0x80 >> (x & 7);
  return b;
}
function load(n) {
  fetch('/api/img?n=' + n).then(function (r) { return r.text(); }).then(function (t) {
    for (var y = 0; y < H; y++) for (var x = 0; x < W; x++) {
      var byteAt = y * ST + (x >> 3);
      var v = parseInt(t.substr(byteAt * 2, 2), 16);
      px[y * W + x] = (v >> (7 - (x & 7))) & 1;
    }
    paint();
    say(true, n + ' loaded into the editor');
  });
}
function sendBmp() {
  fetch('/api/bmp', { method: 'POST', body: hex(pack()) })
    .then(function (r) { return r.text(); })
    .then(function (t) { say(true, 'bitmap: ' + t); poll(); });
}
function scene(n) { cmd('scene', { n: n }); }
function setPeriod() { cmd('period', { ms: el('per').value }); }

// ---- text ------------------------------------------------------------------
function sendText() {
  cmd('text', { t: el('mt').value, scroll: el('msc').checked ? 1 : 0, ms: el('mms').value,
                icon: el('mic').value, src: el('msr').value, fmt: el('mfm').value });
}
function sendPopup() {
  cmd('popup', { t: el('pt').value, icon: el('pic').value, fmt: el('pfm').value });
}
function setTimeNow() {
  var v = el('hh').value;
  if (!/^[0-9][0-9][0-9][0-9]$/.test(v)) { say(false, 'HHMM, four digits'); return; }
  cmd('time', { t: v });
}
function useNow() {
  var d = new Date();
  el('hh').value = ('0' + d.getHours()).slice(-2) + ('0' + d.getMinutes()).slice(-2);
  setTimeNow();
}
var WORDS = ['ROUTE CALCULATED', 'NAVIGATION READY', 'TRAFFIC AHEAD', 'DESTINATION REACHED',
             'NO CD INSERTED', 'SYSTEM UPDATED', 'GPS SIGNAL LOST', 'ARRIVING SHORTLY'];
function pick() { return WORDS[Math.floor(Math.random() * WORDS.length)]; }
function randomFull() { cmd('fullscreen', { a: pick(), b: pick(), c: pick() }); }

// ---- menus -----------------------------------------------------------------
function sendMenu() {
  cmd('menu', { h: el('mh').value, a: el('ma').value, b: el('mb').value, scroll: el('msk').value });
}
function sendMenuN() { cmd('menun', { h: el('nh').value, i: el('ni').value, n: el('nsel').value }); }
function selectItem() { cmd('select', { n: el('nsel').value }); }
function sendConfirm() { cmd('confirm', { h: el('ch').value, a: el('ca').value }); }
function sendRows() {
  cmd('infomenu', { r0: el('r0').value, r1: el('r1').value, r2: el('r2').value,
                    s0: el('s0').checked ? 1 : 0, s1: el('s1').checked ? 1 : 0,
                    s2: el('s2').checked ? 1 : 0, live: el('rlive').checked ? 1 : 0 });
}

// ---- settings --------------------------------------------------------------
function famSet(f) { if (confirm('Reboot into ' + f + '?')) cmd('family', { f: f }); }
function setHold() { cmd('hold', { ms: el('hold').value }); }
function exportWire() {
  fetch('/api/frames').then(function (r) { return r.text(); }).then(function (t) {
    var a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([t], { type: 'text/tab-separated-values' }));
    a.download = 'affa-wire.tsv';
    a.click();
    say(true, 'exported ' + (t.split('\n').length - 1) + ' rows');
  });
}

// ---- status ----------------------------------------------------------------
// SEQUENTIAL, NOT PARALLEL, and that is a bug fix rather than a tidy-up. The Wire tab used
// to fire /api/state, /api/frames and /api/log at the same instant every 1.2 s; a browser
// keeps those connections alive and esp_http_server has seven sockets, so the table filled
// and the next request was REFUSED — the page died with ERR_CONNECTION_REFUSED while the
// board was perfectly healthy and answering curl.
//
// Chaining keeps exactly one request outstanding. It also means a slow response delays the
// next poll instead of stacking another on top of it, which is the behaviour you want from
// a console talking to a microcontroller.
var first = true, polling = false;
function poll() {
  if (polling) return;                 // never overlap two polls either
  polling = true;
  fetch('/api/state').then(function (r) { return r.json(); }).then(function (s) {
    if (first) {
      el('mt').value = s.main; el('msc').checked = s.scroll; el('mms').value = s.mainms;
      el('per').value = s.period;
      for (var i = 0; i < 3; i++) { el('r' + i).value = s.rows[i].t; el('s' + i).checked = s.rows[i].s; }
      el('rlive').checked = s.rowslive;
      first = false;
    }
    el('st').innerHTML =
      '<span class="k">' + s.phase + '</span> &middot; ' +
      (s.live ? '<span class="g">link up</span>' : '<span class="b">LINK DOWN</span>') +
      (s.opened ? '' : ' &middot; <span class="b">NOT OPENED</span>') +
      ' &middot; ' + s.scene + (s.paneon ? '' : ' <span class="w">(pane off)</span>') +
      ' &middot; ' + (s.owner === 'main' ? 'line free' : '<span class="w">screen holds the line</span>') +
      ' &middot; ' + s.ok + '/' + s.frames + ' ok' +
      (s.fail ? ' <span class="b">' + s.fail + ' fail</span>' : '') +
      ' &middot; duty ' + s.duty + '%';
    el('fam').innerHTML = '<small>running <span class="k">' + s.family + '</span>' +
      (s.nav ? '' : ' &mdash; <span class="w">no nav pane</span>') + '</small>';
    el('kc').textContent = s.keys + ' seen';
    el('diag').textContent =
      'family   ' + s.family + '\nphase    ' + s.phase +
      '\nqueued   ' + s.queued + '\nlastslot ' + s.slot +
      '\nframes   ' + s.frames + '  ok ' + s.ok + '  fail ' + s.fail + '  drops ' + s.drops +
      '\nper img  ' + s.avgms + ' ms\nduty     ' + s.duty + '%' +
      '\nheap     ' + s.heap + '\nuptime   ' + s.up + ' s';
  }).then(function () {
    if (cur !== 'wire') return null;
    return fetch('/api/frames').then(function (r) { return r.text(); }).then(function (t) {
      var e = el('fr'); e.textContent = t;
      if (el('wauto').checked) e.scrollTop = e.scrollHeight;
      return fetch('/api/log').then(function (r) { return r.text(); }).then(function (t2) {
        el('log').textContent = t2;
      });
    });
  }).then(function () {
    if (cur !== 'keys') return null;
    return fetch('/api/keys').then(function (r) { return r.json(); }).then(function (a) {
      var b = '';
      for (var i = a.length - 1; i >= 0; i--)
        b += '<tr><td>' + a[i].ms + '</td><td class="k">' + a[i].name +
             '</td><td>0x' + ('000' + a[i].code.toString(16).toUpperCase()).slice(-4) +
             '</td><td>' + a[i].edge + '</td></tr>';
      document.querySelector('#kt tbody').innerHTML = b ||
        '<tr><td colspan="4"><small>nothing yet &mdash; press a key on the stalk</small></td></tr>';
    });
  }).catch(function () {
    el('st').innerHTML = '<span class="b">offline &mdash; retrying</span>';
  }).then(function () { polling = false; });
}
load('tryzub');
poll();
setInterval(poll, 1500);
</script>
</body></html>
)HTML";
