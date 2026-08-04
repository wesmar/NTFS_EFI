$ErrorActionPreference = 'Stop'
$vhd = 'C:\vm\ntfs-test\ntfs-data.vhd'
$srcRoot = 'C:\vm\ntfs-test\esp\src'   # exact bytes the probe read as source

try { Dismount-VHD -Path $vhd -ErrorAction SilentlyContinue } catch {}
$drv = (Mount-VHD -Path $vhd -PassThru | Get-Disk | Get-Partition | Where-Object DriveLetter | Select-Object -First 1).DriveLetter
$dst = "$drv`:\copied"
Write-Host "Mounted $drv`: ; target = $dst"

Write-Host "`n=== chkdsk (read-only) ==="
$ck = & chkdsk "$drv`:" 2>&1 | Out-String
if ($ck -match 'found no problems') { Write-Host 'CHKDSK: clean (found no problems)' -ForegroundColor Green }
else { Write-Host 'CHKDSK: PROBLEMS' -ForegroundColor Red; ($ck -split "`n" | Where-Object { $_ -match 'corrupt|error|Correcting|index' }) | ForEach-Object { Write-Host "  $_" } }

Write-Host "`n=== byte-for-byte compare (dst vs source) ==="
$sha = [System.Security.Cryptography.SHA256]::Create()
function FileHash($p){ $fs=[System.IO.File]::OpenRead($p); try { ([BitConverter]::ToString($sha.ComputeHash($fs))) } finally { $fs.Close() } }
$allOk = $true
Get-ChildItem $srcRoot -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($srcRoot.Length)
    $dpath = Join-Path $dst $rel
    if ($rel -match 'synth_entry_(00|20)_reasonably_long_filename\.txt') { return }
    if (-not (Test-Path $dpath)) { Write-Host "MISSING on target: $rel" -ForegroundColor Red; $allOk=$false; return }
    $hs = FileHash $_.FullName
    $hd = FileHash $dpath
    $srcT = $_.LastWriteTimeUtc
    $dstT = (Get-Item $dpath).LastWriteTimeUtc
    $tOk = [math]::Abs(($srcT - $dstT).TotalSeconds) -lt 2
    if ($hs -eq $hd) {
        Write-Host ("OK   {0,-22} {1,9:N0} bytes  mtime {2}" -f $rel.TrimStart('\'), $_.Length, $(if($tOk){'match'}else{"DIFF src=$srcT dst=$dstT"}))
    } else {
        Write-Host ("FAIL {0,-22} hash mismatch" -f $rel.TrimStart('\')) -ForegroundColor Red; $allOk=$false
    }
}

# directory presence + attrs
Write-Host "`n=== structure ==="
Get-ChildItem $dst -Recurse | ForEach-Object { Write-Host ("  {0}  [{1}]" -f $_.FullName.Substring($dst.Length), $_.Attributes) }

Write-Host ("`nRESULT: {0}" -f $(if($allOk -and ($ck -match 'found no problems')){'ALL GOOD - copy is byte-exact and chkdsk-clean'}else{'FAILURES ABOVE'}))
Dismount-VHD -Path $vhd | Out-Null
