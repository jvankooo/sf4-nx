// SF4's shader containers, found in the browser the way tools/build_shader_library.py finds them natively:
//
//   Shader/*.emb     Capcom EMB archives (tools/extract_shaders.py)
//   default.xex      the 118 containers embedded in the executable (tools/extract_xex_shaders.py scans the loaded
//                    image; here the XEX is decrypted and decompressed into that image first)
//   runtime          Direct3D's own immediate-mode shaders (shaders/sf4_runtime_containers) and masseffect-nx's set
//                    (shaders/me/runtime_containers), published next to the page as runtime_containers.json
//
// Every container is named <vs|ps>_<FNV-1a 64 of its bytes, 16 hex digits>.bin, as the native build names them
// (collect() in build_shader_library.py): the same bytes give the same name, so duplicates collapse.

const SIGNATURE = [0x10, 0x2a, 0x11];

/** FNV-1a 64 of the bytes, as 16 lowercase hex digits. */
export function fnv1a64Hex(bytes) {
  // Two 32-bit halves: (h * prime) mod 2^64 with prime = 2^40 + 0x1B3.
  let hi = 0xCBF29CE4, lo = 0x84222325;
  for (let i = 0; i < bytes.length; i++) {
    lo = (lo ^ bytes[i]) >>> 0;
    // h * 0x1B3 + (h << 40)
    const loMul = lo * 0x1B3;
    const carry = Math.floor(loMul / 0x100000000);
    const newLo = loMul >>> 0;
    const newHi = (Math.imul(hi, 0x1B3) + carry + ((lo << 8) >>> 0)) >>> 0;
    hi = newHi; lo = newLo;
  }
  return hi.toString(16).padStart(8, '0') + lo.toString(16).padStart(8, '0');
}

/** The native name of a container, or null when the bytes are not one (no signature, too short). */
export function containerName(bytes) {
  if (bytes.length < 12 || bytes[0] !== SIGNATURE[0] || bytes[1] !== SIGNATURE[1] || bytes[2] !== SIGNATURE[2]) return null;
  return `${bytes[3] & 1 ? 'vs' : 'ps'}_${fnv1a64Hex(bytes)}.bin`;
}

const be32 = (d, o) => ((d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]) >>> 0;

/**
 * The shader containers of one EMB archive: "#EMB", entry count at +12, entry table at +0x18 (one big-endian
 * (offset, size) pair per entry, the offset counted from the pair itself). Entries whose first word is not
 * 0x102A1100 (pixel) or 0x102A1101 (vertex) are left out, as extract_shaders.py does.
 */
export function embContainers(bytes) {
  if (bytes.length < 0x20 || bytes[0] !== 0x23 || bytes[1] !== 0x45 || bytes[2] !== 0x4d || bytes[3] !== 0x42) {
    throw new Error('not an EMB archive');
  }
  const count = be32(bytes, 12);
  const table = be32(bytes, 0x18);
  const out = [];
  for (let i = 0; i < count; i++) {
    const entry = table + 8 * i;
    if (entry + 8 > bytes.length) break;
    const start = entry + be32(bytes, entry);
    const size = be32(bytes, entry + 4);
    if (start + size > bytes.length || size < 4) continue;
    const word = be32(bytes, start);
    if (word !== 0x102A1100 && word !== 0x102A1101) continue;
    out.push(bytes.slice(start, start + size));
  }
  return out;
}

/**
 * The containers of a loaded image (the XEX's decompressed basefile), with the runtime's layout checks
 * (extract_xex_shaders.py container_size): 24 <= virtual size <= 0x40000, 0 < physical size <= 0x40000, the shader
 * header and its microcode inside the container.
 */
