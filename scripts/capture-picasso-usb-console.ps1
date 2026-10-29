#Requires -Version 5.1

<#
.SYNOPSIS
Automatically find the Picasso USB console and save its log on Windows.
.DESCRIPTION
Run without arguments, or double-click the repository capture launcher.
Matches the Picasso USB identity, waits indefinitely and reconnects even if
its COM number changes. Each block is flushed to disk before async preview.
Close the window or press Ctrl+C to stop; saving does not wait for shutdown.
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\capture-picasso-usb-console.ps1
#>
[CmdletBinding()]
param(
    [switch]$List,
    [ValidatePattern('(?i)^COM[1-9][0-9]*$')][string]$Port,
    [ValidateRange(0, 86400)][int]$Wait = 0,
    [ValidateRange(0, 86400)][int]$Duration = 0,
    [switch]$Reconnect = $true,
    [string]$Output,
    [switch]$NoStdout,
    [switch]$ConfigureLine
)

function Get-PicassoPorts {
    $present = @([System.IO.Ports.SerialPort]::GetPortNames())
    Get-CimInstance Win32_SerialPort -ErrorAction Stop | ForEach-Object {
        if ($present -contains $_.DeviceID) {
            [pscustomobject]@{ Port = $_.DeviceID; PNPDeviceID = $_.PNPDeviceID; Name = $_.Name }
        }
    }
}

function Select-PicassoPort {
    param([object[]]$Devices, [string]$Port, [string]$PNPDeviceID)
    $candidates = @($Devices | Where-Object {
        if ($Port) { $_.Port -eq $Port }
        elseif ($PNPDeviceID) { $_.PNPDeviceID -eq $PNPDeviceID }
        else { $_.PNPDeviceID -match '^USB\\VID_1209&PID_0001(?:&[^\\]+)?\\PICASSO[0-9]+$' }
    })
    if ($candidates.Count -gt 1) {
        throw 'More than one Picasso console is connected. Select one with -Port COMx.'
    }
    if ($candidates.Count -eq 1) { return $candidates[0] }
}

function Open-CapturePort {
    param([object]$Device, [switch]$ConfigureLine)
    return [PicassoCapture.NativeSerial]::new($Device.Port, [bool]$ConfigureLine)
}

function Test-CapturePortPresent {
    param([string]$Port)
    return @([System.IO.Ports.SerialPort]::GetPortNames()) -contains $Port
}

function Get-CaptureError {
    param($ErrorRecord)
    $cause = $ErrorRecord.Exception
    while ($cause.InnerException) { $cause = $cause.InnerException }
    return $cause.Message
}

function Write-CaptureStatus {
    param([string]$Path, [string]$Message, $Preview)
    $line = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff') + ' ' + $Message + [Environment]::NewLine
    [System.IO.File]::AppendAllText($Path, $line, [System.Text.Encoding]::UTF8)
    if ($null -ne $Preview) {
        $encoded = [System.Text.Encoding]::UTF8.GetBytes($line)
        $null = $Preview.Enqueue($encoded, $encoded.Length)
    } else { [Console]::Error.WriteLine($Message) }
}

