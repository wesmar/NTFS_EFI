# deploy.ps1 - Mount Win11 VHD, copy EfiTool.efi to ESP, unmount
# Run as Administrator on Hyper-V host.

param(
    [string]$VhdPath   = 'C:\Virtual hard disks\Win11.vhdx',
    [string]$EspLetter = 'S',
    [string]$EfiSrc    = "$PSScriptRoot\bin\EfiTool.efi",
    [string]$IniSrc    = "$PSScriptRoot\bin\EfiTool.ini",
    # Destination on ESP — matches Hyper-V boot entry \EFI\Boot\EfiTool.efi
    [string]$EfiDst    = 'EFI\Boot\EfiTool.efi'
)

$ErrorActionPreference = 'Stop'

Write-Host "--- EfiTool Deploy ---" -ForegroundColor Cyan

# 1. Mount VHD
Write-Host "Mounting VHD: $VhdPath" -ForegroundColor Gray
$vhd = Mount-VHD -Path $VhdPath -PassThru
$disk = $vhd.DiskNumber
Write-Host "  Disk number: $disk" -ForegroundColor Gray

# 2. Find EFI System Partition (GPT type GUID)
$efiGuid = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$efiPart = Get-Partition -DiskNumber $disk |
    Where-Object { $_.GptType -eq $efiGuid }

if (-not $efiPart) {
    Write-Error "EFI System Partition not found on disk $disk"
    Dismount-VHD -Path $VhdPath
    exit 1
}

Write-Host "  ESP: partition $($efiPart.PartitionNumber)" -ForegroundColor Gray

# 3. Assign drive letter
$accessPath = "${EspLetter}:\"
Add-PartitionAccessPath -DiskNumber $disk `
                        -PartitionNumber $efiPart.PartitionNumber `
                        -AccessPath $accessPath
Write-Host "  ESP mounted as ${EspLetter}:" -ForegroundColor Gray

try {
    # 4. Ensure destination directory exists
    $dstFull = Join-Path $accessPath $EfiDst
    $dstDir  = Split-Path $dstFull -Parent
    if (-not (Test-Path $dstDir)) {
        New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
        Write-Host "  Created: $dstDir" -ForegroundColor Gray
    }

    # 5. Copy EFI binary
    Copy-Item -Path $EfiSrc -Destination $dstFull -Force
    Write-Host "  Copied: $EfiSrc -> $dstFull" -ForegroundColor Green

    # 5a. Copy EfiTool.ini (from bin) next to EFI binary
    $iniDst = Join-Path $dstDir "EfiTool.ini"
    if (Test-Path $IniSrc) {
        Copy-Item -Path $IniSrc -Destination $iniDst -Force
        Write-Host "  Copied: $IniSrc -> $iniDst" -ForegroundColor Green
    }

    # 6. Show ESP contents for confirmation
    Write-Host ""
    Write-Host "ESP contents (EFI\):" -ForegroundColor Cyan
    Get-ChildItem "${EspLetter}:\EFI" -Recurse -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "  $($_.FullName.Substring(3))" -ForegroundColor Gray }

} finally {
    # 7. Remove drive letter and unmount
    Remove-PartitionAccessPath -DiskNumber $disk `
                                -PartitionNumber $efiPart.PartitionNumber `
                                -AccessPath $accessPath
    Dismount-VHD -Path $VhdPath
    Write-Host ""
    Write-Host "VHD unmounted." -ForegroundColor Gray
}

Write-Host ""
Write-Host "Deploy complete." -ForegroundColor Green
