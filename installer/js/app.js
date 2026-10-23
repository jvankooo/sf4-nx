// The page: picks the source, shows the detected edition, runs the pipeline with progress.
import { CONFIG } from '../config.js?v=1';
import { sourceFromIso, sourceFromFileList, sourceFromDirectoryHandle, sourceFromDataTransfer, inspectDisc } from './source.js?v=1';
import { planGameFiles, formatBytes, formatDuration } from './plan.js?v=1';
import { openSink, describeSinkSupport, cleanStaleTemporaryFiles } from './sink.js';
import { run, Cancelled, UserError, stageIds } from './pipeline.js?v=1';

const $ = (id) => document.getElementById(id);
const el = (tag, props = {}, ...kids) => {
  const e = Object.assign(document.createElement(tag), props);
  for (const k of kids) e.append(k);
  return e;
};

const STAGE_NAMES = {
  download: 'Download the build',
  extract: 'Find the shaders on the disc',
  translate: 'Translate the shaders',
  pack: 'Pack the shader library',
  zip: 'Write the zip',
};

// outcome: null | 'ok' | 'err' | 'cancelled' (last run of step 3, drives the step indicator).
const state = { format: 'iso', disc: null, running: false, abort: null, outcome: null };

$('repo-link').href = CONFIG.project.repoUrl;
$('releases-link').href = CONFIG.project.releasesUrl;
$('author').textContent = CONFIG.project.author;

// ---- step indicator --------------------------------------------------------------------------------------------
function updateWizard() {
  const items = document.querySelectorAll('#wizard .wstep');
  const sourceError = $('source-status').classList.contains('err');
  let s;
  if (!state.disc) s = [sourceError ? 'error' : 'active', 'pending', 'pending'];
  else {
    const s3 = state.running ? 'active' : state.outcome === 'ok' ? 'done' : state.outcome === 'err' ? 'error' : 'active';
    s = ['done', state.disc.edition ? 'done' : 'error', state.disc.edition ? s3 : 'pending'];
  }
  s.forEach((st, i) => {
    items[i].className = `wstep ${st}`;
    if (st === 'active') items[i].setAttribute('aria-current', 'step');
    else items[i].removeAttribute('aria-current');
    items[i].querySelector('.wstate').textContent = ` (${st})`;
  });
}

// ---- environment checks ----------------------------------------------------------------------------------------
function supportsModuleWorkers() {
  try {
    let ok = false;
    const url = URL.createObjectURL(new Blob([''], { type: 'text/javascript' }));
    const w = new Worker(url, { get type() { ok = true; return 'module'; } });
    w.terminate();
    URL.revokeObjectURL(url);
    return ok;
  } catch { return false; }
}

function envProblems() {
  const problems = [];
  const warnings = [];
  if (!window.isSecureContext || !crypto?.subtle) problems.push('This page must be opened over https:// (or http://localhost): the browser disables the hashing and decryption it needs otherwise.');
  if (typeof Worker === 'undefined' || !supportsModuleWorkers()) problems.push('This browser does not support module Web Workers. Use a current Chrome, Edge, Firefox or Safari.');
  if (typeof WebAssembly === 'undefined') problems.push('This browser has no WebAssembly.');
  const sink = describeSinkSupport();
  if (sink === 'opfs') warnings.push('This browser cannot save straight to a file you choose. The zip is written to temporary storage first and then downloaded, so you need free disk space of about the zip size (about 5 GB).');
  if (sink === 'memory') warnings.push('This browser can neither save to a chosen file nor use temporary file storage: the whole zip (about 5 GB) would be built in memory and will most likely fail. Use a current Chrome or Edge.');
  return { problems, warnings };
}

(function showEnv() {
  const { problems, warnings } = envProblems();
  const box = $('env-warning');
  if (!problems.length && !warnings.length) return;
  box.hidden = false;
  box.className = `notice ${problems.length ? 'err' : 'warn'}`;
  box.replaceChildren(...[...problems, ...warnings].map((text) => el('p', { textContent: text })));
  if (problems.length) for (const id of ['pick', 'file-iso', 'file-folder']) $(id).disabled = true;
})();
cleanStaleTemporaryFiles();

// ---- step 1 ---------------------------------------------------------------------------------------------------
function updateFormatTexts() {
  const iso = state.format === 'iso';
  $('format-hint').textContent = iso
    ? 'The disc image of the Xbox 360 game (Street Fighter IV, World edition). It is read in place and never loaded as a whole.'
    : 'The folder with the extracted disc: the one that contains default.xex, Battle, Game and Shader (or its parent).';
  $('pick').textContent = iso ? 'Choose .iso file' : 'Choose the folder';
}

