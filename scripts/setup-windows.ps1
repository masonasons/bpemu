<#
.SYNOPSIS
    Build a native Windows qemu-system-arm.exe with the Everest board.

.DESCRIPTION
    Runs the normal scripts/setup.sh inside MSYS2's shell against the
    MinGW-w64 toolchain, producing a self-contained Windows binary that plays
    audio through DirectSound. No WSL involved at run time.

    Requires MSYS2. Install it with `scoop install msys2`, or from msys2.org
    and pass -Msys2Root.

.PARAMETER Msys2Root
    Path to the MSYS2 installation. Auto-detected if omitted.

.PARAMETER QemuDir
    Where to clone/build QEMU. Defaults to <repo>\..\qemu-win.

.PARAMETER SkipDeps
    Skip the pacman dependency install (it is idempotent but slow).
#>
[CmdletBinding()]
param(
    [string]$Msys2Root,
    [string]$QemuDir,
    [switch]$SkipDeps
)

$ErrorActionPreference = 'Stop'

function Find-Msys2 {
    $candidates = @(
        "$env:USERPROFILE\scoop\apps\msys2\current",
        'C:\msys64',
        'C:\tools\msys64'
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c 'usr\bin\bash.exe')) { return $c }
    }
    throw "MSYS2 not found. Install it with 'scoop install msys2', or pass -Msys2Root."
}

if (-not $Msys2Root) { $Msys2Root = Find-Msys2 }
$bash = Join-Path $Msys2Root 'usr\bin\bash.exe'
Write-Host "==> Using MSYS2 at $Msys2Root" -ForegroundColor Cyan

$repo = Split-Path -Parent $PSScriptRoot
if (-not $QemuDir) { $QemuDir = Join-Path (Split-Path -Parent $repo) 'qemu-win' }

# Translate Windows paths to the MSYS2 form the shell expects.
function ConvertTo-MsysPath([string]$p) {
    $full = [System.IO.Path]::GetFullPath($p)
    $drive = $full.Substring(0, 1).ToLower()
    return '/' + $drive + $full.Substring(2).Replace('\', '/')
}

$repoMsys = ConvertTo-MsysPath $repo
$qemuMsys = ConvertTo-MsysPath $QemuDir

$packages = @(
    'mingw-w64-x86_64-gcc'
    'mingw-w64-x86_64-glib2'
    'mingw-w64-x86_64-pixman'
    'mingw-w64-x86_64-ninja'
    'mingw-w64-x86_64-pkgconf'
    'mingw-w64-x86_64-python'
    'mingw-w64-x86_64-python-setuptools'
    # QEMU's configure builds a Python venv for meson and needs these.
    'mingw-w64-x86_64-python-distlib'
    'mingw-w64-x86_64-python-pip'
    'mingw-w64-x86_64-meson'
    'mingw-w64-x86_64-SDL2'
    'mingw-w64-x86_64-libslirp'
    'mingw-w64-x86_64-zlib'
    'git'
    'make'
    'bison'
    'flex'
    'diffutils'
) -join ' '

if (-not $SkipDeps) {
    Write-Host "==> Installing build dependencies (this can take a while)" -ForegroundColor Cyan
    & $bash -lc "pacman -Sy --noconfirm --needed --disable-download-timeout $packages"
    if ($LASTEXITCODE -ne 0) { throw "pacman failed with exit code $LASTEXITCODE" }
}

Write-Host "==> Building QEMU into $QemuDir" -ForegroundColor Cyan
& $bash -lc "QEMU_DIR='$qemuMsys' '$repoMsys/scripts/setup.sh'"
if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }

$exe = Join-Path $QemuDir 'build\qemu-system-arm.exe'
if (-not (Test-Path $exe)) { throw "expected $exe to exist after the build" }

# A MinGW build links against MSYS2's mingw64 runtime DLLs, so running it from
# a plain PowerShell prompt fails with STATUS_DLL_NOT_FOUND. Copy the ones it
# actually needs next to the binary so the build directory stands alone.
Write-Host "==> Bundling runtime DLLs next to the binary" -ForegroundColor Cyan
$exeMsys = ConvertTo-MsysPath $exe
$ldd = & $bash -lc "cd /mingw64/bin && ldd '$exeMsys'"
$mingwBin = Join-Path $Msys2Root 'mingw64\bin'
$copied = 0
foreach ($line in $ldd) {
    if ($line -match '=>\s+(\S+mingw64[\\/]bin[\\/]([^\s]+\.dll))') {
        $name = $matches[2]
        $src = Join-Path $mingwBin $name
        $dst = Join-Path (Split-Path -Parent $exe) $name
        if ((Test-Path $src) -and -not (Test-Path $dst)) {
            Copy-Item $src $dst
            $copied++
        }
    }
}
Write-Host "    copied $copied DLL(s)"

Write-Host ""
Write-Host "Built $exe" -ForegroundColor Green
Write-Host "Run it with:  .\scripts\run.ps1" -ForegroundColor Green
