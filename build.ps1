# build.ps1 - Builds NTFS_EFI, EC, and ntfs_probe with MSVC (without EDK2 BaseTools).
# Outputs: bin\ntfs.efi, bin\EC.efi, bin\ntfs_probe.efi
# The release archive is produced separately by pack-data.sh (data\NTFS_EFI.7z).
#
#   .\build.ps1            # production build
#   .\build.ps1 -Diag      # diagnostic NTFS refusal codes (NTFS_DIAG_STATUS=1)
#   .\build.ps1 -SelfTest  # compile the EC scripted test path (EC_SELFTEST=1)
#
# -Diag is intended only for identifying which NTFS restriction rejected an
# operation. Each NTFS_REFUSE site returns a distinct status instead of the
# generic EFI_UNSUPPORTED status observed by the application. Without this
# switch, the diagnostic argument is removed by the preprocessor and leaves no
# trace in the production image.
#
# -SelfTest applies only to EC. It enables EC_SELFTEST so SelfTest.c is included
# in the image. Without this switch, the translation unit compiles to nothing;
# the production image contains neither the test code nor the activation-flag
# check.
param([switch]$Diag, [switch]$SelfTest)
$ErrorActionPreference = "Stop"

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host " Building NTFS_EFI Release Artifacts (Pure C / MSVC)" -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan

# 1. Locate the newest Visual Studio/MSBuild installation through vswhere.
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe was not found. Install Visual Studio with the MSBuild component."
}

$msBuildPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe
if (-not $msBuildPath) {
    Write-Error "MSBuild.exe was not found in any Visual Studio installation."
}

Write-Host "[build] MSBuild : $msBuildPath" -ForegroundColor Green

# 2. Build configuration.
$config = "Release"
$platform = "x64"

# Prepare the output directory.
$binDir = Join-Path $PSScriptRoot "bin"
if (-not (Test-Path $binDir)) {
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null
}

# 3. Build ntfs.efi.
Write-Host "[build] Target  : ntfs.efi ($config|$platform)" -ForegroundColor Yellow
$ntfsVcxproj = Join-Path $PSScriptRoot "src\ntfs.vcxproj"
$diagArg = if ($Diag) { '/p:NtfsDiag=1' } else { '/p:NtfsDiag=0' }
if ($Diag) { Write-Host "[build] WARNING : NTFS diagnostic refusal codes are enabled; this is not a production image." -ForegroundColor Yellow }
& $msBuildPath $ntfsVcxproj /p:Configuration=$config /p:Platform=$platform $diagArg /t:Rebuild /m /nologo
if ($LASTEXITCODE -ne 0) { Write-Error "ntfs.vcxproj failed with exit code $LASTEXITCODE." }

# 4. Build EC.efi.
Write-Host "[build] Target  : EC.efi ($config|$platform)" -ForegroundColor Yellow
$ecVcxproj = Join-Path $PSScriptRoot "ec\EC.vcxproj"
$selfTestArg = if ($SelfTest) { '/p:EcSelfTest=1' } else { '/p:EcSelfTest=0' }
if ($SelfTest) { Write-Host "[build] WARNING : the EC scripted self-test path is compiled into EC.efi." -ForegroundColor Yellow }
& $msBuildPath $ecVcxproj /p:Configuration=$config /p:Platform=$platform $selfTestArg /t:Rebuild /m /nologo
if ($LASTEXITCODE -ne 0) { Write-Error "EC.vcxproj failed with exit code $LASTEXITCODE." }

# 5. Build ntfs_probe.efi.
Write-Host "[build] Target  : ntfs_probe.efi ($config|$platform)" -ForegroundColor Yellow
$probeVcxproj = Join-Path $PSScriptRoot "probe\ntfs_probe.vcxproj"
& $msBuildPath $probeVcxproj /p:Configuration=$config /p:Platform=$platform /t:Rebuild /m /nologo
if ($LASTEXITCODE -ne 0) { Write-Error "ntfs_probe.vcxproj failed with exit code $LASTEXITCODE." }

# 6. Collect all three EFI images in bin\.
$ntfsEfi = Join-Path $PSScriptRoot "bin\Release\ntfs.efi"
$ecEfi   = Join-Path $PSScriptRoot "ec\x64\Release\EC.efi"
$probeEfi= Join-Path $PSScriptRoot "bin\Release\ntfs_probe.efi"

if (Test-Path $ntfsEfi) { Copy-Item $ntfsEfi (Join-Path $binDir "ntfs.efi") -Force }
if (Test-Path $ecEfi)   { Copy-Item $ecEfi (Join-Path $binDir "EC.efi") -Force }
if (Test-Path $probeEfi){ Copy-Item $probeEfi (Join-Path $binDir "ntfs_probe.efi") -Force }

# 7. Verify that all expected outputs were produced.
$binFiles = @(
    (Join-Path $binDir "ntfs.efi"),
    (Join-Path $binDir "EC.efi"),
    (Join-Path $binDir "ntfs_probe.efi")
)
$missing = $binFiles | Where-Object { -not (Test-Path $_) }
if ($missing) { Write-Error "Missing build outputs: $($missing -join ', ')" }

# 8. Remove intermediate build directories.
if (Test-Path (Join-Path $PSScriptRoot "build")) { Remove-Item (Join-Path $PSScriptRoot "build") -Recurse -Force -ErrorAction SilentlyContinue }
if (Test-Path (Join-Path $PSScriptRoot "bin\Release")) { Remove-Item (Join-Path $PSScriptRoot "bin\Release") -Recurse -Force -ErrorAction SilentlyContinue }
if (Test-Path (Join-Path $PSScriptRoot "ec\x64")) { Remove-Item (Join-Path $PSScriptRoot "ec\x64") -Recurse -Force -ErrorAction SilentlyContinue }
if (Test-Path (Join-Path $PSScriptRoot "ec\build")) { Remove-Item (Join-Path $PSScriptRoot "ec\build") -Recurse -Force -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "========================================================" -ForegroundColor Cyan
Write-Host " Build completed successfully." -ForegroundColor Green
Write-Host " Outputs        : bin\ntfs.efi, bin\EC.efi, bin\ntfs_probe.efi" -ForegroundColor White
Write-Host " Release archive: ./pack-data.sh  ->  data\NTFS_EFI.7z" -ForegroundColor White
Write-Host "========================================================" -ForegroundColor Cyan
