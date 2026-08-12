<#
.SYNOPSIS
    Assemble a standalone folder someone else can unzip and run.

.DESCRIPTION
    Collects the emulator, its runtime DLLs, the firmware images and a
    launcher into one directory that depends on nothing else: no MSYS2, no
    WSL, no Python, no build step. Verify it the way a recipient would, by
    running scripts\test-dist.ps1 against the result.

    Run scripts\setup-windows.ps1 first to produce the binary, and
    scripts\build-image.sh to produce the firmware images.

.PARAMETER OutDir
    Where to assemble it. Defaults to <repo>\dist\BraillePlusEmulator.

.PARAMETER NoFirmware
    Leave the firmware images out. The result is redistributable without any
    copyright question, but the recipient has to supply their own images
    before it will run.

.PARAMETER Zip
    Also produce a .zip archive next to the folder.
#>
[CmdletBinding()]
param(
    [string]$OutDir,
    [switch]$NoFirmware,
    [switch]$Zip
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist\BraillePlusEmulator' }

$buildDir = Join-Path (Split-Path -Parent $repo) 'qemu-win\build'
$qemu     = Join-Path $buildDir 'qemu-system-arm.exe'
$qemuImg  = Join-Path $buildDir 'qemu-img.exe'

if (-not (Test-Path $qemu)) {
    throw "$qemu is missing. Run scripts\setup-windows.ps1 first."
}

# The binary is only useful with the board compiled in, and a stale build is
# indistinguishable from a good one until someone tries to boot it.
$machines = & $qemu -M help
if (-not ($machines -match 'everest')) {
    throw "$qemu has no everest machine. Rebuild with scripts\setup-windows.ps1."
}
$devices = & $qemu -device help 2>&1
if (-not ($devices -match 'usb-rtl8150')) {
    throw "$qemu has no usb-rtl8150 device. Rebuild with scripts\setup-windows.ps1."
}

function Remove-TreeWithRetry([string]$path) {
    # Windows hands out transient locks on a directory that was recently
    # written or scanned -- an indexer or antivirus pass is enough -- and they
    # clear on their own within a second or two. Failing the whole build over
    # that is not worth it.
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        try {
            Remove-Item -Recurse -Force $path -ErrorAction Stop
            return
        } catch {
            if ($attempt -eq 5) {
                throw ("Could not remove $path after $attempt attempts: " +
                       "$($_.Exception.Message)`nClose anything using it, or " +
                       "pass -OutDir to build somewhere else.")
            }
            Start-Sleep -Seconds $attempt
        }
    }
}

Write-Host "==> Assembling into $OutDir" -ForegroundColor Cyan
if (Test-Path $OutDir) { Remove-TreeWithRetry $OutDir }
foreach ($sub in @('bin', 'firmware', 'docs', 'src')) {
    New-Item -ItemType Directory -Force (Join-Path $OutDir $sub) | Out-Null
}

Write-Host "    emulator and runtime libraries"
Copy-Item $qemu    (Join-Path $OutDir 'bin')
Copy-Item $qemuImg (Join-Path $OutDir 'bin')
$dlls = Get-ChildItem (Join-Path $buildDir '*.dll')
if (-not $dlls) {
    throw "no DLLs beside $qemu; setup-windows.ps1 bundles them, so re-run it."
}
$dlls | Copy-Item -Destination (Join-Path $OutDir 'bin')
Write-Host "      $($dlls.Count) DLL(s)"

if (-not $NoFirmware) {
    Write-Host "    firmware images"
    $flash  = Join-Path $repo 'build\flash.img'
    $kernel = Join-Path $repo 'build\parts\kernel.bin'
    foreach ($f in @($flash, $kernel)) {
        if (-not (Test-Path $f)) {
            throw "$f is missing. Run scripts\build-image.sh first."
        }
    }
    Copy-Item $flash  (Join-Path $OutDir 'firmware')
    Copy-Item $kernel (Join-Path $OutDir 'firmware')
} else {
    Write-Host "    firmware images SKIPPED (-NoFirmware)" -ForegroundColor Yellow
    Set-Content -Path (Join-Path $OutDir 'firmware\PUT-FIRMWARE-HERE.txt') -Value @(
        'This package was built without firmware images.'
        ''
        'Put flash.img and kernel.bin in this folder. Both are produced by'
        'scripts/build-image.sh in the bpemu source tree, from a copy of the'
        'device firmware you supply yourself.'
    )
}

Write-Host "    launcher and documentation"
Copy-Item (Join-Path $PSScriptRoot 'dist\Run-BraillePlus.ps1') $OutDir
Copy-Item (Join-Path $PSScriptRoot 'dist\BraillePlus.bat')     $OutDir
Copy-Item (Join-Path $PSScriptRoot 'dist\README.txt')          $OutDir
Copy-Item (Join-Path $repo 'docs\everest-board.md') (Join-Path $OutDir 'docs')

# QEMU is GPL v2: shipping the binary obliges us to make the corresponding
# source available, and the board and device models are the part of it that
# is ours.
Write-Host "    source for the GPL parts"
Copy-Item -Recurse (Join-Path $repo 'qemu\*') (Join-Path $OutDir 'src')
Set-Content -Path (Join-Path $OutDir 'src\README.txt') -Value @(
    'These are the board and device models added to QEMU for this emulator:'
    ''
    '  hw/arm/everest.c          the board itself'
    '  hw/audio/pxa2xx_ac97.c    AC97 controller and WM9713 codec'
    '  hw/usb/dev-rtl8150.c      USB ethernet adapter'
    ''
    'They are GPL v2, the same as QEMU, which they are built into. QEMU''s own'
    'source is at https://gitlab.com/qemu-project/qemu -- this was built from'
    'tag v9.1.0, the last release that still carried PXA2xx support.'
)

$size = (Get-ChildItem -Recurse $OutDir | Measure-Object -Property Length -Sum).Sum
Write-Host ""
Write-Host ("Built $OutDir ({0:N1} MB)" -f ($size / 1MB)) -ForegroundColor Green

if ($Zip) {
    $zipPath = "$OutDir.zip"
    Write-Host "==> Compressing to $zipPath" -ForegroundColor Cyan
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    Compress-Archive -Path $OutDir -DestinationPath $zipPath
    $zipSize = (Get-Item $zipPath).Length
    Write-Host ("Built $zipPath ({0:N1} MB)" -f ($zipSize / 1MB)) -ForegroundColor Green
}

Write-Host ""
Write-Host "Verify it before sending it anywhere:" -ForegroundColor Yellow
Write-Host "    .\scripts\test-dist.ps1 -DistDir '$OutDir'" -ForegroundColor Yellow
