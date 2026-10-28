'use strict';
const $ = id => document.getElementById(id);
const HISTORY = 40;
let session = JSON.parse(sessionStorage.getItem('so2session') || 'null');
let timer = null, sevFilter = '', lastState = null;
const hist = { labels: [], active: [], inactive: [], used: [], swap: [], fault: [], maj: [] };

function escapeHtml(s) { return String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }
const mib = kb => Number(kb || 0) / 1024;
const fmtMiB = kb => mib(kb).toFixed(1);
const isAdmin = () => session && session.role === 'admin_user';

function toast(msg, ok = true) {
  const t = document.createElement('div');
  t.className = 'toast ' + (ok ? 'ok' : 'err'); t.textContent = msg;
  $('toasts').append(t); setTimeout(() => t.remove(), 4500);
}

/* ---------- API ---------- */
async function api(path, method = 'GET', body, raw = false) {
  const headers = { Authorization: 'Bearer ' + (session ? session.token : '') };
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const r = await fetch(path, { method, headers, cache: 'no-store', body: body === undefined ? undefined : (raw ? body : JSON.stringify(body)) });
  let j = null; try { j = await r.json(); } catch (_) { /* sin cuerpo JSON */ }
  if (r.status === 401 && path !== '/api/login') { endSession('La sesión expiró'); throw new Error('Sesión expirada'); }
  if (!r.ok) throw new Error((j && j.error) || ('HTTP ' + r.status));
  return j;
}
async function action(fn, okMsg) {
  try { const r = await fn(); if (okMsg) toast(okMsg); refresh(); return r; }
  catch (e) { toast(e.message, false); refresh(); return null; }
}
function confirmar(title, text) {
  const d = $('confirmDialog'); $('dlgTitle').textContent = title; $('dlgText').textContent = text;
  return new Promise(res => { d.onclose = () => res(d.returnValue === 'ok'); d.returnValue = 'cancel'; d.showModal(); });
}