function Invoke-PicassoCapture {
    [CmdletBinding()]
    param(
        [string]$Port, [int]$Wait = 0, [int]$Duration = 0,
        [bool]$Reconnect = $true, [string]$Output,
        [switch]$NoStdout, [switch]$ConfigureLine
    )
    $ErrorActionPreference = 'Stop'
    if ($env:OS -ne 'Windows_NT') { throw 'This script requires Windows.' }
    if (-not ('PicassoCapture.NativeSerial' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'picasso-windows-serial.cs')
    }
    if (-not $Output) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
        $Output = Join-Path $PSScriptRoot "../out/picasso/logs/picasso-windows-$stamp.log"
    }
    $logPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
    $statusPath = $logPath + '.session.txt'
    $connection = $null
    $log = $null
    $guard = $null
    $preview = $null
    $captureClock = $null
    $initialClock = [System.Diagnostics.Stopwatch]::StartNew()
    $identity = $null
    $device = $null
    $lastMessage = $null
    $lastAnnouncedDevice = $null
    $lastPortCheck = 0.0
    $captureExit = 0
    $buffer = New-Object byte[] 65536
    $instanceLock = New-Object System.Threading.Mutex($false, 'Local\PicassoUsbLogCapture')
    $ownsLock = $false
    try {
        try { $ownsLock = $instanceLock.WaitOne(0) }
        catch [System.Threading.AbandonedMutexException] { $ownsLock = $true }
        if (-not $ownsLock) { throw 'A Picasso capture window is already running. Use that window.' }
        $log = [PicassoCapture.DurableLog]::new($logPath)
        $guard = [PicassoCapture.CloseGuard]::new($log)
        if (-not $NoStdout) { $preview = [PicassoCapture.PreviewWriter]::new() }
        Write-CaptureStatus $statusPath "Log: $logPath" $preview
        Write-CaptureStatus $statusPath 'Auto-save is active. Close this window or press Ctrl+C to stop.' $preview
        Write-CaptureStatus $statusPath 'Waiting for Picasso USB; reconnect is automatic. Start this before booting the phone.' $preview
        while (-not $guard.StopRequested) {
            if ($null -ne $captureClock -and $Duration -gt 0 -and
                $captureClock.Elapsed.TotalSeconds -ge $Duration) { break }
            if ($null -eq $connection) {
                $device = Select-PicassoPort -Devices @(Get-PicassoPorts) -Port $Port -PNPDeviceID $identity
                $message = 'Waiting for the Picasso USB console...'
                if ($null -ne $device) {
                    # Bind identity before Open, including when Open fails.
                    if (-not $identity) { $identity = $device.PNPDeviceID }
                    $message = "Found $($device.Port): $($device.PNPDeviceID); opening..."
                    $deviceKey = $device.Port + '|' + $device.PNPDeviceID
                    if ($deviceKey -ne $lastAnnouncedDevice) {
                        Write-CaptureStatus $statusPath $message $preview
                        $lastAnnouncedDevice = $deviceKey
                    }
                    try { $connection = Open-CapturePort -Device $device -ConfigureLine:$ConfigureLine }
                    catch { $message = 'Open failed: ' + (Get-CaptureError $_) }
                }
                if ($null -eq $connection) {
                    if ($message -ne $lastMessage) {
                        Write-CaptureStatus $statusPath $message $preview
                        $lastMessage = $message
                    }
                    if ($null -eq $captureClock -and $Wait -gt 0 -and
                        $initialClock.Elapsed.TotalSeconds -ge $Wait) {
                        throw 'Initial wait expired; no device connection was established. See the session log.'
                    }
                    Start-Sleep -Milliseconds 250
                    continue
                }
                if ($null -eq $captureClock) { $captureClock = [System.Diagnostics.Stopwatch]::StartNew() }
                $lastPortCheck = $captureClock.Elapsed.TotalSeconds
                $lastMessage = $null
                Write-CaptureStatus $statusPath "Connected to $($device.Port). Receiving and saving raw bytes." $preview
            }
            $readError = $null
            $received = 0
            try { $received = $connection.Read($buffer, 500) }
            catch { $readError = Get-CaptureError $_ }
            if ($received -gt 0) {
                # Disk is authoritative; a full preview queue never drops raw log bytes.
                $log.Write($buffer, $received)
                if ($null -ne $preview) { $null = $preview.Enqueue($buffer, $received) }
            }
            if ($captureClock.Elapsed.TotalSeconds - $lastPortCheck -ge 1) {
                $lastPortCheck = $captureClock.Elapsed.TotalSeconds
                if (-not (Test-CapturePortPresent $device.Port)) { $readError = "$($device.Port) disappeared" }
            }
            if ($null -ne $readError) {
                Write-CaptureStatus $statusPath "Disconnected: $readError" $preview
                try { $connection.Dispose() } catch { }
                $connection = $null
                if (-not $Reconnect) { throw $readError }
                Start-Sleep -Milliseconds 250
            }
        }
    } catch {
        $captureExit = 1
        $message = Get-CaptureError $_
        if ($null -ne $log) { Write-CaptureStatus $statusPath "Capture error: $message" $preview }
        else { [Console]::Error.WriteLine($message) }
    } finally {
        if ($null -ne $connection) { try { $connection.Dispose() } catch { } }
        if ($null -ne $log) {
            $savedBytes = $log.BytesWritten
            $log.Dispose()
            Write-CaptureStatus $statusPath "Saved $savedBytes bytes to $logPath" $preview
        }
        if ($null -ne $guard) { $guard.Dispose() }
        if ($null -ne $preview) { $preview.Dispose() }
        if ($ownsLock) { $instanceLock.ReleaseMutex() }
        $instanceLock.Dispose()
    }
    return $captureExit
}

# Dot-sourcing exposes the production functions to host tests without capturing.
if ($MyInvocation.InvocationName -eq '.') { return }
if ($List) {
    $ports = @(Get-PicassoPorts)
    if ($ports.Count) { $ports | Format-Table Port, Name, PNPDeviceID -AutoSize }
    else { 'No connected COM ports. Default capture will keep waiting.' }
    exit 0
}
exit (Invoke-PicassoCapture -Port $Port -Wait $Wait -Duration $Duration -Reconnect ([bool]$Reconnect) -Output $Output -NoStdout:$NoStdout -ConfigureLine:$ConfigureLine)
