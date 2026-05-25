# package_release.ps1 — Stage + zip a Windows release of gbarecomp.
#
# Builds bios_smoke (the BIOS canary; offline interpreter oracle per
# PRINCIPLES.md, not the runtime hot path), copies it next to its
# SDL2/mingw runtime dependencies, drops a README + RELEASE_NOTES +
# LICENSE + START_HERE alongside, and zips the lot.
#
# Usage:
#   .\tools\package_release.ps1
#   .\tools\package_release.ps1 -Version v0.1.0 -BuildDir build

param(
    [string]$Version  = "v0.1.0",
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"

$Root      = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Join-Path $Root $BuildDir
$StageRoot = Join-Path $Root "release-stage"
$Stage     = Join-Path $StageRoot ("gbarecomp-{0}-windows-x64" -f $Version)
$ZipPath   = Join-Path $Root ("gbarecomp-{0}-windows-x64.zip" -f $Version)
$MingwBin  = "C:\msys64\mingw64\bin"

$env:PATH = "$MingwBin;$env:PATH"

# Configure / build. If the user already configured with a different
# generator, this is a no-op for them; otherwise we use Ninja for
# parity with the dev loop.
if (-not (Test-Path $BuildPath)) {
    cmake -S $Root -B $BuildPath -G Ninja -DCMAKE_BUILD_TYPE=Release
}
cmake --build $BuildPath --target bios_smoke

# Clean stage area.
if (Test-Path $StageRoot) {
    Remove-Item -Recurse -Force $StageRoot
}
New-Item -ItemType Directory -Force $Stage | Out-Null

# Binary.
Copy-Item (Join-Path $BuildPath "bios_smoke.exe") $Stage

# Top-level docs.
Copy-Item (Join-Path $Root "README.md")        $Stage
Copy-Item (Join-Path $Root "LICENSE")          $Stage
Copy-Item (Join-Path $Root "RELEASE_NOTES.md") $Stage

# Runtime DLLs from mingw64. Every binary built against MSYS2's gcc
# needs these next to the .exe on a vanilla Windows install.
foreach ($Dll in @("SDL2.dll",
                   "libgcc_s_seh-1.dll",
                   "libstdc++-6.dll",
                   "libwinpthread-1.dll")) {
    $Source = Join-Path $MingwBin $Dll
    if (-not (Test-Path $Source)) {
        throw "Required runtime DLL not found: $Source"
    }
    Copy-Item $Source $Stage
}

# START_HERE.txt — short pointer to RELEASE_NOTES for first-launch
# instructions. Plain ASCII so Notepad on a fresh Windows install
# opens it cleanly.
@"
gbarecomp $Version
==================

This release does NOT include the GBA BIOS. You provide your own
gba_bios.bin (16384 bytes; SHA-1 300c20df6731a33952ded8c436f7f186d25d3492;
CRC32 0x21A2AE0A).

First launch:
  1. Run bios_smoke.exe.
  2. A file picker appears. Select your gba_bios.bin.
  3. The runtime hash-verifies. Match -> smoke test runs.
     Mismatch -> warning dialog quoting actual + expected hashes, then
     the smoke test runs anyway so you can validate atypical dumps.
  4. The validated path is remembered in bios.cfg next to bios_smoke.exe.

Delete bios.cfg to pick a different file later, or pass
--bios <path> on the CLI to override.

See RELEASE_NOTES.md for what works, what does not, and what changed
since the previous tag. LICENSE is PolyForm Noncommercial 1.0.0.
"@ | Set-Content -Encoding ASCII (Join-Path $Stage "START_HERE.txt")

# Zip.
if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath

Write-Host ""
Write-Host "Staged: $Stage"
Write-Host "Zipped: $ZipPath"
Write-Host ""
Get-Item $ZipPath | Format-List FullName, Length
