# hyperv-bigtest.ps1 - run the big real-file copy on NATIVE Hyper-V (Gen2 UEFI),
# far faster than QEMU's software emulation. Boots ntfs_probe.efi as
# \EFI\BOOT\BOOTX64.EFI; it self-loads ntfs.efi, copies BIGSRC->BIGDST, writes
# \_RESULT.txt onto BIGDST, and powers off. verify-bigtest.ps1 checks the copy.
#
# Prereq: make-bigtest.ps1 built big-src.vhd + big-dst.vhd. Run elevated.
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root   = $PSScriptRoot
$vmDir  = 'C:\vm\ntfs-test'
$espVhd = "$vmDir\big-esp.vhdx"
$srcVhd = "$vmDir\big-src.vhd"
$dstVhd = "$vmDir\big-dst.vhd"
$vmName = 'ntfs-bigtest'

function Log($m){ Write-Host "[hyperv] $m" -ForegroundColor Cyan }

if (-not $SkipBuild) { & "$root\build.ps1" | Out-Null; if ($LASTEXITCODE -ne 0){ throw 'build failed' } }

# tear down any prior VM
Get-VM $vmName -EA SilentlyContinue | ForEach-Object { Stop-VM $_ -TurnOff -Force -EA SilentlyContinue; Remove-VM $_ -Force }

# --- build the ESP: FAT VHDX with \EFI\BOOT\BOOTX64.EFI (probe) + \ntfs.efi ---
try { Dismount-VHD $espVhd -EA SilentlyContinue } catch {}
if (Test-Path $espVhd) { Remove-Item $espVhd -Force }
Log 'building ESP VHDX'
$d = New-VHD -Path $espVhd -Dynamic -SizeBytes 256MB | Mount-VHD -Passthru | Get-Disk
Initialize-Disk -Number $d.Number -PartitionStyle GPT | Out-Null
$p = New-Partition -DiskNumber $d.Number -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' -UseMaximumSize -AssignDriveLetter
Format-Volume -DriveLetter $p.DriveLetter -FileSystem FAT32 -NewFileSystemLabel 'ESP' -Confirm:$false | Out-Null
$e = "$($p.DriveLetter):"
New-Item -ItemType Directory -Force -Path "$e\EFI\Boot" | Out-Null
Copy-Item "$root\bin\ntfs_probe.efi" "$e\EFI\Boot\BOOTX64.EFI" -Force
Copy-Item "$root\bin\ntfs.efi"       "$e\ntfs.efi" -Force
Dismount-VHD $espVhd

# --- reformat BIGDST empty ---
try { Dismount-VHD $dstVhd -EA SilentlyContinue } catch {}
$d2 = Mount-VHD $dstVhd -Passthru | Get-Disk
$p2 = $d2 | Get-Partition | Where-Object DriveLetter | Select-Object -First 1
Format-Volume -DriveLetter $p2.DriveLetter -FileSystem NTFS -AllocationUnitSize 4096 -NewFileSystemLabel 'BIGDST' -Confirm:$false | Out-Null
Dismount-VHD $dstVhd

# --- create + configure the Gen2 VM ---
Log 'creating Gen2 VM'
$vm = New-VM -Name $vmName -Generation 2 -MemoryStartupBytes 4GB -NoVHD
Set-VM $vm -ProcessorCount 2 -AutomaticCheckpointsEnabled $false
Add-VMHardDiskDrive $vm -Path $espVhd     # boot ESP
Add-VMHardDiskDrive $vm -Path $srcVhd      # BIGSRC (FAT)
Add-VMHardDiskDrive $vm -Path $dstVhd      # BIGDST (NTFS)
$espDrive = Get-VMHardDiskDrive $vm | Where-Object Path -eq $espVhd
Set-VMFirmware $vm -EnableSecureBoot Off -FirstBootDevice $espDrive

Log 'starting VM (native speed)'
$sw = [Diagnostics.Stopwatch]::StartNew()
Start-VM $vm
# probe powers off when done; wait for Off (cap 20 min)
while ((Get-VM $vmName).State -ne 'Off' -and $sw.Elapsed.TotalMinutes -lt 20) { Start-Sleep -Seconds 5 }
$sw.Stop()
Log ("VM finished/stopped after {0:N1} min (state={1})" -f $sw.Elapsed.TotalMinutes, (Get-VM $vmName).State)
if ((Get-VM $vmName).State -ne 'Off') { Stop-VM $vm -TurnOff -Force }
Remove-VM $vm -Force

# --- read the result the probe wrote onto BIGDST ---
$drv = (Mount-VHD $dstVhd -ReadOnly -Passthru | Get-Disk | Get-Partition | Where-Object DriveLetter | Select-Object -First 1).DriveLetter
if (Test-Path "$drv`:\_RESULT.txt") { Log ('RESULT: ' + (Get-Content "$drv`:\_RESULT.txt" -Raw).Trim()) }
else { Log 'no _RESULT.txt - probe may not have run' }
Dismount-VHD $dstVhd | Out-Null
Log 'done. Run verify-bigtest.ps1 for SHA256 + chkdsk.'
