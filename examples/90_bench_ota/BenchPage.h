// The console page. ONE self-contained document: no CDN, no external stylesheet, no font,
// no image. It is used in a car, on a phone, on a board whose only uplink may be its own
// SoftAP — anything fetched from the internet is a page that does not load when it matters.
//
// It is a raw string literal in flash. PsychicResponse keeps a `const char*` and does not
// copy, so serving it costs no heap.
//
// The page contains NO protocol knowledge. It renders what /api/screen decoded and posts
// what the user pressed to /api/nav, /api/key and the render endpoints. The one exception
// is cosmetic: it maps the scroll byte and the row tag to arrows and a highlight, which is
// presentation of a value the twin already decoded.
#pragma once

static const char kBenchPage[] = R"PAGE(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>AffaDisplay bench</title>
<style>
:root{--bg:#0d1117;--pn:#161b22;--bd:#30363d;--fg:#c9d1d9;--dim:#8b949e;--ac:#58a6ff;
--ok:#3fb950;--wa:#d29922;--er:#f85149;--tx:#a371f7}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
h2{font-size:12px;margin:0;padding:6px 10px;background:#1c2128;border-bottom:1px solid var(--bd);
letter-spacing:.08em;text-transform:uppercase;color:var(--dim)}
.wrap{max-width:1200px;margin:0 auto;padding:8px;display:flex;flex-direction:column;gap:8px}
.card{background:var(--pn);border:1px solid var(--bd);border-radius:6px;overflow:hidden}
.card>div.body{padding:8px 10px;display:flex;flex-direction:column;gap:8px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:8px}
.row{display:flex;flex-wrap:wrap;gap:6px;align-items:center}
label{color:var(--dim);font-size:11px}
input,select,button{font:inherit;background:#0d1117;color:var(--fg);border:1px solid var(--bd);
border-radius:4px;padding:4px 7px}
input:focus,select:focus{outline:1px solid var(--ac)}
input.s{width:64px}input.m{width:110px}input.l{width:150px}
button{cursor:pointer;background:#21262d}
button:hover{border-color:var(--ac)}
button.go{background:#1f6feb;border-color:#1f6feb;color:#fff}
button.warn{background:#6e2c1f;border-color:#8a3a28}
button.on{background:#1a4d2b;border-color:var(--ok)}
.pill{padding:2px 7px;border-radius:10px;border:1px solid var(--bd);background:#0d1117;font-size:11px;white-space:nowrap}
.pill b{color:var(--fg);font-weight:600}
.pill.ok{border-color:var(--ok);color:var(--ok)}
.pill.er{border-color:var(--er);color:var(--er)}
.pill.wa{border-color:var(--wa);color:var(--wa)}
.strip{display:flex;flex-wrap:wrap;gap:5px;padding:8px 10px}
pre{margin:0;padding:6px 8px;background:#010409;border:1px solid var(--bd);border-radius:4px;
height:210px;overflow:auto;font-size:11px;line-height:1.35;white-space:pre}
.glass{background:#04120a;border:2px solid #1b4d2c;border-radius:5px;padding:8px 10px;
color:#7ee787;font-size:14px;letter-spacing:.06em}
.glass .hd{color:#58a6ff;border-bottom:1px dashed #1b4d2c;padding-bottom:3px;margin-bottom:4px}
.glass .ln{display:flex;justify-content:space-between;padding:1px 0}
.glass .sel{background:#7ee787;color:#04120a}
.glass .ar{color:#d29922;width:1.2em;text-align:right}
.dpad{display:grid;grid-template-columns:repeat(3,54px);grid-auto-rows:34px;gap:5px}
.dpad button{padding:0}
.keys{display:grid;grid-template-columns:repeat(auto-fit,minmax(88px,1fr));gap:5px}
.tiny{font-size:11px;color:var(--dim)}
.rx{color:#7ee787}.tx{color:#79c0ff}
a{color:var(--ac)}
.sp{flex:1}
</style></head><body>
<div class="wrap">

<div class="card"><h2>status</h2><div class="strip" id="strip"></div>
<div class="body" style="border-top:1px solid var(--bd)">
 <div class="row">
  <label>panel</label>
  <button id="mReal" onclick="mode('real')">real</button>
  <button id="mVirt" onclick="mode('virtual')">virtual</button>
  <span class="sp"></span>
  <button id="gate" onclick="txgate()">tx gate</button>
  <button onclick="go('/api/selftest')">selftest</button>
  <a href="/update" target="_blank"><button>OTA /update</button></a>
  <button class="warn" onclick="if(confirm('reboot?'))go('/api/reboot')">reboot</button>
 </div>
 <div class="tiny" id="stnote"></div>
</div></div>

<div class="grid">

<div class="card"><h2>screen</h2><div class="body">
 <div class="row"><label>text</label><input class="l" id="tx" value="AFFA OK">
  <label>digit</label><input class="s" id="txd" value="255">
  <button class="go" onclick="go('/api/text?t='+e('tx')+'&d='+e('txd'))">setText</button></div>
 <div class="row"><label>hhmm</label><input class="s" id="tm" value="1234">
  <button class="go" onclick="go('/api/time?hhmm='+e('tm'))">setTime</button>
  <span class="sp"></span>
  <button onclick="go('/api/state?on=1')">power on</button>
  <button onclick="go('/api/state?on=0')">power off</button></div>
 <div class="row"><label>popup</label><input class="m" id="pt" value="VOL 28">
  <label>icon</label><input class="s" id="pi" value="0x09">
  <label>src</label><input class="s" id="ps" value="0xFF">
  <label>fmt</label><input class="s" id="pf" value="0x60">
  <button class="go" onclick="go('/api/popup?t='+e('pt')+'&icon='+e('pi')+'&src='+e('ps')+'&fmt='+e('pf'))">show</button>
  <button onclick="go('/api/popup/hide')">hide</button></div>
 <div class="row"><label>full</label><input class="m" id="f1" value="LINE ONE">
  <input class="m" id="f2" value="LINE TWO"><input class="m" id="f3" value="LINE THREE">
  <button class="go" onclick="go('/api/fullscreen?l1='+e('f1')+'&l2='+e('f2')+'&l3='+e('f3'))">show</button>
  <button onclick="go('/api/fullscreen/hide')">hide</button></div>
 <div class="row"><label>confirm</label><input class="s" id="cc" value="SAVE?">
  <input class="m" id="c1" value="YES"><input class="m" id="c2" value="NO">
  <button class="go" onclick="go('/api/confirm?cap='+e('cc')+'&r1='+e('c1')+'&r2='+e('c2'))">show</button>
  <span class="tiny">caption &gt; 6 chars overwrites row 1</span></div>
 <div class="row"><label>info</label><input class="s" id="i1" value="AUX ON">
  <input class="s" id="i2" value="AF ON"><input class="s" id="i3" value="SPD 0">
  <button class="go" onclick="go('/api/info?l1='+e('i1')+'&l2='+e('i2')+'&l3='+e('i3'))">show</button>
  <button onclick="go('/api/info/hide')">hide</button></div>
 <div class="tiny" id="renderout">&nbsp;</div>
</div></div>

<div class="card"><h2>menu &amp; what the panel shows</h2><div class="body">
 <div class="glass" id="glass">
  <div class="hd" id="gh">&nbsp;</div>
  <div class="ln"><span id="g0">&nbsp;</span><span class="ar" id="a0"></span></div>
  <div class="ln"><span id="g1">&nbsp;</span><span class="ar" id="a1"></span></div>
 </div>
 <div class="tiny" id="ginfo"></div>
 <div class="row">
  <div class="dpad">
   <button onclick="go('/api/nav?c=dec')">&laquo;dec</button>
   <button onclick="go('/api/nav?c=prev')">&uarr; prev</button>
   <button onclick="go('/api/nav?c=inc')">inc&raquo;</button>
   <button onclick="go('/api/nav?c=back')">back</button>
   <button class="go" onclick="go('/api/nav?c=select')">select</button>
   <button onclick="go('/api/nav?c=open')">open</button>
   <button onclick="go('/api/menu/show')">render</button>
   <button onclick="go('/api/nav?c=next')">&darr; next</button>
   <button onclick="go('/api/menu/state').then(showState)">state</button>
  </div>
  <div class="sp"></div>
  <div>
   <div class="row"><label>row</label>
    <button onclick="go('/api/highlight?row=0')">highlight 0</button>
    <button onclick="go('/api/highlight?row=1')">highlight 1</button></div>
   <pre id="mstate" style="height:96px;margin-top:6px"></pre>
  </div>
 </div>
 <div class="row"><label>raw showMenu</label>
  <input class="m" id="mh" value="CLOCK"><input class="m" id="ma" value="Hours">
  <input class="m" id="mb" value="Minutes">
  <select id="ms"><option value="0">0x00 none</option><option value="7">0x07 up</option>
   <option value="11" selected>0x0B down</option><option value="12">0x0C both</option></select>
  <button class="go" onclick="go('/api/menu?h='+e('mh')+'&a='+e('ma')+'&b='+e('mb')+'&s='+v('ms'))">send</button>
 </div>
</div></div>

<div class="card"><h2>keys</h2><div class="body">
 <div class="row">
  <label>hold</label><input type="checkbox" id="khold">
  <label>src</label><select id="ksrc">
   <option value="local" selected>local (no frame)</option>
   <option value="wire">wire (frame only)</option>
   <option value="both">both</option></select>
  <span class="tiny">local drives our menu; wire puts 03 89 .. on 0x1C1</span>
 </div>
 <div class="keys" id="kbtns"></div>
 <pre id="klog" style="height:150px"></pre>
</div></div>

<div class="card"><h2>responsiveness</h2><div class="body">
 <div class="row"><label>rate</label>
  <input type="range" id="chz" min="1" max="50" value="10" oninput="hzl()">
  <span class="pill" id="chzl">10 Hz</span>
  <label>for</label><input class="s" id="cto" value="0"><span class="tiny">ms, 0 = until stopped</span></div>
 <div class="row">
  <button class="go" onclick="go('/api/counter?run=1&hz='+v('chz')+'&to='+e('cto'))">start counter</button>
  <button onclick="go('/api/counter?run=0')">stop</button>
  <button class="warn" onclick="go('/api/abort')">abortPending()</button>
  <span class="sp"></span>
  <button onclick="go('/api/txgate?on=0')">gate shut</button>
  <button onclick="go('/api/txgate?on=1')">gate open</button></div>
 <div class="strip" id="lat" style="padding:0"></div>
 <div class="tiny">key&rarr;callback is measured from the 0x1C1 frame reaching the tap (or the
  injection being issued) to KeyCb. key&rarr;wire is KeyCb to the first data frame it caused.
  poll max is the worst loop period seen, which is the L1 bound. ACK turnaround is the panel's,
  and it is the one number the library cannot compute for you.</div>
</div></div>

</div>

<div class="grid">
<div class="card"><h2>frames <span class="tiny" id="fpaused"></span></h2><div class="body">
 <div class="row"><button id="fpb" onclick="pause('f')">pause</button>
  <label>show</label><input class="s" id="fn" value="128">
  <span class="tiny">dir / id / bytes / ms</span></div>
 <pre id="frames"></pre></div></div>

<div class="card"><h2>log</h2><div class="body">
 <div class="row"><button id="lpb" onclick="pause('l')">pause</button>
  <label>lines</label><input class="s" id="ln" value="48"></div>
 <pre id="log"></pre></div></div>
</div>

</div>
<script>
const $=i=>document.getElementById(i);
const e=i=>encodeURIComponent($(i).value);
const v=i=>$(i).value;
let paused={f:false,l:false};
function pause(w){paused[w]=!paused[w];$(w+'pb').textContent=paused[w]?'resume':'pause';
 $(w+'pb').className=paused[w]?'on':'';}
function hzl(){$('chzl').textContent=v('chz')+' Hz';}

async function go(u){
 try{const r=await fetch(u);const j=await r.json();
  $('renderout').textContent=u+'  ->  '+JSON.stringify(j);
  return j;}
 catch(x){$('renderout').textContent=u+'  ->  '+x;return null;}
}
function mode(m){go('/api/mode?panel='+m).then(status);}
let gateOn=true;
function txgate(){go('/api/txgate?on='+(gateOn?0:1)).then(status);}

// ---- keys -----------------------------------------------------------------
const KEYS=[['Load',0],['SrcNext',1],['SrcPrev',2],['VolUp',3],['VolDown',4],
            ['Pause',5],['RollUp',257],['RollDown',321]];
(function(){let h='';for(const[n,c]of KEYS)
 h+='<button onclick="key('+c+')">'+n+'<br><span class=tiny>0x'+
    c.toString(16).padStart(4,'0')+'</span></button>';
 $('kbtns').innerHTML=h;})();
function key(c){go('/api/key?k='+c+'&hold='+($('khold').checked?1:0)+'&src='+v('ksrc'))
 .then(keys);}

// ---- pollers --------------------------------------------------------------
function pill(t,val,cls){return '<span class="pill '+(cls||'')+'">'+t+' <b>'+val+'</b></span>';}

async function status(){
 let s;try{s=await(await fetch('/api/status')).json();}catch(x){
  $('strip').innerHTML=pill('link','unreachable','er');return;}
 const y=s.sync.synced,r=s.sync.registered;
 gateOn=s.txGate;
 $('strip').innerHTML=
  pill('wifi',s.wifi.mode+' '+s.wifi.ip,'ok')+
  pill('rssi',s.wifi.rssi)+
  pill('panel',s.panel,s.panel=='virtual'?'wa':'ok')+
  pill('can',s.canUp?'up':'down',s.canUp?'ok':'er')+
  pill('tx',s.txGate?'open':'SHUT',s.txGate?'ok':'er')+
  pill('sync','0x'+s.sync.state.toString(16),y?'ok':'er')+
  pill('registered',r?'yes':'no',r?'ok':'wa')+
  pill('busy',s.busy?'yes':'no')+
  pill('queue',s.queued+'/'+s.txQueueDepth)+
  pill('enq',s.lastEnqueue.result+' #'+s.lastEnqueue.ticket,
       s.lastEnqueue.result=='Ok'?'ok':'wa')+
  pill('delivered',s.lastDelivered.result+' #'+s.lastDelivered.ticket,
       s.lastDelivered.result=='Ok'?'ok':'wa')+
  pill('rx',s.link.rxFrames)+pill('tx',s.link.txFrames)+
  pill('drop',s.link.txDropped,s.link.txDropped?'wa':'')+
  pill('ovf',s.link.ringOverflow,s.link.ringOverflow?'er':'')+
  pill('twin',s.twin.emulating?'emulating':'passive')+
  pill('heap',(s.heap/1024).toFixed(1)+'k')+
  pill('up',(s.uptimeMs/1000).toFixed(0)+'s')+
  pill('selftest',s.selftest.note,s.selftest.ok?'ok':(s.selftest.running?'wa':'er'));
 $('mReal').className=s.panel=='real'?'on':'';
 $('mVirt').className=s.panel=='virtual'?'on':'';
 $('gate').className=s.txGate?'on':'warn';
 $('stnote').textContent='menu open='+s.menu.open+' editing='+s.menu.editing+
  ' sel='+s.menu.selected+' row='+s.menu.row+' items='+s.menu.count+
  '   |   counter run='+s.counter.run+' n='+s.counter.n+' rejected='+s.counter.rejected;
 const L=s.lat;
 $('lat').innerHTML=
  pill('key&rarr;callback',(L.keyToCbUs/1000).toFixed(2)+' ms')+
  pill('key&rarr;wire',(L.keyToWireUs/1000).toFixed(2)+' ms')+
  pill('poll max',(L.pollMaxUs/1000).toFixed(2)+' ms')+
  pill('stale dropped',L.staleDropped,L.staleDropped?'wa':'')+
  pill('ACK n',L.ackN)+
  pill('ACK min',(L.ackMinUs/1000).toFixed(2)+' ms')+
  pill('ACK mean',(L.ackMeanUs/1000).toFixed(2)+' ms')+
  pill('ACK max',(L.ackMaxUs/1000).toFixed(2)+' ms');
}

const ARROW={0:['',''],7:['▲',''],11:['','▼'],12:['▲','▼']};
async function screen(){
 let s;try{s=await(await fetch('/api/screen')).json();}catch(x){return;}
 $('gh').textContent=s.header||' ';
 $('g0').textContent=s.row0||' ';
 $('g1').textContent=s.row1||' ';
 $('g0').className=(s.sel==126)?'sel':'';
 $('g1').className=(s.sel==127)?'sel':'';
 const a=ARROW[s.scroll]||['',''];$('a0').textContent=a[0];$('a1').textContent=a[1];
 $('ginfo').textContent='mode='+s.mode+' scroll=0x'+s.scroll.toString(16)+
  ' sel='+(s.sel<0?'none':'0x'+s.sel.toString(16))+
  ' decoded@'+s.lastDecodeMs+'ms (now '+s.nowMs+')'+
  (s.info.length?'  info: '+s.info.join(' | '):'')+
  '   source: '+s.panel+' twin';
}
function showState(j){if(j)$('mstate').textContent=JSON.stringify(j,null,1);}

async function frames(){
 if(paused.f)return;
 let s;try{s=await(await fetch('/api/frames?n='+v('fn'))).json();}catch(x){return;}
 let o='';
 for(const f of s.f){
  const d=f[1]==1?'RX':'TX';
  o+='<span class="'+(f[1]==1?'rx':'tx')+'">'+d+'</span> '+
     f[2].toString(16).toUpperCase().padStart(3,'0')+'  '+
     (f[3].match(/../g)||[]).join(' ').padEnd(23,' ')+'  '+f[0]+'\n';}
 const p=$('frames');const b=p.scrollTop+p.clientHeight>=p.scrollHeight-20;
 p.innerHTML=o;if(b)p.scrollTop=p.scrollHeight;
}
const LV=['','E','W','I','D','T'];
async function log(){
 if(paused.l)return;
 let s;try{s=await(await fetch('/api/log?n='+v('ln'))).json();}catch(x){return;}
 let o='';for(const l of s.l)o+=l[0]+' '+(LV[l[1]]||'?')+' '+l[2]+': '+l[3]+'\n';
 const p=$('log');const b=p.scrollTop+p.clientHeight>=p.scrollHeight-20;
 p.textContent=o;if(b)p.scrollTop=p.scrollHeight;
}
async function keys(){
 let s;try{s=await(await fetch('/api/keys')).json();}catch(x){return;}
 let o='';for(const k of s.k)
  o+=k[0]+'  '+k[2].padEnd(9,' ')+' 0x'+k[1].toString(16).padStart(4,'0')+
     (k[3]?'  HOLD':'      ')+'  '+k[4]+'\n';
 const p=$('klog');p.textContent=o;p.scrollTop=p.scrollHeight;
}

status();screen();frames();log();keys();
setInterval(status,1000);
setInterval(screen,700);
setInterval(frames,800);
setInterval(log,1200);
setInterval(keys,1000);
</script></body></html>
)PAGE";
