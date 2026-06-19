/*
	Copyright (C) 2024-2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	OrangeFox is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _FOX_HTTPD_PAGE_H
#define _FOX_HTTPD_PAGE_H

// Self-contained remote-control dashboard. No external assets so it works on a
// freshly booted recovery with no internet. The session token is entered once
// and kept in localStorage; every API call carries it as ?token= / X-Auth-Token.
static const char* FOX_DASHBOARD_HTML = R"FOXHTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>OrangeFox Remote</title>
<style>
  :root { --bg:#161318; --fg:#f3eef6; --accent:#ff7a18; --muted:#8a8590; --panel:#211c25; }
  * { box-sizing:border-box; }
  body { margin:0; font-family:system-ui,Segoe UI,Roboto,sans-serif; background:var(--bg); color:var(--fg); }
  header { display:flex; align-items:center; gap:.6rem; padding:.6rem .9rem; background:var(--panel); position:sticky; top:0; }
  header b { color:var(--accent); }
  .wrap { display:flex; flex-wrap:wrap; gap:1rem; padding:1rem; }
  .screen { flex:1 1 320px; min-width:280px; }
  .screen img { width:100%; background:#000; border:1px solid #3a3340; border-radius:8px; touch-action:none; cursor:crosshair; }
  .side { flex:1 1 320px; min-width:280px; display:flex; flex-direction:column; gap:.8rem; }
  .card { background:var(--panel); border:1px solid #3a3340; border-radius:8px; padding:.8rem; }
  .card h3 { margin:.1rem 0 .6rem; font-size:.85rem; text-transform:uppercase; letter-spacing:.05em; color:var(--muted); }
  input, button, select { font:inherit; }
  input[type=text], input[type=password] { width:100%; padding:.5rem; border-radius:6px; border:1px solid #3a3340; background:#15121a; color:var(--fg); }
  button { padding:.5rem .8rem; border-radius:6px; border:1px solid #3a3340; background:#2c2531; color:var(--fg); cursor:pointer; }
  button.accent { background:var(--accent); color:#161318; border-color:var(--accent); font-weight:600; }
  button:active { transform:translateY(1px); }
  .row { display:flex; gap:.4rem; flex-wrap:wrap; }
  .keys button { flex:1 1 auto; }
  pre { background:#100d13; border-radius:6px; padding:.6rem; max-height:42vh; overflow:auto; white-space:pre-wrap; font-size:.8rem; margin:0; }
  .muted { color:var(--muted); font-size:.8rem; }
  .quick button { flex:1 1 30%; }
</style>
</head>
<body>
<header><b>🦊 OrangeFox</b> <span>Remote</span> <span id="dim" class="muted" style="margin-left:auto"></span></header>
<div class="wrap">
  <div class="screen card">
    <h3>Screen</h3>
    <img id="screen" alt="screen">
    <div class="row" style="margin-top:.5rem">
      <button onclick="reloadStream()">Reconnect</button>
      <label class="muted"><input type="checkbox" id="livetouch" checked> tap-to-touch</label>
    </div>
  </div>
  <div class="side">
    <div class="card">
      <h3>Session</h3>
      <input type="password" id="token" placeholder="session token">
      <div class="row" style="margin-top:.5rem">
        <button class="accent" onclick="saveToken()">Connect</button>
        <span id="status" class="muted"></span>
      </div>
    </div>
    <div class="card keys">
      <h3>Keys</h3>
      <div class="row">
        <button onclick="key('back')">Back</button>
        <button onclick="key('home')">Home</button>
        <button onclick="key('menu')">Menu</button>
        <button onclick="key('volup')">Vol+</button>
        <button onclick="key('voldown')">Vol-</button>
        <button onclick="key('power')">Power</button>
      </div>
    </div>
    <div class="card">
      <h3>Command</h3>
      <input type="text" id="cmd" placeholder="e.g. status   |   wlan info" onkeydown="if(event.key==='Enter')runCmd()">
      <div class="row quick" style="margin-top:.5rem">
        <button onclick="quick('status')">status</button>
        <button onclick="quick('storages')">storages</button>
        <button onclick="quick('mount')">mount</button>
        <button onclick="quick('backup')">backup list</button>
        <button onclick="quick('log')">log</button>
      </div>
      <div class="row" style="margin-top:.5rem"><button class="accent" onclick="runCmd()">Run</button></div>
    </div>
    <div class="card">
      <h3>Output</h3>
      <pre id="out">(no job yet)</pre>
    </div>
  </div>
</div>
<script>
const $ = s => document.querySelector(s);
let token = localStorage.getItem('foxtoken') || '';
$('#token').value = token;

function qs(extra){ return '?token='+encodeURIComponent(token)+(extra?('&'+extra):''); }
async function api(path, opts){
  opts = opts || {};
  opts.headers = Object.assign({'X-Auth-Token': token}, opts.headers||{});
  const r = await fetch(path + (path.includes('?')?'&':'?') + 'token='+encodeURIComponent(token), opts);
  return r;
}
function saveToken(){ token = $('#token').value.trim(); localStorage.setItem('foxtoken', token); refreshStatus(); reloadStream(); }

async function refreshStatus(){
  try {
    const r = await api('/api/status');
    if(!r.ok){ $('#status').textContent = r.status===401?'bad token':('err '+r.status); return; }
    const j = await r.json();
    $('#status').textContent = 'connected';
    $('#dim').textContent = j.width+'×'+j.height+(j.input_ready?' · input ✓':' · input ✗');
    window._fbw = j.width; window._fbh = j.height;
  } catch(e){ $('#status').textContent = 'offline'; }
}

function reloadStream(){
  const img = $('#screen');
  img.src = '/stream.mjpeg?token='+encodeURIComponent(token)+'&t='+Date.now();
}

function mapCoords(ev){
  const img = $('#screen');
  const rect = img.getBoundingClientRect();
  const w = window._fbw || img.naturalWidth || rect.width;
  const h = window._fbh || img.naturalHeight || rect.height;
  const x = Math.round((ev.clientX - rect.left) / rect.width * w);
  const y = Math.round((ev.clientY - rect.top) / rect.height * h);
  return {x, y};
}
async function touch(x, y, action){
  await api('/api/input/touch', {method:'POST', body: JSON.stringify({x,y,action})});
}
(function(){
  const img = $('#screen');
  let down = false;
  img.addEventListener('pointerdown', e => { if(!$('#livetouch').checked) return; e.preventDefault(); down=true; const p=mapCoords(e); touch(p.x,p.y,'down'); });
  img.addEventListener('pointermove', e => { if(!down) return; e.preventDefault(); const p=mapCoords(e); touch(p.x,p.y,'move'); });
  img.addEventListener('pointerup',   e => { if(!down) return; e.preventDefault(); down=false; touch(0,0,'up'); });
  img.addEventListener('pointercancel', e => { if(down){ down=false; touch(0,0,'up'); } });
})();

async function key(name){ await api('/api/input/key', {method:'POST', body: JSON.stringify({key:name, action:'tap'})}); }

let pollTimer = null, curJob = 0, curOff = 0;
async function runCmd(){
  const raw = $('#cmd').value.trim();
  if(!raw) return;
  const argv = raw.split(/\s+/);
  const r = await api('/api/cmd', {method:'POST', body: JSON.stringify({argv})});
  const j = await r.json();
  if(j.id){ curJob = j.id; curOff = 0; $('#out').textContent=''; pollJob(); }
  else $('#out').textContent = 'error: '+JSON.stringify(j);
}
function quick(v){ $('#cmd').value = v==='backup'?'backup':v; runCmd(); }

async function pollJob(){
  if(!curJob) return;
  const r = await api('/api/jobs/'+curJob+'/output?offset='+curOff);
  if(r.ok){
    const j = await r.json();
    if(j.data){ $('#out').textContent += j.data; $('#out').scrollTop = $('#out').scrollHeight; }
    curOff = j.offset;
    if(j.state === 'done'){ $('#out').textContent += '\n[exit '+j.exit_code+']\n'; curJob=0; return; }
  }
  pollTimer = setTimeout(pollJob, 600);
}

refreshStatus();
reloadStream();
setInterval(refreshStatus, 5000);
</script>
</body>
</html>
)FOXHTML";

#endif // _FOX_HTTPD_PAGE_H
