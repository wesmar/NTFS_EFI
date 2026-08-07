# hyperv-win11probe.ps1 - run the ntfs_probe BATTERY against the REAL Win11.vhdx
# NTFS volume on native Hyper-V (no EC, my tooling only). The probe detects
# \Windows, drains several real directories + does a copy, writes results to
# \_WIN11.txt, then powers off. We mount read-only afterward and print it.
param(
    [switch]$SkipBuild,
    # Which Windows volume to probe. Whatever is passed here gets attached to the
    # test VM as a live NTFS install, so point it at an image you can lose.
    [string]$Vhd    = 'C:\Virtual hard disks\Win11.vhdx',
    [string]$VmName = 'ntfs-win11probe',
    # Minutes to let the VM run. The plain battery finishes in seconds; the
    # -BigCopy tree copy of a real \Windows\System32 needs far longer.
    [int]$TimeoutMin = 10,
    # Drop \_EFICOPY.on onto the ESP, which switches the probe's heavy
    # same-volume copy of \Windows\System32 -> \_EFITEST_COPY on.
    [switch]$BigCopy
)
$ErrorActionPreference = 'Stop'
$root   = (Split-Path $PSScriptRoot -Parent)
$vmDir  = 'C:\vm\ntfs-test'
$espVhd = "$vmDir\win11probe-esp.vhdx"
$win11  = $Vhd
$vmName = $VmName
if (-not (Test-Path $win11)) { throw "no such VHDX: $win11" }
Write-Host "[win11probe] target: $win11" -ForegroundColor Yellow

if (-not $SkipBuild) { & "$root\build.ps1" | Out-Null; if ($LASTEXITCODE -ne 0) { throw 'build failed' } }

# Free Win11.vhdx from any VM / host mount - and REMEMBER where it came from.
# Borrowing the disk from someone else's VM and not giving it back leaves that
# VM booting with no data disk at all (a dual-panel file manager pointed at
# fs2: then shows two empty panels, which looks exactly like a driver failure
# and is not one). Every borrowed attachment is restored in the finally block
# at the bottom, whatever happens in between.
$borrowed = @()
Get-VM | Where-Object { $_.HardDrives.Path -contains $win11 } | ForEach-Object {
    $owner = $_
    if ($owner.State -ne 'Off') { Stop-VM $owner -TurnOff -Force }
    Get-VMHardDiskDrive -VMName $owner.Name | Where-Object { $_.Path -eq $win11 } | ForEach-Object {
        $borrowed += [pscustomobject]@{
            VMName             = $owner.Name
            ControllerType     = $_.ControllerType
            ControllerNumber   = $_.ControllerNumber
            ControllerLocation = $_.ControllerLocation
        }
        Remove-VMHardDiskDrive $_
    }
}
if ($borrowed.Count) {
    Write-Host ("[win11probe] borrowed {0} from: {1} (will be restored)" -f
        (Split-Path $win11 -Leaf), (($borrowed | ForEach-Object { $_.VMName }) -join ', ')) -ForegroundColor Yellow
}
try { Dismount-VHD $win11 -EA SilentlyContinue } catch {}

# Restore the borrowed attachments no matter how this script ends: a throw
# half-way through must not cost the owner its disk.
$restore = {
    foreach ($b in $script:borrowed) {
        if (-not (Get-VM $b.VMName -EA SilentlyContinue)) { continue }
        $already = Get-VMHardDiskDrive -VMName $b.VMName |
                   Where-Object { $_.Path -eq $script:win11 }
        if ($already) { continue }
        try {
            Add-VMHardDiskDrive -VMName $b.VMName -ControllerType $b.ControllerType `
                -ControllerNumber $b.ControllerNumber -ControllerLocation $b.ControllerLocation `
                -Path $script:win11 -EA Stop
            Write-Host ("[win11probe] restored {0} to {1} ({2} {3}:{4})" -f
                (Split-Path $script:win11 -Leaf), $b.VMName, $b.ControllerType,
                $b.ControllerNumber, $b.ControllerLocation) -ForegroundColor Green
        } catch {
            Write-Host ("[win11probe] COULD NOT restore {0} to {1}: {2}" -f
                (Split-Path $script:win11 -Leaf), $b.VMName, $_.Exception.Message) -ForegroundColor Red
        }
    }
}

