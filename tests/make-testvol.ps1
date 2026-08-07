# make-testvol.ps1 - build a PRISTINE synthetic NTFS test volume from scratch.
#
# Replaces C:\vm\ntfs-test\ntfs-data.vhd with a freshly formatted 128 MiB
# NTFS volume containing exactly the fixtures the QEMU probe + test-qemu.ps1
# expect, and nothing else - so `chkdsk` starts from a provably clean state
# and any corruption a later test introduces is unambiguously ours.
#
# Fixtures:
#   \hello.txt              - MarkerHello (resident, small)
#   \big.txt                - MarkerBig at offset 0, padded multi-cluster
#                             (non-resident, exercises the read path)
#   \subdir\nested.txt      - MarkerNested
#   \cs_dir\                 - working directory for create/delete/write tests
#   \cs_dir\Foo.txt,foo.txt - case-sensitive pair (best-effort; needs
#                             per-directory case sensitivity support)
#   \cs_dir\real_ref.txt    - a genuine Windows-written reference file
#
# Usage:  .\make-testvol.ps1

$ErrorActionPreference = 'Stop'

$vmDir   = 'C:\vm\ntfs-test'
$vhd     = "$vmDir\ntfs-data.vhd"
$SizeMB  = 128

$MarkerHello  = 'NTFS_EFI_TEST_MARKER_9f3ab21c'
$MarkerNested = 'NTFS_EFI_TEST_NESTED_ok'
$MarkerBig    = 'NTFS_EFI_TEST_BIGFILE_MARKER_c471a0'

function Log($m) { Write-Host "[make-testvol] $m" -ForegroundColor Cyan }

# --- tear down any previous mount / file ---
try { Dismount-VHD -Path $vhd -ErrorAction SilentlyContinue } catch {}
if (Test-Path $vhd) { Remove-Item $vhd -Force; Log "removed old $vhd" }

New-Item -ItemType Directory -Force -Path $vmDir | Out-Null

# --- create + format ---
Log "creating fixed $SizeMB MiB VHD"
New-VHD -Path $vhd -Fixed -SizeBytes ($SizeMB * 1MB) | Out-Null

$disk = Mount-VHD -Path $vhd -PassThru | Get-Disk
Initialize-Disk -Number $disk.Number -PartitionStyle MBR | Out-Null
$part = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter
$drv  = $part.DriveLetter
Format-Volume -DriveLetter $drv -FileSystem NTFS -AllocationUnitSize 4096 -NewFileSystemLabel 'NTFSEFITEST' -Confirm:$false | Out-Null
$root = "$drv`:"
Log "formatted NTFS at $root"

# --- fixtures ---
Set-Content -Path "$root\hello.txt" -Value $MarkerHello -NoNewline -Encoding Ascii

# big.txt: marker first, then pad well past one cluster to force non-resident
$bigContent = $MarkerBig + "`n" + ('BIGDATA_' * 40000)   # ~320 KB
[System.IO.File]::WriteAllText("$root\big.txt", $bigContent, [System.Text.Encoding]::ASCII)

New-Item -ItemType Directory -Path "$root\subdir" | Out-Null
Set-Content -Path "$root\subdir\nested.txt" -Value $MarkerNested -NoNewline -Encoding Ascii

New-Item -ItemType Directory -Path "$root\cs_dir" | Out-Null
Set-Content -Path "$root\cs_dir\real_ref.txt" -Value ('x' * 29) -NoNewline -Encoding Ascii

# case-sensitive pair (best effort)
$csOk = $false
try {
    & fsutil.exe file setCaseSensitiveInfo "$root\cs_dir" enable | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Set-Content -Path "$root\cs_dir\Foo.txt" -Value 'UPPER_Foo_case_test_ok!!' -NoNewline -Encoding Ascii
        Set-Content -Path "$root\cs_dir\foo.txt" -Value 'lower_foo_case_test_ok!!' -NoNewline -Encoding Ascii
        $csOk = $true
    }
} catch {}
if (-not $csOk) {
    Set-Content -Path "$root\cs_dir\Foo.txt" -Value 'UPPER_Foo_case_test_ok!!' -NoNewline -Encoding Ascii
    Log "per-directory case sensitivity unavailable - only Foo.txt created"
} else {
    Log "case-sensitive Foo.txt/foo.txt created"
}

