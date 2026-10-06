# DOOM benchmark on the *Windows* QEMU (WHPX-accelerated), to compare against
# the WSL QEMU run, which has no /dev/kvm and therefore falls back to TCG
# software emulation.
param(
    [string]$Mode = "exec",
    [int]$BootWait = 16,
    [int]$RunSecs = 30,
    [string]$Accel = "whpx"
)

$ErrorActionPreference = "Stop"
Set-Location C:\osproj

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$serial = "C:\osproj\serial-windoom.log"
foreach ($f in @($serial, "C:\osproj\qemu-windoom.log")) {
    if (Test-Path $f) { Remove-Item $f -Force }
}

$qargs = @(
    "-M", "q35", "-accel", $Accel, "-m", "1G", "-smp", "4",
    "-boot", "d", "-cdrom", "C:/osproj/os.iso",
    "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0",
    "-drive", "file=C:/osproj/sata_disk.img,format=raw,index=0",
    "-drive", "file=C:/osproj/nvme_disk.img,format=raw,index=1,media=disk",
    "-device", "virtio-vga,xres=1920,yres=1080",
    "-display", "none", "-no-reboot",
    "-serial", "file:$serial",
    "-qmp", "tcp:127.0.0.1:4449,server,nowait"
)

Write-Host "starting Windows QEMU (accel=$Accel)..."
$p = Start-Process -FilePath $qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden `
                   -RedirectStandardError "C:\osproj\qemu-windoom.log"
Start-Sleep -Seconds $BootWait

$py = (Get-Command py -ErrorAction SilentlyContinue).Source
if (-not $py) { $py = (Get-Command python -ErrorAction SilentlyContinue).Source }

& $py "C:\osproj\qmp_scenario.py" 4449 "C:\osproj\shot-windoom" `
    "wait:2" "type:prof reset" "wait:2" "type:prof start 64" "wait:2" `
    "type:exec doom.exe" "wait:$RunSecs" "shot:1-run" `
    "key:ctrl,c" "wait:3" "type:prof stop" "wait:2" "type:prof stats" "wait:2"

Start-Sleep -Seconds 1
if (-not $p.HasExited) { $p.Kill() }

Write-Host "`n--- timing / frames ---"
Select-String -Path $serial -Pattern "Shareware Startup|InitTextures|InitFlats|InitSprites|DOOM fullscreen|enter_usermode|returned from user mode|frames=|exception" |
    ForEach-Object { $_.Line.Trim() }
