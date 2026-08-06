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
.fl{display:inline-flex;gap:4px;flex-wrap:wrap}
.fl button{padding:2px 7px;font-size:11px}
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
      <button onclick="img('globe')">globe</button>
      <button onclick="img('tryzub')">tryzub</button>
      <button onclick="img('tryzubclock')">tryzub+clock</button>
      <button onclick="img('renault')">renault</button>
    </div>
    <div class="r">
      <button onclick="img('dash')">dash</button>
      <button onclick="img('gauges')">gauges</button>
      <button onclick="img('combo')">combo</button>
      <button onclick="img('fontsheet')">font</button>
      <button onclick="img('checker')">checker</button>
    </div>
    <p><small>
      Goes <b>straight to the glass</b> and into the editor, same as the animation buttons
      below &mdash; the image lives in device flash, so this costs one command, not a 288-byte
      upload. <b>SEND TO PANEL</b> above is for what you have <i>drawn</i>: it posts the
      editor's own bytes.
    </small></p>

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
    <h2 style="margin-top:15px">Icons &mdash; a bitmask, and A SET BIT MEANS OFF</h2>
    <div class="r" id="icb"></div>
    <div class="r">
      <small>icon byte</small><input id="mic" size="4" value="0x55" style="width:64px">
      <small>bank 2</small><input id="mf2" size="4" value="0x55" style="width:64px">
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
      <br><br>
      <b>Chasing an icon that will not go away?</b> The polarity is inverted: every named bit
      is a <span class="k">NO_</span> bit, so <b>ticking a box turns that icon OFF</b>, and
      <span class="k">0x55</span> is simply all four off. Confirmed against 13 OEM frames &mdash;
      the radio sends <span class="k">0x05</span>/<span class="k">0x09</span> on FM and
      <span class="k">0x15</span>/<span class="k">0x55</span> on MW, LW and AUX, so bit 4
      tracks the band exactly as an inverted mask predicts.
      <br><br>
      <b>bit 7</b> is the one the origin never named and that is <b>clear in every value
      ever observed</b>. If a glyph is lit that nothing else turns off, it is the only bit
      left &mdash; try <span class="k">0xD5</span>. <b>bank 2</b> is <i>not</i> a second mask:
      the OEM sends <span class="k">0x55</span> there in 13 of 13 frames across every source,
      so it is a constant of unknown meaning. It is editable only so it can be ruled out.
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
      <button onclick="sendText()">close it with SET TEXT</button>
    </div>
    <p><small>
      <b>There is no hide, and there is no longer one in the library either.</b> A fullscreen
      is not an overlay: the next render replaces it, which is what lets 09_golden animate
      these at about eight a second with nothing sent in between.
      <span class="k">hideFullscreenText()</span> emitted the same
      <span class="k">02 54 03</span> as <span class="k">hidePopup()</span>, so it was a
      second name for one command and it implied a teardown nobody owes. Removed 2026-08-06.
      <br><br>Until the same day, SET TEXT could not close one either &mdash; but that was
      this console's bug, not the panel's: the repaint gate that stops a scroll tick from
      painting over a screen you are reading was also swallowing the deliberate press. An
      explicit SET TEXT now takes the line back immediately.
    </small></p>
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
      <select id="msk"><option value="0">none 00</option><option value="7">up 07</option><option value="11">down 0B</option><option value="3">both 03</option></select>
      <button class="p" onclick="sendMenu()">showMenu</button>
    </div>
    <p><small>
      <b>Every value here draws nothing on a two-row menu, and that is the panel being
      right.</b> This screen sends <span class="k">[6] = 0x82</span> &mdash; two items in a
      two-row viewport &mdash; so there is nothing to scroll to and no arrow to draw. Use the
      <b>N-item list</b> below to see this byte work; every OEM capture with an arrow is a
      4- or 6-item list.
      <br><br>The library said <b>both</b> was <span class="k">0x0C</span> until 2026-08-06.
      That came from the origin's hand-written constant and appears in <b>no capture</b>; the
      OEM sends <span class="k">0x03</span>. The high bits read as suppressors &mdash;
      <span class="k">0x03|0x08 = 0x0B</span> at the top of a list, <span class="k">0x03|0x04
      = 0x07</span> at the bottom &mdash; which would make <span class="k">0x0C</span>
      "suppress both", the exact opposite of its name.
    </small></p>
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
    <div class="r">
      <small>scroll mask</small>
      <select id="nsk"><option value="0">none 00</option><option value="7">up 07</option><option value="11">down 0B</option><option value="3" selected>both 03</option></select>
      <small>&larr; this is the screen where it shows</small>
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

    <h2 style="margin-top:15px">Message box &mdash; 0, 1 or 2 buttons</h2>
    <div class="r">
      <input id="ca" size="14" value="DELETE ENTRY?"><input id="cb" size="10" placeholder="second line">
    </div>
    <div class="r">
      <small>buttons</small>
      <select id="cbn" onchange="cLabels()">
        <option value="0">0 &mdash; message only</option>
        <option value="1" selected>1 &mdash; OK</option>
        <option value="2">2 &mdash; Yes / No</option>
      </select>
      <span id="clb">
        <input id="cl0" size="6" maxlength="6" value="OK">
        <input id="cl1" size="6" maxlength="6" value="NO">
      </span>
    </div>
    <div class="r">
      <button class="p" onclick="sendConfirm()">show box</button>
      <button onclick="cmd('boxsel',{n:0})">select 0</button>
      <button onclick="cmd('boxsel',{n:1})">select 1</button>
    </div>
    <p><small>
      The button count is payload byte <span class="k">[4]</span> and it is a real field, not
      a constant &mdash; it was hard-coded to 1 until 2026-08-06, which is why this only ever
      drew an OK box. Both lengths follow the count, three-for-three across OEM captures with
      0, 1 and 2 buttons: <span class="k">declared = 105 + 6&times;buttons</span> and
      <span class="k">body = 32 + 6&times;buttons</span>. Labels are <b>six bytes,
      NUL-padded</b> &mdash; the OEM's own box carries <span class="k">"Yes"</span> and
      <span class="k">"No"</span> in exactly those fields.
      <br><br>The two-button form is <b>119 wire bytes and wraps the ISO-TP counter</b> from
      <span class="k">0x2F</span> to <span class="k">0x20</span>. That is what the OEM does
      and the panel ACKs it &mdash; and it is what the nav pane has done 24 912 times here.
      <b>select</b> sends <span class="k">03 29 05 &lt;n&gt;</span>, a three-byte single
      frame; on a one-button box only index 0 exists.
      <br><br><b>There is no close button, and that is not an omission.</b> A box is replaced
      by the next render &mdash; draw a menu, or press SET TEXT on the Text tab. Only the
      popup needs a close command, because only the popup is a true overlay.
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
    <div class="r"><small>id</small><span id="fid" class="fl"></span>
      <button onclick="wAll('id',1)">all</button><button onclick="wAll('id',0)">none</button></div>
    <div class="r"><small>byte&nbsp;0</small><span id="fb0" class="fl"></span>
      <button onclick="wAll('b0',1)">all</button><button onclick="wAll('b0',0)">none</button></div>
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
      <br><br><b>This switch did nothing until 2026-08-06.</b> It stored the choice and
      rebooted correctly &mdash; but the firmware was built with Carminat only, so the
      UpdateList branch did not exist and boot reset the family every time. The board came
      back on Carminat and the console reported success. <b>Both families are compiled in
      now</b>, so the choice survives. The header line above says which one is actually
      running; trust that, not this button.
    </small></p>

    <h2 style="margin-top:15px">Go back to the scrolling line by itself, after</h2>
    <div class="r">
      <small>wait</small><input id="hold" type="number" value="0" style="width:80px">
      <small>ms, then resume scrolling &mdash; <b>0 = never, stay put</b></small>
      <button onclick="setHold()">set</button>
    </div>
    <p><small>
      <b>In one sentence: how long a menu or a box stays up before the scrolling text takes
      the screen back on its own.</b>
      <br><br>Here is the problem it exists for. The main line scrolls, which means it
      repaints every few hundred milliseconds. A repaint replaces whatever is on the glass.
      So if you open a menu and walk away, the very next scroll tick would wipe it &mdash;
      a screen closing itself while you are reading it. This console therefore <b>stops the
      scroll entirely</b> whenever a menu, box or fullscreen is showing, and the line stays
      quiet until something takes it back.
      <br><br><b>0 means it waits for ever</b>, which is the default because that is the
      safe answer: nothing on the glass ever disappears on a timer. Set 5000 and a menu
      lingers five seconds after you stop touching it, then the text resumes by itself.
      Either way <b>SET TEXT takes the line back immediately</b> &mdash; you never have to
      wait for this timer to get your screen back.
      <br><br>It knows which screen is up from the library's own record of the last
      acknowledged render (<span class="k">lastRendered()</span>) rather than from a flag
      this example keeps, so it cannot drift out of step with what was actually drawn.
    </small></p>

    <h2 style="margin-top:15px">Board</h2>
    <div class="r">
      <button onclick="location='/update'">OTA update</button>
      <button class="d" onclick="cmd('reboot')">reboot</button>
    </div>
    <p><small>
      <b>OTA reflashes this board over WiFi &mdash; no cable.</b> The button opens
      <span class="k">/update</span>; pick the firmware at
      <span class="k">.pio/build/ex17_mediascreen/firmware.bin</span> and upload. CAN
      transmit is gated off while it runs and the board reboots into the new build by itself.
      <br><br>Two things worth knowing, both of which have cost a cable here before: the OTA
      route is registered <b>first</b>, because PsychicHttp's URI table silently drops
      handlers once it is full and <span class="k">/ota/upload</span> is the one you cannot
      afford to lose; and a board that answers ping and mDNS but refuses HTTP has usually
      exhausted its socket table rather than crashed &mdash; wait, then retry.
    </small></p>
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
// "0xNN" — the form the selects and the byte fields both use, so a value read back from
// /api/state matches an <option value> exactly and the select actually selects.
function hx(v) { return '0x' + ('0' + (v & 255).toString(16).toUpperCase()).slice(-2); }
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
  return fetch('/api/img?n=' + n).then(function (r) { return r.text(); }).then(function (t) {
    for (var y = 0; y < H; y++) for (var x = 0; x < W; x++) {
      var byteAt = y * ST + (x >> 3);
      var v = parseInt(t.substr(byteAt * 2, 2), 16);
      px[y * W + x] = (v >> (7 - (x & 7))) & 1;
    }
    paint();
  });
}
// An image button now does what an animation button does: PUTS IT ON THE GLASS. It used to
// only fill the editor, so the two rows of buttons sat next to each other looking alike and
// behaving differently, and the image ones appeared to do nothing at all.
//
// It sends via `scene`, not by posting the 288 bytes back: every one of these images is
// already in device flash, so the round trip is one command instead of a 576-character
// upload of bytes the board is holding anyway. The editor is loaded in parallel, for
// drawing on top of.
function img(n) { load(n); cmd('scene', { n: n }); }
function sendBmp() {
  fetch('/api/bmp', { method: 'POST', body: hex(pack()) })
    .then(function (r) { return r.text(); })
    .then(function (t) { say(true, 'bitmap: ' + t); poll(); });
}
function scene(n) { cmd('scene', { n: n }); }
function setPeriod() { cmd('period', { ms: el('per').value }); }

