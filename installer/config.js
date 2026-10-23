// Everything the installer page needs to know about the project lives here. Change a name, a URL, an edition or a
// wasm file name in this file only; nothing else in the page hard-codes them.

export const CONFIG = {
  project: {
    title: 'Street Fighter IV for Nintendo Switch',
    author: 'jvankooo',
    repoUrl: 'https://github.com/jvankooo/sf4-nx',
    releasesUrl: 'https://github.com/jvankooo/sf4-nx/releases',
    issuesUrl: 'https://github.com/jvankooo/sf4-nx/issues',
  },

  // Names inside the zip. The zip is extracted into sdmc:/switch/, so `root` becomes sdmc:/switch/sf4-nx/.
  zip: {
    root: 'sf4-nx',
    fullName: 'sf4-nx.zip',
    updateName: 'sf4-nx-update.zip',
    gameRootDir: 'game_root',
  },

  // File names inside <zip.root>/. The shader package names are fixed by the port (sf4_shaders.mesp next to the NRO).
  files: {
    nro: 'sf4-nx.nro',
    toml: 'sf4.toml',
    shaders: 'sf4_shaders.mesp',
    shadersIndex: 'sf4_shaders.mesp.idx',
  },

  // Where the page gets the build. GitHub release assets cannot be fetched by a browser from another origin, so the
  // deploy workflow (.github/workflows/installer.yml) copies the latest release's sf4-nx.nro and sf4.toml into the
  // site next to the page and writes releases/manifest.json (size and SHA-256 of each).
  build: {
    siteDir: 'releases/',
    manifest: 'releases/manifest.json',
    toml: 'sf4.toml',
  },

  // Known game editions, recognised by the SHA-256 of default.xex. The NRO is the recompiled program of one exact
  // default.xex: another edition would not run.
  editions: [
    {
      id: 'world',
      name: 'Street Fighter IV (World) (En,Ja,Fr,De,Es,It,Ko)',
      xexSha256: '817ff48d05f7d8a80853aa2634c2832679c53c1d9f4254a76c47b9e4bc58545a',
      header: { titleId: 0x434307E1, mediaId: 0x682D11B5, imageSize: 8060928, entryPoint: 0x822B0FE8 },
      nro: 'sf4-nx.nro',
    },
  ],

  // Which disc files go to <root>/game_root/. Paths are relative to the disc root, matched case-insensitively.
  disc: {
    xex: 'default.xex',
    skip: [
      { prefix: '$SystemUpdate/', why: 'Xbox 360 console system update, never read by the game' },
    ],
    // "English only" (tools/english_only.py): the other languages' text and UI folders and the Japanese voices.
    // The game needs only the ENG folders and the _E voices with the console language English (the port's default).
    otherLanguageDirs: ['JPN', 'FRA', 'GER', 'ITA', 'SPA', 'KOR'],
    japaneseVoice: '_J\\.(csb|cpk)$',
  },

  // The WebAssembly tools (built by shaders/wasm/build_wasm_tools.sh and published by the workflow into wasm/).
  wasm: {
    dir: 'wasm/',
    // Cache tag of the tools (added as ?v= to their URLs): change it whenever the translator or the packer change.
    version: '2026-10-23a',
    hlsl: 'hlsl.mjs',
    dxc: 'dxc_web.mjs',
    pack: 'pack.mjs',
    // shader_common.h of the translator (shaders/me/XenosRecomp/shader_common.h), copied next to the tools.
    shaderCommon: 'shader_common.h',
    // Direct3D's own immediate-mode shaders and masseffect-nx's runtime set (shaders/*runtime_containers).
    runtimeContainers: 'runtime_containers.json',
    extraFiles: ['hlsl.wasm', 'dxc_web.wasm', 'pack.wasm'],
  },

  limits: {
    // Parallel translate+compile workers: min(hardwareConcurrency - 1, this). Each holds a DXC instance (~0.5 GB).
    maxShaderWorkers: 6,
    // A container whose job produces no answer in this time is retried once in a fresh worker.
    jobTimeoutMs: 180000,
    // Read size when copying disc files into the zip.
    copyChunkBytes: 4 * 1024 * 1024,
    // A complete disc: 778 distinct containers, 714 of them translated (the translator rejects 64, as natively).
    expectedContainers: 778,
    expectedShaders: 714,
    // Rough sizes for the up-front estimate.
    expectedShaderBytes: 19 * 1000 * 1000,
    expectedNroBytes: 43 * 1000 * 1000,
    // Without a disk-backed save target the whole zip is built in memory: warn above this size.
    blobWarnBytes: 1.5 * 1024 * 1024 * 1024,
  },
};

export default CONFIG;