function resetDisc() {
  if (state.running) return;
  state.disc = null;
  $('step-edition').hidden = true;
  $('step-create').hidden = true;
  status('');
  state.outcome = null;
  updateWizard();
}

function status(text, isError = false) {
  const s = $('source-status');
  s.textContent = text;
  s.className = isError ? 'status err' : 'status';
  updateWizard();
}

function setFormat(f) {
  state.format = f;
  document.querySelector(`input[name=format][value=${f}]`).checked = true;
  updateFormatTexts();
  resetDisc();
}

for (const r of document.querySelectorAll('input[name=format]')) r.addEventListener('change', () => r.checked && setFormat(r.value));

$('pick').addEventListener('click', async () => {
  if (state.format === 'folder' && typeof showDirectoryPicker === 'function') {
    try {
      const handle = await showDirectoryPicker({ mode: 'read' });
      await load(() => sourceFromDirectoryHandle(handle, (n) => status(`Listing the folder: ${n} files`)));
    } catch (e) {
      if (e?.name !== 'AbortError') status(`Could not open the folder: ${e.message}`, true);
    }
    return;
  }
  (state.format === 'iso' ? $('file-iso') : $('file-folder')).click();
});

$('file-iso').addEventListener('change', (e) => {
  const file = e.target.files?.[0];
  e.target.value = '';
  if (file) load(() => sourceFromIso(file));
});

$('file-folder').addEventListener('change', (e) => {
  const list = [...e.target.files];
  e.target.value = '';
  if (list.length) load(async () => sourceFromFileList(list));
});

const drop = $('drop');
for (const ev of ['dragenter', 'dragover']) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add('over'); });
for (const ev of ['dragleave', 'drop']) drop.addEventListener(ev, () => drop.classList.remove('over'));
drop.addEventListener('drop', async (e) => {
  e.preventDefault();
  if (state.running) return;
  try {
    const dropped = await sourceFromDataTransfer(e.dataTransfer, (n) => status(`Listing the folder: ${n} files`));
    if (dropped.file) { setFormat('iso'); load(() => sourceFromIso(dropped.file)); }
    else if (dropped.source) { setFormat('folder'); load(async () => dropped.source); }
  } catch (err) { status(`Could not read the drop: ${err.message}`, true); }
});

async function load(makeSource) {
  if (state.running) return;
  resetDisc();
  status('Reading the disc…');
  try {
    const source = await makeSource();
    status('Checking default.xex…');
    const info = await inspectDisc(source, CONFIG);
    state.disc = { source, ...info };
    status(`${source.label}: ${info.files.length.toLocaleString('en-US')} files (${source.detail}).`);
    showEdition();
  } catch (e) {
    status(e.message || String(e), true);
  }
}

// ---- step 2 ---------------------------------------------------------------------------------------------------
function currentPlan() {
  return planGameFiles(state.disc.files, CONFIG.disc, {
    englishOnly: $('english-only').checked,
    keepJapaneseVoices: $('keep-japanese').checked,
  });
}

function showEdition() {
  const d = state.disc;
  $('step-edition').hidden = false;
  const badge = d.edition
    ? el('span', { className: 'badge ok', textContent: 'supported' })
    : el('span', { className: 'badge warn', textContent: 'not supported' });
  const kids = [
    el('dl', { className: 'ed' },
      el('dt', { textContent: 'Edition' }),
      el('dd', {}, el('strong', { textContent: d.edition ? d.edition.name : 'Unknown edition' }), ' ', badge),
      el('dt', { textContent: 'default.xex' }), el('dd', {}, el('code', { textContent: d.sha256 })),
    ),
  ];
  if (!d.edition) {
    kids.push(el('div', { className: 'notice err' },
      el('p', { textContent: `This default.xex is not the one the port was built from (${CONFIG.editions.map((e) => e.name).join(', ')}). ` +
        'The port is the recompiled program of one exact executable, so it cannot run another edition or a modified disc.' })));
    $('edition-body').replaceChildren(...kids);
    $('step-create').hidden = true;
    updateWizard();
    return;
  }
  $('edition-body').replaceChildren(...kids);
  showCreate();
}