export function imageContainers(image) {
  const out = [];
  const n = image.length;
  for (let i = 0; i + 28 <= n; i++) {
    if (image[i] !== 0x10 || image[i + 1] !== 0x2a || image[i + 2] !== 0x11) continue;
    if (image[i + 3] > 1) continue;
    const virt = be32(image, i + 4), phys = be32(image, i + 8);
    if (!(virt >= 24 && virt <= 0x40000 && phys > 0 && phys <= 0x40000 && i + virt + phys <= n)) continue;
    const header = be32(image, i + 24);
    if (header + 8 > virt) continue;
    const offset = be32(image, i + header), size = be32(image, i + header + 4);
    if (!size || offset + size > phys) continue;
    out.push(image.slice(i, i + virt + phys));
  }
  return out;
}

// ---- XEX2 ---------------------------------------------------------------------------------------------------------
// The retail XEX key (public: Xenia's xex_module.cc). It decrypts the per-title key stored in the security info.
const RETAIL_KEY = new Uint8Array([0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91]);
const DEVKIT_KEY = new Uint8Array(16);

/**
 * AES-128-CBC decryption without padding, with WebCrypto (which only knows PKCS#7): a block that decrypts to a full
 * padding block is appended (E(0x10 x16 XOR last ciphertext block)), so the browser strips exactly that block.
 */
async function aesCbcDecryptRaw(keyBytes, iv, data) {
  if (data.length % 16) throw new Error('encrypted data is not a whole number of AES blocks');
  const subtle = globalThis.crypto.subtle;
  const key = await subtle.importKey('raw', keyBytes, 'AES-CBC', false, ['encrypt', 'decrypt']);
  const last = data.length ? data.subarray(data.length - 16) : iv;
  const pad = new Uint8Array(16).fill(16);
  // encrypt(pad, iv = last) = [E(pad ^ last), E(<padding of its own>)]: the first block is the one wanted.
  const extra = new Uint8Array(await subtle.encrypt({ name: 'AES-CBC', iv: last }, key, pad)).subarray(0, 16);
  const joined = new Uint8Array(data.length + 16);
  joined.set(data, 0);
  joined.set(extra, data.length);
  return new Uint8Array(await subtle.decrypt({ name: 'AES-CBC', iv }, key, joined));
}

/**
 * Decrypts and decompresses a XEX2 into its loaded image (the "basefile"). Supports what SF4's executable uses:
 * normal encryption (AES-128-CBC, IV 0, with the title key from the security info, decrypted with the retail key)
 * and basic compression (blocks of data, each followed by a run of zeros). Returns {image, imageSize, encryption,
 * compression}.
 */
export async function xexImage(xex) {
  if (be32(xex, 0) !== 0x58455832) throw new Error('default.xex is not an XEX2 executable');
  const headerSize = be32(xex, 8);
  const securityOffset = be32(xex, 0x10);
  const optionalCount = be32(xex, 0x14);
  let formatInfo = -1;
  for (let i = 0; i < optionalCount; i++) {
    const key = be32(xex, 0x18 + 8 * i);
    if (key === 0x000003FF) formatInfo = be32(xex, 0x18 + 8 * i + 4);
  }
  if (formatInfo < 0) throw new Error('default.xex has no file format information');
  const infoSize = be32(xex, formatInfo);
  const encryption = (xex[formatInfo + 4] << 8) | xex[formatInfo + 5];
  const compression = (xex[formatInfo + 6] << 8) | xex[formatInfo + 7];
  const imageSize = be32(xex, securityOffset + 4);
  if (compression !== 1 && compression !== 0) {
    throw new Error(`default.xex uses compression ${compression}; only the basic compression of SF4's executable is supported`);
  }
  let data = xex.subarray(headerSize);
  if (encryption === 1) {
    const wrapped = xex.slice(securityOffset + 0x150, securityOffset + 0x160);
    const iv = new Uint8Array(16);
    let lastError = null;
    for (const master of [RETAIL_KEY, DEVKIT_KEY]) {
      try {
        const titleKey = (await aesCbcDecryptRaw(master, iv, wrapped)).subarray(0, 16);
        const whole = data.length - (data.length % 16);
        const plain = await aesCbcDecryptRaw(titleKey, iv, data.subarray(0, whole));
        const image = decompress(plain, xex, formatInfo, infoSize, compression, imageSize);
        if (image[0] === 0x4D && image[1] === 0x5A) return { image, imageSize, encryption, compression }; // "MZ"
        lastError = new Error('decrypted image has no PE header');
      } catch (e) { lastError = e; }
    }
    throw new Error(`default.xex could not be decrypted (${lastError?.message ?? 'unknown error'})`);
  }
  const image = decompress(data, xex, formatInfo, infoSize, compression, imageSize);
  return { image, imageSize, encryption, compression };
}

