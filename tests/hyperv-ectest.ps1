# hyperv-ectest.ps1 - run EC's scripted self-test under Hyper-V and print the
# result. Nothing here is interactive: build, lay a fixture on a scratch NTFS
# volume, boot EC with the flag file, wait for it to power off, read the report.
#
#   .\tests\hyperv-ectest.ps1                # build, run, report
#   .\tests\hyperv-ectest.ps1 -SkipBuild     # reuse bin\EC.efi as it stands
#   .\tests\hyperv-ectest.ps1 -KeepVolume    # do not recreate the NTFS fixture
#
# The NTFS volume is a scratch VHDX this script owns; it is formatted from
# nothing on every run unless -KeepVolume is given, so a failed run never
# leaves the next one guessing.
param(
    [switch]$SkipBuild,
    [switch]$KeepVolume,
    [string]$NtfsDriver = '',
    [string]$VmName     = 'ec-selftest',
    [int]$TimeoutMin    = 10
)
$ErrorActionPreference = 'Stop'

$root    = Split-Path $PSScriptRoot -Parent
$vmDir   = 'C:\vm\ec-test'
$espVhd  = "$vmDir\ec-esp.vhdx"
$dataVhd = "$vmDir\ec-data.vhdx"
$espLtr  = 'V'      # pinned high letters: D and E belong to real disks
$dataLtr = 'W'

function Say ($m) { Write-Host "[ectest] $m" -ForegroundColor Cyan }
function Die ($m) { Write-Host "[ectest] $m" -ForegroundColor Red; exit 1 }

if (-not (Test-Path $vmDir)) { New-Item -ItemType Directory -Path $vmDir -Force | Out-Null }

# EC lives in its own tree here and inside the NTFS_EFI repository there. The
# driver comes from bin\ when the two share a tree, from the driver repository
# when they do not; -NtfsDriver overrides both.
if ($NtfsDriver -eq '') {
    $NtfsDriver = if (Test-Path "$root\bin\ntfs.efi") { "$root\bin\ntfs.efi" }
                  else { 'C:\Projekty\other\ntfs\bin\ntfs.efi' }
}
if (-not (Test-Path $NtfsDriver)) { Die "no ntfs.efi at $NtfsDriver" }
Say "ntfs driver: $NtfsDriver"

# --- 1. build with the test path compiled in --------------------------------
# build.ps1 writes into the shared bin\, which is also where the deployment
# script picks EC.efi up. The test binary is therefore taken out of the way at
# once and a release build put back, so nobody copies a self-test image onto a
# real stick by accident.
$staged = "$vmDir\EC-selftest.efi"
if (-not $SkipBuild) {
    Say 'building EC with -SelfTest'
    & "$root\build.ps1" -SelfTest | Out-Null
    if ($LASTEXITCODE -ne 0) { Die 'build failed' }
    Copy-Item "$root\bin\EC.efi" $staged -Force
    Say 'restoring the release build in bin\'
    & "$root\build.ps1" | Out-Null
}
if (-not (Test-Path $staged)) { Die "no staged self-test binary at $staged" }

# --- 2. the NTFS volume the test works on -----------------------------------
# The fixture is deliberately shaped for the search: three .tag files at three
# depths, two directories whose names match one mask, and one file whose
# attributes and timestamp get rewritten.
if (-not $KeepVolume -or -not (Test-Path $dataVhd)) {
    Say 'creating the NTFS fixture volume'
    try { Dismount-VHD $dataVhd -EA SilentlyContinue } catch {}
    if (Test-Path $dataVhd) { Remove-Item $dataVhd -Force }

    $d = New-VHD -Path $dataVhd -Dynamic -SizeBytes 2GB | Mount-VHD -Passthru | Get-Disk
    Initialize-Disk -Number $d.Number -PartitionStyle GPT | Out-Null
    New-Partition -DiskNumber $d.Number -UseMaximumSize -DriveLetter $dataLtr | Out-Null
    Format-Volume -DriveLetter $dataLtr -FileSystem NTFS -NewFileSystemLabel ECTEST -Confirm:$false | Out-Null

    $base = "${dataLtr}:\_ECTEST"
    New-Item -ItemType Directory "$base\level1\level2" -Force | Out-Null
    New-Item -ItemType Directory "$base\other" -Force | Out-Null
    Set-Content "$base\top.tag"                 -Value 'top'    -NoNewline
    Set-Content "$base\level1\middle.tag"       -Value 'middle' -NoNewline
    Set-Content "$base\level1\level2\deep.tag"  -Value 'deep'   -NoNewline
    Set-Content "$base\other\decoy.txt"         -Value 'decoy'  -NoNewline
    Set-Content "$base\attr.bin"                -Value 'attr'   -NoNewline

    # Two directories for the panel comparison: one pair identical down to the
    # timestamp, one pair differing only in size, and one entry on each side
    # that the other does not have at all.
    New-Item -ItemType Directory "$base\cmp_a" -Force | Out-Null
    New-Item -ItemType Directory "$base\cmp_b" -Force | Out-Null
    Set-Content "$base\cmp_a\same.txt"   -Value 'identical' -NoNewline
    Set-Content "$base\cmp_b\same.txt"   -Value 'identical' -NoNewline
    Set-Content "$base\cmp_a\diff.txt"   -Value 'short'     -NoNewline
    Set-Content "$base\cmp_b\diff.txt"   -Value 'much longer content' -NoNewline
    Set-Content "$base\cmp_a\only_a.txt" -Value 'a'         -NoNewline
    Set-Content "$base\cmp_b\only_b.txt" -Value 'b'         -NoNewline
    New-Item -ItemType Directory "$base\cmp_a\subdir" -Force | Out-Null
    $stamp = [datetime]'2020-01-02 03:04:05'
    foreach ($f in "$base\cmp_a\same.txt", "$base\cmp_b\same.txt") {
        (Get-Item $f).LastWriteTime = $stamp
    }

    Dismount-VHD $dataVhd
} else {
    Say 'reusing the existing NTFS fixture volume'
    try { Dismount-VHD $dataVhd -EA SilentlyContinue } catch {}
}

