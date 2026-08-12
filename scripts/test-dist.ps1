<#
.SYNOPSIS
    Check that a built distribution actually runs on a machine that has none
    of the build tools.

.DESCRIPTION
    Copies the package somewhere else entirely, strips the PATH down to the
    Windows directories, and boots it through its own launcher. That is the
    only way to catch the thing that makes a package like this fail for its
    recipient and nobody else: a DLL or tool that resolves on the build
    machine because MSYS2 happens to be on the PATH.

    Boots headless and silent, so it needs no sound card and opens no window.

.PARAMETER DistDir
    The package to test. Defaults to <repo>\dist\BraillePlusEmulator.

.PARAMETER TimeoutSec
    How long to wait for the login prompt.
#>
[CmdletBinding()]
param(
    [string]$DistDir,
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $DistDir) { $DistDir = Join-Path $repo 'dist\BraillePlusEmulator' }
if (-not (Test-Path $DistDir)) { throw "no package at $DistDir" }

$sandbox = Join-Path ([System.IO.Path]::GetTempPath()) "bpemu-disttest-$PID"
$target  = Join-Path $sandbox 'BraillePlusEmulator'
$log     = Join-Path $sandbox 'console.log'
$errLog  = Join-Path $sandbox 'stderr.log'

Write-Host "==> Copying the package to $sandbox" -ForegroundColor Cyan
New-Item -ItemType Directory -Force $sandbox | Out-Null
Copy-Item -Recurse $DistDir $target

# A recipient has no MSYS2, no mingw64 and quite possibly no Python. Prove we
# do not quietly need any of them.
$savedPath = $env:PATH
$psDir = "$env:SystemRoot\System32\WindowsPowerShell\v1.0"
$env:PATH = "$env:SystemRoot\system32;$env:SystemRoot;$psDir"
Write-Host "==> PATH stripped to: $env:PATH" -ForegroundColor Cyan

$psExe = Join-Path $psDir 'powershell.exe'

$proc = $null

function Invoke-BootCheck {
    param([string]$Label, [string[]]$LauncherArgs)

    Remove-Item $log, $errLog -ErrorAction SilentlyContinue
    Write-Host "==> $Label (up to $TimeoutSec s)" -ForegroundColor Cyan
    $script:proc = Start-Process -FilePath $psExe `
        -ArgumentList (@(
            '-NoProfile', '-ExecutionPolicy', 'Bypass',
            '-File', (Join-Path $target 'Run-BraillePlus.ps1')
        ) + $LauncherArgs) `
        -WorkingDirectory $target `
        -RedirectStandardOutput $log `
        -RedirectStandardError $errLog `
        -NoNewWindow -PassThru

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $booted = $false
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $log) {
            $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
            if ($text -match 'everest login:') { $booted = $true; break }
        }
        if ($script:proc.HasExited) { break }
        Start-Sleep -Milliseconds 500
    }

    if (-not $booted) {
        Write-Host "--- console ---" -ForegroundColor Red
        if (Test-Path $log)    { Get-Content $log    -Tail 40 }
        Write-Host "--- stderr ---" -ForegroundColor Red
        if (Test-Path $errLog) { Get-Content $errLog -Tail 40 }
        throw "$Label did not reach a login prompt"
    }
    Write-Host "    reached the login prompt" -ForegroundColor Green

    if (-not $script:proc.HasExited) {
        Stop-Process -Id $script:proc.Id -Force -ErrorAction SilentlyContinue
    }
    Get-Process qemu-system-arm -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$sandbox*" } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

try {
    # Headless first: no window, no sound card, so a failure here is the
    # emulator itself rather than the environment.
    Invoke-BootCheck 'Booting headless and silent' @('-Display', 'none', '-AudioDrv', 'none')

    $hdd = Join-Path $target 'firmware\hdd.qcow2'
    if (-not (Test-Path $hdd)) {
        throw "the launcher did not create the drive at $hdd"
    }
    Write-Host "    created the drive on demand" -ForegroundColor Green

    # Then the defaults, which is what a recipient double-clicking actually
    # gets: an SDL window and DirectSound. A missing SDL2.dll or a broken
    # audio backend shows up only here.
    Invoke-BootCheck 'Booting with the default window and sound' @()

    Write-Host ""
    Write-Host "The package runs standalone." -ForegroundColor Green
} finally {
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    # The emulator is a grandchild of this script, so it outlives the
    # PowerShell that launched it unless it is killed by name and path.
    Get-Process qemu-system-arm -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$sandbox*" } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    $env:PATH = $savedPath
    Start-Sleep -Milliseconds 500
    Remove-Item -Recurse -Force $sandbox -ErrorAction SilentlyContinue
}
