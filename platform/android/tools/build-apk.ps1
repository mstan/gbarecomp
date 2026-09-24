<#
.SYNOPSIS
    Builds a gbarecomp Android game APK (debug or release) at BelowNormal
    process priority so it does not swamp the host machine.

.DESCRIPTION
    Thin wrapper around a game repo's gradlew.bat assembleDebug/assembleRelease.
    Any gbarecomp-app.gradle Gradle property (-PgbarecompRoot=, -PrecompUiRoot=,
    -PgbaAbis=, -PgbaNativeJobs=, -PprivateRom=, -PprivateBios=) is forwarded
    when the corresponding parameter is supplied.

.PARAMETER GameAndroidDir
    Path to the game's android/ Gradle project (contains gradlew.bat).

.PARAMETER EngineRoot
    Optional engine checkout override -> -PgbarecompRoot=<path>.

.PARAMETER RecompUiRoot
    Optional recomp-ui checkout override -> -PrecompUiRoot=<path>.

.PARAMETER Abis
    Comma list narrowing the native ABI build -> -PgbaAbis=<list>.
    Default 'arm64-v8a,x86_64' (matches the template's debug default; not
    forwarded unless it differs from that default or -Abis is explicitly
    passed).

.PARAMETER Release
    Build assembleRelease instead of assembleDebug.

.PARAMETER Jobs
    Native compile job pool size -> -PgbaNativeJobs=<n>. Default 8.

.PARAMETER PrivateRom
    Path to a verified ROM dump to embed for a private build -> -PprivateRom=.

.PARAMETER PrivateBios
    Path to a verified BIOS dump to embed for a private build -> -PprivateBios=.

