<#
.SYNOPSIS
Synchronizes the minimal EDK II header set used by this repository.

.EXAMPLE
.\include\sync.ps1

.EXAMPLE
.\include\sync.ps1 -Check

.EXAMPLE
.\include\sync.ps1 -VisualUefiRoot D:\src\VisualUEFI -Prune
#>

[CmdletBinding()]
param(
    [string]$VisualUefiRoot,
    [switch]$Check,
    [switch]$Prune
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Check -and $Prune) {
    throw '-Check and -Prune cannot be used together.'
}

function Find-MdeIncludeRoot {
    param([string]$RequestedRoot)

    $candidates = @()
    if ($RequestedRoot) { $candidates += $RequestedRoot }
    if (${env:VISUAL_UEFI_ROOT}) { $candidates += ${env:VISUAL_UEFI_ROOT} }

    # The primary layout used by this repository's maintainer.
    $candidates += 'C:\Projekty\VisualUEFI'

    # Also find a VisualUEFI checkout next to any ancestor of this repository.
    $repositoryRoot = Split-Path $PSScriptRoot -Parent
    $ancestor = [IO.DirectoryInfo]::new($repositoryRoot)
    while ($null -ne $ancestor) {
        $candidates += (Join-Path $ancestor.FullName 'VisualUEFI')
        $ancestor = $ancestor.Parent
    }

    $tried = [Collections.Generic.List[string]]::new()
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }

        # Accept a VisualUEFI root, an edk2 root, or MdePkg\Include directly.
        foreach ($path in @(
            $candidate,
            (Join-Path $candidate 'edk2\MdePkg\Include'),
            (Join-Path $candidate 'MdePkg\Include')
        )) {
            $fullPath = [IO.Path]::GetFullPath($path)
            if (-not $seen.Add($fullPath)) { continue }
            [void]$tried.Add($fullPath)

            if ((Test-Path -LiteralPath (Join-Path $fullPath 'Uefi.h') -PathType Leaf) -and
                (Test-Path -LiteralPath (Join-Path $fullPath 'X64\ProcessorBind.h') -PathType Leaf)) {
                return (Resolve-Path -LiteralPath $fullPath).Path
            }
        }
    }

    throw ("Could not find EDK II MdePkg\Include. " +
           "Pass -VisualUefiRoot or set VISUAL_UEFI_ROOT. Tried:`n  " +
           ($tried -join "`n  "))
}

$requiredHeaders = @(
    'Base.h'
    'Guid\FileInfo.h'
    'Guid\FileSystemInfo.h'
    'Guid\FileSystemVolumeLabelInfo.h'
    'Guid\HiiFormMapMethodGuid.h'
    'Guid\PcAnsi.h'
    'Guid\WinCertificate.h'
    'IndustryStandard\Acpi.h'
    'IndustryStandard\Acpi10.h'
    'IndustryStandard\Acpi20.h'
    'IndustryStandard\Acpi30.h'
    'IndustryStandard\Acpi40.h'
    'IndustryStandard\Acpi50.h'
    'IndustryStandard\Acpi51.h'
    'IndustryStandard\Acpi60.h'
    'IndustryStandard\Acpi61.h'
    'IndustryStandard\Acpi62.h'
    'IndustryStandard\Acpi63.h'
    'IndustryStandard\Acpi64.h'
    'IndustryStandard\Acpi65.h'
    'IndustryStandard\Acpi66.h'
    'IndustryStandard\AcpiAml.h'
    'IndustryStandard\Bluetooth.h'
    'Library\BaseLib.h'
    'Library\BaseMemoryLib.h'
    'Library\MemoryAllocationLib.h'
    'Library\PrintLib.h'
    'Library\UefiBootServicesTableLib.h'
    'Library\UefiLib.h'
    'Library\UefiRuntimeServicesTableLib.h'
    'Protocol\BlockIo.h'
    'Protocol\ComponentName.h'
    'Protocol\ComponentName2.h'
    'Protocol\DevicePath.h'
    'Protocol\DiskIo.h'
    'Protocol\DriverBinding.h'
    'Protocol\DriverConfiguration.h'
    'Protocol\DriverConfiguration2.h'
    'Protocol\DriverDiagnostics.h'
    'Protocol\DriverDiagnostics2.h'
    'Protocol\GraphicsOutput.h'
    'Protocol\HiiFont.h'
    'Protocol\HiiImage.h'
    'Protocol\LoadedImage.h'
    'Protocol\SimpleFileSystem.h'
    'Protocol\SimpleTextIn.h'
    'Protocol\SimpleTextInEx.h'
    'Protocol\SimpleTextOut.h'
    'Uefi.h'
    'Uefi\UefiBaseType.h'
    'Uefi\UefiGpt.h'
    'Uefi\UefiInternalFormRepresentation.h'
    'Uefi\UefiMultiPhase.h'
    'Uefi\UefiPxe.h'
    'Uefi\UefiSpec.h'
    'X64\ProcessorBind.h'
)

