// The installer's work, start to finish. Runs in the page's main thread and drives Web Workers; it never touches the
// DOM.
//
//   download  the build (sf4-nx.nro) and sf4.toml from the site, checked against releases/manifest.json
//   extract   the shader containers: Shader/*.emb, the decrypted default.xex, the runtime set   (js/sf4_shaders.js)
//   translate every container: Xbox 360 microcode -> HLSL -> SPIR-V                (workers: hlsl.mjs + dxc_web.mjs)
//   pack      sf4_shaders.mesp and its .idx                                        (worker: pack.mjs)
//   zip       stream the zip: nro, toml, shader package, then the disc files (full install)
import { ZipWriter } from './zip.js';
import { planGameFiles, sha256Hex } from './plan.js';
import { sf4Containers } from './sf4_shaders.js';

export class Cancelled extends Error {
  constructor() { super('Cancelled'); this.name = 'Cancelled'; }
}

export class UserError extends Error {
  constructor(message, code = 'error') { super(message); this.name = 'UserError'; this.code = code; }
}

export const stageIds = ['download', 'extract', 'translate', 'pack', 'zip'];

function abs(rel, baseUrl) {
  return new URL(rel, baseUrl).href;
}

export function wasmUrls(config, baseUrl) {
  const w = config.wasm;
  // ?v=: a browser that cached an older tool must not keep using it (the workers pass the same query on to each
  // module's .wasm, see handlers.js createModule).
  const url = (name) => abs(w.dir + name, baseUrl) + (w.version ? `?v=${encodeURIComponent(w.version)}` : '');
  return {
    hlsl: url(w.hlsl),
    dxc: url(w.dxc),
    pack: url(w.pack),
    shaderCommon: url(w.shaderCommon),
    runtimeContainers: url(w.runtimeContainers),
  };
}

/** HEAD-checks every file of the shader toolchain. Returns the list of missing paths (empty = all present). */
export async function checkToolchain(config, baseUrl, fetchImpl = fetch) {
  const w = config.wasm;
  const names = [w.hlsl, w.dxc, w.pack, w.shaderCommon, w.runtimeContainers, ...w.extraFiles];
  const missing = [];
  await Promise.all(names.map(async (n) => {
    try {
      const r = await fetchImpl(abs(w.dir + n, baseUrl), { method: 'HEAD' });
      if (!r.ok) missing.push(w.dir + n);
    } catch {
      missing.push(w.dir + n);
    }
  }));
  return missing.sort();
}

async function fetchWithProgress(url, fetchImpl, signal, onBytes) {
  let r;
  try {
    r = await fetchImpl(url, { signal, cache: 'no-cache' });
  } catch (e) {
    if (signal?.aborted) throw new Cancelled();
    throw new UserError(`Could not download ${url}: ${e.message}. Check your connection and try again.`, 'network');
  }
  if (!r.ok) {
    throw new UserError(r.status === 404
      ? `${url} was not found on the site (HTTP 404). No build has been published yet: see the project's releases.`
      : `Could not download ${url} (HTTP ${r.status}).`, 'download');
  }
  const total = Number(r.headers.get('content-length')) || 0;
  const parts = [];
  let got = 0;
  const reader = r.body.getReader();
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    parts.push(value);
    got += value.length;
    onBytes?.(got, total);
  }
  const out = new Uint8Array(got);
  let p = 0;
  for (const part of parts) { out.set(part, p); p += part.length; }
  return out;
}

/** Fetches the NRO and sf4.toml; verifies them against releases/manifest.json when present. */
export async function fetchBuild(config, edition, { baseUrl, fetchImpl = fetch, signal, progress, log }) {
  let manifest = null;
  try {
    const r = await fetchImpl(abs(config.build.manifest, baseUrl), { signal, cache: 'no-cache' });
    if (r.ok) manifest = await r.json();
  } catch {
    if (signal?.aborted) throw new Cancelled();
  }
  const files = [
    { key: 'nro', label: edition.nro, url: abs(config.build.siteDir + edition.nro, baseUrl) },
    { key: 'toml', label: config.files.toml, url: abs(config.build.toml, baseUrl) },
  ];
  const out = {};
  let doneBytes = 0;
  const totalBytes = files.reduce((a, f) => a + (manifest?.assets?.[f.label]?.size ?? 0), 0) || 0;
  for (const f of files) {
    const base = doneBytes;
    const data = await fetchWithProgress(f.url, fetchImpl, signal, (got, total) => {
      progress({ done: base + got, total: totalBytes || base + (total || got), label: `Downloading ${f.label}` });
    });
    const entry = manifest?.assets?.[f.label];
    if (entry) {
      if (entry.size !== data.length) throw new UserError(`${f.label} has the wrong size (${data.length}, expected ${entry.size}). Try again in a minute; the site may be updating.`, 'integrity');
      if (entry.sha256 && (await sha256Hex(data)) !== entry.sha256.toLowerCase()) {
        throw new UserError(`${f.label} does not match its checksum in manifest.json. Try again in a minute; the site may be updating.`, 'integrity');
      }
    } else if (f.key === 'nro') {
      log?.('No manifest.json on the site: the build could not be verified against a checksum.', 'warn');
    }
    out[f.key] = data;
    doneBytes += data.length;
  }
  out.tag = manifest?.tag ?? null;
  progress({ done: 1, total: 1, label: 'Build downloaded' });
  return out;
}