function decompress(plain, xex, formatInfo, infoSize, compression, imageSize) {
  const image = new Uint8Array(imageSize);
  if (compression === 0) {
    image.set(plain.subarray(0, Math.min(plain.length, imageSize)));
    return image;
  }
  // Basic: after the 8-byte header of the format info, (data size, zero size) pairs.
  let src = 0, dst = 0;
  for (let p = formatInfo + 8; p + 8 <= formatInfo + infoSize; p += 8) {
    const dataSize = be32(xex, p), zeroSize = be32(xex, p + 4);
    if (src + dataSize > plain.length || dst + dataSize + zeroSize > imageSize) throw new Error('basic compression block runs past the data');
    image.set(plain.subarray(src, src + dataSize), dst);
    src += dataSize;
    dst += dataSize + zeroSize; // the zero run is already zero
  }
  return image;
}

/**
 * All of SF4's shader containers from the disc files. files: [{path, size, blob}] re-rooted at the disc root.
 * runtime: {name: Uint8Array} (the published runtime containers). Returns {containers: Map name -> bytes,
 * counts: {emb, xex, runtime, distinct}, log: [string]}.
 */
export async function sf4Containers(files, runtime, { onProgress, signal } = {}) {
  const containers = new Map();
  const log = [];
  const add = (bytes) => {
    const name = containerName(bytes);
    if (name && !containers.has(name)) containers.set(name, bytes);
    return name ? 1 : 0;
  };
  const embs = files.filter((f) => /^shader\/[^/]+\.emb$/i.test(f.path)).sort((a, b) => (a.path < b.path ? -1 : 1));
  if (!embs.length) throw new Error('The disc has no Shader/*.emb archives. Is this a Street Fighter IV disc?');
  const xex = files.find((f) => f.path.toLowerCase() === 'default.xex');
  if (!xex) throw new Error('default.xex was not found on the disc.');
  const steps = embs.length + 2;
  let step = 0;
  const tick = (label) => onProgress?.({ done: ++step, total: steps, label });

  let emb = 0;
  for (const f of embs) {
    if (signal?.aborted) throw Object.assign(new Error('Cancelled'), { name: 'Cancelled' });
    const list = embContainers(new Uint8Array(await f.blob.arrayBuffer()));
    for (const c of list) emb += add(c);
    log.push(`${f.path}: ${list.length} shader containers`);
    tick(`Reading ${f.path}`);
  }
  const { image, encryption, compression } = await xexImage(new Uint8Array(await xex.blob.arrayBuffer()));
  const inImage = imageContainers(image);
  let xexCount = 0;
  for (const c of inImage) xexCount += add(c);
  log.push(`default.xex: decrypted (encryption ${encryption}, compression ${compression}), ${image.length.toLocaleString('en-US')}-byte image, ${inImage.length} embedded shader containers`);
  tick('Reading default.xex');
  let rt = 0;
  for (const bytes of Object.values(runtime ?? {})) rt += add(bytes);
  log.push(`${rt} runtime shader containers (Direct3D immediate mode)`);
  tick('Runtime shaders');
  log.push(`${containers.size} distinct shader containers (${emb} from the EMB archives, ${xexCount} from default.xex, ${rt} runtime)`);
  return { containers, counts: { emb, xex: xexCount, runtime: rt, distinct: containers.size }, log };
}