# aged_dir: 80 Windows-created long-named files. Windows auto-generates a DOS
# 8.3 alias for each (LONGFI~1.TXT), so every file has TWO $I30 index entries
# (Win32 + DOS) - exactly like real \Windows\Temp. Forces $INDEX_ALLOCATION
# with separators. This reproduces the two-index-entries-per-file delete path
# that driver-created single-entry files never exercise.
New-Item -ItemType Directory -Path "$root\aged_dir" | Out-Null
# Build a big index (200 files) then delete the middle 150 via WINDOWS, leaving
# ~50 files in an AGED B-tree: $INDEX_ALLOCATION blocks that Windows emptied /
# collapsed over time, with gaps and sparse/near-empty INDX blocks - the shape a
# real \Windows\Panther has, unlike a freshly-grown driver-created tree. This is
# what breaks the delete-rebalance. VARIABLE-length names (Delta != 0 promotion).
for ($i = 0; $i -lt 200; $i++) {
    $pad = 'x' * ($i % 40)
    Set-Content -Path ("$root\aged_dir\aged_{0:D3}_$pad.dat" -f $i) -Value ("d$i") -NoNewline -Encoding Ascii
}
for ($i = 25; $i -lt 175; $i++) {
    $pad = 'x' * ($i % 40)
    Remove-Item -Path ("$root\aged_dir\aged_{0:D3}_$pad.dat" -f $i) -Force
}
# interleave SUBDIRECTORIES (sort among the files) - the drain skips dirs, so if
# deleting sibling files corrupts a dir's index entry it shows as an orphaned
# dir (like RtBackup under WMI). Also a couple multi-stream (attr-list) files.
foreach ($n in 5, 15, 90, 180, 195) {
    New-Item -ItemType Directory -Force -Path ("$root\aged_dir\aged_{0:D3}_subdir" -f $n) | Out-Null
    Set-Content -Path ("$root\aged_dir\aged_{0:D3}_subdir\keep.txt" -f $n) -Value 'k' -NoNewline -Encoding Ascii
}
foreach ($n in 8, 92, 185) {
    $af = ("$root\aged_dir\aged_{0:D3}_multistream.etl" -f $n)
    Set-Content -Path $af -Value ('E' * 40000) -NoNewline -Encoding Ascii
    for ($s = 0; $s -lt 40; $s++) { Set-Content -Path ("${af}:s{0:D2}" -f $s) -Value ('z' * 150) -NoNewline -Encoding Ascii }
}
Log ("aged_dir aged: files=" + ((Get-ChildItem "$root\aged_dir" -File).Count) + " dirs=" + ((Get-ChildItem "$root\aged_dir" -Directory).Count))
# A file with MANY alternate data streams overflows its 1 KB base MFT record,
# forcing an $ATTRIBUTE_LIST with EXTENSION records - exactly the shape real
# fragmented Windows files (CBS.log, setup.exe) have. Deleting it exercises the
# extension-record free path (NtfsFreeAttributeListRecords). Put it in aged_dir
# so the drain test deletes it.
$alf = "$root\aged_dir\attrlist_ads.bin"
Set-Content -Path $alf -Value ('BASE' * 20000) -NoNewline -Encoding Ascii   # ~80 KB non-resident base $DATA
for ($s = 0; $s -lt 48; $s++) {
    Set-Content -Path ("${alf}:stream{0:D2}" -f $s) -Value (('s' * 200)) -NoNewline -Encoding Ascii
}
Log ("attrlist_ads.bin streams=48, attrs: " + ((cmd /c dir /r "$root\aged_dir\attrlist_ads.bin" 2>$null | Select-String ':\$DATA' | Measure-Object).Count))
Log ("aged_dir: 80 long-named files, sample 8.3: " + ((cmd /c dir /x "$root\aged_dir" 2>$null | Select-String '~' | Select-Object -First 1) -replace '\s+',' '))