/** The published runtime containers: {name: base64} JSON -> {name: Uint8Array}. */
async function fetchRuntime(url, fetchImpl, signal) {
  const r = await fetchImpl(url, { signal, cache: 'no-cache' });
  if (!r.ok) throw new UserError(`The runtime shaders (${url}) are missing on the site (HTTP ${r.status}).`, 'wasm-missing');
  const data = await r.json();
  const out = {};
  for (const [name, b64] of Object.entries(data)) out[name] = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
  return out;
}

// ---- worker plumbing -------------------------------------------------------------------------------------------
/** One outstanding request at a time per worker: send a message, wait for the next message of an expected type. */
class Remote {
  constructor(worker) {
    this.worker = worker;
    this.waiting = null;
    worker.onmessage = (e) => this.#message(e.data);
    worker.onerror = (e) => this.#fail(new Error(e?.message || 'worker error'));
  }
  #message(m) {
    if (!this.waiting) return;
    const w = this.waiting;
    if (m.type === 'fatal') { this.waiting = null; w.reject(Object.assign(new UserError(`${m.file}: ${m.error}`, 'wasm'), { file: m.file })); return; }
    if (m.type === w.type) { this.waiting = null; clearTimeout(w.timer); w.resolve(m); }
  }
  #fail(e) {
    if (!this.waiting) return;
    const w = this.waiting;
    this.waiting = null;
    clearTimeout(w.timer);
    w.reject(e);
  }
  call(message, transfer, replyType, timeoutMs) {
    return new Promise((resolve, reject) => {
      const w = { type: replyType, resolve, reject, timer: null };
      if (timeoutMs) w.timer = setTimeout(() => { this.waiting = null; reject(Object.assign(new Error('timeout'), { timeout: true })); }, timeoutMs);
      this.waiting = w;
      this.worker.postMessage(message, transfer ?? []);
    });
  }
  terminate() { this.worker.terminate?.(); }
  cancel() { this.#fail(new Cancelled()); this.terminate(); }
}

class Workers {
  constructor(create) { this.create = create; this.live = new Set(); }
  spawn(kind) { const r = new Remote(this.create(kind)); this.live.add(r); return r; }
  kill(r) { r.terminate(); this.live.delete(r); }
  killAll() { for (const r of [...this.live]) r.cancel(); this.live.clear(); }
}

function throwIfCancelled(signal) { if (signal?.aborted) throw new Cancelled(); }

function workerBudget() {
  const hc = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4;
  return Math.max(1, hc - 1);
}

// The translator refuses some containers outright ("translation rejected: vertex FETCH without a declared element"
// and the like); the native build (tools/build_shader_library.py) skips the same 64, and the game draws without them.
// Anything else (a DXC error, a crash, a timeout) is a real failure: the run stops instead of writing a package with
// holes.
const REJECTED = /translation rejected|not a shader container/;

