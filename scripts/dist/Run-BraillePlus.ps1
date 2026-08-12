<#
.SYNOPSIS
    Run the APH Braille+ / LevelStar Icon emulator.

.DESCRIPTION
    Boots the device's own firmware on an emulated PXA270 board. Everything
    needed is in this folder -- there is nothing to install and nothing to
    build.

    The window that opens stays black. That is correct: the device has no
    screen, so the firmware never draws anything. The window is there to
    capture your keystrokes and feed them to the emulated keypad, so give it
    focus when you want to type on the device. The device speaks through your
    sound card.

    Give it about half a minute to boot. The serial console appears in this
    terminal, and the device's spoken interface starts on its own.

.PARAMETER Net
    Attach an emulated USB ethernet adapter. The real hardware's WiFi and
    Bluetooth use a proprietary combo chip that cannot be emulated, so this
    wired adapter stands in for them. The firmware does not configure it
    automatically; from the device's shell run:

        ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up
        route add default gw 10.0.2.2

.PARAMETER AudioDrv
    Sound backend: 'dsound' (default), 'sdl', or 'none' to boot silently.

.PARAMETER Display
    Set to 'none' for a headless run driven only from this terminal. Note that
    the keypad is then unreachable, since the window is what captures keys.

.PARAMETER Ram
    Megabytes of RAM. The real device's size is not recorded in the firmware;
    64 is what this has been tested with.

.EXAMPLE
    .\Run-BraillePlus.ps1
    .\Run-BraillePlus.ps1 -Net
    .\Run-BraillePlus.ps1 -AudioDrv none
#>
[CmdletBinding()]
param(
    [switch]$Net,
    [ValidateSet('dsound', 'sdl', 'none')]
    [string]$AudioDrv = 'dsound',
    [ValidateSet('sdl', 'none')]
    [string]$Display = 'sdl',
    [int]$Ram = 64,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = 'Stop'

$here     = $PSScriptRoot
$qemu     = Join-Path $here 'bin\qemu-system-arm.exe'
$qemuImg  = Join-Path $here 'bin\qemu-img.exe'
$kernel   = Join-Path $here 'firmware\kernel.bin'
$flash    = Join-Path $here 'firmware\flash.img'
$hdd      = Join-Path $here 'firmware\hdd.qcow2'

foreach ($f in @($qemu, $kernel, $flash)) {
    if (-not (Test-Path $f)) {
        throw "This folder is incomplete: $f is missing."
    }
}

# The firmware mounts /dev/hda1 on /media/hdd, and without a drive its sysmon
# applet dies in setup() and is restarted forever, so the interface never comes
# up and never speaks. Create the drive on demand; qcow2 keeps the 60GB disk
# down to a couple of hundred KB until the guest actually writes to it.
if (-not (Test-Path $hdd)) {
    Write-Host "Creating the device's internal 60GB drive..." -ForegroundColor Cyan
    & $qemuImg create -f qcow2 $hdd 60G | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "could not create $hdd" }
    Write-Host "It is blank, so the device will offer to format it on first boot." -ForegroundColor Cyan
}

# The stock kernel's built-in command line points at mtdblock2, but the shipped
# /etc/fstab mounts mtdblock3 -- the real bootloader passes a four-partition
# map. Supply that map explicitly. Keep it in step with the flash image.
$mtdparts = 'mtdparts=onenand:1024k(bootloader),1024k(params),4096k(kernel),-(root)'
$append   = "root=/dev/mtdblock3 rw rootfstype=jffs2 console=ttyS0,115200 $mtdparts"

$qemuArgs = @(
    '-M', 'everest,board-id=2,keypad-id=1'
    '-m', "$Ram"
    '-kernel', $kernel
    '-append', $append
    '-drive', "if=mtd,format=raw,file=$flash"
    '-drive', "if=ide,index=0,format=qcow2,file=$hdd"
    '-audio', $AudioDrv
    '-serial', 'mon:stdio'
    '-display', $Display
)
if ($Net) {
    $qemuArgs += @('-netdev', 'user,id=bpnet', '-device', 'usb-rtl8150,netdev=bpnet')
}
if ($ExtraArgs) { $qemuArgs += $ExtraArgs }

Write-Host ""
Write-Host "Starting the Braille+. It takes about 30 seconds to boot." -ForegroundColor Green
Write-Host "Type into the black window; listen on your speakers." -ForegroundColor Green
Write-Host "Close that window, or press Ctrl+C here, to shut it down." -ForegroundColor Green
Write-Host ""

& $qemu @qemuArgs
