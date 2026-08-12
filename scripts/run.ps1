<#
.SYNOPSIS
    Boot the Braille+ firmware on the native Windows build of QEMU.

.DESCRIPTION
    Equivalent to scripts/run.sh, but for the qemu-system-arm.exe produced by
    scripts/setup-windows.ps1. No WSL involved.

.PARAMETER AudioDrv
    QEMU audio backend. 'dsound' (default) is DirectSound, which Windows itself
    implements over WASAPI. 'sdl' goes through SDL2, whose Windows backend is
    WASAPI directly and usually has lower latency. 'none' boots silently.
    You can also pass a full spec, e.g. 'driver=wav,path=C:\out.wav'.

.PARAMETER Display
    QEMU display backend, 'sdl' by default. The window it opens stays black --
    the device has no screen, so the guest never initialises a framebuffer --
    but it is what captures your keystrokes and feeds them to the emulated
    keypad. Give that window focus to type on the device. Pass 'none' for a
    headless run driven purely from the serial console.
#>
[CmdletBinding()]
param(
    [string]$Qemu,
    [string]$Kernel,
    [string]$Flash,
    [string]$Hdd,
    [int]$Ram = 64,
    [int]$BoardId = 2,
    [int]$KeypadId = 1,
    [string]$AudioDrv = 'dsound',
    [string]$Display = 'sdl',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Qemu)   { $Qemu   = Join-Path (Split-Path -Parent $repo) 'qemu-win\build\qemu-system-arm.exe' }
if (-not $Kernel) { $Kernel = Join-Path $repo 'build\parts\kernel.bin' }
if (-not $Flash)  { $Flash  = Join-Path $repo 'build\flash.img' }
if (-not $Hdd)    { $Hdd    = Join-Path $repo 'build\hdd.qcow2' }

foreach ($f in @($Qemu, $Kernel, $Flash)) {
    if (-not (Test-Path $f)) { throw "missing: $f" }
}

# The firmware mounts /dev/hda1 on /media/hdd, and without a drive its sysmon
# applet dies in setup() and is restarted forever, so the user interface never
# comes up and never speaks. Create the drive on demand; qcow2 keeps a 60GB
# disk down to a couple of hundred KB until the guest writes to it.
if (-not (Test-Path $Hdd)) {
    Write-Host "Creating the internal 60GB drive at $Hdd" -ForegroundColor Cyan
    & python (Join-Path $repo 'tools\bpimage.py') mkdisk $Hdd
    if ($LASTEXITCODE -ne 0) { throw "could not create $Hdd" }
    Write-Host "It is blank, so the device will offer to format it on first boot." -ForegroundColor Cyan
}

# Keep this in step with tools/bpimage.py's PARTITIONS.
$mtdparts = 'mtdparts=onenand:1024k(bootloader),1024k(params),4096k(kernel),-(root)'
$append   = "root=/dev/mtdblock3 rw rootfstype=jffs2 console=ttyS0,115200 $mtdparts"

$qemuArgs = @(
    '-M', "everest,board-id=$BoardId,keypad-id=$KeypadId"
    '-m', "$Ram"
    '-kernel', $Kernel
    '-append', $append
    '-drive', "if=mtd,format=raw,file=$Flash"
    '-drive', "if=ide,index=0,format=qcow2,file=$Hdd"
    '-audio', $AudioDrv
    '-serial', 'mon:stdio'
    '-display', $Display
)
if ($ExtraArgs) { $qemuArgs += $ExtraArgs }

& $Qemu @qemuArgs