async function translateStage({ workers, urls, containers, config, signal, progress, log, packRemote }) {
  const names = [...containers.keys()];
  const total = names.length;
  if (total === 0) throw new UserError('No shader containers were found on the disc. Is this a complete Street Fighter IV disc?', 'disc');
  const failures = [];
  let done = 0, ok = 0;
  let batch = [];
  let addChain = Promise.resolve();
  const flush = () => {
    const items = batch;
    batch = [];
    if (items.length) {
      addChain = addChain.then(() => packRemote.call({ type: 'add', id: 0, items }, items.flatMap((i) => [i.spirv.buffer]), 'added'));
    }
  };
  const jobMs = config.limits.jobTimeoutMs;
  const startWorker = async () => {
    const remote = workers.spawn('shader');
    await remote.call({ type: 'init', urls }, [], 'ready', 120000);
    return remote;
  };
  const pass = async (queue, n, failed, counted) => {
    const runners = [];
    for (let i = 0; i < n; i++) {
      runners.push((async () => {
        let remote = await startWorker();
        for (let name = queue.shift(); name; name = queue.shift()) {
          throwIfCancelled(signal);
          const data = containers.get(name);
          let r;
          try {
            r = await remote.call({ type: 'job', id: done, name, data }, [], 'result', jobMs);
          } catch (e) {
            if (!e.timeout) throw e;
            workers.kill(remote);
            r = { ok: false, stage: 'translate', reason: `no answer after ${Math.round(jobMs / 1000)} s` };
            remote = await startWorker();
          }
          if (counted) done++;
          if (r.ok) {
            ok++;
            batch.push({ name, container: data, spirv: r.spirv });
            if (batch.length >= 64) flush();
          } else {
            failed.push({ name, stage: r.stage, reason: r.reason });
          }
          if (counted && (done % 10 === 0 || done === total)) {
            progress({ done, total, label: `Translating shaders: ${done} of ${total}` });
          }
        }
        workers.kill(remote);
      })());
    }
    await Promise.all(runners);
  };
  await pass(names.slice(), Math.max(1, Math.min(config.limits.maxShaderWorkers, total, workerBudget())), failures, true);

  const rejected = failures.filter((f) => REJECTED.test(f.reason));
  let failed = failures.filter((f) => !REJECTED.test(f.reason));
  if (failed.length) {
    log(`${failed.length} shader containers failed; trying them once more in a fresh worker.`, 'warn');
    const again = [];
    await pass(failed.map((f) => f.name), 1, again, false);
    failed = again;
  }
  flush();
  await addChain;
  progress({ done: total, total, label: `Translating shaders: ${total} of ${total}` });
  for (const f of rejected) log(`Skipped ${f.name}: ${f.reason}`, 'skip');
  for (const f of failed) log(`FAILED ${f.name} (${f.stage}): ${f.reason}`, 'error');
  log(`Translated ${ok} of ${total} shader containers; ${rejected.length} rejected by the translator (the native build skips them too)` +
    `${failed.length ? `; ${failed.length} FAILED` : ''}.`, failed.length ? 'error' : 'info');
  return { ok, failures: failed, rejected: rejected.length, total };
}

async function* readChunks(blob, chunkBytes, signal) {
  const read = (pos) => blob.slice(pos, Math.min(pos + chunkBytes, blob.size)).arrayBuffer().then((b) => new Uint8Array(b));
  if (!blob.size) return;
  let next = read(0);
  for (let pos = 0; pos < blob.size; pos += chunkBytes) {
    throwIfCancelled(signal);
    const chunk = await next;
    const after = pos + chunkBytes;
    next = after < blob.size ? read(after) : null;
    yield chunk;
  }
}

async function* bytesOnce(bytes) { if (bytes.length) yield bytes; }

async function* packageChunks(packRemote, file, size, chunkBytes, signal) {
  for (let offset = 0; offset < size; offset += chunkBytes) {
    throwIfCancelled(signal);
    const length = Math.min(chunkBytes, size - offset);
    const r = await packRemote.call({ type: 'read', id: offset, file, offset, length }, [], 'chunk');
    yield r.data;
  }
}

/**
 * Runs the whole thing.
 * options: { config, edition, files (disc files, re-rooted), mode: 'full'|'update', plan (planGameFiles result),
 *            sink, createWorker(kind), baseUrl, fetchImpl, signal, onProgress(stageId, {done,total,label}), onLog }
 * Returns { zipBytes, shaders, sinkResult, tag }.
 */
