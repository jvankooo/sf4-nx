# Installer page

A static web page (plain HTML, CSS and ES modules, no build step) that makes the SD card folder of the port from the
user's own Street Fighter IV disc, in the style of the NFSMW-NX and masseffect-nx installers. It is deployed to GitHub
Pages by `.github/workflows/installer.yml`. **Everything runs in the browser: the game files are never uploaded.** The
only downloads are the page, the WebAssembly tools, `sf4-nx.nro` and `sf4.toml`, all from the same site.

The user picks the disc image (`.iso`) or the extracted disc folder; the page checks `default.xex` (SHA-256, see
`config.js` editions) and writes either

* `sf4-nx.zip` (full install): `sf4-nx/{sf4-nx.nro, sf4.toml, sf4_shaders.mesp, sf4_shaders.mesp.idx, game_root/...}`,
  to be extracted into `sdmc:/switch/`, or
* `sf4-nx-update.zip` (update): the same without `game_root/`.

"English only" (on by default) leaves out what `tools/english_only.py` removes: the other languages' folders
(JPN, FRA, GER, ITA, SPA, KOR) and the Japanese voices (`*_J.csb`, `*_J.cpk`; kept with the second checkbox).

## How it works

```
 source (ISO or folder) --> file list [{path, size, lazy Blob}]         js/xdvdfs.js, js/source.js
        |  default.xex --> SHA-256 --> edition (config.js)              js/plan.js
        v
 1 download   releases/sf4-nx.nro + sf4.toml, checked against releases/manifest.json
 2 extract    Shader/*.emb + default.xex (decrypted, decompressed) + runtime set -> 778 containers   js/sf4_shaders.js
 3 translate  container -> HLSL -> SPIR-V                     shader workers (wasm/hlsl.mjs + wasm/dxc_web.mjs)
 4 pack       (container, SPIR-V) pairs -> .mesp + .idx       pack worker   (wasm/pack.mjs)
 5 zip        nro, toml, .mesp, .idx, game_root/...           streaming ZIP64 writer -> disk   js/zip.js, js/sink.js
```

* **Extract** does in the browser what `tools/extract_shaders.py`, `tools/extract_xex_shaders.py` and
  `tools/build_shader_library.py` do natively, and names every container `<vs|ps>_<FNV-1a 64>.bin` as they do. The
  executable is AES-128-CBC encrypted (the title key in its security info, decrypted with the public retail key) and
  uses basic compression (data blocks and zero runs); both are undone with WebCrypto, no WebAssembly. Checked against
  the native build on the World disc: the same 778 containers by name (379 from the EMB archives, 118 from
  default.xex, 301 runtime).
* **Translate** runs masseffect-nx's translator (the one the native build uses) and the pinned DXC build of the
  NFSMW-NX installer. The translator rejects 64 of the 778 containers ("translation rejected: vertex FETCH without a
  declared element" and the like), as it does natively; those are skipped. Any other failure (a DXC error, a crash, a
  timeout) is tried once more in a fresh worker and then stops the run: no package with holes is ever written.
  A complete run gives 714 shaders.
* **Zip** streams to the file the user picks (File System Access API), or to the browser's private storage and then
  a download, so the 4-5 GB zip is never held in memory.

## Deploying

1. Settings > Pages > Source: **GitHub Actions**.
2. Publish a release with `sf4-nx.nro` and `sf4.toml` attached (`build/sf4-nx/sf4-nx.nro`, `app/sf4.toml`).
   Releases trigger the workflow; without one the page deploys and says that no build is published.

The workflow builds `hlsl` and `pack` with Emscripten (`shaders/wasm/build_wasm_tools.sh`), fetches DXC
(`tools/fetch_dxc.sh`, pinned and checksummed), checks that DXC compiles (`tools/wasm_smoke.mjs`) and stages the site
(`tools/stage_site.sh`: the page, the tools, `wasm/runtime_containers.json` from `shaders/sf4_runtime_containers` and
`shaders/me/runtime_containers`, and `releases/` with the NRO and `manifest.json`).

## Local test

With Emscripten active: `shaders/wasm/build_wasm_tools.sh`, `installer/tools/fetch_dxc.sh shaders/wasm/out`, then
`installer/tools/stage_site.sh /tmp/site --wasm shaders/wasm/out --releases <folder with sf4-nx.nro>` and serve
`/tmp/site` over http://localhost (the page needs a secure context).
