// web_ui.h — the console for 17_mediascreen.
//
// It mirrors the pane rather than describing it: the same 48x48 is drawn in the browser from
// the same state, so what the glass is doing can be checked without standing over the car.
#pragma once

static const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AffaMedia</title>
<style>
:root{--bg:#0f1115;--fg:#e8eaf0;--dim:#868ea3;--line:#262b38;--acc:#5ac8fa;--ok:#4ade80;--warn:#ffb020;--bad:#ff5c5c}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.5 ui-monospace,Menlo,Consolas,monospace}
header{padding:12px 16px;border-bottom:1px solid var(--line);display:flex;gap:18px;align-items:baseline;flex-wrap:wrap}
h1{font-size:14px;margin:0;letter-spacing:.1em}
#st{color:var(--dim);font-size:12px}
main{display:flex;gap:18px;padding:16px;flex-wrap:wrap;align-items:flex-start}
section{border:1px solid var(--line);border-radius:8px;padding:14px;min-width:290px}
h2{font-size:11px;margin:0 0 12px;color:var(--dim);letter-spacing:.14em;text-transform:uppercase}
canvas{image-rendering:pixelated;background:#000;border:1px solid var(--line);display:block}
button{background:#1a1f2b;color:var(--fg);border:1px solid var(--line);border-radius:5px;padding:6px 11px;font:inherit;cursor:pointer}
button:hover{border-color:var(--acc)}
button.pri{background:#123a4f;border-color:var(--acc);color:#d6f1ff}
input{background:#0b0d12;color:var(--fg);border:1px solid var(--line);border-radius:5px;padding:5px 7px;font:inherit}
.row{display:flex;gap:7px;flex-wrap:wrap;align-items:center;margin-bottom:9px}
pre{background:#080a0e;border:1px solid var(--line);border-radius:5px;padding:9px;margin:0;max-height:280px;overflow:auto;font-size:11px;color:var(--dim)}
.k{color:var(--acc)}.g{color:var(--ok)}.w{color:var(--warn)}.r{color:var(--bad)}
small{color:var(--dim)}
</style></head><body>

<header>
  <h1>AFFA<span class="k">MEDIA</span></h1>
  <div id="st">connecting&hellip;</div>
</header>

<main>

<section>
  <h2>Scene</h2>
  <div class="row">
    <button onclick="sc('spectrum')">spectrum</button>
    <button onclick="sc('vu')">VU</button>
    <button onclick="sc('wave')">wave</button>
    <button onclick="sc('clock')">clock</button>
    <button onclick="sc('stars')">stars</button>
    <button onclick="sc('bounce')">bounce</button>
    <button onclick="sc('rings')">rings</button>
  </div>
  <div class="row">
    <button onclick="sc('globe')">globe</button>
    <button onclick="sc('tryzub')">tryzub</button>
    <button onclick="sc('tryzubclock')">tryzub+clock</button>
    <button onclick="sc('renault')">renault</button>
    <button onclick="sc('dash')">dash</button>
    <button onclick="sc('gauges')">gauges</button>
    <button onclick="sc('combo')">combo</button>
    <button onclick="sc('fontsheet')">font</button>
    <button onclick="sc('checker')">checker</button>
  </div>
  <div class="row">
    <button onclick="fetch('/api/scene?on=1').then(poll)">resume sending</button>
    <button onclick="fetch('/api/scene?on=0').then(poll)">stop sending</button>
    <button onclick="sc('blank')">blank the pane</button>
    <button class="pri" onclick="fetch('/api/opening').then(poll)">replay OEM opening</button>
  </div>
  <p><small>
    The first seven are drawn on the device from a frame counter; the rest are the generated
    48&times;48 set, shared with <span class="k">16_navlab</span>. Same channel either way
    &mdash; which is the contrast worth showing.
  </small></p>
  <p><small>
    <b>stop sending</b> halts the frame pump. <span class="w">It does not clear the glass</span>
    &mdash; the last image stays there, because no command to erase this pane is known. To
    empty it, send <b>blank</b>: 288 zero bytes, the only way we have found.
    <b>display OFF</b> is a different thing again &mdash; that is the backlight.
    <br><br>Choosing any scene <b>resumes sending and pushes one frame immediately</b>, so a
    still image reaches the glass at once. Identical frames are then <b>not resent</b> &mdash;
    a static scene costs one transfer and then nothing, which is why <b>duty</b> falls to zero
    on a logo and sits near 25% on the spectrum.
  </small></p>
</section>

<section>
  <h2>The pane &mdash; mirrored from the same state</h2>
  <canvas id="cv" width="48" height="48" style="width:288px;height:288px"></canvas>
  <p><small>
    The browser redraws this from <span class="k">/api/state</span>, so the spectrum will not
    match the glass frame for frame &mdash; the clock, the progress bar and the transport
    glyph will.
  </small></p>
</section>

<section>
  <h2>Now playing</h2>
  <div class="row"><small>title</small><input id="ti" size="24" value=""></div>
  <div class="row"><small>artist</small><input id="ar" size="18" value=""></div>
  <div class="row">
    <small>track</small><input id="tk" type="number" style="width:56px">
    <small>of</small><input id="tc" type="number" style="width:56px">
    <small>len s</small><input id="tt" type="number" style="width:66px">
  </div>
  <div class="row">
    <button class="pri" onclick="apply()">apply</button>
    <button onclick="send('play=1')">&#9654; play</button>
    <button onclick="send('play=0')">&#9646;&#9646; pause</button>
    <button onclick="send('elapsed=0')">&#9664;&#9664; restart</button>
  </div>
  <div class="row">
    <button onclick="call('power_on')" title="151 03 52 09 00">display ON</button>
    <button onclick="call('power_off')" title="151 03 52 00 00">display OFF</button>
  </div>
  <p><small>
    <b>display ON/OFF</b> is the panel's backlight — <span class="k">52 09 00</span> /
    <span class="k">52 00 00</span>. Nothing draws at all while it is off. Not the same thing
    as <b>pane</b> above, which only stops us sending images.
  </small></p>
</section>

<section>
  <h2>Every render call</h2>
  <div class="row">
    <input id="ca" value="AFFA" size="9"><input id="cb" value="ROW ONE" size="9"><input id="cc" value="ROW TWO" size="9">
    <small>n</small><input id="cn" type="number" value="0" style="width:52px">
  </div>
  <div class="row">
    <button onclick="call('text')">setText</button>
    <button onclick="call('power_on')">power ON</button>
    <button onclick="call('power_off')">power OFF</button>
  </div>
  <div class="row">
    <small>time HHMM</small>
    <input id="tmv" value="1056" size="5" maxlength="4" style="width:64px">
    <button class="pri" onclick="callA('time',tmv.value)">setTime</button>
    <button onclick="useNow()">use browser clock</button>
    <label style="margin-left:4px"><input type="checkbox" id="tks" onchange="tick()"> keep it ticking</label>
  </div>
  <div class="row">
    <button onclick="call('menu')">showMenu</button>
    <button onclick="call('menun')">showMenuN</button>
    <button onclick="call('hilite')">highlightItem n</button>
    <button onclick="call('select')">selectMenuItem n</button>
  </div>
  <div class="row">
    <button onclick="call('infomenu')">showInfoMenu</button>
    <button onclick="call('infopopup')">showInfoPopup</button>
    <button onclick="call('infohide')">hideInfoPopup</button>
  </div>
  <div class="row">
    <button onclick="call('popup')">showPopupText</button>
    <button onclick="call('pophide')">hidePopup</button>
    <button onclick="call('fullscreen')">showFullscreen</button>
    <button onclick="call('fshide')">hideFullscreen</button>
  </div>
  <div class="row">
    <button onclick="call('confirm')">showConfirmBox</button>
    <button onclick="call('navtick')">navTick n</button>
  </div>
  <div class="row"><small>menuN items</small><input id="ci" value="DESTINATION|ROUTE|MAP|TRAFFIC|SETTINGS|BACK" style="flex:1;min-width:200px"></div>
  <p><small>
    <b>showMenuN</b> tracks all six items but the glass is a <b>two-row viewport</b> &mdash;
    send it once, then move the selection with <b>selectMenuItem</b> (eight bytes) and the
    panel scrolls itself.
  </small></p>
</section>

<section>
  <h2>Running text</h2>
  <div class="row">
    <label><input type="checkbox" id="mt" onchange="send('mqtitle='+(mt.checked?1:0))"> marquee the title</label>
    <small>ms</small><input id="mtm" type="number" value="700" style="width:64px" onchange="send('titlems='+mtm.value)">
  </div>
  <div class="row">
    <label><input type="checkbox" id="mr" onchange="send('mqrows='+(mr.checked?1:0))"> marquee info rows</label>
    <label><input type="checkbox" id="rl" onchange="send('rowslive='+(rl.checked?1:0))"> keep repainting</label>
    <small>ms</small><input id="mrm" type="number" value="700" style="width:64px" onchange="send('rowms='+mrm.value)">
  </div>
  <div class="row"><input id="r0" size="20" value="DESTINATION MEMORY"><button onclick="send('row0='+encodeURIComponent(r0.value))">row 0</button></div>
  <div class="row"><input id="r1" size="20" value="TRAFFIC INFORMATION"><button onclick="send('row1='+encodeURIComponent(r1.value))">row 1</button></div>
  <div class="row"><input id="r2" size="20" value="SYSTEM SETTINGS"><button onclick="send('row2='+encodeURIComponent(r2.value))">row 2</button></div>
  <p><small>
    The main line shows <b>8 characters</b> and an info-menu row shows <b>8</b> &mdash;
    measured with the ruler, not assumed. Anything longer has to scroll, which is what this
    is. <span class="w">Three rows repainting on a timer is a lot of traffic next to one main
    line</span>, so it is off by default.
  </small></p>
</section>

<section>
  <h2>Panel family &mdash; a BOOT choice</h2>
  <div class="row">
    <span id="fam"><small>&nbsp;</small></span>
    <button onclick="if(confirm('Reboot into Carminat?'))location='/api/family?f=carminat'">Carminat</button>
    <button onclick="if(confirm('Reboot into UpdateList?'))location='/api/family?f=updatelist'">UpdateList</button>
  </div>
  <p><small>
    Carminat syncs on <span class="k">0x3AF</span>, UpdateList on <span class="k">0x3DF</span>,
    and the handshake, the ids and the text encoding all differ. There is no honest runtime
    switch, so the choice is stored in NVS and applied on the next boot. The nav pane and the
    N-item menu are <b>Carminat only</b>.
  </small></p>
</section>

<section>
  <h2>Bus budget</h2>
  <div class="row">
    <small>frame ms</small><input id="pm" type="number" value="250" style="width:70px">
    <small>title ms</small><input id="tm" type="number" value="700" style="width:70px">
    <button onclick="send('period='+pm.value+'&titlems='+tm.value)">set</button>
  </div>
  <div class="row">
    <button onclick="pm.value=120;send('period=120')">120 &mdash; hard</button>
    <button onclick="pm.value=250;send('period=250')">250 &mdash; default</button>
    <button onclick="pm.value=478;send('period=478')">478 &mdash; OEM</button>
    <button onclick="pm.value=1000;send('period=1000')">1000 &mdash; idle</button>
  </div>
  <pre id="bud">&nbsp;</pre>
  <p><small>
    One image is 304 wire bytes = 44 CAN frames, and the panel runs ISO-TP
    <span class="k">BlockSize 1</span>, so every consecutive frame costs a round trip &mdash;
    47&ndash;54 ms measured. <b>duty</b> is how much of the link the images are actually
    taking; <b>drops</b> counts frames skipped because the previous one was still in flight,
    which is the honest signal that the period is too short.
  </small></p>
</section>

<section style="flex:1;min-width:320px">
  <h2>Log</h2>
  <pre id="log">&nbsp;</pre>
</section>

</main>

<script>
const cv=document.getElementById('cv'), cx=cv.getContext('2d');
let S={elapsed:0,total:1,track:1,tracks:1,playing:false};
let bars=[4,9,14,11,7,12,6,3], peak=[0,0,0,0,0,0,0,0], rng=0x1F1A5EED;
const W=48,H=48;
let px=new Uint8Array(W*H);

const P=(x,y)=>{if(x>=0&&x<W&&y>=0&&y<H)px[y*W+x]=1;};
const HL=(a,b,y)=>{for(let x=a;x<=b;x++)P(x,y);};
const VL=(x,a,b)=>{for(let y=a;y<=b;y++)P(x,y);};
const RC=(x0,y0,x1,y1,f)=>{if(f){for(let y=y0;y<=y1;y++)HL(x0,x1,y);}else{HL(x0,x1,y0);HL(x0,x1,y1);VL(x0,y0,y1);VL(x1,y0,y1);}};
// Same 5x7 table as media_render.h — the mirror is only useful if the glyphs match.
const D=[[0x70,0x88,0x98,0xA8,0xC8,0x88,0x70],[0x20,0x60,0x20,0x20,0x20,0x20,0x70],
[0x70,0x88,0x08,0x10,0x20,0x40,0xF8],[0x70,0x88,0x08,0x30,0x08,0x88,0x70],
[0x10,0x30,0x50,0x90,0xF8,0x10,0x10],[0xF8,0x80,0xF0,0x08,0x08,0x88,0x70],
[0x30,0x40,0x80,0xF0,0x88,0x88,0x70],[0xF8,0x08,0x10,0x20,0x40,0x40,0x40],
[0x70,0x88,0x88,0x70,0x88,0x88,0x70],[0x70,0x88,0x88,0x78,0x08,0x10,0x60]];
const COL=[0x00,0x20,0x20,0x00,0x20,0x20,0x00], SL=[0x08,0x08,0x10,0x20,0x40,0x80,0x80];
const G=(g,x,y)=>{for(let r=0;r<7;r++)for(let c=0;c<5;c++)if(g[r]&(0x80>>c))P(x+c,y+r);};
function T(x,y,s){const m=Math.floor(s/60)%100,sec=s%60;let a=x;
  if(m>=10){G(D[Math.floor(m/10)],a,y);a+=6;} G(D[m%10],a,y);a+=6; G(COL,a,y);a+=4;
  G(D[Math.floor(sec/10)],a,y);a+=6; G(D[sec%10],a,y);}
function TR(x,y,n,t){let a=x;G(D[Math.floor(n/10)%10],a,y);a+=6;G(D[n%10],a,y);a+=6;
  G(SL,a,y);a+=6;G(D[Math.floor(t/10)%10],a,y);a+=6;G(D[t%10],a,y);}

function stepBars(){
  const nx=()=>{rng^=rng<<13;rng>>>=0;rng^=rng>>>17;rng^=rng<<5;rng>>>=0;return rng;};
  for(let i=0;i<8;i++){
    if(S.playing){
      const pull=i>0?Math.trunc((bars[i-1]-bars[i])/3):0, kick=(nx()%9)-4;
      bars[i]=Math.max(1,Math.min(21,bars[i]+pull+kick));
    } else if(bars[i]>1) bars[i]--;
    if(bars[i]>peak[i]) peak[i]=bars[i]; else if(peak[i]>0&&(nx()&3)===0) peak[i]--;
  }
}
function draw(){
  px.fill(0);
  T(0,0,S.elapsed);
  T(26,0,Math.max(0,S.total-S.elapsed));
  const base=30;
  for(let i=0;i<8;i++){
    const x0=i*6,x1=x0+4,top=base-bars[i];
    RC(x0,top,x1,base,true);
    const pk=base-peak[i]-2;
    if(pk>=9&&pk<top-1) HL(x0,x1,pk);
  }
  RC(0,33,47,36,false);
  if(S.total){const f=Math.floor(46*Math.min(S.elapsed,S.total)/S.total); if(f)RC(1,34,1+f,35,true);}
  if(S.playing){for(let r=0;r<7;r++){const w=4-(r>3?r-3:3-r);for(let c=0;c<=w;c++)P(1+c,39+r);}}
  else {RC(1,39,2,45,true);RC(5,39,6,45,true);}
  TR(18,39,S.track,S.tracks);
  const im=cx.createImageData(W,H);
  for(let i=0;i<W*H;i++){const v=px[i]?255:0;im.data[i*4]=v;im.data[i*4+1]=v;im.data[i*4+2]=v;im.data[i*4+3]=255;}
  cx.putImageData(im,0,0);
}

const send=q=>fetch('/api/player?'+q).then(poll);
function apply(){
  send('title='+encodeURIComponent(ti.value)+'&artist='+encodeURIComponent(ar.value)+
       '&track='+tk.value+'&tracks='+tc.value+'&total='+tt.value);
}
function sc(n){fetch('/api/scene?n='+n).then(poll);}
// setTime wants exactly four digits; anything else is a silent no-op on the panel, so it is
// rejected here where it can be seen instead.
function callA(w,a){
  if(w==='time'&&!/^d{4}$/.test(a)){alert('HHMM — four digits');return;}
  const q=new URLSearchParams({w,a,b:cb.value,c:cc.value,n:cn.value,i:ci.value});
  fetch('/api/call?'+q).then(async r=>{
    const t=await r.text();
    document.getElementById('bud').insertAdjacentHTML('afterbegin',
      '<span class="'+(r.ok?'g':'r')+'">'+t+'</span>
');
  }).then(poll);
}
function hhmm(){const d=new Date();
  return String(d.getHours()).padStart(2,'0')+String(d.getMinutes()).padStart(2,'0');}
function useNow(){tmv.value=hhmm();callA('time',tmv.value);}
// The panel has no clock of its own on this family — the radio owns it, so it only stays
// right if something keeps sending it. Once a minute is enough and costs one short frame.
let tkTimer=null;
function tick(){
  if(tkTimer){clearInterval(tkTimer);tkTimer=null;}
  if(tks.checked){useNow();tkTimer=setInterval(useNow,60000);}
}
function call(w){
  const q=new URLSearchParams({w,a:ca.value,b:cb.value,c:cc.value,n:cn.value,i:ci.value});
  fetch('/api/call?'+q).then(async r=>{
    const t=await r.text();
    const el=document.getElementById('bud');
    el.insertAdjacentHTML('afterbegin','<span class="'+(r.ok?'g':'r')+'">'+t+'</span>\n');
  }).then(poll);
}
let first=true;
async function poll(){
  try{
    const s=await (await fetch('/api/state')).json();
    S=s;
    if(first){ti.value=s.title;ar.value=s.artist;tk.value=s.track;tc.value=s.tracks;
             tt.value=s.total;pm.value=s.periodms;
             mt.checked=s.mqtitle;mr.checked=s.mqrows;rl.checked=s.rowslive;first=false;}
    document.getElementById('fam').innerHTML='<small>running <span class="k">'+s.family+
      '</span>'+(s.nav?'':' &mdash; <span class="w">no nav pane</span>')+'</small>';
    document.getElementById('st').innerHTML=
      'phase <span class="k">'+s.phase+'</span> &middot; '+
      (s.live?'<span class="g">link up</span>':'<span class="r">LINK DOWN</span>')+
      ' &middot; '+(s.opened?'':'<span class="r">NOT OPENED</span> &middot; ')+
      '<span class="k">'+s.scene+'</span>'+(s.paneon?'':' <span class="w">(pane off)</span>')+
      ' &middot; '+(s.playing?'<span class="g">&#9654; playing</span>':'&#9646;&#9646; paused')+
      ' &middot; heap '+s.heap;
    document.getElementById('bud').innerHTML=
      'frames  '+s.frames+'   ok <span class="g">'+s.ok+'</span>   fail <span class="'+(s.fail?'r':'')+'">'+s.fail+'</span>\n'+
      'drops   <span class="'+(s.drops?'w':'')+'">'+s.drops+'</span>   (previous image still in flight)\n'+
      'per img '+s.avgms+' ms\n'+
      'duty    <span class="'+(s.duty>60?'r':s.duty>35?'w':'g')+'">'+s.duty+'%</span> of the link\n'+
      'period  '+s.periodms+' ms';
    document.getElementById('log').textContent=await (await fetch('/api/log')).text();
  }catch(e){document.getElementById('st').innerHTML='<span class="r">offline</span>';}
}
setInterval(()=>{stepBars();draw();},250);
setInterval(poll,1000);
poll();draw();
</script>
</body></html>
)HTML";