export async function run(options) {
  const { config, edition, files, mode, sink, createWorker, baseUrl, signal } = options;
  const fetchImpl = options.fetchImpl ?? fetch;
  const log = options.onLog ?? (() => {});
  const stage = (id) => (p) => options.onProgress?.(id, p);
  const workers = new Workers(createWorker);
  const urls = wasmUrls(config, baseUrl);
  const onAbort = () => workers.killAll();
  signal?.addEventListener('abort', onAbort);
  const names = config.files;
  const root = config.zip.root;
  try {
    throwIfCancelled(signal);
    const missing = await checkToolchain(config, baseUrl, fetchImpl);
    if (missing.length) {
      throw new UserError(`The shader tools are not on this site: missing ${missing.join(', ')}. ` +
        'The site was deployed without its WebAssembly tools (see installer/README.md).', 'wasm-missing');
    }
    const build = await fetchBuild(config, edition, { baseUrl, fetchImpl, signal, progress: stage('download'), log });
    log(build.tag ? `Build ${build.tag} downloaded.` : 'Build downloaded.', 'info');

    throwIfCancelled(signal);
    const runtime = await fetchRuntime(urls.runtimeContainers, fetchImpl, signal);
    let extracted;
    try {
      extracted = await sf4Containers(files, runtime, { signal, onProgress: stage('extract') });
    } catch (e) {
      if (e?.name === 'Cancelled') throw new Cancelled();
      throw new UserError(e.message, 'disc');
    }
    for (const line of extracted.log) log(line, 'info');
    const containers = extracted.containers;
    if (containers.size < config.limits.expectedContainers) {
      log(`Only ${containers.size} shader containers (a complete disc has ${config.limits.expectedContainers}). The game may be missing graphics.`, 'warn');
    }

    // The pack worker starts now so SPIR-V goes straight into it as the translators finish.
    const packRemote = workers.spawn('pack');
    await packRemote.call({ type: 'init', urls }, [], 'ready');
    const shaders = await translateStage({ workers, urls, containers, config, signal, progress: stage('translate'), log, packRemote });
    containers.clear();
    if (shaders.ok === 0) throw new UserError('No shader could be translated. The WebAssembly tools may be broken; see the log.', 'shaders');
    if (shaders.failures.length) {
      const first = shaders.failures.slice(0, 5).map((f) => `${f.name} (${f.stage}: ${f.reason})`).join('; ');
      throw new UserError(`${shaders.failures.length} shaders could not be made, so no package was written (the game would miss graphics). ` +
        `First: ${first}. Try again in a fresh browser tab with other tabs closed; if it fails again, report it with the Details log.`, 'shaders-incomplete');
    }

    throwIfCancelled(signal);
    stage('pack')({ done: 0, total: 1, label: 'Packing the shader library' });
    const packed = await packRemote.call({ type: 'pack', names: { package: names.shaders, index: names.shadersIndex } }, [], 'packed');
    if (!packed.ok) throw new UserError(`Packing the shaders failed: ${packed.error}`, 'pack');
    stage('pack')({ done: 1, total: 1, label: `Shader library packed: ${packed.sizes.package.toLocaleString('en-US')} bytes + ${packed.sizes.index.toLocaleString('en-US')} index` });
    log(packed.summary || 'Packed.', 'info');

    // ---- the zip ----
    throwIfCancelled(signal);
    const entries = [
      { name: `${root}/${names.nro}`, size: build.nro.length, chunks: () => bytesOnce(build.nro) },
      { name: `${root}/${names.toml}`, size: build.toml.length, chunks: () => bytesOnce(build.toml) },
      { name: `${root}/${names.shaders}`, size: packed.sizes.package, chunks: () => packageChunks(packRemote, names.shaders, packed.sizes.package, 8 * 1024 * 1024, signal) },
      { name: `${root}/${names.shadersIndex}`, size: packed.sizes.index, chunks: () => packageChunks(packRemote, names.shadersIndex, packed.sizes.index, 8 * 1024 * 1024, signal) },
    ];
    if (mode === 'full') {
      const plan = options.plan ?? planGameFiles(files, config.disc);
      for (const f of plan.copy) {
        entries.push({ name: `${root}/${config.zip.gameRootDir}/${f.path}`, size: f.size, chunks: () => readChunks(f.blob, config.limits.copyChunkBytes, signal) });
      }
    }
    const totalBytes = entries.reduce((a, e) => a + e.size, 0);
    let written = 0;
    const zip = new ZipWriter({ write: async (b) => { await sink.write(b); written += b.length; } });
    let fileNo = 0;
    for (const e of entries) {
      throwIfCancelled(signal);
      const tick = async function* () {
        for await (const c of e.chunks()) {
          yield c;
          stage('zip')({ done: written, total: totalBytes, label: `Writing the zip: file ${fileNo + 1} of ${entries.length}` });
        }
      };
      await zip.addFile(e.name, e.size, tick());
      fileNo++;
    }
    await zip.finish();
    stage('zip')({ done: totalBytes, total: totalBytes, label: 'Finishing the zip' });
    const sinkResult = await sink.close();
    return { zipBytes: zip.bytesWritten, shaders, sinkResult, tag: build.tag };
  } catch (e) {
    try { await sink.abort?.(); } catch { /* ignore */ }
    throw e;
  } finally {
    signal?.removeEventListener('abort', onAbort);
    workers.killAll();
  }
}
