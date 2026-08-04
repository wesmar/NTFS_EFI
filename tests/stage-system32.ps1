# stage-system32.ps1 - copy a representative selection of REAL
# C:\Windows\System32 files (plus a subdirectory) into the QEMU ESP under
# \src, so the EFI probe can recursively copy them onto the NTFS volume
# through ntfs.efi. Regular Copy-Item transparently decompresses any
# WOF/system-compressed system files, so \src holds plain bytes.

$ErrorActionPreference = 'Stop'
$esp = 'C:\vm\ntfs-test\esp'
$src = "$esp\src"

if (Test-Path $src) { Remove-Item $src -Recurse -Force }
New-Item -ItemType Directory -Force -Path $src        | Out-Null
New-Item -ItemType Directory -Force -Path "$src\etc"  | Out-Null

# mix of sizes: small resident-sized text, medium + larger PE files (force
# non-resident + multi-run growth), and a nested directory
$files = @(
    @{ From = 'C:\Windows\System32\notepad.exe';            To = "$src\notepad.exe" }
    @{ From = 'C:\Windows\System32\xcopy.exe';              To = "$src\xcopy.exe" }
    @{ From = 'C:\Windows\System32\cmd.exe';                To = "$src\cmd.exe" }
    @{ From = 'C:\Windows\System32\drivers\etc\hosts';      To = "$src\etc\hosts" }
    @{ From = 'C:\Windows\System32\drivers\etc\services';   To = "$src\etc\services" }
    @{ From = 'C:\Windows\System32\drivers\etc\protocol';   To = "$src\etc\protocol" }
)

$total = 0
foreach ($f in $files) {
    if (Test-Path $f.From) {
        Copy-Item $f.From $f.To -Force
        $sz = (Get-Item $f.To).Length
        $total += $sz
        Write-Host ("staged {0,-12} {1,10:N0} bytes" -f (Split-Path $f.To -Leaf), $sz)
    } else {
        Write-Host "MISSING (skipped): $($f.From)" -ForegroundColor Yellow
    }
}

# one BIG file (2-6 MB) to force non-resident multi-run growth during copy
$big = Get-ChildItem 'C:\Windows\System32\*.dll' -File |
       Where-Object { $_.Length -gt 2MB -and $_.Length -lt 6MB } |
       Select-Object -First 1
if ($big) {
    Copy-Item $big.FullName "$src\bigdll.dll" -Force
    $total += $big.Length
    Write-Host ("staged {0,-12} {1,10:N0} bytes (big: {2})" -f 'bigdll.dll', $big.Length, $big.Name)
}

# Create 5 subdirectories with 20 files each to test MFT expansion to > 64 files
# without overflowing any single directory index beyond its B-tree capacity
# Create a single directory many_split with 80 files to force multiple INDX leaf splits
$dirName = "many_split"
New-Item -ItemType Directory -Force -Path "$src\$dirName" | Out-Null
for ($i = 0; $i -lt 62; $i++) {
    Set-Content -Path ("$src\$dirName\synth_entry_{0:D2}_reasonably_long_filename.txt" -f $i) -Value ("data-$i") -NoNewline -Encoding Ascii
}
Write-Host "staged many_split dir with 62 files total to force leaf splits"

Write-Host ("staged total {0:N0} bytes into {1}" -f $total, $src)
