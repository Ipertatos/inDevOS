# Run the same file-scanning commands on the Windows QEMU (WHPX) to separate
# emulation cost from kernel cost.
param(
    [int]$BootWait = 16,
    [int]$RunSecs = 20
)
$ErrorActionPreference = "Stop"
Set-Location C:\osproj

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$serial = "C:\osproj\serial-winfast.log"
foreach ($f in @($serial, "C:\osproj\qemu-winfast.log")) {
    if (Test-Path $f) { Remove-Item $f -Force }
}

$qargs = @(
    "-M", "q35", "-accel", "whpx", "-m", "1G", "-smp", "4",
    "-boot", "d", "-cdrom", "C:/osproj/os.iso",
    "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0",
    "-drive", "file=C:/osproj/sata_disk.img,format=raw,index=0",
    "-drive", "file=C:/osproj/nvme_disk.img,format=raw,index=1,media=disk",
    "-device", "virtio-vga,xres=1920,yres=1080",
    "-display", "none", "-no-reboot",
    "-serial", "file:$serial",
    "-qmp", "tcp:127.0.0.1:4465,server,nowait"
)

Write-Host "starting Windows QEMU (whpx)..."
$p = Start-Process -FilePath $qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden `
                   -RedirectStandardError "C:\osproj\qemu-winfast.log"
Start-Sleep -Seconds $BootWait

$py = (Get-Command py -ErrorAction SilentlyContinue).Source
if (-not $py) { $py = (Get-Command python -ErrorAction SilentlyContinue).Source }

& $py "C:\osproj\qmp_scenario.py" 4465 "C:\osproj\shot-winfast" `
    "wait:2" "type:wc /doom1.wad"      "wait:$RunSecs" `
    "wait:2" "type:grep WAD /doom1.wad" "wait:$RunSecs" `
    "wait:2" "type:cp /doom1.wad /BIG.WAD" "wait:$RunSecs"

Start-Sleep -Seconds 1
if (-not $p.HasExited) { $p.Kill() }

Write-Host "`n--- timings (enter usermode -> returned) ---"
Select-String -Path $serial -Pattern "wc /doom1|grep WAD|cp /doom1|enter_usermode|returned from user mode|/doom1.wad" |
    ForEach-Object { $_.Line.Trim() } | Select-Object -Last 20
