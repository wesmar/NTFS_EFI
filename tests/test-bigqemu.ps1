# test-bigqemu.ps1 - boot ntfs.efi in QEMU/OVMF with a FAT source (BIGSRC) and
# an empty NTFS target (BIGDST); the probe copies the whole source onto the
# target through ntfs.efi. Reuses the ESP built by test-qemu's layout.
#
# Prereq: .\make-bigtest.ps1 has built big-src.vhd + big-dst.vhd.
# Usage:  .\test-bigqemu.ps1 [-TimeoutSec 900] [-SkipBuild]
param(
    [int]$TimeoutSec = 900,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root     = (Split-Path $PSScriptRoot -Parent)
$vmDir    = 'C:\vm\ntfs-test'
$espDir   = "$vmDir\esp"
$srcVhd   = "$vmDir\big-src.vhd"
$dstVhd   = "$vmDir\big-dst.vhd"
$ovmfVars = "$vmDir\ovmf-vars.fd"
$ovmfVarsTemplate = "$vmDir\ovmf-vars.template.fd"
$ovmfCode = 'C:\Program Files\qemu\share\edk2-x86_64-code.fd'
$qemu     = 'C:\Program Files\qemu\qemu-system-x86_64w.exe'
$logDir   = "$root\logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

function St($m,$c='Gray'){ Write-Host "[$(Get-Date -Format HH:mm:ss)] $m" -ForegroundColor $c }

if (-not $SkipBuild) {
    St 'Building...' Cyan
    & "$root\build.ps1" | Out-Null
    if ($LASTEXITCODE -ne 0) { St 'BUILD FAILED' Red; exit 1 }
}
Copy-Item "$root\bin\ntfs.efi"       "$espDir\ntfs.efi"       -Force
Copy-Item "$root\bin\ntfs_probe.efi" "$espDir\ntfs_probe.efi" -Force

foreach ($f in @($qemu,$ovmfCode,$ovmfVarsTemplate,$srcVhd,$dstVhd,$espDir)) {
    if (-not (Test-Path $f)) { Write-Error "Missing: $f"; exit 1 }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$serial = "$vmDir\bigserial-$stamp.log"
if (Test-Path $serial) { Remove-Item $serial -Force }
Copy-Item $ovmfVarsTemplate $ovmfVars -Force

$args = @(
    '-M','q35','-cpu','max','-smp','2','-m','2048M'
    '-drive',"`"if=pflash,format=raw,readonly=on,file=$ovmfCode`""
    '-drive',"`"if=pflash,format=raw,file=$ovmfVars`""
    '-drive',"`"file=fat:rw:$espDir,format=raw,if=virtio,cache=writethrough`""
    '-drive',"`"file=$srcVhd,format=vpc,if=virtio`""                       # BIGSRC (FAT, read)
    '-drive',"`"file=$dstVhd,format=vpc,if=virtio,cache=writethrough`""     # BIGDST (NTFS, write)
    '-display','none','-serial',"`"file:$serial`"",'-monitor','none','-no-reboot','-net','none'
    '-name','ntfs-bigtest'
)
St "Booting QEMU (big test, timeout ${TimeoutSec}s)..." Cyan
$p = Start-Process -FilePath $qemu -ArgumentList $args -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit($TimeoutSec*1000)) { St "timeout - killing" Yellow; try{ Stop-Process -Id $p.Id -Force }catch{} }

$saved = "$logDir\bigtest-$stamp.log"
Copy-Item $serial $saved -Force -EA SilentlyContinue
$clean = (Get-Content $serial -Raw -EA SilentlyContinue) -replace "`e\[[0-9;?]*[a-zA-Z]",''
$clean -split "`n" | Where-Object { $_ -match 'BIG TEST|big-copy-done|big-clean|NTFS-PERF|PROBE-END|Exception|ASSERT' } |
    ForEach-Object { Write-Host "  $_" }
St "log: $saved" Gray