/* ---------- Sesión ---------- */
function startSession(s) {
  session = s; sessionStorage.setItem('so2session', JSON.stringify(s));
  $('loginPanel').hidden = true; $('dashboard').hidden = false; $('sessionBox').hidden = false;
  $('sessionUser').textContent = s.username; $('role').textContent = s.role;
  $('role').className = 'pill ' + (s.role === 'admin_user' ? 'admin' : '');
  document.body.classList.toggle('is-admin', s.role === 'admin_user');
  refresh(); clearInterval(timer); timer = setInterval(refresh, 2000);
}
function endSession(msg) {
  session = null; sessionStorage.removeItem('so2session'); clearInterval(timer);
  document.body.classList.remove('is-admin');
  $('dashboard').hidden = true; $('sessionBox').hidden = true; $('loginPanel').hidden = false;
  $('password').value = ''; $('loginMessage').textContent = msg || '';
  $('connection').textContent = 'Sin sesión'; $('connection').className = 'pill';
}
$('loginForm').addEventListener('submit', async e => {
  e.preventDefault(); $('loginMessage').textContent = 'Autenticando…';
  try {
    const r = await fetch('/api/login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ username: $('username').value, password: $('password').value }) });
    const j = await r.json();
    if (!r.ok || !j.ok) throw new Error(j.error || 'No autorizado');
    $('loginMessage').textContent = ''; startSession(j);
  } catch (err) { $('loginMessage').textContent = err.message; }
});
$('logout').addEventListener('click', async () => { try { await api('/api/logout', 'POST', {}); } catch (_) { /* ya no importa */ } endSession(); });

/* ---------- Gráficas (canvas) ---------- */
function setupCanvas(c) {
  const dpr = window.devicePixelRatio || 1, w = c.clientWidth, h = c.clientHeight;
  if (c.width !== w * dpr || c.height !== h * dpr) { c.width = w * dpr; c.height = h * dpr; }
  const ctx = c.getContext('2d'); ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, w, h);
  return { ctx, w, h };
}
function legend(id, series, values) {
  $(id).innerHTML = series.map((s, i) => `<span><i style="background:${s.color}"></i>${escapeHtml(s.name)}${values ? ': <b>' + escapeHtml(values[i]) + '</b>' : ''}</span>`).join('');
}
function axes(ctx, w, h, pad, max, fmt) {
  ctx.font = '11px system-ui'; ctx.fillStyle = '#7f8da7'; ctx.strokeStyle = '#26324a'; ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = pad.t + (h - pad.t - pad.b) * i / 4;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(w - pad.r, y); ctx.stroke();
    ctx.textAlign = 'right'; ctx.fillText(fmt(max * (4 - i) / 4), pad.l - 6, y + 4);
  }
}
function lineChart(id, series, labels, fmt = v => v.toFixed(0)) {
  const { ctx, w, h } = setupCanvas($(id)), pad = { l: 56, r: 10, t: 10, b: 22 };
  const max = Math.max(1, ...series.flatMap(s => s.data)) * 1.1;
  axes(ctx, w, h, pad, max, fmt);
  const n = labels.length; if (n < 2) return;
  const x = i => pad.l + (w - pad.l - pad.r) * i / (n - 1), y = v => h - pad.b - (h - pad.t - pad.b) * v / max;
  ctx.textAlign = 'center'; ctx.fillStyle = '#7f8da7';
  [0, Math.floor((n - 1) / 2), n - 1].forEach(i => ctx.fillText(labels[i].slice(11), x(i), h - 6));
  series.forEach(s => {
    ctx.strokeStyle = s.color; ctx.lineWidth = 2; ctx.beginPath();
    s.data.forEach((v, i) => i ? ctx.lineTo(x(i), y(v)) : ctx.moveTo(x(i), y(v))); ctx.stroke();
  });
}
function barChart(id, series, labels) {
  const { ctx, w, h } = setupCanvas($(id)), pad = { l: 56, r: 10, t: 10, b: 22 };
  const max = Math.max(1, ...series.flatMap(s => s.data)) * 1.1;
  axes(ctx, w, h, pad, max, v => v >= 1000 ? (v / 1000).toFixed(1) + 'k' : v.toFixed(0));
  const n = labels.length; if (!n) return;
  const slot = (w - pad.l - pad.r) / n, bw = Math.max(1, slot / (series.length + 1));
  series.forEach((s, k) => {
    ctx.fillStyle = s.color;
    s.data.forEach((v, i) => { const bh = (h - pad.t - pad.b) * v / max; ctx.fillRect(pad.l + i * slot + bw * (k + 0.5), h - pad.b - bh, bw - 1, bh); });
  });
  ctx.textAlign = 'center'; ctx.fillStyle = '#7f8da7';
  [0, n - 1].forEach(i => ctx.fillText(labels[i].slice(11), pad.l + i * slot + slot / 2, h - 6));
}
function donutChart(id, parts) {
  const { ctx, w, h } = setupCanvas($(id)), total = parts.reduce((a, p) => a + p.value, 0) || 1;
  const r = Math.min(w, h) / 2 - 8, cx = w / 2, cy = h / 2; let a = -Math.PI / 2;
  parts.forEach(p => {
    const da = 2 * Math.PI * p.value / total;
    ctx.beginPath(); ctx.moveTo(cx, cy); ctx.arc(cx, cy, r, a, a + da); ctx.closePath(); ctx.fillStyle = p.color; ctx.fill(); a += da;
  });
  ctx.beginPath(); ctx.arc(cx, cy, r * 0.58, 0, 2 * Math.PI); ctx.fillStyle = '#131b2e'; ctx.fill();
  ctx.fillStyle = '#e8edf5'; ctx.font = '600 14px system-ui'; ctx.textAlign = 'center';
  ctx.fillText((mib(total) / 1024).toFixed(2) + ' GiB', cx, cy + 5);
}
function drawCharts(m) {
  const pages = [{ name: 'Activas', color: '#83a7ff', data: hist.active }, { name: 'Inactivas', color: '#f0c36b', data: hist.inactive }];
  lineChart('chartPages', pages, hist.labels); legend('legendPages', pages);
  const mem = [{ name: 'Memoria usada', color: '#83a7ff', data: hist.used }, { name: 'Swap usada', color: '#ff8f8f', data: hist.swap }];
  lineChart('chartMemory', mem, hist.labels); legend('legendMemory', mem);
  const faults = [{ name: 'Menores (pgfault)', color: '#6ad8a0', data: hist.fault }, { name: 'Mayores (pgmajfault)', color: '#ff8f8f', data: hist.maj }];
  barChart('chartFaults', faults, hist.labels); legend('legendFaults', faults, [hist.fault.at(-1) ?? 0, hist.maj.at(-1) ?? 0]);
  const used = Math.max(0, m.total_kb - m.free_kb - m.cached_kb - m.buffers_kb);
  const dist = [{ name: 'Libre', color: '#6ad8a0', value: m.free_kb }, { name: 'Caché + buffers', color: '#f0c36b', value: m.cached_kb + m.buffers_kb }, { name: 'Utilizada', color: '#83a7ff', value: used }];
  donutChart('chartDist', dist); legend('legendDist', dist, dist.map(p => fmtMiB(p.value) + ' MiB'));
}
function pushHistory(m) {
  if (!m.valid || hist.labels.at(-1) === m.timestamp) return;
  hist.labels.push(m.timestamp); hist.active.push(mib(m.active_kb)); hist.inactive.push(mib(m.inactive_kb));
  hist.used.push(mib(m.used_kb)); hist.swap.push(mib(m.swap_total_kb - m.swap_free_kb));
  hist.fault.push(m.pgfault_delta); hist.maj.push(m.pgmajfault_delta);
  if (hist.labels.length > HISTORY) Object.values(hist).forEach(a => a.shift());
}

