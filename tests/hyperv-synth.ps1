# hyperv-synth.ps1 - run the probe BATTERY (incl. same-fs copy repro) on native
# Hyper-V against the synthetic ntfs-data.vhd, to catch Hyper-V-only bugs the
# QEMU virtio path doesn't show. Reads cs_dir\_SAMEFS.txt afterwards.
param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$root=(Split-Path $PSScriptRoot -Parent); $vmDir='C:\vm\ntfs-test'
$espVhd="$vmDir\synth-esp.vhdx"; $dataVhd="$vmDir\ntfs-data.vhd"; $vmName='ntfs-synth'
if(-not $SkipBuild){ & "$root\build.ps1"|Out-Null; if($LASTEXITCODE -ne 0){throw 'build failed'} }
Get-VM $vmName -EA SilentlyContinue|%{ Stop-VM $_ -TurnOff -Force -EA SilentlyContinue; Remove-VM $_ -Force }
# ESP
try{Dismount-VHD $espVhd -EA SilentlyContinue}catch{}; if(Test-Path $espVhd){Remove-Item $espVhd -Force}
$d=New-VHD -Path $espVhd -Dynamic -SizeBytes 256MB|Mount-VHD -Passthru|Get-Disk
Initialize-Disk -Number $d.Number -PartitionStyle GPT|Out-Null
$p=New-Partition -DiskNumber $d.Number -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' -UseMaximumSize -AssignDriveLetter
Format-Volume -DriveLetter $p.DriveLetter -FileSystem FAT32 -NewFileSystemLabel ESP -Confirm:$false|Out-Null
$e="$($p.DriveLetter):"; New-Item -ItemType Directory -Force -Path "$e\EFI\Boot"|Out-Null
Copy-Item "$root\bin\ntfs_probe.efi" "$e\EFI\Boot\BOOTX64.EFI" -Force
Copy-Item "$root\bin\ntfs.efi" "$e\ntfs.efi" -Force
# stage \src\many_split (62 long-named files -> INDX split with root separators)
# so the probe's copy test builds \copied\many_split and the drain/separator-
# delete test has a real B+tree directory to empty.
New-Item -ItemType Directory -Force -Path "$e\src\many_split" | Out-Null
0..61 | ForEach-Object { Set-Content -Path ("$e\src\many_split\synth_entry_{0:D2}_reasonably_long_filename.txt" -f $_) -Value ("data-$_") -NoNewline -Encoding Ascii }
Write-Host "[synth] staged \src\many_split (62 files)" -f DarkGray
Dismount-VHD $espVhd
# VM (ntfs-data.vhd is the NTFS synthetic with cs_dir fixtures)
try{Dismount-VHD $dataVhd -EA SilentlyContinue}catch{}
$vm=New-VM -Name $vmName -Generation 2 -MemoryStartupBytes 2GB -NoVHD
Set-VM $vm -AutomaticCheckpointsEnabled $false
Add-VMHardDiskDrive $vm -Path $espVhd
Add-VMHardDiskDrive $vm -Path $dataVhd
$boot=Get-VMHardDiskDrive $vm|?{$_.Path -eq $espVhd}
Set-VMFirmware $vm -EnableSecureBoot Off -BootOrder $boot
Write-Host "[synth] starting VM" -f Cyan
Start-VM $vm
$sw=[Diagnostics.Stopwatch]::StartNew()
while((Get-VM $vmName).State -ne 'Off' -and $sw.Elapsed.TotalMinutes -lt 8){ Start-Sleep 3 }
if((Get-VM $vmName).State -ne 'Off'){ Stop-VM $vm -TurnOff -Force }
Remove-VM $vm -Force
Write-Host ("[synth] VM done after {0:N1} min" -f $sw.Elapsed.TotalMinutes)
$drv=(Mount-VHD $dataVhd -ReadOnly -Passthru|Get-Disk|Get-Partition|?{$_.DriveLetter}|Select -First 1).DriveLetter
if(Test-Path "$drv`:\cs_dir\_SAMEFS.txt"){ Write-Host ("[synth] SAMEFS: "+(Get-Content "$drv`:\cs_dir\_SAMEFS.txt" -Raw).Trim()) -f Green }
else{ Write-Host "[synth] no _SAMEFS.txt - probe battery may not have reached it" -f Yellow }
if(Test-Path "$drv`:\cs_dir\samefs_dst.bin"){ Write-Host ("[synth] samefs_dst.bin actual size = "+(Get-Item "$drv`:\cs_dir\samefs_dst.bin").Length) }
if(Test-Path "$drv`:\cs_dir\_SEPTEST.txt"){ Write-Host ("[synth] SEPTEST: "+(Get-Content "$drv`:\cs_dir\_SEPTEST.txt" -Raw).Trim()) -f Green }
else{ Write-Host "[synth] no _SEPTEST.txt - drain test did not run" -f Yellow }
Dismount-VHD $dataVhd|Out-Null
