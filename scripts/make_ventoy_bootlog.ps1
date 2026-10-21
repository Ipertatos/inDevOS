<#
.SYNOPSIS
  Create the two preallocated inDevOS boot-log files on a Ventoy stick.

.DESCRIPTION
  Writes INDEVOS_BOOTLOG.TXT and INDEVOS_BOOTLOG.OLD (1 MiB each, real
  bytes: a header line followed by spaces) to the root of the given drive,
  normally the Ventoy exFAT data partition.  The inDevOS kernel finds them
  while booting and overwrites their contents in place, so after a crash or
  hard power-off INDEVOS_BOOTLOG.TXT shows how far the boot got (and
  INDEVOS_BOOTLOG.OLD the boot before it).  Existing files are left alone
  unless -Force is given.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File make_ventoy_bootlog.ps1 -Drive E
#>
param(
    [Parameter(Mandatory = $true)][string]$Drive,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$letter = $Drive.Trim().TrimEnd('\').TrimEnd(':')
if ($letter.Length -ne 1) { throw "Give a drive letter, e.g. -Drive E" }
$root = "${letter}:\"
if (-not (Test-Path $root)) { throw "Drive $root not found" }

$vol = Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue
if ($vol) {
    Write-Host ("Drive {0}  label '{1}'  filesystem {2}  free {3:N0} MB" -f $root, $vol.FileSystemLabel, $vol.FileSystem, ($vol.SizeRemaining / 1MB))
    if ($vol.FileSystem -ne 'exFAT') {
        Write-Warning "Filesystem is $($vol.FileSystem), not exFAT. inDevOS looks for these files on exFAT volumes (Ventoy data partition)."
    }
}

$size = 1MB
$header = [Text.Encoding]::ASCII.GetBytes("inDevOS boot log - no boot recorded yet`r`n")
$bytes = New-Object byte[] $size
for ($i = 0; $i -lt $size; $i++) { $bytes[$i] = 0x20 }
[Array]::Copy($header, $bytes, $header.Length)
$bytes[$size - 1] = 0x0A

foreach ($name in 'INDEVOS_BOOTLOG.TXT', 'INDEVOS_BOOTLOG.OLD') {
    $path = Join-Path $root $name
    if ((Test-Path $path) -and -not $Force) {
        Write-Host "$path already exists - kept (use -Force to recreate it)"
        continue
    }
    [IO.File]::WriteAllBytes($path, $bytes)
    $len = (Get-Item $path).Length
    if ($len -ne $size) { throw "$path has $len bytes, expected $size" }
    Write-Host "Created $path ($len bytes)"
}
Write-Host "Done. After booting inDevOS, open ${root}INDEVOS_BOOTLOG.TXT (latest boot) or INDEVOS_BOOTLOG.OLD (the boot before)."
