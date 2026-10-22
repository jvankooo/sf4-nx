#!/usr/bin/env python3
"""Builds the shader library of the native renderer on Windows: SF4's shader containers -> HLSL -> SPIR-V -> package.

The same pipeline as masseffect-nx (shaders/me/README.md), whose bash scripts need macOS or Linux:

    containers: assets/shaders (tools/extract_shaders.py, Shader/*.emb), assets/shaders_xex (tools/extract_xex_shaders.py,
                the 118 embedded in the executable), assets/shaders_runtime (Direct3D's own immediate-mode shaders
                as discovered at run time: MASSEFFECT_SHADER_MISSING), shaders/me/runtime_containers (masseffect-nx's)
        | out/tools/xenos_hlsl.exe    XenosRecomp with masseffect-nx's changes, one process per container
        v
    HLSL  ->  tools/dxc/bin/x64/dxc.exe (-spirv, Vulkan 1.2, the flags of shaders/me/tools/compile_spirv_one.sh)
        v
    SPIR-V  ->  out/tools/me_pack_shaders.exe  ->  sf4_shaders.mesp + sf4_shaders.mesp.idx

    python tools/build_shader_library.py [out=assets/shaderlib]

The two tools are built by tools/build_shader_tools.sh. <out> is made from the user's disc: keep it out of any
repository (assets/ is ignored). Rerunning reuses the HLSL and SPIR-V already in <out> and rewrites the package.
"""
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XENOS_HLSL = os.path.join(ROOT, "out", "tools", "xenos_hlsl.exe")
PACK = os.path.join(ROOT, "out", "tools", "me_pack_shaders.exe")
COMMON = os.path.join(ROOT, "shaders", "me", "XenosRecomp", "shader_common.h")
RUNTIME = os.path.join(ROOT, "shaders", "me", "runtime_containers")
DXC = os.path.join(ROOT, "tools", "dxc", "bin", "x64", "dxc.exe")
SPIRV_VAL = shutil.which("spirv-val")


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def collect(sources, out):
    """Copies every container once, named <vs|ps>_<FNV-1a of the bytes>.bin (the stage prefix is what the tools read)."""
    os.makedirs(out, exist_ok=True)
    seen = set()
    for src in sources:
        for name in sorted(os.listdir(src)):
            if not name.endswith(".bin"):
                continue
            data = open(os.path.join(src, name), "rb").read()
            if len(data) < 12 or data[:3] != b"\x10\x2a\x11":
                continue
            stage = "vs" if data[3] & 1 else "ps"
            target = f"{stage}_{fnv1a64(data):016x}.bin"
            if target in seen:
                continue
            seen.add(target)
            with open(os.path.join(out, target), "wb") as f:
                f.write(data)
    return sorted(seen)


def translate(container, hlsl_dir):
    name = os.path.basename(container)[:-4]
    if os.path.exists(os.path.join(hlsl_dir, name + ".hlsl")):
        return name, 0, ""
    with tempfile.TemporaryDirectory() as t:
        shutil.copy(container, os.path.join(t, name + ".bin"))
        r = subprocess.run([XENOS_HLSL, t, os.path.join(t, "o"), COMMON], capture_output=True, text=True)
        h = os.path.join(t, "o", name + ".hlsl")
        if r.returncode == 0 and os.path.exists(h):
            shutil.move(h, os.path.join(hlsl_dir, name + ".hlsl"))
            return name, 0, ""
        return name, r.returncode or 1, (r.stderr or r.stdout)[-400:]


def compile_spirv(hlsl, spirv_dir):
    name = os.path.basename(hlsl)[:-5]
    spv = os.path.join(spirv_dir, name + ".spv")
    err = os.path.join(spirv_dir, name + ".err")
    if os.path.exists(spv):
        return name, True
    profile, extra = ("ps_6_6", []) if name.startswith("ps_") else ("vs_6_6", ["-fvk-invert-y"])
    r = subprocess.run([DXC, "-spirv", "-T", profile, "-E", "main", "-HV", "2021", "-fspv-target-env=vulkan1.2",
                        "-fvk-use-dx-layout", "-Werror=parameter-usage", *extra, "-Fo", spv, hlsl],
                       capture_output=True, text=True)
    ok = r.returncode == 0 and os.path.exists(spv)
    message = r.stderr
    if ok and SPIRV_VAL:
        v = subprocess.run([SPIRV_VAL, "--target-env", "vulkan1.2", "--scalar-block-layout", spv],
                           capture_output=True, text=True)
        ok = v.returncode == 0
        message += v.stderr
    if ok:
        if os.path.exists(err):
            os.remove(err)
    else:
        if os.path.exists(spv):
            os.remove(spv)
        with open(err, "w") as f:
            f.write(message)
    return name, ok


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "assets", "shaderlib")
    sources = [os.path.join(ROOT, "assets", name) for name in ("shaders", "shaders_xex", "shaders_runtime")]
    sources = [s for s in sources if os.path.isdir(s)] + [RUNTIME]
    for tool in (XENOS_HLSL, PACK, DXC):
        if not os.path.exists(tool):
            sys.exit(f"missing {tool} (tools/build_shader_tools.sh builds the first two)")
    containers = os.path.join(out, "containers")
    hlsl = os.path.join(out, "hlsl")
    spirv = os.path.join(out, "spirv")
    os.makedirs(hlsl, exist_ok=True)
    os.makedirs(spirv, exist_ok=True)

    names = collect(sources, containers)
    print(f"{len(names)} distinct containers ({sum(n.startswith('vs_') for n in names)} vertex)")

    failed = []
    with cf.ThreadPoolExecutor(os.cpu_count() or 8) as ex:
        for name, rc, message in ex.map(lambda n: translate(os.path.join(containers, n), hlsl), names):
            if rc:
                failed.append(f"{name} {rc} {message.strip()}")
    with open(os.path.join(out, "translate_failed.txt"), "w") as f:
        f.write("\n".join(failed) + ("\n" if failed else ""))
    print(f"HLSL: {len(names) - len(failed)} translated, {len(failed)} failed (translate_failed.txt)")

    files = sorted(os.path.join(hlsl, f) for f in os.listdir(hlsl) if f.endswith(".hlsl"))
    with cf.ThreadPoolExecutor(os.cpu_count() or 8) as ex:
        results = list(ex.map(lambda f: compile_spirv(f, spirv), files))
    bad = [n for n, ok in results if not ok]
    print(f"SPIR-V: {len(results) - len(bad)} compiled, {len(bad)} failed (spirv/*.err)"
          f"{'' if SPIRV_VAL else '; spirv-val not found, not validated'}")

    package = os.path.join(out, "sf4_shaders.mesp")
    for path in (package, package + ".idx"):
        if os.path.exists(path):
            os.remove(path)
    r = subprocess.run([PACK, containers, spirv, package], capture_output=True, text=True)
    print((r.stdout + r.stderr).strip())
    if r.returncode != 0 or not os.path.exists(package):
        sys.exit("packing failed")
    print(f"{package}: {os.path.getsize(package) // 1024} KB, index {os.path.getsize(package + '.idx') // 1024} KB")


if __name__ == "__main__":
    main()
