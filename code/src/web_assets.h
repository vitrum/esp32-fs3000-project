#pragma once

// ============================================================================
// Wi-Fi 网页前端资源（PROGMEM 内嵌）
// 实时监控主页：线条仪表 + WebSocket 实时刷新 + 历史记录下载
// 轻量：原生 HTML/JS，无外部构建，适合 ESP32 内存
// ============================================================================

static const char WIFI_WEB_INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>AeroProbe 实时监控</title>
<style>
  :root{color-scheme:light;--bg:#f8fafb;--fg:#1b282c;--muted:#65767b;--line:#d3dddf;--accent:#1464bf;--ok:#16805d;--err:#b44236;}
  *{box-sizing:border-box;letter-spacing:0;}
  body{margin:0;background:var(--bg);color:var(--fg);font-family:"DIN Alternate","Bahnschrift","PingFang SC","Microsoft YaHei",sans-serif;}
  button,input{font:inherit;}button{cursor:pointer;border:1px solid var(--line);border-radius:0;background:transparent;color:var(--fg);min-height:44px;padding:10px 14px;}
  button:hover{background:#eaf0f2;}button:focus-visible{outline:2px solid var(--accent);outline-offset:3px;}button:disabled{opacity:.4;cursor:not-allowed;}
  .icon{font-size:22px;line-height:1;display:inline-block;min-width:20px;text-align:center;}
  .icon-button{width:44px;height:44px;padding:0;border:0;display:inline-flex;align-items:center;justify-content:center;color:var(--accent);flex:none;}
  .shell{max-width:1120px;margin:auto;padding:28px 32px 48px;}
  header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding-bottom:22px;border-bottom:1px solid var(--line);}
  h1{font-size:22px;font-weight:650;margin:0;}h2{font-size:15px;font-weight:600;margin:0;}
  .status{font-size:13px;color:var(--muted);}#conn{display:flex;gap:8px;align-items:center;white-space:nowrap;}#conn::before{content:"";width:7px;height:7px;background:var(--err);border-radius:50%;}#conn.ok::before{background:var(--ok);}
  main{display:grid;grid-template-columns:minmax(0,1.2fr) minmax(0,1fr);gap:64px;align-items:start;}
  .instrument,.support{min-width:0;}.instrument-top{display:flex;align-items:center;justify-content:space-between;min-height:62px;border-bottom:1px solid var(--line);gap:12px;}
  .instrument-status{display:flex;align-items:center;gap:12px;min-width:0;}#src{font-size:13px;color:var(--muted);}#log-icon{color:var(--muted);font-size:13px;}#log-icon.active{color:var(--ok);}
  #clock{font-size:17px;white-space:nowrap;font-variant-numeric:tabular-nums;}
  .metric{display:grid;grid-template-columns:100px minmax(0,1fr);align-items:center;min-height:128px;border-bottom:1px solid var(--line);column-gap:12px;}
  .metric-label{display:flex;flex-direction:column;gap:10px;font-size:14px;}.unit{font-size:13px;color:var(--muted);}.value{font-size:60px;font-weight:500;text-align:right;line-height:1.15;font-variant-numeric:tabular-nums;white-space:nowrap;min-width:0;}.value.compact{font-size:40px;}.power .value{color:var(--accent);}
  .metric.cda{padding:20px 0 12px;row-gap:12px;}.metric-note{grid-column:1/-1;font-size:12px;color:var(--muted);display:flex;justify-content:space-between;gap:12px;}
  .instrument-footer{display:flex;align-items:center;justify-content:space-between;min-height:64px;gap:12px;}.cadence{font-size:14px;}#cad{font-size:22px;font-variant-numeric:tabular-nums;margin:0 6px 0 12px;}
  .section{padding:24px 0;border-bottom:1px solid var(--line);}.section-heading{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:16px;min-height:24px;}
  .details{margin:0;display:grid;grid-template-columns:1fr 1fr;gap:18px 24px;}.details div{min-width:0;}.details dt{color:var(--muted);font-size:12px;margin-bottom:6px;}.details dd{margin:0;font-size:19px;font-variant-numeric:tabular-nums;}
  .actions{display:flex;flex-wrap:wrap;gap:8px;margin:16px 0;}.actions button{display:flex;align-items:center;gap:8px;}#start{background:var(--accent);border-color:var(--accent);color:#fff;}#stop{color:var(--err);}
  #feedback{min-height:20px;line-height:1.6;overflow-wrap:anywhere;}.trend-legend{display:flex;gap:18px;font-size:12px;color:var(--muted);}.trend-legend span::before{content:"";display:inline-block;width:14px;height:2px;background:var(--accent);vertical-align:middle;margin-right:6px;}.trend-legend span:last-child::before{background:var(--ok);}
  canvas{display:block;width:100%;height:110px;margin-top:12px;}table{width:100%;table-layout:fixed;border-collapse:collapse;font-size:13px;}th{font-weight:500;color:var(--muted);}td,th{padding:12px 0;text-align:left;border-bottom:1px solid var(--line);overflow-wrap:anywhere;}th:first-child{width:58%;}th:nth-child(2){width:24%;}th:last-child{width:18%;}td:last-child,th:last-child{text-align:right;}.file-name{padding-right:12px;}
  [hidden]{display:none!important;}
  @media(max-width:760px){.shell{max-width:560px;padding:20px 20px 32px;}header{padding-bottom:16px;}h1{font-size:20px;}main{grid-template-columns:minmax(0,1fr);gap:0;}.metric{min-height:112px;}.value{font-size:54px;}.value.compact{font-size:36px;}.support{border-top:1px solid var(--line);}.instrument-top{min-height:56px;}}
  @media(max-width:350px){.shell{padding:16px 14px 28px;}.metric{grid-template-columns:82px minmax(0,1fr);column-gap:8px;}.value{font-size:46px;}.value.compact{font-size:30px;}#conn{font-size:12px;}}
</style>
</head>
<body>
<div class="shell"><header>
  <h1>AeroProbe 实时监控</h1>
  <span id="conn">未连接</span>
</header>
<main>
  <section class="instrument" aria-label="实时骑行仪表">
    <div class="instrument-top"><div class="instrument-status"><span id="src">--</span><span id="log-icon" title="未记录" aria-label="未记录">●</span></div><time id="clock">--:--:--</time></div>
    <div class="metric cda"><div class="metric-label">估算 CdA<span class="unit">m²</span></div><div class="value" id="cda">--</div><div class="metric-note"><span id="cda-state">等待有效数据</span><span id="zone">Z--</span></div></div>
    <div class="metric"><div class="metric-label">风速<span class="unit">km/h</span></div><div class="value" id="vair">--</div></div>
    <div class="metric power"><div class="metric-label">功率<span class="unit">W</span></div><div class="value" id="p">--</div></div>
    <div class="instrument-footer"><div class="cadence">踏频<span id="cad">--</span><span class="unit">rpm</span></div><button class="icon-button" id="settings-button" title="记录与连接设置" aria-label="记录与连接设置" aria-controls="controls-panel" aria-expanded="true"><span class="icon" aria-hidden="true">⚙</span></button></div>
    <section class="section"><div class="section-heading"><h2>最近 60 次采样</h2><div class="trend-legend"><span>功率</span><span>风速</span></div></div><canvas id="trend" role="img" aria-label="功率与风速变化趋势"></canvas></section>
  </section>
  <aside class="support">
    <section class="section"><div class="section-heading"><h2>骑行数据</h2><span id="logstate" class="status">记录状态未知</span></div>
      <dl class="details"><div><dt>地速 · km/h</dt><dd id="vg">--</dd></div><div><dt>空气密度 · kg/m³</dt><dd id="rho">--</dd></div><div><dt>偏航角 · °</dt><dd id="yaw">--</dd></div><div><dt>姿态角 · °</dt><dd id="posture">--</dd></div></dl>
    </section>
    <section class="section" id="controls-panel"><h2>记录与连接</h2><div class="actions"><button id="start" onclick="ctl('start')" disabled><span aria-hidden="true">▶</span>开始记录</button><button id="stop" onclick="ctl('stop')" disabled><span aria-hidden="true">■</span>停止记录</button><button id="provision" onclick="provision()" disabled><span aria-hidden="true">↻</span>重新配网</button></div><div class="status" id="feedback" role="status" aria-live="polite"></div></section>
    <section class="section"><div class="section-heading"><h2>历史记录</h2><button class="icon-button" onclick="loadRecords()" title="刷新记录" aria-label="刷新记录"><span class="icon" aria-hidden="true">↻</span></button></div>
    <table><thead><tr><th>文件</th><th>大小</th><th>操作</th></tr></thead>
    <tbody id="records"><tr><td colspan="3" class="status">加载中…</td></tr></tbody></table>
    </section>
  </aside>
</main></div>

<script>
const connEl = document.getElementById('conn');
let ws = null, retryTimer = null;
let online = false, busy = false, logging = false, lastFrame = 0, lastSample = null;
let deviceUtc = 0, utcReceived = 0, recordsLoading = false;
const history = [];
const finite = value => typeof value === 'number' && Number.isFinite(value);
const fmt = (value, digits) => finite(value) ? value.toFixed(digits) : '--';
const localClock = new Intl.DateTimeFormat('en-GB', {timeZone:'Asia/Shanghai',hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false});

function updateControls(){
  document.getElementById('start').disabled = !online || busy || logging;
  document.getElementById('stop').disabled = !online || busy || !logging;
  document.getElementById('provision').disabled = !online || busy;
}

function setConn(ok){
  online = ok;
  connEl.textContent = ok ? '已连接' : '未连接';
  connEl.className = ok ? 'ok' : '';
  updateControls();
  if(!ok){
    for(const id of ['p','cad','vg','vair','yaw','posture','rho','src','cda']) document.getElementById(id).textContent='--';
    document.getElementById('cda-state').textContent='等待有效数据';
    document.getElementById('zone').textContent='Z--';
    document.getElementById('logstate').textContent='记录状态未知';
    document.getElementById('log-icon').className='';
    document.getElementById('log-icon').title='记录状态未知';
    document.getElementById('log-icon').setAttribute('aria-label','记录状态未知');
  }
}

function render(d){
  if(!d || typeof d !== 'object' || !finite(d.t)) throw new Error('invalid telemetry');
  if(lastSample !== null && d.t < lastSample) history.length=0;
  lastSample=d.t;
  lastFrame=Date.now();
  setConn(true);
  document.getElementById('p').textContent      = fmt(d.p, 0);
  document.getElementById('cad').textContent    = fmt(d.cad, 0);
  document.getElementById('vg').textContent     = fmt(finite(d.vg) ? d.vg*3.6 : null, 1);
  document.getElementById('vair').textContent   = d.windValid ? fmt(finite(d.vair) ? d.vair*3.6 : null, 1) : '--';
  document.getElementById('yaw').textContent    = d.windValid ? fmt(d.yaw, 1) : '--';
  document.getElementById('posture').textContent= fmt(d.posture, 1);
  document.getElementById('rho').textContent    = fmt(d.rho, 3);
  document.getElementById('src').textContent    = d.src;
  const cda = d.windValid && d.src !== 'NONE' && finite(d.cda) && d.cda >= 0 ? d.cda : null;
  document.getElementById('cda').textContent=fmt(cda,2);
  const zone=cda === null ? 0 : 7-[.23,.27,.31,.36,.42,.50,Infinity].findIndex(limit=>cda<limit);
  document.getElementById('zone').textContent=zone ? 'Z'+zone : 'Z--';
  document.getElementById('cda-state').textContent=zone ? ['','直立','低位 / 下把','气动','激进','极致气动','竞赛 TT','精英 TT'][zone] : '等待有效数据';
  for(const id of ['p','vair','cda']) document.getElementById(id).classList.toggle('compact',document.getElementById(id).textContent.length>5);
  logging=d.log===true;
  document.getElementById('logstate').textContent=logging ? '记录中' : '已停止';
  const icon=document.getElementById('log-icon');
  icon.className=logging ? 'active' : '';
  icon.title=logging ? '记录中' : '已停止';
  icon.setAttribute('aria-label',icon.title);
  if(finite(d.utc) && d.utc>=1577836800){deviceUtc=d.utc;utcReceived=Date.now();}
  else{deviceUtc=0;}
  history.push({power:finite(d.p)?d.p:null,wind:d.windValid&&finite(d.vair)?d.vair*3.6:null});
  if(history.length>60) history.shift();
  updateControls();
  drawTrend();
}

function drawTrend(){
  const canvas=document.getElementById('trend'), ratio=window.devicePixelRatio||1;
  const width=canvas.clientWidth,height=110;
  canvas.width=Math.round(width*ratio);canvas.height=Math.round(height*ratio);
  const ctx=canvas.getContext('2d');ctx.scale(ratio,ratio);
  ctx.strokeStyle='#d3dddf';ctx.lineWidth=1;
  for(const y of [10,55,100]){ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(width,y);ctx.stroke();}
  for(const [key,color,top,unit] of [['power','#1464bf',10,'W'],['wind','#16805d',64,'km/h']]){
    const values=history.map(row=>row[key]), valid=values.filter(value=>value!==null);
    const max=Math.max(1,...valid),min=Math.min(0,...valid);
    ctx.fillStyle='#65767b';ctx.font='11px sans-serif';
    ctx.fillText(max.toFixed(0)+' '+unit,0,top+10);
    ctx.fillText(min.toFixed(0),0,top+36);
    ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();let drawing=false;
    values.forEach((value,index)=>{if(value===null){drawing=false;return;}const x=52+index/59*Math.max(0,width-52),y=top+36-(value-min)/(max-min)*36;if(drawing)ctx.lineTo(x,y);else ctx.moveTo(x,y);drawing=true;});ctx.stroke();
  }
}

document.getElementById('settings-button').onclick=()=>{
  const panel=document.getElementById('controls-panel');panel.hidden=!panel.hidden;
  document.getElementById('settings-button').setAttribute('aria-expanded',String(!panel.hidden));
  if(!panel.hidden)panel.scrollIntoView({behavior:'smooth',block:'nearest'});
};
window.addEventListener('resize',drawTrend);
setInterval(()=>{
  document.getElementById('clock').textContent=deviceUtc ? localClock.format(new Date((deviceUtc+(Date.now()-utcReceived)/1000)*1000)) : '--:--:--';
  if(online && lastFrame && Date.now()-lastFrame>5000){setConn(false);connEl.textContent='数据超时';}
},1000);

function connect(){
  if(ws && ws.readyState<=1) return;
  if(retryTimer){clearTimeout(retryTimer);retryTimer=null;}
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws');
  ws.onopen = () => { setConn(true); if(retryTimer){clearTimeout(retryTimer);retryTimer=null;} };
  ws.onmessage = (ev) => {
    try { render(JSON.parse(ev.data)); }
    catch(e) { setConn(false); document.getElementById('conn').textContent = '数据格式错误'; }
  };
  ws.onclose = () => {
    setConn(false);
    ws=null;
    if (!document.hidden && !retryTimer) retryTimer = setTimeout(connect, 2000);
  };
  ws.onerror = () => ws.close();
}

async function loadRecords(){
  if(recordsLoading)return;
  recordsLoading=true;
  try{
    const r = await fetch('/api/records');
    if(!r.ok) throw new Error('HTTP '+r.status);
    const list = await r.json();
    const tb = document.getElementById('records');
    tb.replaceChildren();
    if(!list.length){
      const row = tb.insertRow();
      const cell = row.insertCell();
      cell.colSpan = 3;
      cell.className = 'status';
      cell.textContent = '无记录';
      return;
    }
    list.forEach(f => {
      const row = tb.insertRow();
      const name=row.insertCell();name.textContent=f.name;name.className='file-name';
      row.insertCell().textContent = (Number(f.size)/1024).toFixed(1) + ' KB';
      const action = row.insertCell();
      const button = document.createElement('button');
      button.textContent = '↓';
      button.className='icon-button';button.title='下载 '+f.name;button.setAttribute('aria-label',button.title);
      button.onclick = () => downloadRecord(f.name);
      action.appendChild(button);
    });
  }catch(e){
    const tb = document.getElementById('records');
    tb.replaceChildren();
    const row = tb.insertRow();
    const cell = row.insertCell();
    cell.colSpan = 3;
    cell.className = 'status';
    cell.textContent = '加载失败：' + e.message;
  }finally{recordsLoading=false;}
}

async function ctl(cmd){
  if(!online || busy)return;
  busy=true;updateControls();
  const status = document.getElementById('feedback');
  try {
    const r = await fetch('/api/control?cmd='+encodeURIComponent(cmd), {method:'POST'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    status.textContent = cmd === 'start' ? '开始记录请求已发送' : '停止记录请求已发送';
  } catch(e) {
    status.textContent = '控制失败：' + e.message;
  }finally{busy=false;updateControls();}
}

async function downloadRecord(name){
  try{
    const r = await fetch('/api/download?name='+encodeURIComponent(name));
    if(!r.ok) throw new Error('HTTP '+r.status);
    const url = URL.createObjectURL(await r.blob());
    const a = document.createElement('a');
    a.href = url;
    a.download = name.split('/').pop();
    a.click();
    document.getElementById('feedback').textContent = '下载已准备：' + a.download;
    setTimeout(()=>URL.revokeObjectURL(url),1000);
  }catch(e){
    document.getElementById('feedback').textContent = '下载失败：' + e.message;
  }
}

async function provision(){
  if(!confirm('设备将断开当前 Wi-Fi 并进入 SmartConfig 配网。继续？')) return;
  try{
    const r = await fetch('/api/wifi/provision', {method:'POST'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    document.getElementById('feedback').textContent = '配网已启动，请使用 ESP-Touch App 配置';
  }catch(e){
    document.getElementById('feedback').textContent = '配网启动失败：' + e.message;
  }
}

document.addEventListener('visibilitychange', () => {
  if(!document.hidden && (!ws || ws.readyState>1)) connect();
});

connect();
loadRecords();
setInterval(loadRecords, 10000);
</script>
</body>
</html>
)HTML";
