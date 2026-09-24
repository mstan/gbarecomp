<#
.SYNOPSIS
    Ensures the phone + tablet AVDs used for gbarecomp Android validation
    exist and are configured for host-GPU acceleration.

.DESCRIPTION
    Idempotently creates a tablet AVD (default name gbarecomp_tablet_api35)
    on system-images;android-35;google_apis;x86_64, using the 'pixel_tablet'
    device profile when avdmanager offers one, falling back to 'Nexus 10' or
    any other tablet-like profile it can find. Its config.ini is set to
    hw.gpu.enabled=yes, hw.gpu.mode=host, hw.ramSize=4096, hw.keyboard=yes.

    With -FixPhone, applies the same GPU/RAM settings (hw.gpu.enabled=yes,
    hw.gpu.mode=host, hw.ramSize=4096) to an existing phone AVD (default
    Pixel_7_API_35), which by default ships hw.gpu.enabled=no / a smaller
    RAM size.

    With -Start <avdname>, launches that AVD (-no-snapshot-save) and blocks
    until sys.boot_completed=1 or a timeout is hit.

.PARAMETER TabletAvdName
    Name for the tablet AVD to ensure exists. Default gbarecomp_tablet_api35.

.PARAMETER SystemImage
    sdkmanager-style system image id. Default
    system-images;android-35;google_apis;x86_64.

.PARAMETER FixPhone
    Also apply hw.gpu.enabled=yes / hw.gpu.mode=host / hw.ramSize=4096 to
    -PhoneAvdName's config.ini.

.PARAMETER PhoneAvdName
    Phone AVD to fix when -FixPhone is given. Default Pixel_7_API_35.

.PARAMETER Start
    Name of an AVD to launch and wait for boot. Do not use this while
    another emulator instance you care about is already running unless you
    intend to run two side by side.

.PARAMETER BootTimeoutSeconds
    Max seconds to wait for -Start's sys.boot_completed. Default 180.

.EXAMPLE
    .\avd-bootstrap.ps1
    Creates the tablet AVD only (idempotent no-op if it already exists).

.EXAMPLE
    .\avd-bootstrap.ps1 -FixPhone
    Also fixes Pixel_7_API_35's GPU/RAM settings.

.EXAMPLE
    .\avd-bootstrap.ps1 -Start gbarecomp_tablet_api35
    Creates the tablet AVD if needed, then boots it and waits.
#>
[CmdletBinding()]
param(
    [string]$TabletAvdName = "gbarecomp_tablet_api35",
    [string]$SystemImage = "system-images;android-35;google_apis;x86_64",
    [switch]$FixPhone,
    [string]$PhoneAvdName = "Pixel_7_API_35",
    [string]$Start,
    [int]$BootTimeoutSeconds = 180
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Win32 command-line construction + process invocation
#
# System.Diagnostics.ProcessStartInfo.ArgumentList is left uninitialized
# (null) on Windows PowerShell 5.1 / .NET Framework, and PowerShell's own
# native `&` argument passing does not reliably preserve a single logical
# argument that itself contains embedded spaces/quotes (e.g. a device
# profile name, or a `sh -c "..."` script). Build the Win32 command line by
# hand instead, and drive avdmanager/adb via Process.Start directly so
# stdin (the avdmanager "custom hardware profile? [no]" prompt) and
# stdout/stderr are fully under our control.
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

function Invoke-Process {
    param(
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)][string[]]$ArgList,
        [string]$StdinText,
        [switch]$AllowFailure
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FileName
    $psi.Arguments = ConvertTo-WindowsCommandLine -ArgList $ArgList
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    if ($PSBoundParameters.ContainsKey('StdinText')) { $psi.RedirectStandardInput = $true }
    $proc = [System.Diagnostics.Process]::Start($psi)
    if ($PSBoundParameters.ContainsKey('StdinText')) {
        $proc.StandardInput.Write($StdinText)
        $proc.StandardInput.Close()
    }
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
        throw "$FileName $($ArgList -join ' ') failed (exit $code): $($lines -join [Environment]::NewLine)"
    }
    [pscustomobject]@{ Output = @($lines); ExitCode = $code }
}

