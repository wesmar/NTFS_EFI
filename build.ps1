# build.ps1 - Kompilacja NTFS_EFI + EC + ntfs_probe (MSVC, bez EDK2 BaseTools).
# Wynik: bin\ntfs.efi, bin\EC.efi, bin\ntfs_probe.efi
# Archiwum release tworzy pack-data.sh (data\NTFS_EFI.7z).
#
#   .\build.ps1            # build produkcyjny
#   .\build.ps1 -Diag      # diagnostyczne kody odmowy (NTFS_DIAG_STATUS=1)
#   .\build.ps1 -SelfTest  # EC z wkompilowana sciezka testu (EC_SELFTEST=1)
#
# -Diag sluzy WYLACZNIE do namierzenia, ktore ograniczenie odmowilo: kazde
# miejsce NTFS_REFUSE zwraca wtedy wlasny status zamiast jednego
# EFI_UNSUPPORTED, ktory widzi aplikacja. Bez tego przelacznika argument znika
# w preprocesorze, wiec build produkcyjny nie nosi po nim zadnego sladu.
#
# -SelfTest dotyczy wylacznie EC: wlacza EC_SELFTEST, przez co SelfTest.c w
# ogole trafia do binarki. Bez tego przelacznika plik kompiluje sie do niczego,
# wiec wersja produkcyjna nie zawiera ani kodu testu, ani sprawdzania flagi.
param([switch]$Diag, [switch]$SelfTest)
$ErrorActionPreference = "Stop"

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host " Building NTFS_EFI Release Package (Pure C / MSVC)" -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan

# 1. Wykrywanie najnowszej wersji Visual Studio / MSBuild przez vswhere
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe nie został znaleziony w systemie."
}

$msBuildPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe
if (-not $msBuildPath) {
    Write-Error "MSBuild.exe nie został znaleziony."
}

Write-Host "[+] Używam MSBuild: $msBuildPath" -ForegroundColor Green

# 2. Konfiguracja kompilacji
$config = "Release"
$platform = "x64"

# Przygotowanie katalogu bin
$binDir = Join-Path $PSScriptRoot "bin"
if (-not (Test-Path $binDir)) {
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null
}

# 3. Kompilacja projektu ntfs (ntfs.efi)
Write-Host "[+] Kompilacja ntfs.vcxproj (Release)..." -ForegroundColor Yellow
$ntfsVcxproj = Join-Path $PSScriptRoot "src\ntfs.vcxproj"
$diagArg = if ($Diag) { '/p:NtfsDiag=1' } else { '/p:NtfsDiag=0' }
if ($Diag) { Write-Host "[!] DIAGNOSTYKA: kody odmowy WLACZONE - to nie jest build release" -ForegroundColor Yellow }
& $msBuildPath $ntfsVcxproj /p:Configuration=$config /p:Platform=$platform $diagArg /t:Rebuild /m /nologo
if ($LASTEXITCODE -ne 0) { Write-Error "Kompilacja ntfs.vcxproj nie powiodła się." }

# 4. Kompilacja projektu EC (EC.efi)
Write-Host "[+] Kompilacja EC.vcxproj (Release)..." -ForegroundColor Yellow
$ecVcxproj = Join-Path $PSScriptRoot "ec\EC.vcxproj"
$selfTestArg = if ($SelfTest) { '/p:EcSelfTest=1' } else { '/p:EcSelfTest=0' }
if ($SelfTest) { Write-Host "    build SELF-TEST: sciezka testu jest wkompilowana w EC.efi" -ForegroundColor Yellow }
& $msBuildPath $ecVcxproj /p:Configuration=$config /p:Platform=$platform $selfTestArg /t:Rebuild /m /nologo
if ($LASTEXITCODE -ne 0) { Write-Error "Kompilacja EC.vcxproj nie powiodła się." }

# 5. Kompilacja projektu probe (ntfs_probe.efi)
Write-Host "[+] Kompilacja ntfs_probe.vcxproj (Release)..." -ForegroundColor Yellow
$probeVcxproj = Join-Path $PSScriptRoot "probe\ntfs_probe.vcxproj"
& $msBuildPath $probeVcxproj /p:Configuration=$config /p:Platform=$platform /t:Rebuild /m /nologo
if ($LASTEXITCODE -ne 0) { Write-Error "Kompilacja ntfs_probe.vcxproj nie powiodła się." }

# 6. Kopiowanie 3 binarek bezpośrednio do katalogu bin\
$ntfsEfi = Join-Path $PSScriptRoot "bin\Release\ntfs.efi"
$ecEfi   = Join-Path $PSScriptRoot "ec\x64\Release\EC.efi"
$probeEfi= Join-Path $PSScriptRoot "bin\Release\ntfs_probe.efi"

if (Test-Path $ntfsEfi) { Copy-Item $ntfsEfi (Join-Path $binDir "ntfs.efi") -Force }
if (Test-Path $ecEfi)   { Copy-Item $ecEfi (Join-Path $binDir "EC.efi") -Force }
if (Test-Path $probeEfi){ Copy-Item $probeEfi (Join-Path $binDir "ntfs_probe.efi") -Force }

# 7. Weryfikacja obecności wszystkich trzech binarek
$binFiles = @(
    (Join-Path $binDir "ntfs.efi"),
    (Join-Path $binDir "EC.efi"),
    (Join-Path $binDir "ntfs_probe.efi")
)
$missing = $binFiles | Where-Object { -not (Test-Path $_) }
if ($missing) { Write-Error "Brak plików wyjściowych: $($missing -join ', ')" }

# 8. Czyszczenie katalogów tymczasowych
if (Test-Path (Join-Path $PSScriptRoot "build")) { Remove-Item (Join-Path $PSScriptRoot "build") -Recurse -Force -ErrorAction SilentlyContinue }
if (Test-Path (Join-Path $PSScriptRoot "bin\Release")) { Remove-Item (Join-Path $PSScriptRoot "bin\Release") -Recurse -Force -ErrorAction SilentlyContinue }
if (Test-Path (Join-Path $PSScriptRoot "ec\x64")) { Remove-Item (Join-Path $PSScriptRoot "ec\x64") -Recurse -Force -ErrorAction SilentlyContinue }
if (Test-Path (Join-Path $PSScriptRoot "ec\build")) { Remove-Item (Join-Path $PSScriptRoot "ec\build") -Recurse -Force -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "========================================================" -ForegroundColor Cyan
Write-Host " KOMPILACJA ZAKOŃCZONA SUKCESEM" -ForegroundColor Green
Write-Host " Binarki w bin\: ntfs.efi, EC.efi, ntfs_probe.efi" -ForegroundColor White
Write-Host " Paczka release: ./pack-data.sh  ->  data\NTFS_EFI.7z" -ForegroundColor White
Write-Host "========================================================" -ForegroundColor Cyan
