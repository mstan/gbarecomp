<#
.SYNOPSIS
    Drives a gbarecomp Android game on a device/emulator and captures
    validation evidence (screenshots per rotation, crash buffer, staged
    private-storage artifacts) into a JSON summary.

.DESCRIPTION
    Optionally installs an APK, stages a ROM/BIOS into the app's private
    storage, launches the game, walks it through a rotation sequence with a
    screenshot at each step, pulls diagnostic files out of app-private
    storage, and checks Android's crash ring buffer for fatal signals
    belonging to the package. Everything lands under -OutDir, and a
    summary.json ties it together.

    Crash detection reads the ALWAYS-ON crash ring buffer
    (`adb logcat -b crash -d`) as-is; it never clears it first. A clear
    would only lose whatever the buffer already holds from before this run
    -- the dump-as-is approach is the only way to see events that happened
    before this script attached.

    All adb invocations go through Process.Start with a hand-built Win32
    command line (see ConvertTo-WindowsCommandLine below), not PowerShell's
    native `&` argument passing and not PowerShell's `>` redirection.
    Windows PowerShell 5.1 (.NET Framework) leaves
    System.Diagnostics.ProcessStartInfo.ArgumentList uninitialized (null),
    and `adb shell` on Windows rejoins whatever trailing arguments it is
    given with plain spaces before it forwards them to the device -- so a
    single logical argument that itself contains embedded quotes/spaces
    (e.g. a `sh -c "..."` script) must be sent as ONE already-quoted
    command-line argument, never split across several. Splitting it (or
    trusting `&`'s automatic quoting) silently truncates the remote
    command to its first word. This file learned that the hard way; see
    the tools/README.md for the short version.

.PARAMETER Package
    Application id to validate, e.g. com.mstan.emeraldrecomp.

.PARAMETER Apk
    Optional path to an APK to install (adb install -r) before validating.

.PARAMETER Serial
    Optional adb device serial (adb -s). Defaults to whatever a bare `adb`
    command targets (fails if more than one device/emulator is attached).

.PARAMETER Rom
    Optional path to a ROM dump to stage into files/roms/<basename> inside
    the app's private storage via run-as. The basename MUST match what the
    game's manifest expects (e.g. emerald_usa.gba for EmeraldRecomp) --
    this script has no way to read that expectation back out of the
    installed APK, so it trusts the caller's filename. Skipped if a file of
    the same name and byte size is already staged.

.PARAMETER Bios
    Optional path to a 16 KiB GBA BIOS dump, staged to the fixed destination
    files/bios/gba_bios.bin (every gbarecomp game expects that exact name).
    Skipped if already staged with the same byte size.

.PARAMETER Launch
    Starts the launcher activity (resolved live via
    `adb shell cmd package resolve-activity --brief`) before the rotation
    sequence. Without this switch the script observes whatever is already
    on screen -- use that when the game is already running.

.PARAMETER Rotations
    Comma list of rotation steps to walk through, in order. Each of:
    portrait, landscape, reverse-portrait, reverse-landscape. A screenshot
    is captured after each step settles. Omit to skip rotation entirely.

.PARAMETER SettleSeconds
    Seconds to wait after requesting a rotation (or after launch, before the
    first rotation) before capturing its screenshot. Default 4.

.PARAMETER OutDir
    Destination directory for screenshots, pulled files and summary.json.
    Default: a fresh timestamped folder under
    $env:TEMP\gbarecomp-android-validate\.

.PARAMETER ObservePort
    Optional TCP port to `adb forward tcp:<port> tcp:<port>` (e.g. the
    game's debug_port from game_android.toml) so a host-side probe can
    reach the runtime's live debug bridge while this script runs.

.PARAMETER PullArtifacts
    Pulls files/android-runtime.log, files/recomp_master_misses*.toml.frag,
    files/recomp_coverage*.json, and any *.suspend.* markers under files/,
    binary-safe, verifying each pulled file's byte size against the
    on-device size.

.EXAMPLE
    .\android-validate.ps1 -Package com.mstan.emeraldrecomp `
        -Rotations 'portrait,landscape,portrait' -PullArtifacts

.EXAMPLE
    .\android-validate.ps1 -Package com.mstan.emeraldrecomp `
        -Apk .\artifacts\EmeraldRecomp-debug.apk `
        -Rom F:\roms\emerald_usa.gba -Bios F:\bios\gba_bios.bin `
        -Launch -Rotations 'portrait,landscape,reverse-landscape,portrait' `
        -ObservePort 19892 -PullArtifacts
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Package,

    [string]$Apk,
    [string]$Serial,
    [string]$Rom,
    [string]$Bios,
    [switch]$Launch,
    [string]$Rotations = "",
    [int]$SettleSeconds = 4,
    [string]$OutDir,
    [Nullable[int]]$ObservePort,
    [switch]$PullArtifacts
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Win32 command-line construction + adb invocation (see header comment)
# ---------------------------------------------------------------------------

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

$adbPath = (Get-Command adb -ErrorAction Stop).Source

function Get-AdbFullArgList {
    param([string[]]$ArgList)
    $full = @()
    if ($Serial) { $full += @('-s', $Serial) }
    $full += $ArgList
    return $full
}

# Runs adb with ArgList as the logical argv (one array element per logical
# argument -- an element containing spaces/quotes stays grouped, e.g. a
# `sh -c "..."` script must be ONE element, not five). Returns
# { Output = string[]; ExitCode = int }. Never routes through PowerShell's
# `>`/`2>&1`, so adb's stderr chatter can't be mistaken for a tool failure.
function Invoke-Adb {
    param(
        [Parameter(Mandatory = $true)][string[]]$ArgList,
        [switch]$AllowFailure
    )
    $full = Get-AdbFullArgList -ArgList $ArgList
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $adbPath
    $psi.Arguments = ConvertTo-WindowsCommandLine -ArgList $full
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $proc.WaitForExit()
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    $lines = New-Object System.Collections.Generic.List[string]
    if ($stdout) { $lines.AddRange([string[]]($stdout -split "`r?`n")) }
    if ($stderr) { $lines.AddRange([string[]]($stderr -split "`r?`n")) }
    while ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    $code = $proc.ExitCode
    if ($code -ne 0 -and -not $AllowFailure) {
        throw "adb $($ArgList -join ' ') failed (exit $code): $($lines -join [Environment]::NewLine)"
    }
    [pscustomobject]@{ Output = @($lines); ExitCode = $code }
}

# Binary-safe pull of one app-private file: streams adb's raw stdout bytes
# straight to disk (BaseStream, never a text reader), so it can't corrupt
# non-ASCII content the way PowerShell's `>`/Out-File would.
function Save-AppFile {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$DestPath
    )
    $full = Get-AdbFullArgList -ArgList @('exec-out', 'run-as', $Package, 'cat', $RelativePath)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $adbPath
    $psi.Arguments = ConvertTo-WindowsCommandLine -ArgList $full
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $fileStream = [System.IO.File]::Create($DestPath)
    try {
        $proc.StandardOutput.BaseStream.CopyTo($fileStream)
    } finally {
        $fileStream.Dispose()
    }
    $proc.WaitForExit()
    [void]$stderrTask.Result
    return (Test-Path -LiteralPath $DestPath) -and (Get-Item -LiteralPath $DestPath).Length -gt 0
}

function Get-RemoteFileSize {
    param([Parameter(Mandatory = $true)][string]$RelativePath)
    $result = Invoke-Adb -ArgList @('shell', 'run-as', $Package, 'wc', '-c', $RelativePath) -AllowFailure
    if ($result.ExitCode -ne 0 -or $result.Output.Count -eq 0) { return -1 }
    $firstToken = ($result.Output[0].Trim() -split '\s+')[0]
    if ($firstToken -match '^\d+$') { return [int64]$firstToken }
    return -1
}

if (-not $OutDir) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutDir = Join-Path $env:TEMP "gbarecomp-android-validate\$stamp"
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
Write-Host "OutDir: $OutDir"

$summary = [ordered]@{
    package            = $Package
    serial             = $Serial
    outDir             = $OutDir
    timestamp          = (Get-Date).ToString("o")
    apk                = $null
    deviceModel        = $null
    wmSize             = $null
    launcherActivity   = $null
    launched           = $false
    romStaged          = $null
    biosStaged         = $null
    processRunning     = $null
    foregroundActivity = $null
    observePort        = $null
    rotationResults    = @()
    screenshots        = @()
    pulledFiles        = @()
    crashLines         = @()
    failures           = @()
}

# ---------------------------------------------------------------------------
# Device facts
# ---------------------------------------------------------------------------

$summary.deviceModel = ((Invoke-Adb -ArgList @('shell', 'getprop', 'ro.product.model')).Output | Select-Object -First 1)
if ($summary.deviceModel) { $summary.deviceModel = $summary.deviceModel.Trim() }

$wmSizeText = (Invoke-Adb -ArgList @('shell', 'wm', 'size')).Output -join "`n"
$wmMatches = [regex]::Matches($wmSizeText, "(\d+)x(\d+)")
if ($wmMatches.Count -gt 0) {
    $last = $wmMatches[$wmMatches.Count - 1]
    $summary.wmSize = "$($last.Groups[1].Value)x$($last.Groups[2].Value)"
}

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

if ($Apk) {
    $apkPath = (Resolve-Path -LiteralPath $Apk).Path
    Write-Host "Installing $apkPath ..."
    $installResult = Invoke-Adb -ArgList @('install', '-r', $apkPath) -AllowFailure
    $summary.apk = [ordered]@{
        path     = $apkPath
        exitCode = $installResult.ExitCode
        output   = $installResult.Output -join "`n"
    }
    if ($installResult.ExitCode -ne 0) {
        $summary.failures += "adb install failed with exit code $($installResult.ExitCode)"
    }
}

# ---------------------------------------------------------------------------
# Stage ROM / BIOS into app-private storage
# ---------------------------------------------------------------------------

function Stage-PrivateAsset {
    param(
        [Parameter(Mandatory = $true)][string]$LocalPath,
        [Parameter(Mandatory = $true)][string]$RemoteRelativeDir,
        [Parameter(Mandatory = $true)][string]$RemoteFileName
    )
    $local = (Resolve-Path -LiteralPath $LocalPath).Path
    $localSize = (Get-Item -LiteralPath $local).Length
    $remoteRelative = "$RemoteRelativeDir/$RemoteFileName"
    $remoteSize = Get-RemoteFileSize -RelativePath $remoteRelative
    if ($remoteSize -eq $localSize) {
        return [ordered]@{
            local = $local; remote = $remoteRelative
            skipped = $true; reason = "already staged with matching size ($localSize bytes)"
        }
    }
    $tmpRemote = "/data/local/tmp/gbarecomp_validate_$RemoteFileName"
    Invoke-Adb -ArgList @('push', $local, $tmpRemote) | Out-Null
    Invoke-Adb -ArgList @('shell', 'run-as', $Package, 'mkdir', '-p', $RemoteRelativeDir) -AllowFailure | Out-Null
    Invoke-Adb -ArgList @('shell', 'run-as', $Package, 'cp', $tmpRemote, $remoteRelative) | Out-Null
    Invoke-Adb -ArgList @('shell', 'rm', $tmpRemote) -AllowFailure | Out-Null
    $verifySize = Get-RemoteFileSize -RelativePath $remoteRelative
    return [ordered]@{
        local = $local; remote = $remoteRelative
        skipped = $false; localSize = $localSize; remoteSize = $verifySize
        verified = ($verifySize -eq $localSize)
    }
}

if ($Rom) {
    $romName = Split-Path -Leaf $Rom
    $summary.romStaged = Stage-PrivateAsset -LocalPath $Rom -RemoteRelativeDir "files/roms" -RemoteFileName $romName
    if ($summary.romStaged.verified -eq $false) {
        $summary.failures += "ROM stage verification failed for $romName"
    }
}
if ($Bios) {
    $summary.biosStaged = Stage-PrivateAsset -LocalPath $Bios -RemoteRelativeDir "files/bios" -RemoteFileName "gba_bios.bin"
    if ($summary.biosStaged.verified -eq $false) {
        $summary.failures += "BIOS stage verification failed"
    }
}

# ---------------------------------------------------------------------------
# Launcher activity resolution + launch
# ---------------------------------------------------------------------------

$resolveResult = Invoke-Adb -ArgList @('shell', 'cmd', 'package', 'resolve-activity', '--brief', $Package) -AllowFailure
$activityLine = $resolveResult.Output | Where-Object { $_ -match [regex]::Escape($Package) + '/' } | Select-Object -Last 1
if ($activityLine) { $summary.launcherActivity = $activityLine.Trim() }

if ($Launch) {
    if (-not $summary.launcherActivity) {
        $summary.failures += "Could not resolve a launcher activity for $Package; not launched"
    } else {
        Invoke-Adb -ArgList @('shell', 'am', 'force-stop', $Package) -AllowFailure | Out-Null
        $startResult = Invoke-Adb -ArgList @('shell', 'am', 'start', '-n', $summary.launcherActivity) -AllowFailure
        $summary.launched = ($startResult.ExitCode -eq 0)
        if (-not $summary.launched) {
            $summary.failures += "am start failed (exit $($startResult.ExitCode)): $($startResult.Output -join ' ')"
        }
        Start-Sleep -Seconds 3
    }
}

# ---------------------------------------------------------------------------
# ObservePort
# ---------------------------------------------------------------------------

if ($ObservePort) {
    $fwdResult = Invoke-Adb -ArgList @('forward', "tcp:$ObservePort", "tcp:$ObservePort") -AllowFailure
    $summary.observePort = [ordered]@{
        port      = $ObservePort
        forwarded = ($fwdResult.ExitCode -eq 0)
    }
    if ($fwdResult.ExitCode -eq 0) {
        Write-Host "Forwarded tcp:$ObservePort -> device tcp:$ObservePort. Connect a probe to localhost:$ObservePort."
    } else {
        $summary.failures += "adb forward tcp:$ObservePort failed: $($fwdResult.Output -join ' ')"
    }
}

# ---------------------------------------------------------------------------
# Rotation sequence
# ---------------------------------------------------------------------------

function Get-RotationValue {
    param([Parameter(Mandatory = $true)][string]$Name)
    switch ($Name.Trim().ToLowerInvariant()) {
        'portrait'          { return 0 }
        'landscape'         { return 1 }
        'reverse-portrait'  { return 2 }
        'reverse-landscape' { return 3 }
        default { throw "Unknown rotation '$Name' (expected portrait|landscape|reverse-portrait|reverse-landscape)" }
    }
}

function Save-Screenshot {
    param([Parameter(Mandatory = $true)][string]$LocalPath)
    $remote = "/sdcard/gbarecomp_validate_screenshot.png"
    Invoke-Adb -ArgList @('shell', 'screencap', '-p', $remote) | Out-Null
    Invoke-Adb -ArgList @('pull', $remote, $LocalPath) | Out-Null
    Invoke-Adb -ArgList @('shell', 'rm', $remote) -AllowFailure | Out-Null
    return (Test-Path -LiteralPath $LocalPath) -and (Get-Item -LiteralPath $LocalPath).Length -gt 0
}

$rotationList = @($Rotations -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$originalAccelRotation = $null
$originalUserRotation = $null

if ($rotationList.Count -gt 0) {
    $originalAccelRotation = ((Invoke-Adb -ArgList @('shell', 'settings', 'get', 'system', 'accelerometer_rotation') -AllowFailure).Output | Select-Object -First 1)
    $originalUserRotation  = ((Invoke-Adb -ArgList @('shell', 'settings', 'get', 'system', 'user_rotation') -AllowFailure).Output | Select-Object -First 1)
    if ($originalAccelRotation) { $originalAccelRotation = $originalAccelRotation.Trim() }
    if ($originalUserRotation)  { $originalUserRotation  = $originalUserRotation.Trim() }

    try {
        Invoke-Adb -ArgList @('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '0') | Out-Null

        for ($i = 0; $i -lt $rotationList.Count; $i++) {
            $name = $rotationList[$i]
            $value = Get-RotationValue -Name $name
            Invoke-Adb -ArgList @('shell', 'settings', 'put', 'system', 'user_rotation', "$value") | Out-Null
            Start-Sleep -Seconds $SettleSeconds

            $shotPath = Join-Path $OutDir ("rotation-{0:00}-{1}.png" -f ($i + 1), $name)
            $ok = Save-Screenshot -LocalPath $shotPath
            $entry = [ordered]@{
                step              = $i + 1
                rotation          = $name
                userRotationValue = $value
                settleSeconds     = $SettleSeconds
                screenshot        = $shotPath
                captured          = $ok
            }
            $summary.rotationResults += $entry
            $summary.screenshots += [ordered]@{ rotation = $name; path = $shotPath; captured = $ok }
            if (-not $ok) { $summary.failures += "Screenshot capture failed for rotation '$name'" }
        }
    } finally {
        # Restore in the reverse order things were changed: user_rotation
        # first, then accelerometer_rotation -- the field this script is
        # explicitly required to restore, even on failure.
        if ($originalUserRotation -match '^\d+$') {
            Invoke-Adb -ArgList @('shell', 'settings', 'put', 'system', 'user_rotation', $originalUserRotation) -AllowFailure | Out-Null
        }
        if ($originalAccelRotation -match '^\d+$') {
            Invoke-Adb -ArgList @('shell', 'settings', 'put', 'system', 'accelerometer_rotation', $originalAccelRotation) -AllowFailure | Out-Null
        }
    }
} elseif (-not $Launch) {
    # No rotation walk requested: still grab one screenshot of current state.
    $shotPath = Join-Path $OutDir "current-state.png"
    $ok = Save-Screenshot -LocalPath $shotPath
    $summary.screenshots += [ordered]@{ rotation = $null; path = $shotPath; captured = $ok }
}

# ---------------------------------------------------------------------------
# Process / foreground checks
# ---------------------------------------------------------------------------

$pidText = ((Invoke-Adb -ArgList @('shell', 'pidof', $Package) -AllowFailure).Output | Select-Object -First 1)
$summary.processRunning = [bool]($pidText -and $pidText.Trim())

$activitiesDump = (Invoke-Adb -ArgList @('shell', 'dumpsys', 'activity', 'activities') -AllowFailure).Output
$foregroundLine = $activitiesDump | Where-Object { $_ -match 'topResumedActivity=.*' + [regex]::Escape($Package) } | Select-Object -First 1
$summary.foregroundActivity = if ($foregroundLine) { $foregroundLine.Trim() } else { $null }

# ---------------------------------------------------------------------------
# Pull artifacts (binary-safe, size-verified)
# ---------------------------------------------------------------------------

if ($PullArtifacts) {
    # Needs remote shell globbing, so the whole sh -c script must travel as
    # ONE argv element (see header comment) -- built here with its own
    # embedded double quotes, passed straight through, never split.
    $listCommand = "run-as $Package sh -c ""ls -1 files/recomp_master_misses*.toml.frag files/recomp_coverage*.json 2>/dev/null; find files -iname '*.suspend.*' -type f 2>/dev/null"""
    $listResult = Invoke-Adb -ArgList @('shell', $listCommand) -AllowFailure
    $discovered = @($listResult.Output | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_.StartsWith('files/') })

    $relativePaths = @('files/android-runtime.log') + $discovered
    $relativePaths = @($relativePaths | Select-Object -Unique)

    foreach ($relative in $relativePaths) {
        $localName = ($relative -replace '^files/', '') -replace '[\\/]', '__'
        $localPath = Join-Path $OutDir $localName
        $remoteSize = Get-RemoteFileSize -RelativePath $relative
        if ($remoteSize -lt 0) {
            $summary.pulledFiles += [ordered]@{
                remote = $relative; local = $null; remoteBytes = -1; localBytes = -1
                verified = $false; note = "not found on device"
            }
            continue
        }
        $ok = Save-AppFile -RelativePath $relative -DestPath $localPath
        $localSize = if ($ok) { (Get-Item -LiteralPath $localPath).Length } else { -1 }
        $verified = $ok -and ($localSize -eq $remoteSize)
        $summary.pulledFiles += [ordered]@{
            remote = $relative; local = $localPath
            remoteBytes = $remoteSize; localBytes = $localSize
            verified = $verified
        }
        if (-not $verified) { $summary.failures += "Pull/size-mismatch for $relative" }
    }
}

# ---------------------------------------------------------------------------
# Crash ring buffer (query as-is -- never clear-then-run; see script header)
# ---------------------------------------------------------------------------

$crashDump = @((Invoke-Adb -ArgList @('logcat', '-b', 'crash', '-d') -AllowFailure).Output)
for ($i = 0; $i -lt $crashDump.Count; $i++) {
    if ($crashDump[$i] -match 'Fatal signal') {
        $windowStart = [Math]::Max(0, $i - 15)
        $windowEnd = [Math]::Min($crashDump.Count - 1, $i + 5)
        $window = ($crashDump[$windowStart..$windowEnd]) -join "`n"
        if ($window -match [regex]::Escape($Package) -or $window -match 'libmain\.so') {
            $summary.crashLines += $crashDump[$i].Trim()
        }
    }
}
if ($summary.crashLines.Count -gt 0) {
    $summary.failures += "Crash buffer contains $($summary.crashLines.Count) fatal-signal line(s) for $Package"
}

# ---------------------------------------------------------------------------
# Emit summary.json
# ---------------------------------------------------------------------------

$summaryPath = Join-Path $OutDir "summary.json"
$json = $summary | ConvertTo-Json -Depth 8
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($summaryPath, $json, $utf8NoBom)

Write-Host "----- summary.json -----"
Write-Host $json
Write-Host "-------------------------"
Write-Host "Wrote $summaryPath"

if ($summary.failures.Count -gt 0) {
    Write-Warning "Validation completed with $($summary.failures.Count) issue(s):"
    foreach ($f in $summary.failures) { Write-Warning "  - $f" }
    exit 1
}
exit 0