.EXAMPLE
    .\build-apk.ps1 -GameAndroidDir F:\Projects\gbarecomp\EmeraldRecomp-android-touch\android `
        -PrivateRom F:\roms\emerald_usa.gba -PrivateBios F:\bios\gba_bios.bin

.EXAMPLE
    .\build-apk.ps1 -GameAndroidDir ..\..\..\EmeraldRecomp-android-touch\android -Release -Abis arm64-v8a
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameAndroidDir,

    [string]$EngineRoot,
    [string]$RecompUiRoot,
    [string]$Abis,
    [switch]$Release,
    [int]$Jobs = 8,
    [string]$PrivateRom,
    [string]$PrivateBios
)

$ErrorActionPreference = "Stop"

# Builds a Win32 command-line string from logical arguments, each escaped
# per the standard C-runtime argv quoting rules. Needed because
# ProcessStartInfo.ArgumentList is unusable here (see the call site below).
function Add-EscapedArgument {
    param([System.Text.StringBuilder]$Builder, [string]$Value)
    $needsQuotes = ($Value.Length -eq 0) -or ($Value -match '[\s"]')
    if (-not $needsQuotes) { [void]$Builder.Append($Value); return }
    [void]$Builder.Append('"')
    $i = 0
    while ($i -lt $Value.Length) {
        $c = $Value[$i]
        if ($c -eq '\') {
            $n = 0
            while ($i -lt $Value.Length -and $Value[$i] -eq '\') { $n++; $i++ }
            if ($i -eq $Value.Length) {
                [void]$Builder.Append('\' * ($n * 2))
            } elseif ($Value[$i] -eq '"') {
                [void]$Builder.Append('\' * ($n * 2 + 1))
                [void]$Builder.Append('"')
                $i++
            } else {
                [void]$Builder.Append('\' * $n)
            }
        } elseif ($c -eq '"') {
            [void]$Builder.Append('\"')
            $i++
        } else {
            [void]$Builder.Append($c)
            $i++
        }
    }
    [void]$Builder.Append('"')
}

function ConvertTo-WindowsCommandLine {
    param([string[]]$ArgList)
    $sb = New-Object System.Text.StringBuilder
    for ($j = 0; $j -lt $ArgList.Count; $j++) {
        if ($j -gt 0) { [void]$sb.Append(' ') }
        Add-EscapedArgument -Builder $sb -Value $ArgList[$j]
    }
    return $sb.ToString()
}

if (-not $env:ANDROID_HOME -and -not $env:ANDROID_SDK_ROOT) {
    $defaultSdk = "C:\Android\Sdk"
    if (Test-Path -LiteralPath $defaultSdk) {
        Write-Host "ANDROID_HOME/ANDROID_SDK_ROOT unset; defaulting to $defaultSdk"
        $env:ANDROID_HOME = $defaultSdk
        $env:ANDROID_SDK_ROOT = $defaultSdk
    }
} elseif (-not $env:ANDROID_HOME) {
    $env:ANDROID_HOME = $env:ANDROID_SDK_ROOT
} elseif (-not $env:ANDROID_SDK_ROOT) {
    $env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
}

$gameDir = (Resolve-Path -LiteralPath $GameAndroidDir).Path
$gradlew = Join-Path $gameDir "gradlew.bat"
if (-not (Test-Path -LiteralPath $gradlew)) {
    throw "No gradlew.bat found under $gameDir (expected a game's android/ Gradle project)"
}

$task = if ($Release) { "assembleRelease" } else { "assembleDebug" }
$variant = if ($Release) { "release" } else { "debug" }

$gradleArgs = [System.Collections.Generic.List[string]]::new()
$gradleArgs.Add($task)
$gradleArgs.Add("-PgbaNativeJobs=$Jobs")
if ($EngineRoot)   { $gradleArgs.Add("-PgbarecompRoot=$((Resolve-Path -LiteralPath $EngineRoot).Path)") }
if ($RecompUiRoot) { $gradleArgs.Add("-PrecompUiRoot=$((Resolve-Path -LiteralPath $RecompUiRoot).Path)") }
if ($Abis)         { $gradleArgs.Add("-PgbaAbis=$Abis") }
if ($PrivateRom)   { $gradleArgs.Add("-PprivateRom=$((Resolve-Path -LiteralPath $PrivateRom).Path)") }
if ($PrivateBios)  { $gradleArgs.Add("-PprivateBios=$((Resolve-Path -LiteralPath $PrivateBios).Path)") }

Write-Host "Building $task in $gameDir"
Write-Host "  args: $($gradleArgs -join ' ')"

Push-Location $gameDir
try {
    # NOTE: System.Diagnostics.ProcessStartInfo.ArgumentList is left
    # uninitialized (null) on Windows PowerShell 5.1 / .NET Framework --
    # calling .Add() on it throws "cannot call a method on a null-valued
    # expression". Build the Win32 command line ourselves instead (each
    # gradle arg individually quote-escaped), matching what ArgumentList
    # would have produced.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gradlew
    $psi.Arguments = ConvertTo-WindowsCommandLine -ArgList $gradleArgs
    $psi.WorkingDirectory = $gameDir
    $psi.UseShellExecute = $false

    $proc = [System.Diagnostics.Process]::Start($psi)
    try {
        $proc.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal
    } catch {
        Write-Warning "Could not lower process priority: $($_.Exception.Message)"
    }
    $proc.WaitForExit()
    $exitCode = $proc.ExitCode
} finally {
    Pop-Location
}

$apkDir = Join-Path $gameDir "app\build\outputs\apk\$variant"
if (Test-Path -LiteralPath $apkDir) {
    $apks = Get-ChildItem -LiteralPath $apkDir -Filter "*.apk" -Recurse
    foreach ($apk in $apks) {
        $sizeMb = [Math]::Round($apk.Length / 1MB, 2)
        Write-Host "APK: $($apk.FullName) ($sizeMb MB)"
    }
    if ($apks.Count -eq 0) {
        Write-Warning "No .apk files found under $apkDir (build may have failed)"
    }
} else {
    Write-Warning "Expected output directory not found: $apkDir"
}

Write-Host "gradlew exited with code $exitCode"
exit $exitCode