/* ---------- Render ---------- */
const empty = (cols, txt) => `<tr><td colspan="${cols}" class="muted">${escapeHtml(txt)}</td></tr>`;
const STATUS = { benigno: 'Benigno', infectado: 'Infectado', cuarentena: 'En cuarentena' };
const STATE = { R: 'Ejecutando', S: 'Durmiendo', D: 'Espera E/S', Z: 'Zombi', T: 'Detenido', t: 'Traza', I: 'Inactivo', X: 'Muerto' };

function render(d) {
  const m = d.memory;
  setSwitch('swMetrics', d.metrics_enabled); setSwitch('swProcs', d.procscan_enabled); setSwitch('swFiles', d.file_scan_enabled);
  $('metricsOff').hidden = d.metrics_enabled;
  $('memUsed').textContent = fmtMiB(m.used_kb); $('memUsedSub').textContent = `MiB · ${(100 * m.used_kb / (m.total_kb || 1)).toFixed(1)} %`;
  $('memAvail').textContent = fmtMiB(m.available_kb); $('memTotal').textContent = `MiB de ${fmtMiB(m.total_kb)} MiB`;
  $('swapUsed').textContent = fmtMiB(m.swap_total_kb - m.swap_free_kb); $('swapSub').textContent = `MiB de ${fmtMiB(m.swap_total_kb)} MiB`;
  const ps = d.processes_state;
  $('procTotal').textContent = ps.total || '—'; $('running').textContent = ps.running; $('sleeping').textContent = ps.sleeping; $('zombie').textContent = ps.zombie;
  pushHistory(m); drawCharts(m);

  $('procSource').hidden = d.proc_source === 'syscall' && d.procscan_enabled;
  $('procSource').textContent = !d.procscan_enabled ? 'El escaneo de procesos está desactivado: la tabla no se actualiza.'
    : 'El kernel arrancado no devuelve la lista de procesos en sys_get_processes_state; el top 20 se obtiene de /proc mientras se instala el kernel actualizado.';
  $('procBody').innerHTML = d.top_processes.length ? d.top_processes.map(p => `<tr>
      <td>${p.pid}</td><td>${escapeHtml(p.name)}</td><td>${escapeHtml(STATE[p.state] || p.state)} <span class="muted">(${escapeHtml(p.state)})</span></td>
      <td><div class="bar-pct"><i style="width:${Math.min(100, p.mem_percent)}%"></i></div>${p.mem_percent.toFixed(2)} %</td>
      <td>${fmtMiB(p.rss_kb)} MiB</td>
      <td class="admin-only"><button class="danger small" data-kill="${p.pid}" data-name="${escapeHtml(p.name)}">Matar</button></td></tr>`).join('')
    : empty(6, 'Sin datos de procesos');

  $('sensorOff').hidden = d.sensor_available; $('monitorPath').textContent = d.monitor_path;
  if (document.activeElement !== $('pathInput') && !$('pathInput').value) $('pathInput').value = d.monitor_path;
  const files = d.files.slice().reverse();
  $('filesBody').innerHTML = files.length ? files.map(f => `<tr>
      <td>${escapeHtml(f.path)}${f.threat ? `<br><small class="muted">${escapeHtml(f.threat)}</small>` : ''}</td>
      <td><span class="status ${escapeHtml(f.status)}">${escapeHtml(STATUS[f.status] || f.status)}</span></td>
      <td>${escapeHtml(f.mtime_str)}</td><td><code title="${escapeHtml(f.sha256)}">${escapeHtml(f.sha256.slice(0, 16))}…</code></td>
      <td class="admin-only">${f.status === 'cuarentena' ? '–' : `<button class="small" data-quarantine="${escapeHtml(f.path)}">Cuarentena</button>`}</td></tr>`).join('')
    : empty(5, d.file_scan_enabled ? 'Sin archivos analizados en la ruta configurada' : 'El escaneo de archivos está desactivado');

  $('blacklistInfo').textContent = `${d.blacklist_count} firmas cargadas`;
  $('threatsBody').innerHTML = d.threats.length ? d.threats.map(t => `<tr><td><b>${escapeHtml(t.name)}</b></td><td>${escapeHtml(t.path)}</td><td>${escapeHtml(t.description)}</td><td>${escapeHtml(t.timestamp)}</td></tr>`).join('')
    : empty(4, 'No se han detectado amenazas');

  $('logwatchBody').innerHTML = d.logwatch.length ? d.logwatch.map(l => `<tr><td>${l.id}</td><td>${escapeHtml(l.path)}</td><td>${escapeHtml(l.central)}</td><td><code>${escapeHtml(l.keyword)}</code></td><td>${escapeHtml(l.started)}</td>
      <td class="admin-only"><button class="secondary small" data-stoplw="${l.id}">Finalizar</button></td></tr>`).join('')
    : empty(6, 'No hay logwatch activos');

  const alerts = d.alerts.slice().reverse();
  $('alertCount').textContent = alerts.length; $('alertCount').className = 'pill small' + (alerts.some(a => a.severity === 'HIGH') ? ' HIGH' : '');
  const shown = alerts.filter(a => !sevFilter || a.severity === sevFilter);
  $('alertsBody').innerHTML = shown.length ? shown.map(a => `<tr class="row-${escapeHtml(a.severity)}"><td>${escapeHtml(a.timestamp)}</td><td>${escapeHtml(a.type)}</td>
      <td><span class="severity ${escapeHtml(a.severity)}">${escapeHtml(a.severity)}</span></td><td><b>${escapeHtml(a.reason)}</b></td><td>${escapeHtml(a.description)}</td></tr>`).join('')
    : empty(5, 'Sin alertas');
  if (!$('quarantineBox').hidden) loadQuarantine();
}
function setSwitch(id, on) { const s = $(id); if (!s.dataset.busy) s.checked = !!on; }

