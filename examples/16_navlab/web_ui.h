// web_ui.h — the console for 16_navlab. One page, no CDN, no build step.
//
// The bitmap lives in the BROWSER, not on the board: the canvas is the source of truth and
// /api/nav receives header and bitmap together on every send. That is what makes the text
// tool free — the browser already has a font renderer, so any string can be thresholded
// into 48x48 and sent without a single glyph crossing the flash.
#pragma once

static const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>navlab &mdash; Carminat 0x1F1</title>
<style>
:root{--bg:#12141a;--fg:#e6e8ee;--dim:#8b93a7;--line:#2a2f3d;--acc:#5ac8fa;--warn:#ffb020;--bad:#ff5c5c;--ok:#4ade80}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.45 ui-monospace,Menlo,Consolas,monospace}
header{padding:10px 14px;border-bottom:1px solid var(--line);display:flex;gap:16px;align-items:baseline;flex-wrap:wrap}
h1{font-size:14px;margin:0;letter-spacing:.06em}
#state{color:var(--dim);font-size:12px}
main{display:flex;gap:16px;padding:14px;flex-wrap:wrap;align-items:flex-start}
section{border:1px solid var(--line);border-radius:6px;padding:12px;min-width:300px}
h2{font-size:11px;margin:0 0 10px;color:var(--dim);letter-spacing:.12em;text-transform:uppercase}
canvas{image-rendering:pixelated;background:#000;border:1px solid var(--line);cursor:crosshair;touch-action:none}
button{background:#1d212b;color:var(--fg);border:1px solid var(--line);border-radius:4px;padding:5px 9px;font:inherit;cursor:pointer}
button:hover{border-color:var(--acc)}
button.pri{background:#14344a;border-color:var(--acc);color:#cdeeff}
button.warn{border-color:var(--warn);color:var(--warn)}
input,select{background:#0e1015;color:var(--fg);border:1px solid var(--line);border-radius:4px;padding:4px 6px;font:inherit}
.row{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin-bottom:8px}
.hdr{display:grid;grid-template-columns:repeat(7,1fr);gap:4px}
.hdr div{text-align:center}
.hdr label{display:block;font-size:9px;color:var(--dim);white-space:nowrap;overflow:hidden}
.hdr input{width:100%;text-align:center;padding:3px 0}
.hdr input.known{border-color:#2f6d4a}
.hdr input.str{border-color:#6d5a2f}
pre{background:#0b0d12;border:1px solid var(--line);border-radius:4px;padding:8px;margin:0;max-height:300px;overflow:auto;font-size:11px;color:var(--dim);white-space:pre-wrap;word-break:break-all}
.k{color:var(--acc)}.g{color:var(--ok)}.r{color:var(--bad)}.w{color:var(--warn)}
small{color:var(--dim)}
</style></head><body>

<header>
  <h1>NAVLAB &mdash; Carminat <span class="k">0x1F1</span></h1>
  <div id="state">connecting&hellip;</div>
</header>

<main>

<section>
  <h2>Bitmap &mdash; 48 &times; 48, 288 bytes</h2>
  <canvas id="cv" width="48" height="48" style="width:288px;height:288px"></canvas>
  <div class="row" style="margin-top:10px">
    <select id="preset">
      <option value="globe">globe (OEM)</option>
      <option value="renault">renault</option>
      <option value="tryzub">tryzub</option>
      <option value="tryzubclock">tryzub + clock</option>
      <option value="clock">clock</option>
      <option value="temp">temperature</option>
      <option value="volts">volts</option>
      <option value="checker">checker (orientation probe)</option>
    </select>
    <button onclick="loadPreset()">load</button>
    <button onclick="inv()">invert</button>
    <button onclick="clr()">clear</button>
  </div>
  <div class="row">
    <small>shift</small>
    <button onclick="shift(0,-1)">&uarr;</button>
    <button onclick="shift(0,1)">&darr;</button>
    <button onclick="shift(-1,0)">&larr;</button>
    <button onclick="shift(1,0)">&rarr;</button>
    <small>&nbsp;draw: left = set, right = clear</small>
  </div>
  <div class="row">
    <input id="txt" placeholder="text to render" size="10" value="AFFA">
    <input id="tsz" type="number" value="20" min="5" max="48" style="width:56px" title="px">
    <button onclick="drawText()">render text</button>
  </div>
</section>

<section>
  <h2>Header &mdash; the 14 bytes in front of the bitmap</h2>
  <div class="hdr" id="hdr"></div>
  <div class="row" style="margin-top:10px">
    <small>bytes 4&ndash;10 as text</small>
    <input id="hstr" size="8" maxlength="7" value="ABCDEF">
    <button onclick="strToHdr()">write</button>
    <button onclick="resetHdr()">OEM reset</button>
  </div>
  <p><small>
    <span class="g">green</span> = understood (0x30 0x30 = 48&times;48).
    <span class="w">amber</span> = the seven-byte string slot.
    The rest are unknown &mdash; that is what the sweep is for.
  </small></p>

  <h2 style="margin-top:16px">Send</h2>
  <div class="row">
    <button class="pri" onclick="sendNav()">SEND 0x1F1</button>
    <span id="navmsg"><small>&nbsp;</small></span>
  </div>
  <div class="row">
    <button onclick="get('/api/power?on=1')">display ON <small>52 09 00</small></button>
    <button onclick="get('/api/power?on=0')">display OFF <small>52 00 00</small></button>
  </div>
  <div class="row">
    <input id="txtline" value="RENAULT" size="10">
    <button onclick="get('/api/text?t='+encodeURIComponent(txtline.value))">setText</button>
    <button onclick="get('/api/oemtext?t='+encodeURIComponent(txtline.value))" title="77 09 55 FF 31 01 + 8 chars, verbatim from the capture">OEM 0x77</button>
  </div>
  <div class="row">
    <button class="pri" onclick="get('/api/replay?t='+encodeURIComponent(txtline.value))">REPLAY OEM OPENING</button>
  </div>
  <p><small>
    Replay sends the captured order: <span class="k">52 09 00</span> &rarr;
    <span class="k">54 01</span> &rarr; <span class="k">54 03</span> &rarr; setText &rarr;
    <span class="k">0x1F1</span>, 250 ms apart. In the capture the globe arrives 9 ms after
    the text &mdash; a bare 0x1F1 into a panel that has been sent nothing may draw nothing,
    and that would prove nothing about the message.
  </small></p>
  <div class="row">
    <button onclick="get('/api/probe?w=5401')">54 01</button>
    <button onclick="get('/api/probe?w=5403')">54 03</button>
    <button onclick="get('/api/probe?w=25')">25 00 00 00</button>
  </div>
</section>

<section>
  <h2>Sweep one header byte</h2>
  <div class="row">
    <small>byte</small><input id="si" type="number" value="1" min="0" max="13" style="width:52px">
    <small>from</small><input id="sf" type="number" value="0" style="width:60px">
    <small>to</small><input id="st" type="number" value="255" style="width:60px">
  </div>
  <div class="row">
    <small>step</small><input id="ss" type="number" value="1" style="width:52px">
    <small>ms</small><input id="sm" type="number" value="1500" style="width:70px">
    <button class="warn" onclick="sweep()">start</button>
    <button onclick="get('/api/sweep?stop=1')">stop</button>
  </div>
  <p><small>
    Sends the CURRENT canvas once per step with only that byte changed, then restores it.
    Watch the glass, not this page. Start with byte 1 (0x0B) and byte 3 (0x25).
  </small></p>

  <h2 style="margin-top:16px">Raw payload</h2>
  <div class="row">
    <small>id</small><input id="rid" value="151" size="4">
    <label><input type="checkbox" id="rpci" checked> add ISO-TP header</label>
  </div>
  <div class="row">
    <input id="rhex" placeholder="52 09 00" style="flex:1;min-width:170px">
    <button onclick="rawTx()">send</button>
  </div>
</section>

<section style="flex:1;min-width:340px">
  <h2>Wire preview</h2>
  <pre id="wire">&nbsp;</pre>
  <h2 style="margin-top:14px">Log</h2>
  <pre id="log">&nbsp;</pre>
</section>

</main>

<script>
const W=48,H=48,STRIDE=6,NBYTES=288;
const cv=document.getElementById('cv'), cx=cv.getContext('2d');
let px=new Uint8Array(W*H);

function paint(){
  const im=cx.createImageData(W,H);
  for(let i=0;i<W*H;i++){
    const v=px[i]?255:0;
    im.data[i*4]=v; im.data[i*4+1]=v; im.data[i*4+2]=v; im.data[i*4+3]=255;
  }
  cx.putImageData(im,0,0);
  preview();
}
// Canvas -> the OEM's packing: row-major, 6 bytes a row, MSB-first.
function pack(){
  const b=new Uint8Array(NBYTES);
  for(let y=0;y<H;y++)for(let x=0;x<W;x++)
    if(px[y*W+x]) b[y*STRIDE+(x>>3)] |= 0x80>>(x&7);
  return b;
}
function unpack(b){
  for(let y=0;y<H;y++)for(let x=0;x<W;x++)
    px[y*W+x]=(b[y*STRIDE+(x>>3)]>>(7-(x&7)))&1;
  paint();
}
const hex=a=>Array.from(a).map(v=>v.toString(16).toUpperCase().padStart(2,'0')).join('');
function parseHexStr(s){
  const c=s.replace(/[^0-9a-fA-F]/g,'');
  const o=new Uint8Array(c.length>>1);
  for(let i=0;i<o.length;i++)o[i]=parseInt(c.substr(i*2,2),16);
  return o;
}

// ---- drawing -------------------------------------------------------------
let drawing=0;
function at(e){
  const r=cv.getBoundingClientRect();
  return [Math.floor((e.clientX-r.left)/r.width*W), Math.floor((e.clientY-r.top)/r.height*H)];
}
function put(e,v){
  const [x,y]=at(e);
  if(x>=0&&x<W&&y>=0&&y<H&&px[y*W+x]!==v){px[y*W+x]=v;paint();}
}
cv.addEventListener('pointerdown',e=>{drawing=e.button===2?2:1;cv.setPointerCapture(e.pointerId);put(e,drawing===2?0:1);});
cv.addEventListener('pointermove',e=>{if(drawing)put(e,drawing===2?0:1);});
cv.addEventListener('pointerup',()=>drawing=0);
cv.addEventListener('contextmenu',e=>e.preventDefault());

const inv=()=>{for(let i=0;i<px.length;i++)px[i]^=1;paint();};
const clr=()=>{px.fill(0);paint();};
function shift(dx,dy){
  const n=new Uint8Array(W*H);
  for(let y=0;y<H;y++)for(let x=0;x<W;x++){
    const sx=x-dx, sy=y-dy;
    if(sx>=0&&sx<W&&sy>=0&&sy<H) n[y*W+x]=px[sy*W+sx];
  }
  px=n; paint();
}
// The browser's own font renderer, thresholded. No glyph data on the board.
function drawText(){
  const s=document.getElementById('txt').value||'';
  const size=+document.getElementById('tsz').value||20;
  const o=document.createElement('canvas'); o.width=W; o.height=H;
  const c=o.getContext('2d');
  c.fillStyle='#000'; c.fillRect(0,0,W,H);
  c.fillStyle='#fff'; c.textAlign='center'; c.textBaseline='middle';
  c.font='bold '+size+'px monospace';
  c.fillText(s,W/2,H/2,W);
  const d=c.getImageData(0,0,W,H).data;
  for(let i=0;i<W*H;i++) px[i]=d[i*4]>110?1:0;
  paint();
}

// ---- header --------------------------------------------------------------
const LBL=['cmd','?','?','?','s0','s1','s2','s3','s4','s5','NUL','fmt','W=48','H=48'];
let hdr=new Uint8Array(14);
function buildHdrUi(){
  const el=document.getElementById('hdr'); el.innerHTML='';
  for(let i=0;i<hdr.length;i++){
    const d=document.createElement('div');
    const cls = (i>=12) ? 'known' : (i>=4&&i<=10) ? 'str' : '';
    d.innerHTML='<label>'+i+' '+LBL[i]+'</label>'+
      '<input class="'+cls+'" id="h'+i+'" maxlength="2" value="'+
      hdr[i].toString(16).toUpperCase().padStart(2,'0')+'">';
    el.appendChild(d);
  }
  for(let i=0;i<hdr.length;i++)
    document.getElementById('h'+i).addEventListener('input',()=>{
      const v=parseInt(document.getElementById('h'+i).value,16);
      if(!isNaN(v)){hdr[i]=v&0xFF;preview();}
    });
}
function strToHdr(){
  const s=document.getElementById('hstr').value;
  for(let i=0;i<6;i++) hdr[4+i]= i<s.length ? s.charCodeAt(i)&0x7F : 0x20;
  hdr[10]=0;
  buildHdrUi(); preview();
}
async function resetHdr(){
  hdr=parseHexStr(await (await fetch('/api/oemhdr')).text());
  buildHdrUi(); preview();
}

function preview(){
  const b=pack();
  const wire = hdr.length+NBYTES;
  const pci = wire<=7 ? [wire] : [0x10|(wire>>8), wire&0xFF];
  const frames = 1 + Math.ceil(Math.max(0,(pci.length+wire)-8)/7);
  document.getElementById('wire').innerHTML =
    '<span class="k">pci  </span>'+hex(pci)+'   <span class="w">len '+wire+' (0x'+wire.toString(16).toUpperCase()+')</span>\n'+
    '<span class="k">hdr  </span>'+hex(hdr)+'\n'+
    '<span class="k">bmp  </span>'+hex(b.slice(0,24))+'&hellip;\n'+
    '<span class="k">tail </span>&hellip;'+hex(b.slice(-12))+'\n'+
    frames+' CAN frames, '+(pci.length+wire)+' wire bytes';
}

// ---- transport -----------------------------------------------------------
async function get(u){
  const m=document.getElementById('navmsg');
  try{ const r=await fetch(u); m.innerHTML='<small class="'+(r.ok?'g':'r')+'">'+await r.text()+'</small>'; }
  catch(e){ m.innerHTML='<small class="r">'+e+'</small>'; }
  poll();
}
async function loadPreset(){
  const n=document.getElementById('preset').value;
  unpack(parseHexStr(await (await fetch('/api/img?n='+n)).text()));
}
async function sendNav(){
  const body=hex(hdr)+'|'+hex(pack());
  const m=document.getElementById('navmsg');
  try{
    const r=await fetch('/api/nav',{method:'POST',body});
    m.innerHTML='<small class="'+(r.ok?'g':'r')+'">'+await r.text()+'</small>';
  }catch(e){ m.innerHTML='<small class="r">'+e+'</small>'; }
  poll();
}
function sweep(){
  const q=new URLSearchParams({i:document.getElementById('si').value,
    from:document.getElementById('sf').value, to:document.getElementById('st').value,
    step:document.getElementById('ss').value, ms:document.getElementById('sm').value});
  get('/api/sweep?'+q);
}
async function rawTx(){
  const id=document.getElementById('rid').value.replace(/^0x/i,'');
  const pci=document.getElementById('rpci').checked?1:0;
  const r=await fetch('/api/tx?id='+id+'&pci='+pci,{method:'POST',
    body:document.getElementById('rhex').value});
  document.getElementById('navmsg').innerHTML=
    '<small class="'+(r.ok?'g':'r')+'">'+await r.text()+'</small>';
  poll();
}

// ---- status --------------------------------------------------------------
async function poll(){
  try{
    const s=await (await fetch('/api/state')).json();
    document.getElementById('state').innerHTML =
      'phase <span class="k">'+s.phase+'</span> &middot; link '+
      (s.live?'<span class="g">up</span>':'<span class="r">DOWN</span>')+
      ' &middot; sent '+s.sent+' ok <span class="g">'+s.ok+'</span> fail <span class="'+
      (s.fail?'r':'')+'">'+s.fail+'</span>'+
      ' &middot; fc '+s.fc+' ack '+s.ack+
      ' &middot; last '+s.last+
      (s.busy?' &middot; <span class="w">BUSY</span>':'')+
      (s.sweep?' &middot; <span class="w">SWEEP hdr['+s.sweepidx+']='+s.sweepcur+'</span>':'')+
      ' &middot; heap '+s.heap;
    document.getElementById('log').textContent=await (await fetch('/api/log')).text();
  }catch(e){ document.getElementById('state').innerHTML='<span class="r">offline</span>'; }
}

(async function(){
  await resetHdr();
  await loadPreset();
  poll(); setInterval(poll,1500);
})();
</script>
</body></html>
)HTML";