# --- 3. the ESP EC boots from ------------------------------------------------
Say 'building the boot ESP'
try { Dismount-VHD $espVhd -EA SilentlyContinue } catch {}
if (Test-Path $espVhd) { Remove-Item $espVhd -Force }

$d = New-VHD -Path $espVhd -Dynamic -SizeBytes 256MB | Mount-VHD -Passthru | Get-Disk
Initialize-Disk -Number $d.Number -PartitionStyle GPT | Out-Null
$p = New-Partition -DiskNumber $d.Number -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' -UseMaximumSize -DriveLetter $espLtr
Format-Volume -DriveLetter $espLtr -FileSystem FAT32 -NewFileSystemLabel ESP -Confirm:$false | Out-Null
New-Item -ItemType Directory "${espLtr}:\EFI\Boot" -Force | Out-Null
Copy-Item $staged             "${espLtr}:\EFI\Boot\BOOTX64.EFI" -Force
Copy-Item $NtfsDriver         "${espLtr}:\EFI\Boot\ntfs.efi"    -Force
Copy-Item $NtfsDriver         "${espLtr}:\ntfs.efi"             -Force
Set-Content "${espLtr}:\_ECTEST.on" -Value 'on' -NoNewline -Encoding Ascii
Dismount-VHD $espVhd

# --- 4. run it ---------------------------------------------------------------
Get-VM $VmName -EA SilentlyContinue | ForEach-Object {
    Stop-VM $_ -TurnOff -Force -EA SilentlyContinue
    Remove-VM $_ -Force
}
$vm = New-VM -Name $VmName -Generation 2 -MemoryStartupBytes 2GB -NoVHD
Set-VM $vm -AutomaticCheckpointsEnabled $false
Add-VMHardDiskDrive $vm -Path $espVhd
Add-VMHardDiskDrive $vm -Path $dataVhd
Set-VMFirmware $vm -EnableSecureBoot Off -BootOrder (Get-VMHardDiskDrive $vm | Where-Object Path -eq $espVhd)

Say 'starting the VM'
Start-VM $vm
$sw = [Diagnostics.Stopwatch]::StartNew()
while ((Get-VM $VmName).State -ne 'Off' -and $sw.Elapsed.TotalMinutes -lt $TimeoutMin) {
    Start-Sleep 2
}
$timedOut = (Get-VM $VmName).State -ne 'Off'
if ($timedOut) { Stop-VM $vm -TurnOff -Force }
Remove-VM $vm -Force
Say ("VM done after {0:N1} min{1}" -f $sw.Elapsed.TotalMinutes, $(if ($timedOut) { ' (TIMED OUT)' } else { '' }))

# --- 5. report ---------------------------------------------------------------
$disk = Mount-VHD $espVhd -Passthru | Get-Disk
$pn = (Get-Partition -DiskNumber $disk.Number | Where-Object Type -eq 'System').PartitionNumber
Set-Partition -DiskNumber $disk.Number -PartitionNumber $pn -NewDriveLetter $espLtr -EA SilentlyContinue
$resultPath = "${espLtr}:\_ECTEST_RESULT.txt"

$failed = 1
if (Test-Path $resultPath) {
    Write-Host ''
    Get-Content $resultPath | ForEach-Object { Write-Host $_ }
    Write-Host ''
    $tail = Get-Content $resultPath | Select-String 'failed=(\d+)'
    if ($tail) { $failed = [int]$tail.Matches[-1].Groups[1].Value }
} else {
    Write-Host "[ectest] no result file - EC did not reach the self-test" -ForegroundColor Yellow
}
Dismount-VHD $espVhd

if ($failed -eq 0 -and -not $timedOut) {
    Write-Host "[ectest] RESULT: ALL GOOD" -ForegroundColor Green
    exit 0
}
Write-Host "[ectest] RESULT: $failed check(s) failed" -ForegroundColor Red
exit 1
