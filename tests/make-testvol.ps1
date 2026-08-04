# make-testvol.ps1 - build a PRISTINE synthetic NTFS test volume from scratch.
#
# Replaces C:\vm\ntfs-test\ntfs-data.vhd with a freshly formatted 128 MiB
# NTFS volume containing exactly the fixtures the QEMU probe + test-qemu.ps1
# expect, and nothing else - so `chkdsk` starts from a provably clean state
# and any corruption a later test introduces is unambiguously ours.
#
# Fixtures:
#   \hello.txt              - MarkerHello (resident, small)
#   \big.txt                - MarkerBig at offset 0, padded multi-cluster
#                             (non-resident, exercises the read path)
#   \subdir\nested.txt      - MarkerNested
#   \cs_dir\                 - working directory for create/delete/write tests
#   \cs_dir\Foo.txt,foo.txt - case-sensitive pair (best-effort; needs
#                             per-directory case sensitivity support)
#   \cs_dir\real_ref.txt    - a genuine Windows-written reference file
#
# Usage:  .\make-testvol.ps1

$ErrorActionPreference = 'Stop'

$vmDir   = 'C:\vm\ntfs-test'
$vhd     = "$vmDir\ntfs-data.vhd"
$SizeMB  = 128

$MarkerHello  = 'NTFS_EFI_TEST_MARKER_9f3ab21c'
$MarkerNested = 'NTFS_EFI_TEST_NESTED_ok'
$MarkerBig    = 'NTFS_EFI_TEST_BIGFILE_MARKER_c471a0'

function Log($m) { Write-Host "[make-testvol] $m" -ForegroundColor Cyan }

# --- tear down any previous mount / file ---
try { Dismount-VHD -Path $vhd -ErrorAction SilentlyContinue } catch {}
if (Test-Path $vhd) { Remove-Item $vhd -Force; Log "removed old $vhd" }

New-Item -ItemType Directory -Force -Path $vmDir | Out-Null

# --- create + format ---
Log "creating fixed $SizeMB MiB VHD"
New-VHD -Path $vhd -Fixed -SizeBytes ($SizeMB * 1MB) | Out-Null

$disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
Initialize-Disk -Number $disk.Number -PartitionStyle MBR | Out-Null
$part = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter
$drv  = $part.DriveLetter
Format-Volume -DriveLetter $drv -FileSystem NTFS -AllocationUnitSize 4096 -NewFileSystemLabel 'NTFSEFITEST' -Confirm:$false | Out-Null
$root = "$drv`:"
Log "formatted NTFS at $root"

# --- fixtures ---
Set-Content -Path "$root\hello.txt" -Value $MarkerHello -NoNewline -Encoding Ascii

# big.txt: marker first, then pad well past one cluster to force non-resident
$bigContent = $MarkerBig + "`n" + ('BIGDATA_' * 40000)   # ~320 KB
[System.IO.File]::WriteAllText("$root\big.txt", $bigContent, [System.Text.Encoding]::ASCII)

New-Item -ItemType Directory -Path "$root\subdir" | Out-Null
Set-Content -Path "$root\subdir\nested.txt" -Value $MarkerNested -NoNewline -Encoding Ascii

New-Item -ItemType Directory -Path "$root\cs_dir" | Out-Null
Set-Content -Path "$root\cs_dir\real_ref.txt" -Value ('x' * 29) -NoNewline -Encoding Ascii

# case-sensitive pair (best effort)
$csOk = $false
try {
    & fsutil.exe file setCaseSensitiveInfo "$root\cs_dir" enable | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Set-Content -Path "$root\cs_dir\Foo.txt" -Value 'UPPER_Foo_case_test_ok!!' -NoNewline -Encoding Ascii
        Set-Content -Path "$root\cs_dir\foo.txt" -Value 'lower_foo_case_test_ok!!' -NoNewline -Encoding Ascii
        $csOk = $true
    }
} catch {}
if (-not $csOk) {
    Set-Content -Path "$root\cs_dir\Foo.txt" -Value 'UPPER_Foo_case_test_ok!!' -NoNewline -Encoding Ascii
    Log "per-directory case sensitivity unavailable - only Foo.txt created"
} else {
    Log "case-sensitive Foo.txt/foo.txt created"
}

Log ("root now contains: " + ((Get-ChildItem $root -Force | Select-Object -Expand Name) -join ', '))

Dismount-VHD -Path $vhd
Log "dismounted - volume ready at $vhd"