$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { "C:\Android\Sdk" }
$avdManager = Join-Path $sdkRoot "cmdline-tools\latest\bin\avdmanager.bat"
$emulatorExe = Join-Path $sdkRoot "emulator\emulator.exe"
$adbPath = (Get-Command adb -ErrorAction Stop).Source
if (-not (Test-Path -LiteralPath $avdManager)) { throw "avdmanager.bat not found at $avdManager" }
if (-not (Test-Path -LiteralPath $emulatorExe)) { throw "emulator.exe not found at $emulatorExe" }

$avdHome = if ($env:ANDROID_AVD_HOME) { $env:ANDROID_AVD_HOME } else { Join-Path $env:USERPROFILE ".android\avd" }

function Test-AvdExists {
    param([Parameter(Mandatory = $true)][string]$Name)
    return Test-Path -LiteralPath (Join-Path $avdHome "$Name.ini")
}

function Set-ConfigValues {
    param(
        [Parameter(Mandatory = $true)][string]$ConfigPath,
        [Parameter(Mandatory = $true)][hashtable]$Values
    )
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.AddRange([string[]](Get-Content -LiteralPath $ConfigPath))
    foreach ($key in $Values.Keys) {
        $value = $Values[$key]
        $pattern = "^$([regex]::Escape($key))="
        $found = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match $pattern) {
                $lines[$i] = "$key=$value"
                $found = $true
                break
            }
        }
        if (-not $found) { $lines.Add("$key=$value") }
    }
    $text = ($lines -join "`n") + "`n"
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($ConfigPath, $text, $utf8NoBom)
}

function Find-TabletDeviceProfile {
    $listing = (Invoke-Process -FileName $avdManager -ArgList @('list', 'device') -AllowFailure).Output
    if ($listing -match '"pixel_tablet"') { return "pixel_tablet" }
    if ($listing -match '"Nexus 10"') { return "Nexus 10" }
    for ($i = 0; $i -lt $listing.Count; $i++) {
        if ($listing[$i] -match 'id:\s*\d+\s+or\s+"([^"]+)"') {
            $candidateId = $Matches[1]
            $nameLine = if ($i + 1 -lt $listing.Count) { $listing[$i + 1] } else { "" }
            if ($nameLine -match 'Name:\s*(.+)$' -and $Matches[1] -match '(?i)tablet') {
                return $candidateId
            }
        }
    }
    return $null
}

# ---------------------------------------------------------------------------
# Tablet AVD (idempotent create)
# ---------------------------------------------------------------------------

if (Test-AvdExists -Name $TabletAvdName) {
    Write-Host "Tablet AVD '$TabletAvdName' already exists; skipping create."
} else {
    $profile = Find-TabletDeviceProfile
    if ($profile) {
        Write-Host "Creating '$TabletAvdName' with device profile '$profile' on $SystemImage ..."
    } else {
        Write-Warning "No tablet-like device profile found via 'avdmanager list device'; creating '$TabletAvdName' with avdmanager's default profile."
    }

    $createArgs = @('create', 'avd', '-n', $TabletAvdName, '-k', $SystemImage)
    if ($profile) { $createArgs += @('-d', $profile) }

    # avdmanager prompts "Do you wish to create a custom hardware profile? [no]"
    # -- answer no and take the system image's / device profile's defaults.
    $createResult = Invoke-Process -FileName $avdManager -ArgList $createArgs -StdinText "no`n" -AllowFailure
    $createResult.Output | ForEach-Object { Write-Host $_ }
    if ($createResult.ExitCode -ne 0) {
        throw "avdmanager create avd failed (exit $($createResult.ExitCode)) for '$TabletAvdName'"
    }
    if (-not (Test-AvdExists -Name $TabletAvdName)) {
        throw "avdmanager reported success but '$TabletAvdName' was not registered under $avdHome"
    }

    $configPath = Join-Path $avdHome "$TabletAvdName.avd\config.ini"
    if (-not (Test-Path -LiteralPath $configPath)) {
        throw "Created '$TabletAvdName' but its config.ini is missing at $configPath"
    }
    Set-ConfigValues -ConfigPath $configPath -Values @{
        "hw.gpu.enabled" = "yes"
        "hw.gpu.mode"    = "host"
        "hw.ramSize"     = "4096"
        "hw.keyboard"    = "yes"
    }
    Write-Host "Tablet AVD '$TabletAvdName' created and configured ($configPath)."
}

