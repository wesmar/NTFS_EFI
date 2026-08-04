# make-bigtest.ps1 - build the large real-file test:
#   * an NTFS source VHD populated with a subset of C:\D\Download. NTFS (not FAT)
#     because the UEFI firmware's FAT driver only enumerates ~8540 of 20000 deeply
#     nested entries; OUR driver's read path sees all of them. So this now tests
#     BOTH our read (source) and write (dest) paths.
#   * an empty NTFS destination VHD our driver writes into.
# The probe copies BIGSRC -> BIGDST; verify-bigtest.ps1 checks SHA256 + chkdsk.
#
# Usage:  .\make-bigtest.ps1 -TargetGB 7 -MaxFiles 20000
param(
    [double]$TargetGB = 7,
    [int]$MaxFiles    = 20000,
    [string]$Source   = 'C:\D\Download'
)
$ErrorActionPreference = 'Stop'

$vmDir   = 'C:\vm\ntfs-test'
$srcVhd  = "$vmDir\big-src.vhd"     # FAT32, filled from $Source
$dstVhd  = "$vmDir\big-dst.vhd"     # NTFS, empty, driver writes here
$srcGB   = [math]::Ceiling($TargetGB) + 2
$dstGB   = [math]::Ceiling($TargetGB) + 5

function Log($m){ Write-Host "[make-bigtest] $m" -ForegroundColor Cyan }

New-Item -ItemType Directory -Force -Path $vmDir | Out-Null
foreach($v in @($srcVhd,$dstVhd)){ try{ Dismount-VHD $v -EA SilentlyContinue }catch{}; if(Test-Path $v){ Remove-Item $v -Force } }

# --- pick a subset of the source up to TargetGB / MaxFiles, keeping structure ---
Log "enumerating $Source ..."
$cap = $TargetGB * 1GB
$sum = 0; $picked = New-Object System.Collections.ArrayList
foreach($f in (Get-ChildItem $Source -Recurse -File -EA SilentlyContinue)){
    if($sum + $f.Length -gt $cap){ continue }        # skip too-big, keep filling
    if($picked.Count -ge $MaxFiles){ break }
    [void]$picked.Add($f); $sum += $f.Length
}
Log ("picked {0} files, {1:N2} GB" -f $picked.Count, ($sum/1GB))
if($picked.Count -eq 0){ throw "no files picked from $Source" }

# --- create + format the NTFS source, copy the subset in (preserving paths) ---
Log "creating NTFS source VHD ($srcGB GB)"
$d = New-VHD -Path $srcVhd -Dynamic -SizeBytes ($srcGB*1GB) | Mount-VHD -Passthru | Get-Disk
Initialize-Disk -Number $d.Number -PartitionStyle MBR | Out-Null
$p = New-Partition -DiskNumber $d.Number -UseMaximumSize -AssignDriveLetter
Format-Volume -DriveLetter $p.DriveLetter -FileSystem NTFS -AllocationUnitSize 4096 -NewFileSystemLabel 'BIGSRC' -Confirm:$false | Out-Null
$srcRoot = "$($p.DriveLetter):"
Log "copying subset to $srcRoot ..."
$srcLen = $Source.TrimEnd('\').Length
$n=0
foreach($f in $picked){
    $rel = $f.FullName.Substring($srcLen).TrimStart('\')
    $dst = Join-Path $srcRoot $rel
    $dir = Split-Path $dst -Parent
    if(-not (Test-Path $dir)){ New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Copy-Item $f.FullName $dst -Force
    $n++; if($n % 2000 -eq 0){ Log "  $n / $($picked.Count)" }
}
# manifest for verify step (relative paths actually copied)
$picked | ForEach-Object { $_.FullName.Substring($srcLen).TrimStart('\') } | Set-Content "$vmDir\big-manifest.txt" -Encoding UTF8
Dismount-VHD $srcVhd
Log "source ready: $srcVhd (label BIGSRC)"

# --- create the empty NTFS destination ---
Log "creating NTFS destination VHD ($dstGB GB)"
$d2 = New-VHD -Path $dstVhd -Dynamic -SizeBytes ($dstGB*1GB) | Mount-VHD -Passthru | Get-Disk
Initialize-Disk -Number $d2.Number -PartitionStyle MBR | Out-Null
$p2 = New-Partition -DiskNumber $d2.Number -UseMaximumSize -AssignDriveLetter
Format-Volume -DriveLetter $p2.DriveLetter -FileSystem NTFS -AllocationUnitSize 4096 -NewFileSystemLabel 'BIGDST' -Confirm:$false | Out-Null
Dismount-VHD $dstVhd
Log "destination ready: $dstVhd (label BIGDST)"
Log "done. Subset = $($picked.Count) files, $([math]::Round($sum/1GB,2)) GB"