// ---- step 3 ---------------------------------------------------------------------------------------------------
function showCreate() {
  const plan = currentPlan();
  state.disc.plan = plan;
  const fullBytes = plan.copyBytes + CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes;
  const updBytes = CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes;
  const card = (label, value) => el('div', { className: 'info-card' }, el('span', { textContent: label }), el('b', { textContent: value }));
  const lines = [
    el('div', { className: 'info-cards' },
      card('Game files', `${plan.copy.length.toLocaleString('en-US')} · ${formatBytes(plan.copyBytes)}`),
      card('Left out', `${plan.skipped.length.toLocaleString('en-US')} · ${formatBytes(plan.skippedBytes)}`),
      card('Full zip', `~${formatBytes(fullBytes)}`),
      card('Update zip', `~${formatBytes(updBytes)}`)),
    el('p', { className: 'muted', textContent: 'Making the shaders takes a few minutes; writing the full zip takes as long as copying the disc.' }),
  ];
  if (describeSinkSupport() === 'memory' && fullBytes > CONFIG.limits.blobWarnBytes) {
    lines.push(el('div', { className: 'notice err', textContent: `This browser would build the ${formatBytes(fullBytes)} zip in memory. Use a current Chrome or Edge.` }));
  }
  $('create-info').replaceChildren(...lines);
  $('keep-japanese').disabled = !$('english-only').checked || state.running;
  $('step-create').hidden = false;
  setButtons(true);
  updateWizard();
}

$('english-only').addEventListener('change', () => state.disc && showCreate());
$('keep-japanese').addEventListener('change', () => state.disc && showCreate());

function setButtons(enabled) {
  const ok = enabled && !state.running && !envProblems().problems.length && !!state.disc?.edition;
  $('create-full').disabled = !ok;
  $('create-update').disabled = !ok;
}

// ---- running ---------------------------------------------------------------------------------------------------
const rows = {};
function buildStages() {
  const list = $('stages');
  list.replaceChildren();
  for (const id of stageIds) {
    const bar = el('i');
    const barBox = el('div', { className: 'bar' }, bar);
    barBox.setAttribute('role', 'progressbar');
    barBox.setAttribute('aria-valuemin', '0');
    barBox.setAttribute('aria-valuemax', '100');
    barBox.setAttribute('aria-label', STAGE_NAMES[id]);
    const name = el('span', { className: 'name', textContent: STAGE_NAMES[id] });
    const pct = el('span', { className: 'pct' });
    const label = el('span', { className: 'label' });
    const eta = el('span', { className: 'eta' });
    const li = el('li', { className: 'stage pending' }, el('div', { className: 'top' }, name, pct), barBox, el('div', { className: 'bottom' }, label, eta));
    rows[id] = { li, bar, barBox, pct, label, eta, t0: 0, f0: 0 };
    list.append(li);
  }
}

function markDone(r) {
  r.li.className = 'stage done';
  r.bar.style.width = '100%';
  r.barBox.classList.remove('indeterminate');
  r.barBox.setAttribute('aria-valuenow', '100');
  r.pct.textContent = '100 %';
  r.eta.textContent = '';
}

function etaText(r, f) {
  const now = performance.now();
  if (!r.t0) { r.t0 = now; r.f0 = f; return 'estimating…'; }
  const dt = (now - r.t0) / 1000;
  const df = f - r.f0;
  if (dt < 3 || df < 0.01) return 'estimating…';
  return `about ${formatDuration((dt / df) * (1 - f))} left`;
}

function onProgress(id, p) {
  const idx = stageIds.indexOf(id);
  if (idx < 0) return;
  stageIds.forEach((sid, i) => { if (i < idx && !rows[sid].li.classList.contains('done')) markDone(rows[sid]); });
  const r = rows[id];
  r.label.textContent = p.label ?? '';
  if (p.total > 0 && p.done >= p.total) { markDone(r); return; }
  r.li.className = 'stage active';
  if (!(p.total > 1)) {
    r.barBox.classList.add('indeterminate');
    r.pct.textContent = 'working';
    r.eta.textContent = '';
    return;
  }
  const f = Math.min(1, Math.max(0, p.done / p.total));
  r.barBox.classList.remove('indeterminate');
  r.bar.style.width = `${f * 100}%`;
  r.barBox.setAttribute('aria-valuenow', String(Math.floor(f * 100)));
  r.pct.textContent = `${Math.floor(f * 100)} %`;
  r.eta.textContent = etaText(r, f);
}

const logLines = [];
function log(text, level = 'info') {
  logLines.push(`${level === 'info' ? '' : `[${level}] `}${text}`);
  $('log').textContent = logLines.join('\n');
  $('log-count').textContent = `(${logLines.length} lines)`;
  $('log').scrollTop = $('log').scrollHeight;
}

function createWorker(kind) {
  const file = { shader: 'shader', pack: 'pack' }[kind];
  return new Worker(new URL(`./workers/${file}.worker.js?v=1`, import.meta.url), { type: 'module' });
}

