# Builds the NRO for the Switch: app/out/sw/sf4.nro, and assembles the SD card folder in build/sf4-nx/.
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-MesaSdk <...\opt\devkitpro\portlibs\switch>]
#
# Needs devkitPro, CMake 3.25+, Ninja, the generated code (tools/codegen.ps1) and the Mesa SDK (mesa/README.md).
# CMake runs through Start-Process: in Windows PowerShell 5.1 every line a native program writes to stderr becomes
# an error that stops the script. The output goes to app/out/sw/configure.log, build.log and their .err files.
param(
    [string]$MesaSdk = 'E:/Dev/SHIP/x360sw/mesa-switch/mesa-unified-install/opt/devkitpro/portlibs/switch',
    [int]$Jobs = 8,
    [string]$CMake = 'C:/Program Files/CMake/bin/cmake.exe',
    [string]$Ninja = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe',
    [switch]$Lto
)
$ErrorActionPreference = 'Stop'
$root = (Split-Path -Parent $PSScriptRoot).Replace('\', '/')
$app = "$root/app"
$out = "$app/out/sw"
$MesaSdk = $MesaSdk.Replace('\', '/')
if (-not $env:DEVKITPRO -or $env:DEVKITPRO -like '/*') { $env:DEVKITPRO = 'C:/devkitPro' }

function Invoke-CMake([string]$name, [string[]]$arguments) {
    $quoted = $arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $p = Start-Process -FilePath $CMake -ArgumentList $quoted -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput "$out/$name.log" -RedirectStandardError "$out/$name.err"
    if ($p.ExitCode) { throw "cmake ($name) failed with $($p.ExitCode), see $out/$name.log and $name.err" }
}

New-Item -ItemType Directory -Force $out | Out-Null
if (-not (Test-Path "$out/CMakeCache.txt")) {
    Invoke-CMake 'configure' @('-S', $app, '-B', $out, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_MAKE_PROGRAM=$Ninja",
        "-DCMAKE_TOOLCHAIN_FILE=$root/tools/switch/cmake/switch-devkitA64.cmake", "-DREXSDK_DIR=$root/sdk",
        "-DREXGLUE_SWITCH_NVK_SDK=$MesaSdk", "-DSF4_LTO=$(if ($Lto) { 'ON' } else { 'OFF' })")
}
$start = Get-Date
Invoke-CMake 'build' @('--build', $out, '-j', "$Jobs")
Write-Output "Built $out/sf4.nro in $([int]((Get-Date) - $start).TotalMinutes) min"

# SD card folder: build/sf4-nx/ goes to sdmc:/switch/sf4-nx/.
$dist = "$root/build/sf4-nx"
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item "$out/sf4.nro" "$dist/sf4-nx.nro" -Force
Copy-Item "$app/sf4.toml" "$dist/sf4.toml" -Force
# The native renderer's shader library (tools/build_shader_library.py; made from the disc, so not in the repository).
$library = "$root/assets/shaderlib/sf4_shaders.mesp"
if (Test-Path $library) {
    Copy-Item $library, "$library.idx" $dist -Force
} else {
    Write-Output "No $library (tools/build_shader_library.py): the native renderer will draw nothing"
}
Write-Output "SD folder: $dist"
