# Translates assets/game_root/default.xex to C++ in app/generated/default and runs the post-codegen steps.
#
#   powershell -ExecutionPolicy Bypass -File tools\codegen.ps1 [-Rexglue sdk\out\win-amd64\rexglue.exe]
#
# Steps, as masseffect-nx's tools/codegen.sh:
#   1. rexglue codegen app/sf4_manifest.toml (log in app/codegen.log); -f regenerates every file
#   2. tools/direct_calls.py: calls between recompiled functions without a hook become direct C++ calls
#   3. a check that the generated code has no "Unresolved branch" stubs (a gap declared in the middle of a function)
param([string]$Rexglue = "$PSScriptRoot\..\sdk\out\win-amd64\rexglue.exe")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$app = "$root\app"
Push-Location $app
try {
    $p = Start-Process -FilePath $Rexglue -ArgumentList @('-f', 'codegen', 'sf4_manifest.toml') -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput "$app\codegen.log" -RedirectStandardError "$app\codegen.err"
    if ($p.ExitCode) { throw "codegen failed ($($p.ExitCode)), see $app\codegen.log" }
} finally { Pop-Location }
python "$root\tools\direct_calls.py" --gen "$app\generated\default"
if ($LASTEXITCODE) { throw 'direct_calls.py failed' }
$stubs = (Select-String "$app\generated\default\sf4_recomp.*.cpp" -Pattern 'REX_FATAL\("Unresolved' | Measure-Object).Count
Write-Output "codegen done: $stubs unresolved-branch stubs"
if ($stubs) { exit 2 }