async function start(mode) {
  if (state.running || !state.disc?.edition) return;
  const name = mode === 'full' ? CONFIG.zip.fullName : CONFIG.zip.updateName;
  const d = state.disc;
  const plan = currentPlan();
  const expected = (mode === 'full' ? plan.copyBytes : 0) + CONFIG.limits.expectedShaderBytes + CONFIG.limits.expectedNroBytes;
  const sink = await openSink(name, expected);
  if (!sink) return;

  state.running = true;
  state.abort = new AbortController();
  setButtons(false);
  for (const id of ['pick', 'english-only', 'keep-japanese']) $(id).disabled = true;
  document.querySelectorAll('input[name=format]').forEach((r) => (r.disabled = true));
  $('result').hidden = true;
  state.outcome = null;
  $('progress').hidden = false;
  $('cancel').disabled = false;
  logLines.length = 0;
  $('log').textContent = '';
  buildStages();
  updateWizard();
  const t0 = Date.now();
  const tick = () => { $('elapsed').textContent = `Elapsed ${formatDuration((Date.now() - t0) / 1000)}`; };
  tick();
  const timer = setInterval(tick, 1000);
  const block = (e) => { e.preventDefault(); e.returnValue = ''; };
  window.addEventListener('beforeunload', block);
  let wake = null;
  try { wake = await navigator.wakeLock?.request('screen'); } catch { /* not available */ }
  const where = { file: 'straight to the chosen file', opfs: 'temporary storage, then download' }[sink.kind] ?? 'in memory';
  log(`Mode: ${mode === 'full' ? 'full install' : 'update'}; edition ${d.edition.name}; ${$('english-only').checked ? 'English only' : 'all languages'}; saving as ${name} (${where}).`);
  log(`Source: ${d.source.label} (${d.source.detail}).`);
  if (mode === 'full') log(`Game files: ${plan.copy.length} (${formatBytes(plan.copyBytes)}), ${plan.skipped.length} left out (${formatBytes(plan.skippedBytes)}).`);

  try {
    const result = await run({
      config: CONFIG, edition: d.edition, files: d.files, plan, mode, sink, createWorker,
      baseUrl: new URL('.', location.href).href, signal: state.abort.signal, onProgress, onLog: log,
    });
    stageIds.forEach((id) => markDone(rows[id]));
    state.outcome = 'ok';
    showResult(true, [
      el('p', {}, el('strong', { textContent: `${name} is ready.` }), ` ${formatBytes(result.zipBytes)}.`),
      el('p', { textContent: result.sinkResult === 'saved' ? 'It was saved to the file you chose.' : 'Your browser downloaded it.' }),
      el('p', { innerHTML: 'Extract it into <code>sdmc:/switch/</code> on the SD card (it contains the <code>sf4-nx</code> folder).' }),
      el('p', { className: 'muted', textContent: `${result.shaders.ok} shaders made (${result.shaders.rejected} rejected by the translator, as in the native build).` }),
    ]);
  } catch (e) {
    if (e instanceof Cancelled) {
      state.outcome = 'cancelled';
      showResult(false, [el('p', { textContent: 'Cancelled. Nothing was saved.' })], 'warn');
    } else {
      console.error(e);
      state.outcome = 'err';
      for (const id of stageIds) if (rows[id].li.classList.contains('active')) { rows[id].li.className = 'stage error'; rows[id].eta.textContent = ''; }
      const msg = e instanceof UserError ? e.message : `Something went wrong: ${e?.message ?? e}`;
      log(`ERROR: ${msg}`, 'error');
      showResult(false, [el('p', { textContent: msg }), el('p', { className: 'muted', textContent: 'Nothing was saved. The Details log above has the full story.' })], 'err');
    }
  } finally {
    clearInterval(timer);
    for (const id of stageIds) { rows[id].barBox.classList.remove('indeterminate'); rows[id].eta.textContent = ''; }
    window.removeEventListener('beforeunload', block);
    try { await wake?.release(); } catch { /* ignore */ }
    state.running = false;
    state.abort = null;
    $('pick').disabled = false;
    $('english-only').disabled = false;
    $('keep-japanese').disabled = !$('english-only').checked;
    document.querySelectorAll('input[name=format]').forEach((r) => (r.disabled = false));
    setButtons(true);
    $('cancel').disabled = true;
    updateWizard();
  }
}

function showResult(ok, kids, cls = ok ? 'ok' : 'err') {
  const r = $('result');
  r.className = `notice ${cls}`;
  r.replaceChildren(...kids);
  r.hidden = false;
  r.scrollIntoView({ block: 'nearest' });
}

$('create-full').addEventListener('click', () => start('full'));
$('create-update').addEventListener('click', () => start('update'));
$('cancel').addEventListener('click', () => { $('cancel').disabled = true; state.abort?.abort(); });

updateFormatTexts();
updateWizard();
window.__installer = { state, CONFIG };
