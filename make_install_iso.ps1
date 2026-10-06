# Build os_install.iso from Windows.
# One command:
#     .\make_install_iso.ps1
# The FAT32 root image is packed with mtools, which this tree already uses from
# WSL (apps/build_apps.sh, build_doom.sh). This script just enters that WSL
# distro and runs make_install_iso.sh. It does not commit or push.
$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
if (-not $Root) { $Root = (Get-Location).Path }
Set-Location $Root

if (-not (Get-Command wsl -ErrorAction SilentlyContinue)) {
    Write-Host "[ERROR] WSL is required. The rootfs is a FAT32 image built with mtools (mformat/mcopy), same as apps/build_apps.sh." -ForegroundColor Red
    Write-Host "        Install WSL, or from Linux run:  bash ./make_install_iso.sh" -ForegroundColor Red
    exit 1
}

$drive = $Root.Substring(0, 1).ToLower()
$rest = $Root.Substring(2) -replace '\\', '/'
$wslPath = "/mnt/$drive$rest"
Write-Host "[INFO]  Building install ISO via WSL: $wslPath" -ForegroundColor Green
# The FAT root ships Doom once (/bin/doom.exe) and the WAD once (/doom1.wad).
# make_install_iso.sh is the file list; do not mcopy doom.exe to a second path.
wsl -e bash -lc "cd '$wslPath' && bash ./make_install_iso.sh"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$iso = Join-Path $Root "os_install.iso"
if (Test-Path $iso) {
    $len = (Get-Item $iso).Length
    Write-Host "[INFO]  os_install.iso  ($len bytes)" -ForegroundColor Green
    Write-Host "Boot it from the DVD (CD first). The kernel mounts that disc as the live disk and can install onto another hard disk." -ForegroundColor Green
}