try {

Get-VM $vmName -EA SilentlyContinue | ForEach-Object { Stop-VM $_ -TurnOff -Force -EA SilentlyContinue; Remove-VM $_ -Force }

# ESP with probe as BOOTX64.EFI + ntfs.efi
try { Dismount-VHD $espVhd -EA SilentlyContinue } catch {}
if (Test-Path $espVhd) { Remove-Item $espVhd -Force }
$d = New-VHD -Path $espVhd -Dynamic -SizeBytes 256MB | Mount-VHD -Passthru | Get-Disk
Initialize-Disk -Number $d.Number -PartitionStyle GPT | Out-Null
$p = New-Partition -DiskNumber $d.Number -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' -UseMaximumSize -AssignDriveLetter
Format-Volume -DriveLetter $p.DriveLetter -FileSystem FAT32 -NewFileSystemLabel ESP -Confirm:$false | Out-Null
$e = "$($p.DriveLetter):"; New-Item -ItemType Directory -Force -Path "$e\EFI\Boot" | Out-Null
Copy-Item "$root\bin\ntfs_probe.efi" "$e\EFI\Boot\BOOTX64.EFI" -Force
Copy-Item "$root\bin\ntfs.efi" "$e\ntfs.efi" -Force
if ($BigCopy) {
    Set-Content -Path "$e\_EFICOPY.on" -Value 'on' -NoNewline -Encoding Ascii
    Write-Host "[win11probe] BigCopy ON - probe will copy \Windows\System32 -> \_EFITEST_COPY" -ForegroundColor Yellow
}
Dismount-VHD $espVhd

# VM: boot ESP, attach Win11.vhdx as data
$vm = New-VM -Name $vmName -Generation 2 -MemoryStartupBytes 4GB -NoVHD
Set-VM $vm -AutomaticCheckpointsEnabled $false
Add-VMHardDiskDrive $vm -Path $espVhd
Add-VMHardDiskDrive $vm -Path $win11
$boot = Get-VMHardDiskDrive $vm | Where-Object { $_.Path -eq $espVhd }
Set-VMFirmware $vm -EnableSecureBoot Off -BootOrder $boot
Write-Host "[win11probe] starting VM" -ForegroundColor Cyan
Start-VM $vm
$sw = [Diagnostics.Stopwatch]::StartNew()
while ((Get-VM $vmName).State -ne 'Off' -and $sw.Elapsed.TotalMinutes -lt $TimeoutMin) { Start-Sleep 3 }
if ((Get-VM $vmName).State -ne 'Off') { Stop-VM $vm -TurnOff -Force }
Remove-VM $vm -Force
Write-Host ("[win11probe] VM done after {0:N1} min" -f $sw.Elapsed.TotalMinutes)

$part = Mount-VHD $win11 -ReadOnly -Passthru | Get-Disk | Get-Partition | Where-Object Size -gt 10GB | Select-Object -First 1
if ($part -and -not $part.DriveLetter) {
    Set-Partition -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -NewDriveLetter 'E' -ErrorAction SilentlyContinue
    $part = Get-Partition -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber
}
$drv = $part.DriveLetter
$resFile = Join-Path "$drv`:" "_EFITEST_RESULT.txt"
if (-not (Test-Path $resFile)) { $resFile = Join-Path "$drv`:" "_WIN11.txt" }
if ($drv -and (Test-Path $resFile)) {
    Write-Host "===== $resFile =====" -ForegroundColor Green
    Get-Content $resFile
    Write-Host "======================" -ForegroundColor Green
} else {
    Write-Host "[win11probe] no _WIN11.txt (drive=$drv) - probe may not have reached the test" -ForegroundColor Yellow
}
Dismount-VHD $win11 | Out-Null

} finally {
    & $restore
}