# ---------------------------------------------------------------------------
# Phone AVD fix-up (opt-in)
# ---------------------------------------------------------------------------

if ($FixPhone) {
    if (-not (Test-AvdExists -Name $PhoneAvdName)) {
        Write-Warning "-FixPhone requested but AVD '$PhoneAvdName' does not exist under $avdHome; skipping."
    } else {
        $configPath = Join-Path $avdHome "$PhoneAvdName.avd\config.ini"
        if (-not (Test-Path -LiteralPath $configPath)) {
            Write-Warning "'$PhoneAvdName' has no config.ini at $configPath; skipping."
        } else {
            Set-ConfigValues -ConfigPath $configPath -Values @{
                "hw.gpu.enabled" = "yes"
                "hw.gpu.mode"    = "host"
                "hw.ramSize"     = "4096"
            }
            Write-Host "Applied GPU/RAM settings to '$PhoneAvdName' ($configPath)."
        }
    }
}

# ---------------------------------------------------------------------------
# Start + wait for boot (opt-in)
# ---------------------------------------------------------------------------

if ($Start) {
    if (-not (Test-AvdExists -Name $Start)) {
        throw "-Start requested for '$Start' but no such AVD is registered under $avdHome"
    }

    function Get-EmulatorSerials {
        $out = (Invoke-Process -FileName $adbPath -ArgList @('devices') -AllowFailure).Output
        return @($out | Select-String '^emulator-\d+\s+device' | ForEach-Object { ($_ -split '\s+')[0] })
    }

    $before = Get-EmulatorSerials
    Write-Host "Launching '$Start' (-no-snapshot-save) ..."
    Start-Process -FilePath $emulatorExe -ArgumentList @('-avd', $Start, '-no-snapshot-save') -WindowStyle Minimized | Out-Null

    $serial = $null
    $enumerateDeadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $enumerateDeadline -and -not $serial) {
        Start-Sleep -Seconds 2
        $current = Get-EmulatorSerials
        $new = $current | Where-Object { $before -notcontains $_ }
        if ($new) { $serial = $new | Select-Object -First 1 }
    }
    if (-not $serial) { throw "New emulator instance for '$Start' did not enumerate over adb within 60s" }
    Write-Host "New instance serial: $serial. Waiting for device + boot..."

    Invoke-Process -FileName $adbPath -ArgList @('-s', $serial, 'wait-for-device') | Out-Null
    $bootDeadline = (Get-Date).AddSeconds($BootTimeoutSeconds)
    $booted = $false
    while ((Get-Date) -lt $bootDeadline) {
        $val = ((Invoke-Process -FileName $adbPath -ArgList @('-s', $serial, 'shell', 'getprop', 'sys.boot_completed') -AllowFailure).Output | Select-Object -First 1)
        if ($val -and $val.ToString().Trim() -eq '1') { $booted = $true; break }
        Start-Sleep -Seconds 3
    }
    if (-not $booted) {
        throw "'$Start' ($serial) did not report sys.boot_completed within $BootTimeoutSeconds s"
    }
    Write-Host "'$Start' booted (serial $serial)."
}

Write-Host "avd-bootstrap complete."