async function refresh() {
  if (!session) return;
  try {
    lastState = await api('/api/status');
    $('connection').textContent = 'Conectado'; $('connection').className = 'pill ok';
    render(lastState);
  } catch (e) {
    if (session) { $('connection').textContent = 'Sin conexión'; $('connection').className = 'pill err'; }
  }
}

/* ---------- Acciones de administrador ---------- */
['swMetrics', 'swProcs', 'swFiles'].forEach(id => $(id).addEventListener('change', async e => {
  const s = e.target; s.dataset.busy = '1';
  await action(() => api('/api/control', 'POST', { action: s.checked ? s.dataset.on : s.dataset.off }), s.checked ? 'Activado' : 'Desactivado');
  delete s.dataset.busy;
}));
$('pidForm').addEventListener('submit', async e => {
  e.preventDefault();
  const p = await action(() => api('/api/process?pid=' + encodeURIComponent($('pidInput').value)));
  const box = $('pidInfo'); box.hidden = !p; if (!p) return;
  box.innerHTML = `<b>${escapeHtml(p.name)}</b> · PID ${p.pid} · Tiempo de CPU ${(p.runtime_ms / 1000).toFixed(2)} s · Memoria ${fmtMiB(p.memory_kb)} MiB (${p.mem_percent.toFixed(2)} %)
    <button class="danger small" data-kill="${p.pid}" data-name="${escapeHtml(p.name)}">Matar</button>`;
});
document.addEventListener('click', async e => {
  const b = e.target.closest('button'); if (!b) return;
  if (b.dataset.kill) {
    const ok = await confirmar('¿Terminar proceso?', `Se enviará SIGKILL a ${b.dataset.name} (PID ${b.dataset.kill}). Terminar procesos puede afectar la estabilidad del sistema.`);
    if (ok) action(() => api('/api/kill', 'POST', { pid: Number(b.dataset.kill) }), 'Proceso terminado');
  } else if (b.dataset.quarantine) {
    const ok = await confirmar('¿Poner en cuarentena?', `${b.dataset.quarantine} quedará con permisos 0000: no podrá leerse, escribirse ni ejecutarse.`);
    if (ok) action(() => api('/api/quarantine', 'POST', { ruta: b.dataset.quarantine }), 'Archivo puesto en cuarentena');
  } else if (b.dataset.stoplw) {
    const ok = await confirmar('¿Finalizar logwatch?', `Se detendrá el logwatch ${b.dataset.stoplw} (sys_stop_log_watch).`);
    if (ok) action(() => api('/api/logwatch/stop', 'POST', { id: Number(b.dataset.stoplw) }), 'Logwatch finalizado');
  } else if (b.dataset.sev !== undefined) {
    sevFilter = b.dataset.sev; document.querySelectorAll('#filters .chip').forEach(c => c.classList.toggle('active', c === b));
    if (lastState) render(lastState);
  }
});
$('pathForm').addEventListener('submit', e => {
  e.preventDefault(); action(() => api('/api/path', 'POST', { ruta: $('pathInput').value.trim() }), 'Ruta actualizada');
});
async function loadQuarantine() {
  try {
    const list = await api('/api/quarantine');
    $('quarantineBody').innerHTML = list.length ? list.map(f => `<tr><td>${escapeHtml(f.path)}</td><td>${escapeHtml(f.threat || '—')}</td><td><code>${escapeHtml(f.sha256)}</code></td></tr>`).join('')
      : empty(3, 'No hay archivos en cuarentena');
  } catch (e) { toast(e.message, false); }
}
$('btnQuarantine').addEventListener('click', () => {
  const box = $('quarantineBox'); box.hidden = !box.hidden;
  $('btnQuarantine').textContent = box.hidden ? 'Ver cuarentena' : 'Ocultar cuarentena';
  if (!box.hidden) loadQuarantine();
});
$('blacklistForm').addEventListener('submit', async e => {
  e.preventDefault();
  const file = $('blacklistFile').files[0]; if (!file) return;
  const text = await file.text();
  try { JSON.parse(text); } catch (_) { toast('El archivo no es JSON válido', false); return; }
  if (await action(() => api('/api/blacklist', 'POST', text, true), 'Blacklist actualizada')) $('blacklistForm').reset();
});
$('logwatchForm').addEventListener('submit', async e => {
  e.preventDefault();
  const rutas = $('lwPaths').value.split('\n').map(s => s.trim()).filter(Boolean);
  const r = await action(() => api('/api/logwatch', 'POST', { central: $('lwCentral').value.trim(), palabra: $('lwKeyword').value.trim(), rutas }));
  if (r) { toast(`Logwatch creado (ID ${r.ids.join(', ')})` + (r.aviso ? ' · ' + r.aviso : '')); $('logwatchForm').reset(); }
});
window.addEventListener('resize', () => { if (lastState) drawCharts(lastState.memory); });

if (session) startSession(session);