$sourceRoot = Find-MdeIncludeRoot -RequestedRoot $VisualUefiRoot
$destinationRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'edk2'))
$includeRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
if (-not $destinationRoot.StartsWith($includeRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe destination root: $destinationRoot"
}

$required = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($relativePath in $requiredHeaders) {
    if (-not $required.Add($relativePath)) {
        throw "Duplicate required-header entry: $relativePath"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot $relativePath) -PathType Leaf)) {
        throw "Required header is absent from source: $relativePath"
    }
}

Write-Host "[sync] Source: $sourceRoot" -ForegroundColor Cyan
Write-Host "[sync] Target: $destinationRoot" -ForegroundColor DarkGray

$outdated = [Collections.Generic.List[string]]::new()
foreach ($relativePath in $requiredHeaders) {
    $source = Join-Path $sourceRoot $relativePath
    $destination = Join-Path $destinationRoot $relativePath
    if (-not (Test-Path -LiteralPath $destination -PathType Leaf) -or
        (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash) {
        [void]$outdated.Add($relativePath)
    }
}

if ($Check) {
    if ($outdated.Count -ne 0) {
        $outdated | ForEach-Object { Write-Host "[sync] OUTDATED: $_" -ForegroundColor Yellow }
        throw "$($outdated.Count) required header(s) differ from the source of truth."
    }
    Write-Host "[sync] OK: all $($required.Count) required headers are current." -ForegroundColor Green
    return
}

foreach ($relativePath in $outdated) {
    $source = Join-Path $sourceRoot $relativePath
    $destination = Join-Path $destinationRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
    Write-Host "[sync] Updated: $relativePath"
}

$prunedCount = 0
if ($Prune -and (Test-Path -LiteralPath $destinationRoot)) {
    $extraFiles = @(Get-ChildItem -LiteralPath $destinationRoot -Recurse -File | Where-Object {
        -not $required.Contains([IO.Path]::GetRelativePath($destinationRoot, $_.FullName))
    })
    foreach ($file in $extraFiles) {
        Remove-Item -LiteralPath $file.FullName -Force
        $prunedCount++
    }

    Get-ChildItem -LiteralPath $destinationRoot -Recurse -Directory |
        Sort-Object { $_.FullName.Length } -Descending |
        ForEach-Object {
            if (-not (Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1)) {
                Remove-Item -LiteralPath $_.FullName -Force
            }
        }
}

# Verify the copy instead of trusting timestamps.
foreach ($relativePath in $requiredHeaders) {
    $sourceHash = (Get-FileHash -LiteralPath (Join-Path $sourceRoot $relativePath) -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath (Join-Path $destinationRoot $relativePath) -Algorithm SHA256).Hash
    if ($sourceHash -ne $destinationHash) {
        throw "Post-sync verification failed: $relativePath"
    }
}

$extraCount = @(Get-ChildItem -LiteralPath $destinationRoot -Recurse -File | Where-Object {
    -not $required.Contains([IO.Path]::GetRelativePath($destinationRoot, $_.FullName))
}).Count

Write-Host ("[sync] OK: updated {0}, unchanged {1}, pruned {2}, extras remaining {3}." -f
    $outdated.Count, ($required.Count - $outdated.Count), $prunedCount, $extraCount) -ForegroundColor Green
