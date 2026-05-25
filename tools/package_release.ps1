# package_release.ps1 — Build a fully standalone gba.exe.
#
# Static-links SDL2 + libstdc++ + libgcc + libwinpthread so the
# released binary has zero third-party DLL dependencies. The dev
# tree's `build/` keeps using the dynamic link; this script always
# uses a separate `build-release/` so the two don't fight.
#
# Output: F:\Projects\gbarecomp\gbarecomp\gba.exe (or path of choice).
#
# Usage:
#   .\tools\package_release.ps1
#   .\tools\package_release.ps1 -Version v0.1.0 -BuildDir build-release

param(
    [string]$Version  = "v0.1.0",
    [string]$BuildDir = "build-release"
)

$ErrorActionPreference = "Stop"

$Root      = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Join-Path $Root $BuildDir
$ExeOut    = Join-Path $Root "gba.exe"
$MingwBin  = "C:\msys64\mingw64\bin"

$env:PATH = "$MingwBin;$env:PATH"

# Configure (idempotent — only runs if the build dir is fresh).
if (-not (Test-Path (Join-Path $BuildPath "CMakeCache.txt"))) {
    cmake -S $Root -B $BuildPath -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DGBARECOMP_STATIC_RELEASE=ON `
        "-DCMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++"
}

# Build only what we ship.
cmake --build $BuildPath --target bios_smoke

# Strip debug symbols — knocks the static-linked binary from ~6 MB
# down to ~2 MB without affecting behavior.
$BuiltExe = Join-Path $BuildPath "bios_smoke.exe"
& "$MingwBin\strip.exe" $BuiltExe

# Rename to gba.exe and drop in the repo root.
if (Test-Path $ExeOut) {
    Remove-Item -Force $ExeOut
}
Copy-Item $BuiltExe $ExeOut

Write-Host ""
Write-Host "Built standalone: $ExeOut"
Get-Item $ExeOut | Format-List FullName, Length

# Confirm no third-party DLL dependencies sneaked in (only Windows
# system DLLs should appear).
Write-Host ""
Write-Host "DLL imports (should be Windows system DLLs only):"
& "$MingwBin\objdump.exe" -p $ExeOut | Select-String 'DLL Name'
