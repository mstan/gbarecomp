# fetch_tcc.ps1 — fetch + stage the bundled, toolchain-free overlay compiler.
#
# The Stage-2 self-heal tier compiles each healed function's emitted C into a
# cached DLL. On a dev/release box that means g++ (the GBARECOMP_HEAL_BACKEND=gcc
# producer); on a SHIPPED, toolchain-less player box it means TinyCC (tcc) — a
# self-contained C compiler (own linker + headers) that the runtime locates at
# <exe_dir>/overlay_toolchain/tcc/tcc.exe (see overlay_compile.cpp tcc_path()).
#
# A game's release packager calls this to populate that directory, e.g.:
#   .\..\..\gbarecomp\tools\fetch_tcc.ps1 -Toolchain "$Stage\overlay_toolchain"
#
# Mirrors psxrecomp/MegaManX6Recomp/tools/package_release.ps1 (same fixed tcc
# version, same self-contained zip layout: a top-level tcc/ with tcc.exe +
# libtcc.dll + include/ + lib/).
#
# Usage:
#   .\tools\fetch_tcc.ps1 [-Toolchain <dir>] [-CacheDir <dir>]

param(
    # Where to stage the toolchain tree (the runtime looks for <here>/tcc/tcc.exe
    # and <here>/include/*.h).
    [string]$Toolchain  = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot "..")) "overlay_toolchain"),
    # The gbarecomp engine checkout whose shim headers get staged.
    [string]$EngineRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    # Download cache so repeat packaging never re-downloads.
    [string]$CacheDir   = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot "..")) "tools/_toolchain_cache")
)

$ErrorActionPreference = "Stop"

# Fixed version — self-contained win64 build (own linker, bundles its headers).
$TccUrl = "https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win64-bin.zip"

New-Item -ItemType Directory -Force $Toolchain | Out-Null
New-Item -ItemType Directory -Force $CacheDir  | Out-Null

$TccZip = Join-Path $CacheDir "tcc-0.9.27-win64-bin.zip"
if (-not (Test-Path $TccZip)) {
    Write-Host "Downloading tcc 0.9.27 (win64) ..."
    Invoke-WebRequest -Uri $TccUrl -OutFile $TccZip
}

$TccTmp = Join-Path $CacheDir "tcc_extract"
if (Test-Path $TccTmp) { Remove-Item -Recurse -Force $TccTmp }
Expand-Archive -Path $TccZip -DestinationPath $TccTmp -Force

# The zip has a top-level tcc/ dir (tcc.exe + libtcc.dll + include/ + lib/) —
# ship it whole so the runtime finds <Toolchain>/tcc/tcc.exe.
$Dst = Join-Path $Toolchain "tcc"
if (Test-Path $Dst) { Remove-Item -Recurse -Force $Dst }
Copy-Item -Recurse -Force (Join-Path $TccTmp "tcc") $Dst

$TccExe = Join-Path $Dst "tcc.exe"
if (-not (Test-Path $TccExe)) { throw "tcc.exe missing after extract: $TccExe" }
Write-Host "Staged toolchain-free overlay compiler: $TccExe"

# Stage the overlay shim headers the runtime's tcc/gcc command compiles each
# healed function against. On a source-less player box overlay_compile.cpp falls
# back to <exe>/overlay_toolchain/include. The three headers #include each other
# by filename, so flattening them into one dir resolves cleanly.
$Inc = Join-Path $Toolchain "include"
New-Item -ItemType Directory -Force $Inc | Out-Null
$headers = @(
    (Join-Path $EngineRoot "src\runtime\overlay_runtime_arm.h"),
    (Join-Path $EngineRoot "src\runtime\overlay_abi.h"),
    (Join-Path $EngineRoot "src\armv4t\runtime_arm_types.h")
)
foreach ($hh in $headers) {
    if (-not (Test-Path $hh)) { throw "overlay shim header missing: $hh" }
    Copy-Item $hh $Inc -Force
}
Write-Host "Staged overlay shim headers ($($headers.Count)) -> $Inc"
