// Pure logic: which disc files are copied, how the disc root is found in a folder pick, which edition it is.

export function normalise(path) {
  return path.replace(/\\/g, '/').replace(/^\/+/, '');
}

/**
 * A folder pick may be the disc root itself or its parent. Finds the shallowest default.xex and re-roots the
 * file list there. files: [{path, ...}] -> {files (re-rooted), prefix} or null when there is no default.xex.
 */
export function findDiscRoot(files, xexName) {
  const want = xexName.toLowerCase();
  let best = null;
  for (const f of files) {
    const parts = normalise(f.path).split('/');
    if (parts[parts.length - 1].toLowerCase() !== want) continue;
    if (best === null || parts.length < best.length) best = parts;
  }
  if (!best) return null;
  const prefix = best.slice(0, -1).join('/');
  const cut = prefix ? prefix.length + 1 : 0;
  const lower = prefix.toLowerCase();
  const out = [];
  for (const f of files) {
    const p = normalise(f.path);
    if (prefix && !p.toLowerCase().startsWith(`${lower}/`)) continue;
    out.push({ ...f, path: p.slice(cut) });
  }
  return { files: out, prefix };
}

/**
 * Splits the disc files into those copied to game_root and those left out (with the reason).
 * options: { englishOnly, keepJapaneseVoices } (tools/english_only.py).
 */
export function planGameFiles(files, disc, { englishOnly = true, keepJapaneseVoices = false } = {}) {
  const copy = [];
  const skipped = [];
  const languages = new Set((disc.otherLanguageDirs ?? []).map((d) => d.toUpperCase()));
  const japanese = disc.japaneseVoice ? new RegExp(disc.japaneseVoice, 'i') : null;
  for (const f of files) {
    const lower = f.path.toLowerCase();
    const rule = disc.skip.find((s) => lower.startsWith(s.prefix.toLowerCase()));
    if (rule) { skipped.push({ file: f, why: rule.why }); continue; }
    if (englishOnly) {
      const dirs = f.path.split('/').slice(0, -1);
      if (dirs.some((d) => languages.has(d.toUpperCase()))) { skipped.push({ file: f, why: 'another language (English only)' }); continue; }
      if (!keepJapaneseVoices && japanese?.test(f.path)) { skipped.push({ file: f, why: 'Japanese voice (English only)' }); continue; }
    }
    copy.push(f);
  }
  const sum = (list, get) => list.reduce((a, x) => a + get(x), 0);
  return { copy, skipped, copyBytes: sum(copy, (f) => f.size), skippedBytes: sum(skipped, (s) => s.file.size) };
}

export function findEdition(editions, sha256) {
  const h = sha256.toLowerCase();
  return editions.find((e) => (Array.isArray(e.xexSha256) ? e.xexSha256 : [e.xexSha256]).some((x) => x?.toLowerCase() === h)) ?? null;
}

export async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

export function formatBytes(n) {
  if (n < 1e3) return `${n} B`;
  if (n < 1e6) return `${(n / 1e3).toFixed(1)} KB`;
  if (n < 1e9) return `${(n / 1e6).toFixed(1)} MB`;
  return `${(n / 1e9).toFixed(2)} GB`;
}

export function formatDuration(seconds) {
  if (!isFinite(seconds) || seconds < 0) return '';
  const s = Math.round(seconds);
  if (s < 60) return `${s} s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min ${String(s % 60).padStart(2, '0')} s`;
  return `${Math.floor(m / 60)} h ${String(m % 60).padStart(2, '0')} min`;
}
