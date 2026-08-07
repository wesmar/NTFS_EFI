# deploy-win11.ps1 - attach a REAL Windows NTFS volume (Win11.vhdx) to the EC
# test VM through ntfs.efi, so copy/delete can be exercised against a genuine
# 63 GB Windows filesystem (big $BITMAP, real $MFT, hardlinks, WOF) instead of
# a tiny synthetic volume.
#
# Attaches the ORIGINAL Win11.vhdx directly (user keeps an external backup).
# EC boots from its own FAT ESP; Win11 NTFS is the data disk it writes to.
param(
    [string]$VMName        = "EC-win11",
    [string]$Win11Vhd      = "C:\Virtual hard disks\Win11.vhdx",
    [string]$VhdDirectory  = "C:\Virtual hard disks",
    [string]$BootVhdName   = "ec-win11-boot.vhdx",
    [string]$NtfsDriverPath= "C:\Projekty\other\ntfs\bin\ntfs.efi",
    [switch]$SkipBuild,
    [switch]$NoStart
)
$ErrorActionPreference = "Stop"
$EcRoot      = "C:\projekty\ec"
$EcEfiPath   = Join-Path $EcRoot "bin\EC.efi"
$EcIniPath   = Join-Path $EcRoot "bin\EC.ini"
$BootVhdPath = Join-Path $VhdDirectory $BootVhdName

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]::new($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run elevated."
    }
}
Assert-Admin

if (-not $SkipBuild) {
    Write-Host "Building EC.efi..." -ForegroundColor Cyan
    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $EcRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "EC build failed" }
}
if (-not (Test-Path $EcEfiPath))     { throw "EC.efi not found: $EcEfiPath" }
if (-not (Test-Path $NtfsDriverPath)){ throw "ntfs.efi not found: $NtfsDriverPath" }
if (-not (Test-Path $Win11Vhd))      { throw "Win11 VHD not found: $Win11Vhd" }

# tear down any previous run and free the Win11 vhd from host/other VMs
if (Get-VM -Name $VMName -ErrorAction SilentlyContinue) {
    Write-Host "Removing existing VM '$VMName'..." -ForegroundColor Yellow
    Stop-VM -Name $VMName -Force -TurnOff -ErrorAction SilentlyContinue
    Remove-VM -Name $VMName -Force
}
$owner = Get-VM -ErrorAction SilentlyContinue | Where-Object { $_.HardDrives.Path -contains $Win11Vhd }
if ($owner) {
    Write-Host "Win11.vhdx attached to VM '$($owner.Name)' - detaching..." -ForegroundColor Yellow
    if ($owner.State -ne 'Off') { Stop-VM -Name $owner.Name -Force -TurnOff }
    Get-VMHardDiskDrive -VMName $owner.Name | Where-Object { $_.Path -eq $Win11Vhd } | Remove-VMHardDiskDrive
}
try { Dismount-VHD -Path $Win11Vhd -ErrorAction SilentlyContinue } catch {}
try { Dismount-VHD -Path $BootVhdPath -ErrorAction SilentlyContinue } catch {}

# build the EC boot ESP (FAT32): EC.efi as BOOTX64.EFI + ntfs.efi alongside
if (Test-Path $BootVhdPath) { Remove-Item $BootVhdPath -Force -ErrorAction SilentlyContinue }
Write-Host "Creating EC boot ESP..." -ForegroundColor Green
$disk = New-VHD -Path $BootVhdPath -SizeBytes 512MB -Dynamic | Mount-VHD -Passthru | Get-Disk
Initialize-Disk -Number $disk.Number -PartitionStyle GPT | Out-Null
$part = New-Partition -DiskNumber $disk.Number -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' -UseMaximumSize -AssignDriveLetter
Format-Volume -DriveLetter $part.DriveLetter -FileSystem FAT32 -NewFileSystemLabel EC_BOOT -Confirm:$false | Out-Null
$e = "$($part.DriveLetter):"
New-Item -ItemType Directory -Force -Path "$e\EFI\BOOT" | Out-Null
Copy-Item $EcEfiPath "$e\EFI\BOOT\BOOTX64.EFI" -Force
if (Test-Path $EcIniPath) { Copy-Item $EcIniPath "$e\EFI\BOOT\EC.ini" -Force }
Copy-Item $NtfsDriverPath "$e\EFI\BOOT\ntfs.efi" -Force
Copy-Item $NtfsDriverPath "$e\ntfs.efi" -Force
Dismount-VHD $BootVhdPath

# create the VM: boot from EC ESP, attach the real Win11 NTFS as data disk
Write-Host "Creating VM '$VMName'..." -ForegroundColor Green
New-VM -Name $VMName -Generation 2 -MemoryStartupBytes 4GB -Path $VhdDirectory -NoVHD | Out-Null
Set-VM -Name $VMName -CheckpointType Disabled -AutomaticCheckpointsEnabled $false
Add-VMHardDiskDrive -VMName $VMName -ControllerType SCSI -ControllerNumber 0 -ControllerLocation 0 -Path $BootVhdPath
Add-VMHardDiskDrive -VMName $VMName -ControllerType SCSI -ControllerNumber 0 -ControllerLocation 1 -Path $Win11Vhd
$boot = Get-VMHardDiskDrive -VMName $VMName | Where-Object { $_.Path -eq $BootVhdPath }
Set-VMFirmware -VMName $VMName -EnableSecureBoot Off -FirstBootDevice $boot
Set-VMVideo -VMName $VMName -ResolutionType Single -HorizontalResolution 1920 -VerticalResolution 1080

Write-Host "=== Ready ===" -ForegroundColor Cyan
Write-Host "VM '$VMName': boots EC.efi, REAL Win11 NTFS (63 GB) attached as data disk." -ForegroundColor Cyan
if (-not $NoStart) { Start-VM -Name $VMName; Write-Host "VM started - test copy TO / delete FROM the Win11 volume in EC." -ForegroundColor Green }
