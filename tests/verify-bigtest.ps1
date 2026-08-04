# verify-bigtest.ps1 - validate the BIGDST result: fsutil dirty, chkdsk, and a
# SHA256 byte-for-byte compare of every copied file against its C:\D\Download
# source (via the manifest make-bigtest.ps1 wrote). Read-only mount so Windows
# cannot self-heal and hide corruption.
param(
    [string]$Source = 'C:\D\Download',
    [int]$SampleEvery = 1     # hash every Nth file (1 = all); speeds huge runs
)
$ErrorActionPreference = 'Stop'
$vmDir = 'C:\vm\ntfs-test'
$dst   = "$vmDir\big-dst.vhd"
$manifest = "$vmDir\big-manifest.txt"
if (-not (Test-Path $manifest)) { Write-Error "no manifest - run make-bigtest.ps1"; exit 1 }

try { Dismount-VHD $dst -EA SilentlyContinue } catch {}
$drv = (Mount-VHD $dst -ReadOnly -PassThru | Get-Disk | Get-Partition | Where-Object DriveLetter | Select-Object -First 1).DriveLetter
Write-Host "Mounted BIGDST read-only at $drv`:" -ForegroundColor Cyan

Write-Host "`n=== fsutil dirty ===" ; fsutil dirty query "$drv`:"

Write-Host "`n=== chkdsk (read-only) ==="
$ck = & chkdsk "$drv`:" 2>&1 | Out-String
if ($ck -match 'found no problems') { Write-Host 'CHKDSK: CLEAN' -ForegroundColor Green }
else { Write-Host 'CHKDSK: PROBLEMS' -ForegroundColor Red; ($ck -split "`n" | Where-Object { $_ -match 'corrupt|Attribute|Bitmap|orphan|segment|allocated|index' }) | ForEach-Object { Write-Host "  $_" } }

Write-Host "`n=== SHA256 compare (dst vs source) ==="
$sha = [System.Security.Cryptography.SHA256]::Create()
function Get-Sha256([string]$p){ $fs=[IO.File]::OpenRead($p); try{ [BitConverter]::ToString($sha.ComputeHash($fs)) } finally { $fs.Close() } }
$rel = Get-Content $manifest
$ok=0; $bad=0; $miss=0; $i=0
foreach ($r in $rel) {
    $i++; if ($SampleEvery -gt 1 -and ($i % $SampleEvery -ne 0)) { continue }
    $s = Join-Path $Source $r
    $d = Join-Path "$drv`:" $r
    if (-not (Test-Path $d)) { $miss++; if($miss -le 20){ Write-Host "  MISSING: $r" -ForegroundColor Red }; continue }
    if ((Get-Sha256 $s) -eq (Get-Sha256 $d)) { $ok++ } else { $bad++; if($bad -le 20){ Write-Host "  HASH DIFF: $r" -ForegroundColor Red } }
}
Write-Host ("`nRESULT: ok={0} bad={1} missing={2} (of {3} checked)" -f $ok,$bad,$miss,($ok+$bad+$miss))
$verdict = ($bad -eq 0 -and $miss -eq 0 -and ($ck -match 'found no problems'))
Write-Host ("VERDICT: {0}" -f $(if($verdict){'ALL GOOD - byte-exact + chkdsk-clean'}else{'FAILURES ABOVE'})) -ForegroundColor $(if($verdict){'Green'}else{'Red'})
Dismount-VHD $dst | Out-Null
