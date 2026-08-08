<div align="center">

![Chess Engine Screenshot](images/ec.gif)

[![Latest Release](https://img.shields.io/github/v/release/wesmar/NTFS_EFI?label=Latest%20Release&style=for-the-badge)](https://github.com/wesmar/NTFS_EFI/releases/latest)

**[⬇ Download NTFS_EFI.7z](https://github.com/wesmar/NTFS_EFI/releases/download/latest/NTFS_EFI.7z)**
&nbsp;·&nbsp;
**[Source Code](https://github.com/wesmar/NTFS_EFI/archive/refs/heads/main.zip)**
&nbsp;·&nbsp;
**[All Releases](https://github.com/wesmar/NTFS_EFI/releases)**

> No archive password. Extract and copy the three `.efi` files.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)
[![Platform](https://img.shields.io/badge/Platform-UEFI%20x64-blue.svg)]()
[![Language](https://img.shields.io/badge/Language-Pure%20C11-green.svg)]()
[![Build](https://img.shields.io/badge/Build-MSVC%20vcxproj%20%E2%80%94%20no%20EDK2%20BaseTools-lightgrey.svg)]()

</div>

# EfiNtfs & EFI Commander — NTFS Read/Write Driver and File Manager for UEFI

<br>

<div align="center">

**Write to NTFS from firmware • `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` • chkdsk-clean unmount**

*B+tree index splits and collapses, `$MFT` growth, `$MFTMirr` lock-step mirroring, `$UpCase` collation*

*Pure C, plain MSVC `.vcxproj`, no EDK2 build system, no `ntfs-3g`*

</div>

<!-- SCREENSHOTS / VIDEO — drop files into images/ and uncomment:
![EFI Commander on an NTFS volume](images/ec-panels.png)
![ntfs_probe result on Hyper-V](images/probe-result.png)

<div align="center">

[![Video walkthrough](https://img.youtube.com/vi/VIDEO_ID/maxresdefault.jpg)](https://www.youtube.com/watch?v=VIDEO_ID)

</div>
-->

> **`ntfs.efi`** is a native NTFS **read + write** file system driver for UEFI x64, written in pure C. No EDK2 Python build system, no `ntfs-3g`, no POSIX emulation layer. It registers an `EFI_DRIVER_BINDING_PROTOCOL`, binds to every disk handle carrying a valid NTFS boot sector, and exposes the volume through `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` — exactly the interface the firmware's own FAT driver provides for FAT32. Every existing UEFI application, shell command or bootloader can therefore create, write, rename, move and delete files on NTFS with no code change. Volumes unmount **chkdsk-clean** and copies verify **SHA256 byte-exact** on Windows 11.
>
> **`EC.efi` (EFI Commander)** is a dual-panel file manager for the pre-boot environment (Norton Commander / Far Manager layout), shipped with the driver. It renders directly into the Graphics Output Protocol framebuffer with an embedded 8x16 bitmap font, bridges FAT32 and NTFS volumes behind one VFS layer, loads `ntfs.efi` itself when the firmware has not, and disconnects it cleanly on exit so the dirty flag is cleared.

---

## Table of Contents

- [Overview](#overview)
- [Components](#components)
- [Architecture](#architecture)
- [Read Path](#read-path)
- [Write Path](#write-path)
- [B+Tree Index Engine](#btree-index-engine)
- [Performance Engineering](#performance-engineering)
- [On-Disk Structures](#on-disk-structures)
- [Source Layout](#source-layout)
- [EFI Commander](#efi-commander)
- [ntfs_probe — Test Harness](#ntfs_probe--test-harness)
- [Testing and Validation](#testing-and-validation)
- [Status Codes](#status-codes)
- [Known Limitations](#known-limitations)
- [Deployment Layout](#deployment-layout)
- [Usage](#usage)
- [Building from Source](#building-from-source)
- [License](#license)

---

## Overview

UEFI firmware reads and writes FAT12/FAT16/FAT32 and nothing else. Every task that needs write access to an NTFS partition before an operating system starts — offline registry repair, bootloader patching, driver staging, forensic extraction, unattended recovery — has therefore required booting a full WinPE image or a Linux initrd with `ntfs-3g`, hundreds of megabytes of runtime to move a few files.

EfiNtfs closes that gap at the firmware layer. The driver is a DXE-loadable UEFI driver of a few hundred kilobytes that hands NTFS volumes to the rest of the firmware through the standard protocol, so the caller does not know or care that the volume is not FAT.

The ways of reaching an NTFS partition before an operating system starts, and what each one costs:

| Way in | What it costs | Can it write to NTFS? |
|---|---|---|
| The FAT driver already in the firmware | nothing, it is already there | No — it does not see NTFS at all |
| WinPE / Windows RE | ~400 MB image, a full Windows kernel boot | Yes |
| Linux initrd + `ntfs-3g` | 50-200 MB image, Linux kernel, FUSE, POSIX layer | Yes |
| Other UEFI NTFS drivers (EfiFs, the rEFInd set) | one small driver binary | No — they read NTFS, they refuse every write |
| **EfiNtfs (`ntfs.efi`)** | **one UEFI driver binary, `BlockIo` + `DiskIo` and nothing else** | **Yes, and the volume stays chkdsk-clean** |

The last two rows are the point: a small UEFI driver has always been the cheapest way in, and until now it could only read.

Design constraints that shaped the code:

- **No cache manager, no MCB, no FsRtl.** Run lists are decoded once into a flat array; attribute reads go straight to `EFI_DISK_IO_PROTOCOL`.
- **No CRT, no C++, no exceptions.** Pure C11 against a small set of prebuilt EDK2 static libraries under `lib/`.
- **Fail-closed on anything unrecognised.** A corrupt USA fixup or an unknown compression form rejects the operation instead of guessing.
- **Every write ordered so a crash leaks, never dangles.** Clusters are allocated and data written before the MFT record starts pointing at them; on failure the allocation is released.

---

## Components

| Binary | Role | Project |
|---|---|---|
| `ntfs.efi` | UEFI driver (`EFI_DRIVER_BINDING_PROTOCOL`) — the NTFS read and write engine; mounts every NTFS partition it is connected to | `src/ntfs.vcxproj` |
| `EC.efi` | UEFI application on GOP — dual-panel file manager with viewer, editor, recursive search, SHA-256 integrity tools and driver loader | `ec/EC.vcxproj` |
| `ntfs_probe.efi` | UEFI application — self-loading functional test harness and tree-copy benchmark | `probe/ntfs_probe.vcxproj` |

All three link only against the EDK2 static libraries shipped in `lib/`: `BaseLib`, `BasePrintLib`, `UefiApplicationEntryPoint`, `UefiBootServicesTableLib`, `UefiLib`, `UefiMemoryAllocationLib`, `UefiMemoryLib`, `UefiRuntimeServicesTableLib`. `src/ntfs_entry.c` supplies the `ProcessLibraryConstructorList` / `ProcessModuleEntryPointList` plumbing that the EDK2 AutoGen step would normally generate, which is what makes the plain-MSVC build possible.

---

## Architecture

```mermaid
flowchart TB
    subgraph APP["UEFI application layer"]
        SHELL["UEFI Shell<br/>ls, cp, rm, load"]
        EC["EFI Commander<br/>EC.efi"]
        PROBE["ntfs_probe.efi"]
    end

    subgraph PROTO["Protocol surface"]
        SFSP["EFI_SIMPLE_FILE_SYSTEM_PROTOCOL<br/>OpenVolume"]
        FILE["EFI_FILE_PROTOCOL<br/>Open, Read, Write, Delete, GetInfo, SetInfo, Flush"]
    end

    subgraph DRV["ntfs.efi engine"]
        BIND["Driver binding<br/>NTFS boot-sector probe, mount, unmount"]
        HANDLE["Handle factory and path lookup<br/>ntfs_file.c"]
        BTREE["B+tree index engine<br/>ntfs_btree.c, ntfs_create.c, ntfs_delete.c"]
        ATTR["Attribute engine<br/>ntfs_attr.c, ntfs_runlist.c, ntfs_lznt1.c, ntfs_wof.c"]
        ALLOC["Allocators<br/>ntfs_bitmap.c: $Bitmap, $MFT:$BITMAP"]
        MFT["MFT record I/O<br/>USA fixup, cache, $MFTMirr sync"]
    end

    subgraph FW["Firmware and media"]
        DISKIO["EFI_DISK_IO_PROTOCOL"]
        BLOCKIO["EFI_BLOCK_IO_PROTOCOL"]
        DISK["NTFS partition on NVMe, SATA, USB, VHD/VHDX"]
    end

    SHELL --> FILE
    EC --> FILE
    PROBE --> FILE
    SFSP --> FILE
    FILE --> HANDLE
    BIND --> SFSP
    HANDLE --> BTREE
    HANDLE --> ATTR
    BTREE --> ALLOC
    ATTR --> ALLOC
    BTREE --> MFT
    ATTR --> MFT
    ALLOC --> MFT
    MFT --> DISKIO
    DISKIO --> BLOCKIO
    BLOCKIO --> DISK
```

`NtfsEfiBindingSupported` accepts a controller only when `EFI_BLOCK_IO_PROTOCOL` and `EFI_DISK_IO_PROTOCOL` are present and sector 0 carries the `NTFS    ` OEM ID. `NtfsEfiBindingStart` then mounts the volume: boot-sector geometry, `$MFT` attribute context, `$UpCase` (MFT #10), volume label from `$Volume`, and free-space computation from `$Bitmap` (MFT #6). `NtfsEfiBindingStop` runs the reverse — trim, clear the dirty flag, `FlushBlocks`, free the VCB.

---

## Read Path

| Capability | Implementation |
|---|---|
| Resident `$DATA` | Read straight out of the fixed-up MFT record, no block I/O |
| Non-resident `$DATA` | Mapping pairs decoded once into a flat `NTFS_RUN_ENTRY` array (up to 2048 extents per attribute); multi-extent reads served from it |
| Sparse runs | Detected from the run's own offset-size nibble (`OffBytes == 0`). An LCN delta of zero is a legitimate fragment placement, so it is never treated as a hole |
| LZNT1-compressed `$DATA` | Full decompressor ported from the NT source (`RtlDecompressBufferLZNT1`); handles the "stored uncompressed" unit form and the compressed-run-plus-hole form |
| WOF-backed files | Reads `WofCompressedData` and decodes all four CompactOS algorithms: XPRESS4K, XPRESS8K, XPRESS16K and LZX; unsupported providers fail closed |
| WOF chunk reads | A chunk starts wherever the previous one ended, so its offset in the stream is arbitrary. Reads are widened to whole sectors: firmware DiskIo can refuse an unaligned multi-sector span outright, which is indistinguishable from a corrupt chunk |
| EFS streams | Refused with `EFI_UNSUPPORTED` rather than returning zeros |
| Directory enumeration | In-order B+tree walk across `$INDEX_ROOT` and every `$INDEX_ALLOCATION` `INDX` block, cached per handle on first `Read()` |
| Path lookup | Multi-level, `\` and `/` normalised, case-insensitive through the on-disk `$UpCase` table |
| `$ATTRIBUTE_LIST` | Followed on read, so attributes relocated into extension records are still found |
| Reparse points | `$REPARSE_POINT` symlink resolver for `\??\`, drive-letter and relative targets (MS-FSCC layout) |
| Volume metadata | Label from `$Volume`, free clusters from `$Bitmap`, both returned via `EFI_FILE_SYSTEM_INFO` |

Case folding uses the volume's own 65536-entry `$UpCase` table, the same table `chkdsk` and NTFS.sys collate with, so international names match the way Windows matches them, Polish `Ą Ć Ę Ł Ń Ó Ś Ź Ż` included. Every insert and every lookup goes through that one table, including the resident-`$INDEX_ROOT` insert, so a name is filed in the slot the next lookup will search. If `$UpCase` cannot be read the mount falls back to an ASCII identity table.

---

## Write Path

| Operation | What the driver writes |
|---|---|
| **Create** file or directory | New MFT record from `$MFT:$BITMAP`; full 72-byte extended `$STANDARD_INFORMATION` with inherited `SecurityId`; POSIX `$FILE_NAME`; empty `$DATA`; sorted insert into the parent index |
| **Write** in place, append, past EOF | Resident-to-non-resident promotion; multi-run growth with a 256 KB preallocation quantum; a seek past EOF zero-fills the gap |
| **Delete** file or empty directory | Leaf entry unlinked from the parent index; data clusters returned to `$Bitmap`; MFT record freed, `SequenceNumber` incremented, `$MFT:$BITMAP` bit cleared |
| **Rename** in place | Duplicate check, `$FILE_NAME` resized, old entry unlinked, new entry inserted; sizes and timestamps kept in sync |
| **Move** across directories | `NtfsEfiMoveFile` — generalised rename with a parent-cycle guard |
| **SetInfo** timestamps | `CreationTime`, `ModificationTime`, `LastAccessTime` written to `$STANDARD_INFORMATION`, to the file's own `$FILE_NAME` and to the `$FILE_NAME` copy inside the parent index entry; a zero `EFI_TIME` means "leave unchanged", per spec |
| **SetInfo** attributes | DOS `ReadOnly`, `Hidden`, `System`, `Archive` written to all three of those locations |
| **SetInfo** file size | `NtfsEfiSetFileSize` shrinks resident and non-resident data; growth goes through the seek-write-past-EOF path |
| **Close** | `NtfsEfiTrimAllocation` releases preallocation slack back to `$Bitmap` |
| **`$MFT` growth** | `NtfsGrowMft` appends a zeroed 256-cluster chunk as a merged run — 1024 records for one mapping pair; `$MFT:$BITMAP` grows with it, resident in place or non-resident inside the clusters it owns, taking another run only when those are used up |
| **`$MFTMirr`** | Every write to MFT records 0-3 is mirrored in lock-step |
| **Unmount** | Preallocation trimmed, `$Volume` dirty flag cleared, `BlockIo->FlushBlocks` issued |

The driver always writes the 72-byte `$STANDARD_INFORMATION` form. The shorter pre-NT4 form, and a `SecurityId` of zero, leave the record without a valid reference into the volume-wide `$Secure` store, and NTFS.sys reports such a file as corrupt.

```mermaid
sequenceDiagram
    autonumber
    participant App as UEFI app or EC.efi
    participant Proto as EFI_FILE_PROTOCOL
    participant Drv as ntfs.efi
    participant Bmp as $Bitmap allocator
    participant Mft as MFT record I/O
    participant Disk as DiskIo

    App->>Proto: Write(Handle, Size, Buffer)
    Proto->>Drv: NtfsEfiWrite
    Drv->>Drv: Mark $Volume dirty (once per mount)
    alt needs more clusters
        Drv->>Bmp: allocate ceil(need) + 256 KB quantum
        Bmp->>Bmp: scan in-RAM $Bitmap mirror from alloc cursor
        Bmp->>Disk: write only the changed bitmap bytes
        Bmp-->>Drv: run list, merged with the previous run if contiguous
    end
    Drv->>Disk: write payload to data clusters
    Drv->>Mft: update $DATA sizes, $STANDARD_INFORMATION, $FILE_NAME
    alt MFT record 0..3
        Mft->>Disk: mirror the record into $MFTMirr
    end
    Mft->>Disk: write fixed-up MFT record
    Drv-->>App: EFI_SUCCESS
    note over Drv,Bmp: on any failure the new clusters and MFT record are released first

    App->>Proto: Close(Handle)
    Proto->>Drv: NtfsEfiClose
    Drv->>Bmp: NtfsEfiTrimAllocation (release prealloc slack)
    App->>Drv: DisconnectController
    Drv->>Disk: clear $Volume dirty flag, FlushBlocks
```

---

## B+Tree Index Engine

An NTFS directory is a collation-sorted B+tree. Small directories keep every entry in the resident `$INDEX_ROOT` attribute inside the directory's own MFT record; once that overflows, entries move into non-resident `INDX` blocks addressed by `$INDEX_ALLOCATION`. The driver implements both directions of that transition and both directions of the tree's growth, which is what keeps a volume `chkdsk`-clean after heavy directory mutation.

```mermaid
flowchart TD
    ROOT["$INDEX_ROOT (resident, in the MFT record)<br/>separator keys only once the tree has grown"]
    B0["INDX block 0 (leaf)<br/>cmd.exe, driver.sys, notepad.exe"]
    B1["INDX block 1 (leaf)<br/>kernel32.dll, ntoskrnl.exe, shell.dll"]
    B2["INDX block 2 (leaf)<br/>winload.efi, winresume.efi, zydis.dll"]

    ROOT -->|sub-node VCN| B0
    ROOT -->|sub-node VCN| B1
    ROOT -->|sub-node VCN| B2

    INS["Insert: NtfsBtreeInsertRec<br/>descend, split full leaf, promote median"]
    PUSH["Root overflow: NtfsBtreePushDownRoot<br/>root entries move into a new INDX leaf"]
    DEL["Delete: NtfsEfiIndexRemoveByChild<br/>unlink leaf entry"]
    MAX["Separator delete: NtfsExtractMaxKey<br/>promote in-order predecessor from the left subtree"]
    COL["Shrink: NtfsCollapseIndexToResident<br/>free INDX clusters, fold back into $INDEX_ROOT"]

    INS --> ROOT
    PUSH --> ROOT
    DEL --> B1
    MAX --> B1
    COL --> ROOT
```

Node operations, in the order the code performs them:

1. **Directed descent for `Open()`.** `NtfsEfiSearchIndexBlock` and `NtfsEfiSearchSubNode` compare the search key against the sorted entries of a node and descend into exactly one child, `O(log n)` in the number of entries. The exhaustive walk (`NtfsEfiScanIndexBlock`, `NtfsEfiBrowseSubNode`) serves full enumeration, where every entry has to be visited anyway.
2. **Root overflow.** `NtfsConvertRootToSingleIndexAllocation` and `NtfsBtreePushDownRoot` move the resident entries into the first `INDX` leaf. A push-down runs only while the root still holds real entries, so a root already reduced to its `END` pointer cannot stack further empty levels. Attribute contexts are re-decoded afterwards, since the attribute offset inside the record has shifted.
3. **Leaf and internal splits.** `NtfsBtreeInsertRec` descends recursively, splits a full block, builds the separator (`NtfsMakeSeparator`) and inserts it into the parent (`NtfsRootInsertSep`), propagating upward as far as needed. Split blocks are queued and written only after the record they hang off is on disk, so a failed insert leaves no block that nothing points at.
4. **Rebalance on delete.** Deleting a leaf entry is an unlink. Deleting a separator key that owns a subtree promotes the in-order predecessor out of the left subtree (`NtfsExtractMaxKey`, recursion depth capped at 32); an empty subtree drops the separator. The operation is refused with `EFI_UNSUPPORTED` when the replacement key would not fit in the host block.
5. **Emptied blocks leave the tree.** A block that loses its last key would otherwise stay marked in `$BITMAP:$I30` and stay hanging off an entry in its parent, which `chkdsk` reports as free space marked allocated. `NtfsDropEmptyIndexBlock` reverses the split: the parent's `END` entry gives up its child pointer and returns to its 16-byte form, while a separator carrying a key leaves the node whole and goes back into the tree through the ordinary insert (`NtfsInsertIndexKey`, which re-inserts an entry byte for byte so the `$FILE_NAME` copy Windows checks stays intact). Only a delete that empties a block pays for it.
6. **Collapse.** When deletions bring a directory back within the MFT record's free space, `NtfsCollapseIndexToResident` frees the `INDX` clusters in `$Bitmap` and folds the index back into a resident-only `$INDEX_ROOT`, the inverse of step 2. `NtfsTrimIndexAllocation` then cuts `$INDEX_ALLOCATION` back to a single block: the tail clusters go back to `$Bitmap` and the mapping pairs are re-encoded for what is left, so the record recovers the space they took. One block stays mapped on purpose, which is what lets the next create refill the directory without allocating anything.
7. **Two records for one index.** Windows routinely leaves `$INDEX_ROOT` in the directory's base record and moves `$INDEX_ALLOCATION` and `$BITMAP:$I30` into an `$ATTRIBUTE_LIST` extension record. `NtfsEfiResolveIndexHost` resolves both hosts, and the insert, the delete and the collapse each edit the record their own attributes live in. Every record is written back under the MFT index the resolver reports; the record's own `MFTRecordNumber` field is on-disk data and is never used for that.
8. **Block allocation for the index.** `NtfsBtreeAllocBlock` first hands out a block the directory already owns and has free, which needs no new mapping pair; the driver's delete-all keeps the mapping while clearing the `$BITMAP:$I30` bits, so such blocks are common. Fresh clusters are taken 8 blocks to a run, with a fallback through 4, 2 and 1 when the volume cannot place them contiguously. The mapping pairs live in the directory's own MFT record, so keeping their number down is what sets the practical ceiling on entries per directory.
9. **Rightmost child pointers.** A node's rightmost child VCN lives inside its `END` entry, while `Header + TotalSizeOfEntries` addresses the byte after that entry. Both split paths read the pointer from the entry, and a node flagged `NODE` whose `END` entry is too short to hold a VCN is rejected as corrupt.

---

## Performance Engineering

Every mechanism below was chosen against a measurement. The driver keeps deterministic counters (`gNtfsReadBytes`, `gNtfsWriteBytes`, `gNtfsRecordReads`), which give repeatable numbers where wall-clock timings on a loaded QEMU host do not.

| Mechanism | Problem it removed |
|---|---|
| In-RAM `$Bitmap` mirror, loaded once at mount, only changed bytes written through | The allocator re-read the entire on-disk `$Bitmap` on every allocation — `O(volume)` I/O per run, roughly doubling total disk traffic on a large copy |
| Rolling allocation cursor `BitmapAllocHint` | First-fit restarted from bit 0 each time, rescanning space it had just filled |
| Word-at-a-time free-run scan | Bit-at-a-time walk over up to ~256 million bits |
| MFT record cache: 16 entries x 4096 B, keyed by MFT index, refreshed by every writer | `$MFT`, `$MFTMirr`, `$Bitmap` and the working directory record were re-read constantly; record reads dropped by ~99% in large-copy workloads |
| Per-handle directory enumeration cache, built once, then `O(1)` per entry | `Read()` on a directory handle re-walked the B+tree for every single entry — `O(n^2)` |
| Directed single-branch B+tree descent for `Open()` | Exhaustive index scan per lookup — `O(n)` on directories with thousands of entries |
| Sequential-read cursor inside the attribute context (`CacheIdx`, `CacheOffset`) | Run-list rescan from run 0 on every sequential read |
| Run merging in `NtfsAppendRunToAttr` | One mapping pair per allocated chunk overflowed the 1 KB MFT record at about 127 MB of `$MFT` or `$INDEX_ALLOCATION` growth |
| Longest free run kept during a bitmap scan, not the last one | A caller asking for a large chunk and willing to take less — `$MFT` growth — walked past a long run, reset on a used bit and was handed the few clusters at the end of the range |
| 256 KB preallocation quantum, trimmed on `Close()` | 1 MB left ~2.2x allocation slack that the trim could not fully reclaim at scale; 64 KB quadrupled the number of grow calls. 256 KB lands at ~1.4x slack before trim |

Measured end to end: **7 GB of mixed real data copied from a FAT source to NTFS through the driver in about 4 minutes** on a Hyper-V Gen 2 VM, `bad=0`, volume `chkdsk`-clean afterwards.

---

## On-Disk Structures

`src/ntfs.h` defines every physical NTFS structure the driver touches, with no WDM, ReactOS or ntfs-3g headers behind it. Layouts are raw struct overlays valid on little-endian x64.

### Boot sector

```c
#pragma pack(push, 1)
typedef struct {
    USHORT BytesPerSector;
    UCHAR  SectorsPerCluster;
    UCHAR  Unused0[7];
    UCHAR  MediaId;
    UCHAR  Unused1[2];
    USHORT SectorsPerTrack;
    USHORT Heads;
    UCHAR  Unused2[4];
    UCHAR  Unused3[4];
} NTFS_BPB;

typedef struct {
    USHORT    Unknown[2];
    ULONGLONG SectorCount;
    ULONGLONG MftLocation;          /* LCN of $MFT                          */
    ULONGLONG MftMirrLocation;      /* LCN of $MFTMirr                      */
    CCHAR     ClustersPerMftRecord; /* signed: negative = 2^-n bytes        */
    UCHAR     Unused4[3];
    CCHAR     ClustersPerIndexRecord;
    UCHAR     Unused5[3];
    ULONGLONG SerialNumber;
    UCHAR     Checksum[4];
} NTFS_EBPB;

typedef struct {
    UCHAR     Jump[3];
    UCHAR     OEMID[8];             /* "NTFS    " - the binding probe       */
    NTFS_BPB  BPB;
    NTFS_EBPB EBPB;
    UCHAR     BootStrap[426];
    USHORT    EndSector;
} NTFS_BOOT_SECTOR;
#pragma pack(pop)
```

### MFT file record

```c
typedef struct {
    ULONG     Type;                 /* "FILE" 0x454C4946 / "INDX" 0x58444E49 */
    USHORT    UsaOffset;            /* update sequence array offset          */
    USHORT    UsaCount;             /* USA size in words                     */
    ULONGLONG Lsn;
} NTFS_RECORD_HEADER;

typedef struct {
    NTFS_RECORD_HEADER Ntfs;
    USHORT    SequenceNumber;       /* incremented on record reuse           */
    USHORT    LinkCount;            /* hard links                            */
    USHORT    AttributeOffset;
    USHORT    Flags;                /* 0x0001 in use, 0x0002 directory       */
    ULONG     BytesInUse;
    ULONG     BytesAllocated;
    ULONGLONG BaseFileRecord;       /* 0 for a base record                   */
    USHORT    NextAttributeNumber;
    USHORT    Padding;
    ULONG     MFTRecordNumber;
} FILE_RECORD_HEADER, *PFILE_RECORD_HEADER;
```

Every record read applies the update sequence array fixup and every record write un-applies it (`ntfs_mft.c`); a record whose USN does not match its per-sector copies is rejected as corrupt rather than parsed. `UsaOffset` and `UsaCount` are bounds-checked against the record size the type implies before either direction runs — both loops rewrite the last two bytes of every sector-sized chunk, so an out-of-range count would write outside the buffer.

### Attribute record

```c
typedef struct {
    ULONG  Type;                    /* 0x10 $STD_INFO, 0x30 $FILE_NAME,
                                     * 0x80 $DATA, 0x90 $INDEX_ROOT,
                                     * 0xA0 $INDEX_ALLOCATION, 0xC0 $REPARSE */
    ULONG  Length;
    UCHAR  IsNonResident;
    UCHAR  NameLength;
    USHORT NameOffset;
    USHORT Flags;                   /* compressed / sparse / encrypted       */
    USHORT Instance;
    union {
        struct { ULONG ValueLength; USHORT ValueOffset; UCHAR Flags; UCHAR Reserved; } Resident;
        struct {
            ULONGLONG LowestVCN;
            ULONGLONG HighestVCN;
            USHORT    MappingPairsOffset;
            USHORT    CompressionUnit;
            UCHAR     Reserved[4];
            LONGLONG  AllocatedSize;
            LONGLONG  DataSize;
            LONGLONG  InitializedSize;
            LONGLONG  CompressedSize;
        } NonResident;
    };
} NTFS_ATTR_RECORD, *PNTFS_ATTR_RECORD;
```

### Decoded run list

```c
typedef struct {
    UINT64 VBN;   /* offset within the attribute, in clusters */
    INT64  LBN;   /* logical cluster on disk; -1 = sparse     */
    UINT64 Len;   /* extent length in clusters                */
} NTFS_RUN_ENTRY;

#define NTFS_MAX_RUNS 2048   /* 2048 extents per attribute */
```

Mapping pairs are decoded into this array once, when the attribute context is created — the replacement for `LARGE_MCB` and the `FsRtl` machinery a kernel driver would use.

---

## Source Layout

One responsibility per file, no header outside `src/ntfs.h`, nothing pulled in from WDM, ReactOS or `ntfs-3g`.

```
NTFS_EFI/
├── src/                      # ntfs.efi — the driver: 19 translation units, one header
│   ├── ntfs.h                # Every on-disk structure, VCB and handle types, all prototypes
│   ├── ntfs_entry.c          # Module entry point, AutoGen-replacement plumbing
│   ├── ntfs_binding.c        # EFI_DRIVER_BINDING_PROTOCOL: Supported/Start/Stop, NTFS OEM-ID probe
│   ├── ntfs_volume.c         # Mount and unmount, geometry, $UpCase load, label, free space, dirty flag
│   ├── ntfs_diskio.c         # EFI_DISK_IO_PROTOCOL wrapper, byte and record counters
│   ├── ntfs_mft.c            # USA fixup, record read/write, 16-entry record cache, $MFTMirr sync
│   ├── ntfs_attr.c           # Attribute contexts, ReadAttr/WriteAttr, FindAttribute + $ATTRIBUTE_LIST
│   ├── ntfs_runlist.c        # Mapping-pair decode and encode, explicit sparse-run flag
│   ├── ntfs_btree.c          # Directory lookup: O(log n) descent, in-order collect, $UpCase collation
│   ├── ntfs_bitmap.c         # $Bitmap allocator (RAM mirror + cursor), $MFT:$BITMAP, NtfsGrowMft
│   ├── ntfs_create.c         # Create file/dir, index insert, B+tree split, root push-down, rollback
│   ├── ntfs_delete.c         # Delete, index unlink, separator rebalance, collapse to resident
│   ├── ntfs_setinfo.c        # SetInfo timestamps and attributes, rename, move, resize, prealloc trim
│   ├── ntfs_file.c           # Handle factory, path lookup, all EFI_FILE_PROTOCOL methods, dir cache
│   ├── ntfs_lznt1.c          # LZNT1 decompressor, ported from the NT source codec
│   ├── ntfs_wof.c            # WOF reparse reader, XPRESS-Huffman and LZX decompressors
│   ├── ntfs_symlink.c        # $REPARSE_POINT symlink target resolver
│   ├── ntfs_time.c           # NTFS 100 ns ticks to and from EFI_TIME
│   ├── ntfs_globals.c        # GUIDs, PCD/HII stubs, debug print, performance counters
│   └── DebugLog.c            # Optional ESP log, compiled out unless ENABLE_DEBUG_LOG is set
│
├── ec/                       # EC.efi — EFI Commander
│   ├── Main.c                # Entry point, event loop, key dispatch, F9 menus, quick-find
│   ├── UiConsole.c           # Direct GOP framebuffer output, glyph and rectangle blitting
│   ├── Gui.c                 # Panels, legend, dialogs, list picker, Quick View, help
│   ├── Colors.h              # The whole palette in one place, shared by every drawing file
│   ├── Panel.c               # Panel state, sort by name/extension/size/date, filter mask, scrolling
│   ├── PanelOps.c            # Tagging: all, none, by mask, invert, panel-against-panel compare
│   ├── FileSystem.c          # VFS over EFI_FILE_PROTOCOL, recursive ops, ntfs.efi load and disconnect
│   ├── Checksum.c            # SHA-256 and CRC32 streamed from EFI files, no external crypto
│   ├── Sync.c                # Recursive tree compare and one-way update on top of Checksum.c
│   ├── Search.c              # Recursive find by name, mask or file contents, bounded and cancellable
│   ├── FileProps.c           # DOS attribute bits and modification time of a single entry
│   ├── UefiTools.c           # Read-only BootOrder / BootNext / Boot#### view
│   ├── Console.c             # Ctrl+O prompt: built-in commands, UEFI Shell fallback
│   ├── Navigation.c          # Per-panel path history, directory hotlist
│   ├── Viewer.c              # Read-only viewer: text scrolling, hex dump, byte search
│   ├── Editor.c              # Text and hex editor: line buffer, cursor, save
│   ├── Config.c              # EC.ini parser and writer, driver-path resolution
│   ├── SelfTest.c            # Scripted self-test, compiled out unless EC_SELFTEST is set
│   └── BmpFont.h             # Embedded 8x16 monospace glyph matrix
│
├── probe/
│   └── ntfs_probe.c          # Self-loading harness: unit tests, tree copy, result report, shutdown
│
├── include/edk2/             # EDK2 public headers (MdePkg subset)
├── lib/                      # Prebuilt EDK2 static libraries — the only external dependency
├── tests/                    # QEMU and Hyper-V scripts: VHD creation, staging, verification
├── bin/                      # Build output: ntfs.efi, EC.efi, ntfs_probe.efi
└── build.ps1                 # Finds MSBuild via vswhere, builds all three, packs the release
```

The bulk of the code sits in four files: `ntfs_create.c` and `ntfs_delete.c` are the B+tree engine, `ntfs_file.c` carries the protocol surface and non-resident growth, and `ntfs_bitmap.c` holds the allocator that sets copy throughput.

---

## EFI Commander

`EC.efi` is a complete pre-boot file manager, not a demo of the driver. It runs from the UEFI Shell or straight as `\EFI\Boot\BOOTX64.EFI` on a rescue stick, and if the firmware has not connected `ntfs.efi` yet it loads the driver itself (`LoadImage` + `StartImage` + `ConnectController` over all handles), then scopes a `DisconnectController` on exit so the NTFS volume is unmounted cleanly and the dirty flag cleared.

```mermaid
flowchart LR
    KEY["Key event loop<br/>SimpleTextInputEx with modifier state"]
    PANELS["Two panel contexts<br/>path, sort, filter, tags, history"]
    OPS["Operations<br/>copy, move, delete, mkdir, recursive"]
    INTEG["Integrity<br/>SHA-256, CRC32, tree compare and update"]
    FIND["Search<br/>recursive by name or mask"]
    VFS["VFS layer<br/>FAT32 and NTFS behind EFI_FILE_PROTOCOL"]
    LOADER["Driver loader<br/>ntfs.efi LoadImage and clean disconnect"]
    VIEW["Viewer<br/>text, hex and byte search"]
    EDIT["Editor<br/>text and hex, save in place"]
    QV["Quick View<br/>text, hex or directory summary"]
    FONT["8x16 bitmap font renderer<br/>BmpFont.h"]
    GOP["EFI_GRAPHICS_OUTPUT_PROTOCOL<br/>direct framebuffer blit"]

    KEY --> PANELS
    PANELS --> OPS
    PANELS --> VIEW
    PANELS --> EDIT
    PANELS --> QV
    PANELS --> FIND
    OPS --> INTEG
    OPS --> VFS
    INTEG --> VFS
    FIND --> VFS
    VFS --> LOADER
    PANELS --> FONT
    VIEW --> FONT
    EDIT --> FONT
    QV --> FONT
    FONT --> GOP
```

### What it does

| Area | Detail |
|---|---|
| Panels | Two independent contexts: path, sort order, filter mask, tags, path history. Sort by name, extension, size or modification date; repeating the shortcut toggles the direction. Each panel footer carries the volume label and its free and total size, read when the listing is read rather than on every frame |
| Volumes | FAT32 served by the firmware and NTFS served by `ntfs.efi` appear side by side as `fs0:` … `fsN:`. If the firmware has not connected the NTFS driver, EC loads it from `EC.ini`'s `NtfsDriverPath` or from its own directory, connects it to every handle, and disconnects it again on exit so the volume is unmounted cleanly |
| File operations | Copy, move, rename, delete and create directory, all recursive, with a progress dialog and `Esc` to abort. A failed or aborted copy removes the partial destination file instead of leaving a misleading zero-byte entry. Failures are collected and reported per item, not as one abort |
| Room to copy | Before a copy starts, the source is measured and the destination volume asked how much it has left. A shortfall asks rather than refuses: cluster slack is not counted and an overwrite gives its bytes back, so an estimate must not block a copy that would in fact have fitted |
| Integrity | SHA-256 and CRC32 of the file under the cursor, computed in 64 KB reads with no external crypto library. `VerifyAfterCopy` re-reads both sides after every copy and fails the operation with `EFI_CRC_ERROR` on a mismatch |
| Directory compare | `=` marks, on both panels at once, everything that differs by name, size or modification time; identical pairs stay unmarked, so what stays lit is exactly what would have to be copied. Directories compare by presence only |
| Recursive compare | The F9 menu walks both trees and compares same-sized files by SHA-256, reporting left-only, right-only, different, equal and common-directory counts. It can then update either side one way: missing and differing entries are copied over, entries that exist only at the destination are kept. Both walks report where they are and stop on `Esc`: hashing a tree takes minutes, and a box that never changes is indistinguishable from a hang |
| Search | `Alt+F7` walks the active panel's tree by name or mask, bounded at 24 levels and 512 hits, with `Esc` checked between directories. Taking a hit moves the panel to the directory holding it with the cursor already on the entry |
| Search by contents | The same dialog asks for text the file must hold. Matching is case-insensitive ASCII, the same engine the viewer's `F7` uses, and files are read in 64 KB chunks that carry the tail of the previous chunk forward, so a match lying across a boundary is still found. The mask is what keeps it affordable — `*.ini` with a needle reads kilobytes per file, `*` with a needle reads the volume — so `Esc` is polled per file, not per directory |
| Quick View | `Ctrl+Q` turns the passive panel into a preview of whatever the cursor is on: the first 32 KB as text, the same bytes as a hex dump when the content looks binary, or a count of files, subdirectories and immediate size for a directory |
| Viewer and editor | Read-only viewer with text scrolling and a hex dump, `F4` switching between them at the same offset, `F7` to find plain ASCII and `F3` to repeat. The editor writes back in both text and hex mode |
| Attributes | `Ctrl+F2` shows and edits the four DOS bits (read-only, hidden, system, archive) and the modification time; a field left alone is left alone on disk |
| UEFI tools | Volume details (label, total, free, block size, read-only), a device and filesystem rescan, loading a selected image as an EFI driver, and a read-only view of `BootOrder`, `BootNext` and each `Boot####` description |
| Running images | `Enter` launches an EFI application; the F9 menu can start one with an argument string passed as `LoadOptions`, the way the UEFI Shell would |
| Command line | `Ctrl+O` puts the panels away and gives the firmware's text mode a prompt, the way `Ctrl+O` works in Norton Commander and Far. It carries its own commands — `map`, `cd`, `dir`, `type`, `copy`, `move`, `del`, `md`, `sha256`, `load`, `run`, `cls`, `reset`, `shutdown` — over the same code the panels use, with history on the arrow keys and quoting for names with spaces. A command it does not know is handed to `EFI_SHELL_PROTOCOL` if the firmware has one, which it does when EC was started from the UEFI Shell. Booted straight as `BOOTX64.EFI` there is no shell at all, and the built-in commands are the whole of it — which is the case the prompt exists for |
| Settings | Every item in `F9 → Settings` is written to `EC.ini` the moment it changes |

### Keyboard reference

| Key | Action | Key | Action |
|---|---|---|---|
| `F1` | Help | `F2` | Drive menu for the active panel |
| `F3` | View file | `F4` | Edit file |
| `F5` | Copy, recursive | `F6` | Rename or move |
| `F7` | Create directory | `F8` / `Delete` | Delete, recursive |
| `F9` | Program menu | `F10` | Quit |
| `Alt+F1` / `Alt+F2` | Change left / right drive | `Alt+F7` | Find in the active tree |
| `Alt+F10` | Directory hotlist | `Alt+←` / `Alt+→` | Path history back / forward |
| `Ctrl+O` | Command line, `exit` or `Ctrl+O` returns | `Ctrl+Q` | Quick View in the passive panel |
| `Ctrl+F2` | Attributes and modification time | `Ctrl+F12` | Panel filter mask, `*` clears |
| `Ctrl+F3` / `Ctrl+F4` | Sort by name / extension | `Ctrl+F5` / `Ctrl+F6` | Sort by date / size |
| `Ctrl+A` / `Ctrl+U` | Tag all / clear tags | `Insert` / `Space` | Tag current item |
| `+` / `-` | Tag / untag by mask | `Esc` | Close a dialog, abort an operation |
| `*` | Invert tags | `=` | Tag what differs between the panels |
| `Tab` | Switch active panel | `Enter` | Enter directory or launch an EFI application |
| `Backspace` | Parent directory | letters | Quick prefix jump |
| `/` then `N` | Find anywhere in name, repeat | | |

Inside the viewer: `F4` switches text and hex, `F7` finds ASCII text, `F3` finds the next occurrence. Inside the editor: `F2` saves, `F4` switches text and hex.

### The F9 menu

| Entry | What it does |
|---|---|
| Refresh both panels | Re-reads both listings |
| Change active drive | Same list as `F2` |
| Find file by name or contents | Same as `Alt+F7`, for firmware that swallows `Alt` |
| Compare panel directories | Same as `=` |
| Recursive compare / update | Walks both trees with SHA-256, then optionally updates one side |
| Checksum selected file | SHA-256 and CRC32 of the file under the cursor |
| Toggle Quick View | Same as `Ctrl+Q` |
| Run selected EFI with arguments | `LoadImage`, `LoadOptions`, `StartImage` |
| UEFI tools | Volume details, rescan, load driver, boot entries |
| Selection tools | The `+`, `-`, `*`, `Ctrl+A`, `Ctrl+U` operations as menu items |
| Set active panel filter | Same as `Ctrl+F12`, temporary for this session |
| Directory hotlist | Same as `Alt+F10` |
| Settings | The `EC.ini` options below, saved on every change |
| Command line | Same as `Ctrl+O` |
| Help | Same as `F1` |

### `EC.ini`

Read at startup from the directory EC was launched from, then from the volume root, then from `\EFI\BOOT\EC.ini`. Lines starting with `#`, `;` or `[` are ignored; boolean values accept `1/0`, `yes/no`, `on/off`, `true/false`.

| Key | Effect |
|---|---|
| `ConfirmDelete` | Ask before deleting |
| `ConfirmOverwrite` | Ask before overwriting an existing target |
| `ShowSuccessMessages` | Report individual successful operations |
| `ShowOperationSummary` | Report a summary after group operations |
| `VerifyAfterCopy` | Re-read source and destination after every copy and compare SHA-256 |
| `DefaultLeft` / `DefaultRight` | Startup path per panel |
| `FilterLeft` / `FilterRight` | Startup filter mask per panel |
| `NtfsDriverPath` (alias `NtfsDriver`) | Where to find `ntfs.efi`; defaults to the application directory |
| `HotDir1` … `HotDir9` | Directory hotlist entries reached with `Alt+F10` |

Every setting reachable from `F9 → Settings` is written back the moment it changes. The file is created on first write if it does not exist, next to the running `EC.efi` — usually `\EFI\BOOT\EC.ini` — and if an existing file was found somewhere else at startup, that file is the one updated. A write that fails, for instance on a read-only volume, is reported rather than swallowed.

The one exception is the filter set with `Ctrl+F12` or from the filter entry in the main F9 menu: it applies to the running session only. To keep a filter across restarts, set it under `F9 → Settings → Left/Right filter`.

---

## ntfs_probe — Test Harness

`ntfs_probe.efi` is what makes the results below reproducible without a human at the console. Deployed as `\EFI\Boot\BOOTX64.EFI` it needs no UEFI Shell at all:

1. Look for an already-mounted NTFS volume; if none, `SelfLoadDriver` loads `ntfs.efi` from the ESP and connects it.
2. Run the inline unit tests: create, write, read back, seek-write past EOF, rename, cross-directory move, `SetInfo` timestamps and attributes, resize, delete.
3. Copy a directory tree recursively from the ESP source to the NTFS target, preserving names, sizes and timestamps.
4. Checkpoint progress into `\_PROG.txt` every 200 files, so an unexpected reboot still shows how far the run got.
5. Write `\_RESULT.txt` with file and byte counts, the error summary and the verdict string, and append every line of the run to `\_PROBE_TRACE.txt` on the ESP, which the host can read without mounting the NTFS volume at all.
6. `DisconnectController` on every NTFS handle to force `BindingStop` — prealloc trim, dirty-flag clear, `FlushBlocks`.
7. `ResetSystem(EfiResetShutdown)`, so the host can attach the VHD read-only before Windows can touch it.

---

## Testing and Validation

```mermaid
flowchart LR
    BUILD["build.ps1<br/>ntfs.efi, EC.efi, ntfs_probe.efi"]
    VOL["make-testvol.ps1<br/>fresh 128 MB NTFS VHD"]
    STAGE["stage-system32.ps1<br/>real System32 subset onto the ESP"]
    QEMU["test-qemu.ps1<br/>QEMU + OVMF, 15-30 s"]
    BIG["make-bigtest.ps1 + hyperv-bigtest.ps1<br/>Hyper-V Gen 2, 7 GB, Secure Boot off"]
    RO["Mount-VHD -ReadOnly<br/>Windows must not self-heal first"]
    VER["verify-copy.ps1 / verify-bigtest.ps1<br/>chkdsk /f + SHA256 per file"]
    OUT["RESULT: byte-exact and chkdsk-clean"]

    BUILD --> VOL --> STAGE --> QEMU --> RO
    BUILD --> BIG --> RO
    RO --> VER --> OUT
```

```powershell
# quick cycle - QEMU / OVMF, 15-30 s
.\build.ps1
.\tests\make-testvol.ps1            # fresh 128 MB NTFS VHD
.\tests\stage-system32.ps1          # copy a System32 subset to ESP\src
.\tests\test-qemu.ps1 -SkipBuild -TimeoutSec 120
.\tests\verify-copy.ps1             # chkdsk + SHA256

# big cycle - Hyper-V Gen 2, ~4 min, ~7 GB
.\tests\make-bigtest.ps1 -TargetGB 7 -MaxFiles 20000
.\tests\hyperv-bigtest.ps1 -SkipBuild
.\tests\verify-bigtest.ps1

# EC self-test - Hyper-V Gen 2, ~2 min, nothing interactive
.\tests\hyperv-ectest.ps1
```

The EC self-test is a separate binary: `build.ps1 -SelfTest` defines `EC_SELFTEST`, which is the only thing that compiles `SelfTest.c` into anything at all. Even that build runs the checks only when `\_ECTEST.on` is present on the boot volume, writes its report to `\_ECTEST_RESULT.txt` and powers the machine off. The release binary carries neither the code nor the flag check, and the harness stages the test image outside `bin\` and rebuilds the release binary immediately, so a self-test image cannot end up on a rescue stick by accident.

Success criterion for the quick cycle is the literal string `RESULT: ALL GOOD - copy is byte-exact and chkdsk-clean`.

| Test | Workload | Verdict |
|---|---|---|
| Real `System32` copy | ~3.9 MB of genuine Windows 11 binaries (`notepad.exe`, `cmd.exe`, `xcopy.exe`, core DLLs) | SHA256 byte-exact per file, `chkdsk /f` CLEAN, volume NOT dirty |
| B+tree density | 1500 files in one directory — forces `INDX` leaf splits and separator promotion | `chkdsk` CLEAN |
| Mixed mutation pass | Create, grow, rename, cross-directory move and delete in a single run | `chkdsk` CLEAN, volume NOT dirty |
| Same-volume EC copy and directory refill, Hyper-V | 550 files from Windows 11 `System32\drivers`, including WOF files and hard links, copied to another directory on the same NTFS volume; destination then emptied with `delete *` and filled again | 550/550 SHA256 byte-exact after refill, 0 missing, 0 mismatches, 0 extras, `chkdsk /f` CLEAN, volume NOT dirty |
| Large copy, Hyper-V | 7 GB of mixed data, FAT source to NTFS target, ~4 minutes | `bad=0`, `chkdsk` CLEAN, volume NOT dirty |
| `SetInfo` grow of a resident file, Hyper-V | `FileSize` raised from 10 bytes to 5000 on a still-resident file — forces resident-to-non-resident promotion plus zero-fill | New size and the original prefix both read back, `chkdsk` CLEAN |
| Collation beyond a-z, Hyper-V | Cyrillic capital Ya then small a created in a fresh (still resident-index) directory, then both reopened | `chkdsk` CLEAN — the same test against the previous build reports `Index $I30 ... is incorrectly sorted` |
| Same-filesystem 92 MB copy, Hyper-V | `samefs_src.bin` copied to `samefs_dst.bin` on the same NTFS volume — non-resident growth across many runs within one file | SHA256 byte-exact, `chkdsk` CLEAN |
| `CatRoot` copy on a live Windows 11 install, Hyper-V | `\Windows\System32\CatRoot` (1854 catalog files, names up to 136 chars, 28 MB) copied into one directory on the same 63 GB NTFS volume | **1854 of 1854 created, all 1854 visible to Windows, 0 refusals, 0 orphans**, `chkdsk` reports *found no problems*, every allocated `INDX` block reachable from the root. The index needed 249 blocks, with mapping pairs occupying 216 of the record's ~1024 bytes |
| Fill / empty / refill, Hyper-V | `\Windows\System32\drivers` (548 files) copied into one directory three times, with every file deleted between passes — the state a copy target used over and over ends up in | 548 of 548 every pass, zero refusals, index attribute settling at 4 runs in 104 bytes (25 blocks owned, 20 in use, 5 spare and reachable), `chkdsk` CLEAN |
| `$MFT` exhaustion on a live Windows 11 install, Hyper-V | The volume's MFT filled from Windows down to 40 free records, then files created through the driver until the table itself had to grow | 6344 files created, `$MFT` grown from 136 960 to 143 872 records and `$MFT:$BITMAP` from 17 120 to 17 984 bytes, `chkdsk` reports *found no problems*. Growth ends on MFT record 0 running out of mapping pairs, not on free space |
| `$MFT` exhaustion on a fresh volume, Hyper-V | Same setup on a 20 GB volume with contiguous free space: 30 directories of 1000 files each, created after the last free record was taken | 30 000 of 30 000 created and visible to Windows, `$MFT` grown to 30 976 records in 31 runs, `chkdsk` reports *found no problems* |
| Probe battery on a fresh volume, Hyper-V | Interleaved writes, hole punching, 48 files created and deleted in the volume root, a nested tree deleted recursively | Every phase passes and the volume is left `chkdsk`-clean, including the `$BITMAP:$I30` state of the root directory, which earlier builds left with allocated blocks holding no keys |
| Fill and drain one directory, Hyper-V | 2000 files with long names created in one directory through the driver, then every one of them deleted | 2000 created, 2000 deleted, `chkdsk` reports *found no problems*, and `$INDEX_ALLOCATION` is back to a single 4 KB block in one run — 88 bytes of attribute in the directory's own record |
| WOF LZX read, Hyper-V | Four real Windows binaries compacted with `compact /c /exe:LZX` — 72 KB to 7.6 MB, up to 243 chunks — read back through the driver | Every file byte-exact against the same file read by Windows, checked by full-file checksum: `xcopy.exe` 73 728, `notepad.exe` 360 448, `cmd.exe` 344 064, `shell32.dll` 7 947 416 bytes |
| EC self-test, Hyper-V | `EC.efi` built with `-SelfTest`, booted against a scripted NTFS fixture: recursive search by name and by contents, attribute and timestamp editing, panel compare, SHA-256 and CRC32 vectors, verified copy, Quick View, recursive tree compare and one-way update with progress and cancellation, tree sizing, volume details, viewer byte search, console line splitting and path resolution | 68 checks, `passed=68 failed=0`, the VM powering itself off and the result read back from the ESP. The content search is driven against a 70 000-byte file with the needle placed at offset 65 532, so a match across a read boundary fails the test rather than passing quietly |

> **Verification discipline:** always attach the result image with `Mount-VHD -ReadOnly`. Given write access, Windows silently repairs a volume on first access, and a `chkdsk` run afterwards then reports a clean volume that the driver did not actually leave clean.

---

## Status Codes

| Status | Cause | Driver behaviour |
|---|---|---|
| `EFI_SUCCESS` | Operation completed | Data and metadata written; flushed at unmount |
| `EFI_UNSUPPORTED` | Unknown WOF provider/algorithm or encrypted stream; write to a compressed attribute; separator-key replacement that would overflow its block | Refused with nothing modified |
| `EFI_VOLUME_CORRUPTED` | Bad or out-of-range USA fixup, wrong record signature, malformed run list or mapping-pair offset, an attribute or index header whose declared size falls outside its buffer | Mount or operation rejected, fail-closed |
| `EFI_OUT_OF_RESOURCES` | Pool allocation failed mid-operation | Allocated clusters and MFT records released first |
| `EFI_WRITE_PROTECTED` | Read-only media or write-protected volume | Blocked at the protocol layer |
| `EFI_ACCESS_DENIED` | Attempt to delete an NTFS metadata record (MFT index < 16) or a file with extra hard links | Refused |
| `EFI_NOT_FOUND` | Path component or index entry absent | Returned unchanged |

---

## Known Limitations

Each of these is a boundary the code refuses at, with the operation left unchanged:

1. **No DOS 8.3 alias.** Files are created with a single POSIX `$FILE_NAME`. Windows handles them normally; very old tools that expect a short name will not see one.
2. **Write path assumes a single base MFT record.** `$ATTRIBUTE_LIST` is followed on read — and an index whose `$INDEX_ROOT` and `$INDEX_ALLOCATION` were relocated into *different* records is written correctly — but the driver never creates extension records itself. So a file whose attributes would overflow its 1 KB record cannot be written, and a directory keeps accepting new entries only while `$INDEX_ALLOCATION`'s mapping pairs still fit in that record. Index blocks are allocated 8 to a run and unused owned blocks are reused, which keeps that cost low — 1854 long-named entries in one directory need 249 blocks described in 216 bytes — but the budget is finite. When it runs out the insert returns `EFI_UNSUPPORTED` with nothing modified, and every entry already there stays intact and visible.
3. **Hard links.** A file is deleted only when it has one genuine name. An 8.3 alias is not a second one: a long-named file carries both a WIN32 and a DOS `$FILE_NAME` and so a raw `LinkCount` of 2, and both names are unlinked together. What is refused is a record a *different directory* still names, so a record another name points at is never freed.
4. **No `$LogFile` journal.** Integrity rests on ordered writes, explicit rollback and the `$Volume` dirty flag. After an unclean shutdown Windows will offer to run `chkdsk` — the correct conservative outcome.
5. **Compression is read-only.** LZNT1 and both WOF codecs — XPRESS-Huffman and LZX — decompress on read, so compressed and CompactOS files are fully readable. Nothing compresses on write: a byte-range write into a `$DATA` with a non-zero `CompressionUnit` would mean re-encoding the whole compression unit, so it returns `EFI_UNSUPPORTED` with nothing modified rather than writing plaintext into a compressed run.
6. **Separator-key deletion can still be refused.** Rebalancing handles the normal cases; the rare replacement key that would overflow the host block returns `EFI_UNSUPPORTED` rather than restructuring further.
7. **2048 extents per attribute.** Enough for any realistic file given run merging, but a pathologically fragmented attribute is rejected instead of truncated.
8. **`$MFT` growth ends where record 0 runs out of mapping pairs.** The table and its bitmap both grow, but every chunk that lands away from the previous extent costs one mapping pair inside MFT record 0, and that record is 1 KB with no `$ATTRIBUTE_LIST` to spill into. Chunks of 256 clusters buy 1024 records per pair, so this is far away on a volume with contiguous free space: measured on a fragmented Windows volume with no free record left, growth continued for 6344 more files and stopped at 143 872 records with 156 runs, returning `EFI_VOLUME_FULL` with nothing modified. On a fresh volume the same test placed 30 000 files in 31 runs.
9. **Automated coverage is uneven.** Create, write, delete, rename, move, `SetInfo`, B+tree splits, the copy paths and both WOF codecs are covered by the harness. The LZNT1, symlink and 8.3 short-name read paths are implemented but not yet fixtured for every edge case.
10. **Little-endian x64 only.** On-disk structures are raw struct overlays; a big-endian target would need byte-swapping accessors.

---

## Deployment Layout

Both binaries find each other by convention, so where the files sit matters. The rule is simple: **`ntfs.efi` goes in the same directory as `EC.efi`**, on a FAT32 partition the firmware can read (an ESP or a plain FAT32 stick).

`EC.efi` derives its own application directory from the `MEDIA_FILEPATH` node of its `EFI_LOADED_IMAGE_PROTOCOL` path, then resolves everything relative to it:

| Resource | Resolution order |
|---|---|
| `EC.ini` | `<AppDir>\EC.ini`, then `\EC.ini`, then `\EFI\BOOT\EC.ini` |
| `ntfs.efi` | `<AppDir>\ntfs.efi` by default; `NtfsDriverPath` overrides it — a relative value is joined to `<AppDir>`, a value starting with `\` is taken from the volume root |

`ntfs_probe.efi` is stricter on purpose: it opens `\ntfs.efi` on the **root of the volume it was itself loaded from**, hardcoded, so an unattended test run has nothing to configure.

### Interactive rescue stick

```
FAT32 stick (or ESP)
├── EFI/
│   └── Boot/
│       ├── BOOTX64.EFI   # copy of EC.efi — firmware boots straight into it
│       ├── ntfs.efi      # same directory as the EC copy, so it is found automatically
│       └── EC.ini        # optional; also read from \EC.ini or \EFI\BOOT\EC.ini
└── Tools/                # anything you want to copy onto NTFS afterwards
```

Booting `\EFI\Boot\BOOTX64.EFI` here gives a file manager with NTFS write access and no operating system involved. `EC.efi` and `ntfs.efi` may equally live in any other directory together — `\EC\EC.efi` + `\EC\ntfs.efi` works the same way.

### UEFI Shell layout

```
FAT32 stick
├── ntfs.efi              # load fs0:\ntfs.efi
├── EC.efi                # AppDir is the root, so it finds ntfs.efi right here
└── EC.ini
```

### Unattended test layout — what `hyperv-bigtest.ps1` builds

```
ESP VHDX
├── ntfs.efi              # volume root — the probe's hardcoded self-load path
└── EFI/
    └── Boot/
        └── BOOTX64.EFI   # copy of ntfs_probe.efi, so no UEFI Shell is needed
```

The probe then writes `\_PROG.txt` and `\_RESULT.txt` back to that same ESP.

### `EC.ini` example

```ini
# EC.ini - next to EC.efi
ConfirmDelete        = yes
ConfirmOverwrite     = yes
ShowSuccessMessages  = no
ShowOperationSummary = yes

# panel startup state
DefaultLeft  = fs0:\
DefaultRight = fs1:\
FilterRight  = *.efi

# relative to EC.efi's own directory; use a leading \ for volume-root paths
NtfsDriverPath = ntfs.efi

# Alt+F10 hotlist
HotDir1 = \EFI\Boot
HotDir2 = \Windows\System32\drivers
HotDir3 = \Windows\System32\config
```

---

## Usage

Copy `ntfs.efi`, `EC.efi` and optionally `EC.ini` into one directory on the FAT32 ESP of a USB stick, boot the UEFI Shell and load the driver:

```cmd
Shell> load fs0:\ntfs.efi
[ntfs] Driver binding installed
[ntfs] MountVolume: UpCase table loaded, 131072 bytes
[ntfs] MountVolume: TotalClusters=1835008 FreeClusters=1204736
[ntfs] SimpleFileSystem installed on handle

Shell> map -r
Mapping table
  FS0: Alias(s):HD0a65535a1:;BLK1:
  FS1: Alias(s):HD0a65535a2:;BLK2:          <- NTFS volume, mounted by ntfs.efi

Shell> fs1:
fs1:\> ls
Directory of: fs1:\
  08/04/2026  01:45 <DIR>     4,096  Windows
  08/04/2026  01:45 <DIR>     4,096  Users
  08/04/2026  01:45           118,272  UnderVolter.efi

fs1:\> cp fs0:\patched.sys fs1:\Windows\System32\drivers\
fs1:\> fs0:\EC.efi
```

Launching `EC.efi` directly, with no driver loaded, also works — it loads `ntfs.efi` from its own directory (or from `NtfsDriverPath`) and connects it. For an unattended rescue stick, deploy either `EC.efi` or `ntfs_probe.efi` as `\EFI\Boot\BOOTX64.EFI`.

> **Before pulling the stick:** quit `EC.efi` with `F10`, or issue `DisconnectController` on the NTFS handle. That is what triggers the preallocation trim, the dirty-flag clear and `FlushBlocks`. Cutting power at the panel view leaves the volume marked dirty and Windows will want to check it.

---

## Building from Source

No EDK2 tree, no Python, no BaseTools. Plain MSVC projects linking the prebuilt EDK2 static libraries in `lib/`.

**Requirements:** Visual Studio 2022 or 2026 with the **Desktop development with C++** workload (MSVC `v145` toolset, 14.51+).

```powershell
git clone https://github.com/wesmar/NTFS_EFI.git
cd NTFS_EFI
.\build.ps1
```

`build.ps1` locates MSBuild through `vswhere.exe`, builds `src\ntfs.vcxproj`, `ec\EC.vcxproj` and `probe\ntfs_probe.vcxproj` in `Release|x64`, moves the binaries into `bin\`, and cleans the intermediates.

All three build at `/W4 /WX` — warning-free, with warnings promoted to errors — and `/external:W3` keeps the vendored EDK2 headers from breaking that. `/GS-` and disabled exception handling stay as they are: freestanding UEFI has no runtime for stack cookies or C++ exceptions.

`.\build.ps1 -SelfTest` produces the same application with `EC_SELFTEST=1`, which is what compiles `SelfTest.c` in; the default build defines it as `0` and the file collapses to nothing.

| Output | Description |
|---|---|
| `bin\ntfs.efi` | The NTFS driver |
| `bin\EC.efi` | EFI Commander |
| `bin\ntfs_probe.efi` | Test harness and tree-copy benchmark |

### Diagnostic refusal codes

A dozen separate limits in the write path all refuse the same way, with `EFI_UNSUPPORTED` and nothing modified. That is the right status for an application, and it does not say which limit was reached. A UEFI driver has no log to consult afterwards, so each of those sites names its own distinct status through one macro:

```c
return NTFS_REFUSE (EFI_NO_MEDIA);   /* no record room for another mapping pair */
```

```powershell
.\build.ps1          # production: NTFS_REFUSE(x) expands to EFI_UNSUPPORTED
.\build.ps1 -Diag    # diagnostic: each site returns its own status, printed by %r
```

In a production build the preprocessor discards the argument, so the generated code matches a plain `return EFI_UNSUPPORTED;`. Verified by hash: the release binary is byte-for-byte the same with the mechanism in place as without it. One `-Diag` run names the boundary that refused. The codes come from statuses the driver never returns for real (`EFI_NO_MEDIA`, `EFI_TIMEOUT` and similar), so a diagnostic run cannot be confused with a genuine failure. No caller may test for them, since outside a `-Diag` build they do not exist.

---

## License

MIT — see [LICENSE.md](LICENSE.md).

- **Author:** Marek Wesołowski (WESMAR)
- **Contact:** marek@wesolowski.eu.org
- **Project page:** [kvc.pl/repositories/ntfs_efi](https://kvc.pl/repositories/ntfs_efi)
- **Build host:** Windows 10/11 x64, MSVC, pure C11, no CRT
- **Runtime:** UEFI 2.x+ x64 firmware