# --- charset fixture: filenames Windows itself writes, in scripts whose case
# folding behaves differently from a-z. The driver collates with the volume's own
# $UpCase table, so every one of these must be found, listed and copied exactly
# the way Windows finds it.
#   - Polish diacritics    : folding beyond a-z ($UpCase folds U+0105 to U+0104)
#   - Cyrillic and Greek   : the same problem in other blocks
#   - CJK                  : no case at all, collation is plain code-unit order
#   - non-BMP (surrogates) : ONE character stored as TWO UTF-16 code units
# NTFS counts NameLength in code units, which is what the driver assumes.
# Names are built from code points so this script stays pure ASCII on disk.
function FromCp([int[]]$Points) { -join ($Points | ForEach-Object { [char]::ConvertFromUtf32($_) }) }

New-Item -ItemType Directory -Force -Path "$root\charset" | Out-Null

$csNames = @(
    @{ n = (FromCp @(0x7A,0x61,0x7A,0x6F,0x6C,0x63,0x5F) ) + (FromCp @(0x105,0x107,0x119,0x142,0x144,0xF3,0x15B,0x17A,0x17C)) + '.txt'; v = 'polish-lower' }
    @{ n = (FromCp @(0x5A,0x41,0x5A,0x4F,0x4C,0x43,0x5F) ) + (FromCp @(0x104,0x106,0x118,0x141,0x143,0xD3,0x15A,0x179,0x17B)) + '.txt'; v = 'polish-upper' }
    @{ n = (FromCp @(0x444,0x430,0x439,0x43B,0x5F,0x43A,0x438,0x440,0x438,0x43B,0x43B,0x438,0x446,0x430)) + '.txt';               v = 'cyrillic'     }
    @{ n = (FromCp @(0x3B1,0x3C1,0x3C7,0x3B5,0x3AF,0x3BF,0x5F,0x3B5,0x3BB,0x3BB)) + '.txt';                                      v = 'greek'        }
    @{ n = (FromCp @(0x4E2D,0x6587,0x6587,0x4EF6,0x540D,0x79F0)) + '.txt';                                                       v = 'chinese'      }
    @{ n = (FromCp @(0x65E5,0x672C,0x8A9E,0x30D5,0x30A1,0x30A4,0x30EB)) + '.txt';                                                v = 'japanese'     }
    @{ n = (FromCp @(0x20BB7,0x2000B)) + '_ext-b.txt';                                                                           v = 'cjk-ext-b'    }
    @{ n = 'emoji_' + (FromCp @(0x1F680,0x1F4BE)) + '.txt';                                                                      v = 'emoji'        }
)

$csOk = 0
foreach ($e in $csNames) {
    try {
        Set-Content -Path (Join-Path "$root\charset" $e.n) -Value $e.v -NoNewline -Encoding Ascii -ErrorAction Stop
        $csOk++
    } catch {
        Log "charset: nie utworzono '$($e.v)' - $($_.Exception.Message)"
    }
}

# A case-variant pair that only $UpCase folds: U+0105 vs U+0104. On a normal
# (case-insensitive) NTFS volume these are the SAME name, so the second write
# lands in the same file and there must be exactly ONE entry afterwards. This is
# the case NtfsInsertIndexEntrySmall used to get wrong: its hand-rolled a-z fold
# saw two distinct names and inserted two index entries.
$lower = Join-Path "$root\charset" ((FromCp @(0x6B,0x6F,0x6C,0x61,0x63,0x6A,0x61,0x5F,0x105)) + '.txt')
$upper = Join-Path "$root\charset" ((FromCp @(0x6B,0x6F,0x6C,0x61,0x63,0x6A,0x61,0x5F,0x104)) + '.txt')
Set-Content -Path $lower -Value 'a-ogonek' -NoNewline -Encoding Ascii
Set-Content -Path $upper -Value 'A-OGONEK' -NoNewline -Encoding Ascii
$pairEntries = (Get-ChildItem "$root\charset" -File | Where-Object { $_.Name -like 'kolacja_*' }).Count

Log ("charset: {0}/{1} nazw utworzonych, para ogonek daje {2} wpis(y) (1 = zwijanie jak `$UpCase), plikow razem={3}" -f `
    $csOk, $csNames.Count, $pairEntries, (Get-ChildItem "$root\charset" -File).Count)

Log ("root now contains: " + ((Get-ChildItem $root -Force | Select-Object -Expand Name) -join ', '))

Dismount-VHD -Path $vhd
Log "dismounted - volume ready at $vhd"