// ---- text ------------------------------------------------------------------
// The icon byte, as the bits it actually is. Every named bit is a NO_ bit — [REF]
// affa3.h:39-51 — so the checkbox label is what the tick TURNS OFF. Bit 7 is unnamed in the
// origin and clear in every observed value, which is why it is offered without a name.
var ICONBITS = [
  [0x01, 'no NEWS'], [0x02, 'NEWS arrow'], [0x04, 'no TRAFFIC'], [0x08, 'TRAFFIC arrow'],
  [0x10, 'no AF-RDS'], [0x20, 'AF-RDS arrow'], [0x40, 'no MODE'], [0x80, 'bit7 unknown']
];
function iconByte() { return parseInt(el('mic').value, 16) || 0; }
function iconChips() {
  var v = iconByte(), h = '', i;
  for (i = 0; i < ICONBITS.length; i++)
    h += '<button class="' + ((v & ICONBITS[i][0]) ? 'p' : '') + '" onclick="iconTog(' +
         ICONBITS[i][0] + ')">' + ICONBITS[i][1] + '</button> ';
  el('icb').innerHTML = h;
}
function iconTog(bit) {
  var v = iconByte() ^ bit;
  el('mic').value = '0x' + ('0' + v.toString(16).toUpperCase()).slice(-2);
  iconChips();
  sendText();
}
function sendText() {
  iconChips();
  cmd('text', { t: el('mt').value, scroll: el('msc').checked ? 1 : 0, ms: el('mms').value,
                icon: el('mic').value, src: el('msr').value, fmt: el('mfm').value,
                fmt2: el('mf2').value });
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
function sendMenuN() {
  cmd('menun', { h: el('nh').value, i: el('ni').value, n: el('nsel').value,
                 scroll: el('nsk').value });
}
function selectItem() { cmd('select', { n: el('nsel').value }); }
// Only as many label fields as there are buttons, so the form cannot ask for a label the
// wire has nowhere to put.
function cLabels() {
  var n = +el('cbn').value;
  el('cl0').className = n > 0 ? '' : 'hide';
  el('cl1').className = n > 1 ? '' : 'hide';
}
function sendConfirm() {
  cmd('confirm', { a: el('ca').value, b: el('cb').value, btn: el('cbn').value,
                   l0: el('cl0').value, l1: el('cl1').value, sel: 0 });
}
function sendRows() {
  cmd('infomenu', { r0: el('r0').value, r1: el('r1').value, r2: el('r2').value,
                    s0: el('s0').checked ? 1 : 0, s1: el('s1').checked ? 1 : 0,
                    s2: el('s2').checked ? 1 : 0, live: el('rlive').checked ? 1 : 0 });
}

// ---- settings --------------------------------------------------------------
function famSet(f) { if (confirm('Reboot into ' + f + '?')) cmd('family', { f: f }); }
function setHold() { cmd('hold', { ms: el('hold').value }); }
// ---- wire filter -----------------------------------------------------------
// THE OPTIONS ARE BUILT FROM WHAT HAS ACTUALLY BEEN SEEN, never from a hard-coded list of
// ids. Half the point of this tab is catching a frame nobody expected, and a fixed filter
// list would hide exactly that frame. Anything newly seen enters CHECKED, so the filter can
// only ever be something you narrowed on purpose — it never silently swallows a new id.
//
// Two independent axes, because that is how a question about this bus is actually asked:
// the id says WHO, and byte 0 says WHAT (0x61 a request, 0x70 a registration, 0x10/0x21 an
// ISO-TP first/continuation frame). Filtering on both is what separates one screen's
// transfer from the handshake running underneath it.
var wSeen = { id: {}, b0: {} }, wRaw = '';

function wKeys(g) {
  var k = [];
  for (var v in wSeen[g]) k.push(v);
  k.sort();
  return k;
}
function wAll(g, on) {
  var k = wKeys(g);
  for (var i = 0; i < k.length; i++) wSeen[g][k[i]] = !!on;
  wChips(); wRender();
}
function wTog(g, v) { wSeen[g][v] = !wSeen[g][v]; wChips(); wRender(); }
function wChips() {
  var g, i, k, h;
  var groups = ['id', 'b0'];
  for (g = 0; g < groups.length; g++) {
    k = wKeys(groups[g]); h = '';
    for (i = 0; i < k.length; i++)
      h += '<button class="' + (wSeen[groups[g]][k[i]] ? 'p' : '') + '" onclick="wTog(\'' +
           groups[g] + '\',\'' + k[i] + '\')">' + k[i] + '</button> ';
    el(groups[g] === 'id' ? 'fid' : 'fb0').innerHTML = h ||
      '<small>nothing seen yet</small>';
  }
}
// Returns the rows that survive the filter, header included.
function wRows() {
  var lines = wRaw.split('\n'), out = [], i, c, id, b0;
  for (i = 0; i < lines.length; i++) {
    if (!lines[i]) continue;
    c = lines[i].split('\t');
    if (c.length < 4 || c[0] === 'ms') { out.push(lines[i]); continue; }
    id = c[2];
    b0 = (c[3] || '').trim().split(' ')[0] || '--';
    if (wSeen.id[id] && wSeen.b0[b0]) out.push(lines[i]);
  }
  return out;
}
function wRender() {
  var e = el('fr');
  e.textContent = wRows().join('\n');
  if (el('wauto').checked) e.scrollTop = e.scrollHeight;
}
// Learn any id / byte-0 the ring is carrying, then redraw. Called on every wire poll.
function wLearn(t) {
  var lines = t.split('\n'), i, c, id, b0, fresh = false;
  wRaw = t;
  for (i = 0; i < lines.length; i++) {
    if (!lines[i]) continue;
    c = lines[i].split('\t');
    if (c.length < 4 || c[0] === 'ms') continue;
    id = c[2];
    b0 = (c[3] || '').trim().split(' ')[0] || '--';
    if (wSeen.id[id] === undefined) { wSeen.id[id] = true; fresh = true; }
    if (wSeen.b0[b0] === undefined) { wSeen.b0[b0] = true; fresh = true; }
  }
  if (fresh) wChips();
  wRender();
}
function exportWire() {
  // Exports WHAT YOU ARE LOOKING AT. Exporting the unfiltered ring from a filtered view is
  // how a .tsv ends up disagreeing with the screenshot taken beside it.
  var rows = wRows(), t = rows.join('\n') + '\n';
  var a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([t], { type: 'text/tab-separated-values' }));
  a.download = 'affa-wire.tsv';
  a.click();
  say(true, 'exported ' + (rows.length - 1) + ' rows');
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
      // ADOPT THE BOARD'S HEADER BYTES rather than leaving the form on its HTML defaults.
      // The page used to show "none 55" while the board held something else entirely, so
      // the first SET TEXT silently reverted whatever was actually on the glass.
      el('mic').value = hx(s.icon);
      el('mf2').value = hx(s.fmt2);
      el('msr').value = hx(s.src);
      el('mfm').value = hx(s.fmt);
      iconChips();
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
      wLearn(t);
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
cLabels();
iconChips();
poll();
setInterval(poll, 1500);
</script>
</body></html>
)HTML";
