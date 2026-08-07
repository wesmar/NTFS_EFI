/**
 * ntfs.h - shared header for the NTFS EFI driver.
 *
 * Self-contained: does not include ntfs.h from ReactOS/WDM or any kernel
 * headers. All NTFS on-disk structures are redefined here with UEFI
 * primitive types.
 *
 * Read + write: reads resident/non-resident/LZNT1-compressed data; writes
 * cover create/delete/setinfo, resident<->non-resident + multi-run growth,
 * directory index overflow and B+tree leaf split, $MFT growth, and the
 * $Volume dirty flag with $MFTMirr kept in lock-step. See the per-file
 * comments for the exact, deliberately-scoped limits of each path.
 *
 * File layout (one responsibility per .c file):
 *   ntfs_globals.c  - GUIDs, PCD/debug-lib stubs required by the plain-MSVC
 *                     (non-autogen) build, NtfsEfiDebugPrint (the Print()
 *                     implementation - UefiLib's own Print() pulls in HII/
 *                     PCD tokens this build doesn't link against)
 *   ntfs_entry.c    - UefiMain and the AutoGen-replacement boilerplate
 *                     (ProcessLibraryConstructorList, Debug* stubs, etc.)
 *   ntfs_diskio.c   - thin wrapper around EFI_DISK_IO_PROTOCOL
 *   ntfs_runlist.c  - data-run decoding (replaces LARGE_MCB + FsRtl*)
 *   ntfs_attr.c     - attribute context lifecycle, ReadAttribute,
 *                     FindAttribute (in-record scan + $ATTRIBUTE_LIST follow)
 *   ntfs_mft.c      - USA fixup, Read/WriteFileRecord, $MFTMirr sync
 *   ntfs_btree.c    - directory lookup (B+tree index traversal)
 *   ntfs_bitmap.c   - $Bitmap cluster + $MFT record allocation, $MFT growth
 *   ntfs_lznt1.c    - LZNT1 decompression
 *   ntfs_symlink.c  - reparse-point / symlink target resolution
 *   ntfs_create.c   - file/dir creation, index insert + leaf split
 *   ntfs_delete.c   - file/empty-dir deletion (index unlink, cluster free)
 *   ntfs_setinfo.c  - SetInfo: timestamps + DOS attributes
 *   ntfs_time.c     - NTFS <-> EFI_TIME conversion
 *   ntfs_file.c     - handle factory, path lookup, EFI_FILE_PROTOCOL methods
 *   ntfs_volume.c   - OpenVolume, mount/unmount, VCB<->SFSP casts, dirty flag
 *   ntfs_binding.c  - EFI_DRIVER_BINDING_PROTOCOL (Supported/Start/Stop)
 */

#ifndef _NTFS_EFI_H_
#define _NTFS_EFI_H_

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DriverBinding.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/FileSystemVolumeLabelInfo.h>

/*
 * UefiLib's Print() pulls in HII/Graphics Output globals and PCD tokens
 * that this minimal (non-autogen) build does not link against. Use
 * BasePrintLib + gST->ConOut directly instead (implemented in
 * ntfs_globals.c). Debug logging is intentionally left in place across the
 * driver - it is what found every real bug during bring-up.
 */
#include "DebugLog.h"

VOID NtfsEfiDebugPrint (IN CONST CHAR16 *Fmt, ...);
/*
 * In production (ENABLE_DEBUG_LOG == 0) Print compiles to nothing: the string
 * never gets formatted. NtfsEfiDebugPrint sits in hot paths (ReadAttr per run,
 * ReadFileRecord, the B+tree search) where an always-run UnicodeVSPrint is pure
 * wasted work. With logging on it routes to DebugLog as before.
 */
#if ENABLE_DEBUG_LOG
#define Print NtfsEfiDebugPrint
#else
#define Print(...) ((VOID)0)
#endif

/* perf counters (ntfs_diskio.c) - deterministic DiskIo round-trip metric */
extern UINT64 gNtfsReadCalls;
extern UINT64 gNtfsWriteCalls;
extern UINT64 gNtfsReadBytes;
extern UINT64 gNtfsWriteBytes;
extern UINT64 gNtfsRecordWrites;
extern UINT64 gNtfsRecordReads;
extern UINT64 gNtfsIndexWrites;

/* =========================================================================
 * Section 1 - primitive type aliases (WDM <-> UEFI)
 * ========================================================================= */

typedef UINT8           UCHAR;
typedef UINT8          *PUCHAR;
typedef UINT16          USHORT;
typedef UINT16         *PUSHORT;
typedef UINT32          ULONG;
typedef UINT32         *PULONG;
typedef UINT64          ULONGLONG;
typedef UINT64         *PULONGLONG;
typedef INT8            CCHAR;
typedef INT16           SHORT;
typedef INT32           LONG;
typedef INT32          *PLONG;
typedef INT64           LONGLONG;
typedef INT64          *PLONGLONG;
typedef CHAR16          WCHAR;
typedef CHAR16         *PWCHAR;
typedef CHAR16         *PWSTR;
typedef CONST CHAR16   *PCWSTR;
typedef CHAR8          *PCHAR;
typedef VOID           *PVOID;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define NT_SUCCESS(s)       ((LONG)(s) >= 0)
#define STATUS_SUCCESS                      ((LONG)0x00000000L)
#define STATUS_END_OF_FILE                  ((LONG)0xC0000011L)
#define STATUS_NO_MORE_FILES                ((LONG)0x80000006L)
#define STATUS_NO_SUCH_FILE                 ((LONG)0xC000000FL)
#define STATUS_INSUFFICIENT_RESOURCES       ((LONG)0xC000009AL)
#define STATUS_OBJECT_PATH_NOT_FOUND        ((LONG)0xC000003AL)
#define STATUS_OBJECT_NAME_NOT_FOUND        ((LONG)0xC0000034L)
#define STATUS_FILE_CORRUPT_ERROR           ((LONG)0xC0000102L)
#define STATUS_DATA_ERROR                   ((LONG)0xC000003EL)
#define STATUS_PARTIAL_COPY                 ((LONG)0x8000000DL)
#define STATUS_NOT_IMPLEMENTED              ((LONG)0xC0000002L)
#define STATUS_BUFFER_OVERFLOW              ((LONG)0x80000005L)

#define ROUND_UP(N, S)   ((((N) + (S) - 1) / (S)) * (S))
#define ROUND_DOWN(N, S) ((N) - ((N) % (S)))
#define ALIGN_UP_BY(N, S) ROUND_UP(N, S)
#define FIELD_OFFSET(T, M)  ((UINTN)(&((T*)0)->M))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))

/* =========================================================================
 * Section 2 - NTFS on-disk structures
 *
 * These are cast directly onto buffers read verbatim off disk (e.g.
 * `(PNTFS_BOOT_SECTOR)SectorBuf`), relying on the target being x64 UEFI:
 * little-endian, and tolerant of unaligned multi-byte field access. NTFS's
 * own on-disk byte order is little-endian, so this is correct and free on
 * every platform this driver targets - but it is not portable as-is to a
 * big-endian host; that would need explicit byte-swapping accessors rather
 * than raw struct overlays.
 * ========================================================================= */

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
    ULONGLONG MftLocation;
    ULONGLONG MftMirrLocation;
    CCHAR     ClustersPerMftRecord;
    UCHAR     Unused4[3];
    CCHAR     ClustersPerIndexRecord;
    UCHAR     Unused5[3];
    ULONGLONG SerialNumber;
    UCHAR     Checksum[4];
} NTFS_EBPB;

typedef struct {
    UCHAR    Jump[3];
    UCHAR    OEMID[8];
    NTFS_BPB  BPB;
    NTFS_EBPB EBPB;
    UCHAR    BootStrap[426];
    USHORT   EndSector;
} NTFS_BOOT_SECTOR;
#pragma pack(pop)

typedef struct {
    ULONG     Type;
    USHORT    UsaOffset;
    USHORT    UsaCount;
    ULONGLONG Lsn;
} NTFS_RECORD_HEADER;

#define NRH_FILE_TYPE  0x454C4946UL
#define NRH_INDX_TYPE  0x58444E49UL

typedef struct {
    NTFS_RECORD_HEADER Ntfs;
    USHORT SequenceNumber;
    USHORT LinkCount;
    USHORT AttributeOffset;
    USHORT Flags;
    ULONG  BytesInUse;
    ULONG  BytesAllocated;
    ULONGLONG BaseFileRecord;
    USHORT NextAttributeNumber;
    USHORT Padding;
    ULONG  MFTRecordNumber;
} FILE_RECORD_HEADER, *PFILE_RECORD_HEADER;

#define FRH_IN_USE    0x0001
#define FRH_DIRECTORY 0x0002

typedef struct {
    ULONG  Type;
    ULONG  Length;
    UCHAR  IsNonResident;
    UCHAR  NameLength;
    USHORT NameOffset;
    USHORT Flags;
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

typedef enum {
    AttributeStandardInformation = 0x10,
    AttributeAttributeList       = 0x20,
    AttributeFileName            = 0x30,
    AttributeData                = 0x80,
    AttributeIndexRoot           = 0x90,
    AttributeIndexAllocation     = 0xA0,
    AttributeBitmap              = 0xB0,
    AttributeVolumeName          = 0x60,
    AttributeVolumeInformation   = 0x70,
    AttributeReparsePoint        = 0xC0,
    AttributeEnd                 = 0xFFFFFFFF
} ATTRIBUTE_TYPE;

typedef struct {
    ULONGLONG Reserved;
    UCHAR     MajorVersion;
    UCHAR     MinorVersion;
    USHORT    VolumeFlags;
} NTFS_VOLUME_INFORMATION, *PNTFS_VOLUME_INFORMATION;

#define VOLUME_DIRTY 0x0001

/*
 * $REPARSE_POINT data, symlink variant only. Format is MS-FSCC's
 * REPARSE_DATA_BUFFER "SymbolicLinkReparseBuffer" - public, documented,
 * unchanged since Vista (this NT source snapshot predates it: it only has
 * IO_REPARSE_TAG_MOUNT_POINT, no IO_REPARSE_TAG_SYMLINK/Flags field yet -
 * ported from public MS-FSCC documentation instead of ms/, per instructions).
 */
#define IO_REPARSE_TAG_SYMLINK      0xA000000CUL
#define IO_REPARSE_TAG_MOUNT_POINT  0xA0000003UL
#define SYMLINK_FLAG_RELATIVE       0x00000001UL

#pragma pack(push, 1)
typedef struct {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;   /* byte offset into PathBuffer */
    USHORT SubstituteNameLength;   /* bytes */
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    ULONG  Flags;
    WCHAR  PathBuffer[1];
} NTFS_SYMLINK_REPARSE_BUFFER, *PNTFS_SYMLINK_REPARSE_BUFFER;
#pragma pack(pop)

typedef struct {
    ULONG     Type;
    USHORT    Length;
    UCHAR     NameLength;
    UCHAR     NameOffset;
    ULONGLONG StartingVCN;
    ULONGLONG MFTIndex;
    USHORT    Instance;
} NTFS_ATTR_LIST_ITEM, *PNTFS_ATTR_LIST_ITEM;

/*
 * Extended (NT4+/NTFS 3.x) form - 72 bytes. Real Windows always writes
 * this form, never the bare 32+4=36-byte pre-NT4 legacy form: SecurityId
 * is how a file references its ACL in the volume-wide $Secure store, and
 * a file with SecurityId==0 (no real $Secure entry) reads as having no
 * valid security descriptor at all, which NTFS.sys treats as a corrupt/
 * unusable file - confirmed the hard way (see ntfs_create.c).
 */
typedef struct {
    ULONGLONG CreationTime;
    ULONGLONG ChangeTime;
    ULONGLONG LastWriteTime;
    ULONGLONG LastAccessTime;
    ULONG     FileAttribute;
    ULONG     MaximumVersions;
    ULONG     VersionNumber;
    ULONG     ClassId;
    ULONG     OwnerId;
    ULONG     SecurityId;
    ULONGLONG QuotaCharged;
    ULONGLONG Usn;
} STANDARD_INFORMATION, *PSTANDARD_INFORMATION;

typedef struct {
    ULONGLONG DirectoryFileReferenceNumber;
    ULONGLONG CreationTime;
    ULONGLONG ChangeTime;
    ULONGLONG LastWriteTime;
    ULONGLONG LastAccessTime;
    ULONGLONG AllocatedSize;
    ULONGLONG DataSize;
    ULONG     FileAttributes;
    union {
        struct { USHORT PackedEaSize; USHORT AlignmentOrReserved; } EaInfo;
        ULONG ReparseTag;
    } Extended;
    UCHAR NameLength;
    UCHAR NameType;
    WCHAR Name[1];
} FILENAME_ATTRIBUTE, *PFILENAME_ATTRIBUTE;

typedef struct {
    ULONG FirstEntryOffset;
    ULONG TotalSizeOfEntries;
    ULONG AllocatedSize;
    UCHAR Flags;
    UCHAR Padding[3];
} INDEX_HEADER_ATTRIBUTE;

typedef struct {
    ULONG                AttributeType;
    ULONG                CollationRule;
    ULONG                SizeOfEntry;
    UCHAR                ClustersPerIndexRecord;
    UCHAR                Padding[3];
    INDEX_HEADER_ATTRIBUTE Header;
} INDEX_ROOT_ATTRIBUTE, *PINDEX_ROOT_ATTRIBUTE;

typedef struct {
    NTFS_RECORD_HEADER   Ntfs;
    ULONGLONG            VCN;
    INDEX_HEADER_ATTRIBUTE Header;
} INDEX_BUFFER, *PINDEX_BUFFER;

typedef struct {
    union {
        struct { ULONGLONG IndexedFile; }                    Directory;
        struct { USHORT DataOffset; USHORT DataLength; ULONG Reserved; } ViewIndex;
    } Data;
    USHORT Length;
    USHORT KeyLength;
    USHORT Flags;
    USHORT Reserved;
    FILENAME_ATTRIBUTE FileName;
} INDEX_ENTRY_ATTRIBUTE, *PINDEX_ENTRY_ATTRIBUTE;

#define NTFS_INDEX_ENTRY_NODE  0x0001
#define NTFS_INDEX_ENTRY_END   0x0002
#define INDEX_ROOT_LARGE       0x01
#define INDEX_NODE_LARGE       0x01

#define NTFS_FILE_NAME_POSIX         0
#define NTFS_FILE_NAME_WIN32         1
#define NTFS_FILE_NAME_DOS           2
#define NTFS_FILE_NAME_WIN32_AND_DOS 3

#define NTFS_FILE_MFT             0ULL
#define NTFS_FILE_MFTMIRR         1ULL
#define NTFS_FILE_ROOT            5ULL
#define NTFS_FILE_BITMAP          6ULL
#define NTFS_FILE_UPCASE          10ULL     /* $UpCase - Unicode case-fold table */
#define NTFS_FILE_FIRST_USER_FILE 16ULL
#define NTFS_MFT_MASK             0x0000FFFFFFFFFFFFULL

#define NTFS_FILE_TYPE_READ_ONLY  0x0001
#define NTFS_FILE_TYPE_HIDDEN     0x0002
#define NTFS_FILE_TYPE_SYSTEM     0x0004
#define NTFS_FILE_TYPE_ARCHIVE    0x0020
#define NTFS_FILE_TYPE_DIRECTORY  0x10000000
#define NTFS_FILE_TYPE_COMPRESSED 0x0800

#define FILE_RECORD_END  0x11477982UL
#define ATTR_RECORD_ALIGNMENT 8

/* Smallest legal attribute record: Type..Instance (16 bytes) plus the shorter
 * of the two sub-headers (resident, 8 bytes). Anything below this cannot even
 * hold the fields the attribute walk reads, so it is corruption. */
#define NTFS_ATTR_MIN_HEADER 24

/* FILENAME_ATTRIBUTE up to (not including) Name[0]: everything before the
 * variable-length name. A $FILE_NAME value shorter than this cannot be read
 * at all; the name needs NameLength * sizeof(WCHAR) more bytes on top. */
#define NTFS_FILENAME_FIXED_BYTES 66

/* =========================================================================
 * Section 3 - driver-internal structures
 * ========================================================================= */

/* Run-list entry: one extent (replaces LARGE_MCB entirely) */
typedef struct {
    UINT64  VBN;   /* virtual block number (cluster units), i.e. offset in file */
    INT64   LBN;   /* logical block number on disk; -1 = sparse/unallocated     */
    UINT64  Len;   /* length in clusters                                         */
} NTFS_RUN_ENTRY;

#define NTFS_MAX_RUNS 2048  /* max extents per attribute; 2048x8 bytes of disk = 16 GB frags */

/* Attribute context - replaces WDM NTFS_ATTR_CONTEXT (no LARGE_MCB) */
typedef struct _NTFS_ATTR_CTX {
    PNTFS_ATTR_RECORD  pRecord;         /* heap copy of the attribute record  */
    ULONGLONG          FileMFTIndex;    /* owning file's MFT index            */
    NTFS_RUN_ENTRY    *Runs;            /* decoded run list (non-resident)    */
    ULONG              RunCount;
    /* sequential-read cache: index into Runs[] and corresponding byte offset */
    ULONG              CacheIdx;
    UINT64             CacheOffset;     /* byte offset at start of Runs[CacheIdx] */
} NTFS_ATTR_CTX, *PNTFS_ATTR_CTX;

/* Volume control block */
typedef struct _NTFS_EFI_VCB {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Sfsp;
    EFI_DISK_IO_PROTOCOL  *DiskIo;
    EFI_BLOCK_IO_PROTOCOL *BlockIo;
    UINT32                 MediaId;
    /* NTFS geometry */
    ULONG  BytesPerSector;
    ULONG  BytesPerCluster;
    ULONG  BytesPerFileRecord;
    ULONG  BytesPerIndexRecord;
    ULONG  SectorsPerCluster;
    UINT64 SerialNumber;
    USHORT VolumeLabelLen;              /* in chars (not bytes)               */
    WCHAR  VolumeLabel[128];
    /* MFT state */
    PFILE_RECORD_HEADER  MasterFileTable;
    PNTFS_ATTR_CTX       MFTContext;
    ULONG                MftDataOffset;
    /* volume free-space, computed once at mount time from $Bitmap (MFT=6) */
    UINT64               TotalClusters;
    UINT64               FreeClusters;
    BOOLEAN              VolumeDirtySet;
    /* $Bitmap allocator rolling cursor (bit index). NtfsEfiAllocateClusters
     * starts its first-fit scan here instead of from bit 0 every time, and
     * advances it past each allocation. Zero-initialised by AllocateZeroPool
     * at mount, so the first allocation on a volume still starts at bit 0. */
    UINT64               BitmapAllocHint;
    /* In-RAM mirror of the volume $Bitmap (MFT #6) $DATA, loaded once at
     * mount. The cluster allocator scanned+re-read the ENTIRE on-disk
     * $Bitmap on every single allocation (O(volume) I/O per allocated run -
     * on a large sequential copy that roughly doubled total disk traffic).
     * Now alloc/free scan and mutate this buffer and write ONLY the changed
     * bytes through to disk, so the on-disk $Bitmap and this mirror stay in
     * lock-step. Single mount-lifetime owner; freed at unmount. NULL if the
     * one-time load failed (allocator then falls back to reading on demand). */
    PUCHAR               VolBitmap;
    UINT64               VolBitmapLen;   /* bytes in VolBitmap */
    /* Unicode case-folding table read from $UpCase (MFT #10) at mount time.
     * 65536 entries, one per UTF-16 BMP code unit; UpcaseTable[c] is the
     * uppercase form of c - exactly what chkdsk/Windows use for collation.
     * Always non-NULL after a successful mount (falls back to an ASCII-only
     * identity table if $UpCase can't be read - see NtfsEfiMountVolume). */
    #define NTFS_UPCASE_ENTRIES 65536
    USHORT              *UpcaseTable;
    /* MFT record cache: the same records ($Bitmap #6, $MFT #0, a directory,
     * $MFTMirr) get read over and over during a copy. Cache the last few,
     * fixed-up, keyed by MFT index; every writer goes through
     * NtfsEfiWriteFileRecord which refreshes the entry, so it stays coherent. */
    #define NTFS_MFT_CACHE_ENTRIES 16
    #define NTFS_MFT_CACHE_RECSIZE 4096
    UINT64  MftCacheIndex[NTFS_MFT_CACHE_ENTRIES];
    BOOLEAN MftCacheValid[NTFS_MFT_CACHE_ENTRIES];
    UCHAR   MftCacheData [NTFS_MFT_CACHE_ENTRIES][NTFS_MFT_CACHE_RECSIZE];
    ULONG   MftCacheNext;
} NTFS_EFI_VCB, *PNTFS_EFI_VCB;

/* Open-file handle (EFI_FILE_PROTOCOL MUST be the first member) */
typedef struct _NTFS_EFI_FILE {
    EFI_FILE_PROTOCOL  Protocol;        /* vtable - populated in CreateHandle  */
    PNTFS_EFI_VCB      Vcb;
    ULONGLONG          MFTIndex;
    BOOLEAN            IsDirectory;
    UINT64             FileSize;        /* logical size of $DATA              */
    UINT64             AllocSize;       /* allocated size                     */
    UINT64             CreationTime;    /* NTFS 100 ns ticks                  */
    UINT64             ChangeTime;
    UINT64             LastWriteTime;
    UINT64             LastAccessTime;
    ULONG              NtfsAttribs;     /* raw NTFS file-attribute flags      */
    WCHAR              FileName[256];   /* basename (not full path)           */
    UINTN              FileNameChars;
    BOOLEAN            OpenForWrite;    /* EFI_FILE_MODE_WRITE was requested  */
    BOOLEAN            DidGrow;         /* Write() allocated clusters this open;
                                        * Close() trims prealloc slack back    */
    /* read/enum state */
    UINT64             Position;        /* byte offset for files              */
    ULONG              DirEnumEntry;    /* sequential counter for directories */
    /* directory enumeration cache: built once (full in-order B+tree walk) on
     * the first Read(), then served O(1) per entry - kills the old O(n^2). */
    ULONGLONG         *DirCache;
    ULONG              DirCacheCount;
    BOOLEAN            DirCacheBuilt;
} NTFS_EFI_FILE, *PNTFS_EFI_FILE;

/* =========================================================================
 * Global GUIDs (defined in ntfs_globals.c)
 * ========================================================================= */

extern EFI_GUID gEfiDriverBindingProtocolGuid;
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiBlockIoProtocolGuid;
extern EFI_GUID gEfiDiskIoProtocolGuid;
extern EFI_GUID gEfiFileInfoGuid;
extern EFI_GUID gEfiFileSystemInfoGuid;
extern EFI_GUID gEfiFileSystemVolumeLabelInfoIdGuid;

/* =========================================================================
 * Cross-file function prototypes, grouped by the .c file that implements
 * them. Anything not listed here is file-local (static).
 * ========================================================================= */

/* ntfs_diskio.c */
EFI_STATUS
NtfsEfiReadDisk (
    IN  PNTFS_EFI_VCB Vcb,
    IN  UINT64        ByteOffset,
    IN  UINTN         Length,
    OUT VOID         *Buffer
    );

EFI_STATUS
NtfsEfiWriteDisk (
    IN PNTFS_EFI_VCB Vcb,
    IN UINT64        ByteOffset,
    IN UINTN         Length,
    IN VOID         *Buffer
    );

/* ntfs_runlist.c */
EFI_STATUS
NtfsBuildRunList (
    IN  PNTFS_ATTR_RECORD  AttrRecord,
    OUT NTFS_RUN_ENTRY    *Runs,
    IN  ULONG              MaxRuns,
    OUT ULONG             *RunCount
    );

UINTN
NtfsMappingPairsSize (
    IN PNTFS_ATTR_RECORD AttrRecord
    );

UINTN
NtfsEncodeRunEntry (
    OUT PUCHAR  Dest,
    IN  UINT64  Length,
    IN  INT64   LcnDelta
    );

/* ntfs_attr.c */
VOID
NtfsEfiFreeAttrCtx (
    IN PNTFS_ATTR_CTX Ctx
    );

UINT64
NtfsEfiAttrDataLength (
    IN PNTFS_ATTR_CTX Ctx
    );

ULONG
NtfsEfiReadAttr (
    IN  PNTFS_EFI_VCB  Vcb,
    IN  PNTFS_ATTR_CTX Ctx,
    IN  UINT64         Offset,
    OUT PCHAR          Buffer,
    IN  ULONG          Length
    );

/* in-place overwrite only: no growth, no sparse-run materialization */
ULONG
NtfsEfiWriteAttr (
    IN PNTFS_EFI_VCB  Vcb,
    IN PNTFS_ATTR_CTX Ctx,
    IN UINT64         Offset,
    IN PCHAR          Buffer,
    IN ULONG          Length
    );

PNTFS_ATTR_CTX
NtfsEfiFindAttribute (
    IN  PNTFS_EFI_VCB        Vcb,
    IN  PFILE_RECORD_HEADER  FileRecord,
    IN  ATTRIBUTE_TYPE       Type,
    IN  PCWSTR               Name,           /* NULL or L"" = unnamed          */
    IN  USHORT               NameLength,     /* char count                     */
    OUT ULONG               *AttrOffset      /* may be NULL                    */
    );

PNTFS_ATTR_CTX
NtfsEfiFindAttrInRecord (
    IN  PNTFS_EFI_VCB        Vcb,
    IN  PFILE_RECORD_HEADER  FileRecord,
    IN  ATTRIBUTE_TYPE       Type,
    IN  PCWSTR               Name,
    IN  USHORT               NameLength,
    OUT ULONG               *AttrOffset
    );

/* ntfs_lznt1.c */
ULONG
NtfsEfiReadCompressedAttr (
    IN  PNTFS_EFI_VCB  Vcb,
    IN  PNTFS_ATTR_CTX Ctx,
    IN  UINT64         Offset,
    OUT PCHAR          Buffer,
    IN  ULONG          Length
    );

/* ntfs_wof.c - WOF file-provider XPRESS4K/8K/16K read support */
EFI_STATUS
NtfsEfiReadWofAttr (
    IN  PNTFS_EFI_VCB       Vcb,
    IN  PFILE_RECORD_HEADER Record,
    IN  UINT64              FileSize,
    IN  UINT64              Offset,
    OUT PCHAR               Buffer,
    IN  ULONG               Length,
    OUT ULONG              *BytesRead
    );

/* ntfs_create.c */
EFI_STATUS
NtfsEfiCreateFile (
    IN  PNTFS_EFI_VCB Vcb,
    IN  ULONGLONG      ParentMFT,
    IN  CONST WCHAR   *Name,
    IN  UINTN          NameLen,
    IN  BOOLEAN        IsDirectory,
    OUT ULONGLONG     *NewMFTIndex
    );

BOOLEAN
NtfsAppendRunToAttr (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER Rec,
    IN ULONG                AttrOffset,
    IN UINT64               StartLCN,
    IN UINT64               RunClusters,
    IN INT64                LastRealLCN
    );

/* ntfs_delete.c */
EFI_STATUS
NtfsEfiDeleteFile (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex
    );

/* ntfs_setinfo.c */
EFI_STATUS
NtfsEfiSetFileInfo (
    IN PNTFS_EFI_VCB  Vcb,
    IN ULONGLONG      MFTIndex,
    IN EFI_FILE_INFO *Info
    );

EFI_STATUS
NtfsEfiRenameFile (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex,
    IN CONST WCHAR  *NewName,
    IN UINTN         NewNameLen
    );

/* Truncate a file's $DATA to NewSize (shrink only; grow -> EFI_UNSUPPORTED). */
EFI_STATUS
NtfsEfiSetFileSize (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex,
    IN UINT64        NewSize
    );

/* Release prealloc slack: shrink AllocatedSize back to ceil(DataSize). */
EFI_STATUS
NtfsEfiTrimAllocation (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex
    );

/* General move/rename. DestParentMFT == (ULONGLONG)-1 -> same-dir rename. */
EFI_STATUS
NtfsEfiMoveFile (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex,
    IN ULONGLONG     DestParentMFT,
    IN CONST WCHAR  *NewName,
    IN UINTN         NewNameLen
    );

/*
 * Which MFT record owns a directory's $I30 index attributes. On a big or
 * fragmented directory these move out of the base record into an
 * $ATTRIBUTE_LIST extension record; index writers must edit THAT record.
 * See NtfsEfiResolveIndexHost (ntfs_attr.c).
 */
typedef struct {
    PFILE_RECORD_HEADER Rec;        /* record holding $INDEX_ROOT:$I30 */
    ULONGLONG           MFTIndex;   /* its MFT index - use for WriteFileRecord */
    BOOLEAN             Own;        /* TRUE -> caller must FreePool(Rec) */
    BOOLEAN             HasAlloc;   /* directory already has $INDEX_ALLOCATION:$I30 */
} NTFS_INDEX_HOST, *PNTFS_INDEX_HOST;

EFI_STATUS
NtfsEfiResolveIndexHost (
    IN  PNTFS_EFI_VCB       Vcb,
    IN  PFILE_RECORD_HEADER BaseRec,
    IN  ULONGLONG           BaseMFT,
    OUT PNTFS_INDEX_HOST    Host
    );

/* index insert/remove shared with rename (ntfs_create.c / ntfs_delete.c) */
EFI_STATUS
NtfsInsertIndexEntry (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER DirRec,
    IN ULONGLONG           DirMFT,
    IN UINT64              ChildRef,
    IN UINT64              ParentRef,
    IN UINT64              NowNtfs,
    IN CONST WCHAR        *Name,
    IN UINTN               NameLen,
    IN BOOLEAN             IsDirectory
    );

EFI_STATUS
NtfsEfiIndexRemoveByChild (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     DirMFT,
    IN ULONGLONG     ChildMFT
    );

/* ntfs_bitmap.c */
EFI_STATUS
NtfsEfiAllocateClusters (
    IN  PNTFS_EFI_VCB Vcb,
    IN  UINT64        ClustersNeeded,
    OUT UINT64        *StartLCN,
    OUT UINT64        *GotClusters
    );

EFI_STATUS
NtfsEfiFreeClusters (
    IN PNTFS_EFI_VCB Vcb,
    IN UINT64        StartLCN,
    IN UINT64        Count
    );

EFI_STATUS
NtfsEfiAllocateMftRecord (
    IN  PNTFS_EFI_VCB Vcb,
    OUT ULONGLONG    *NewIndex
    );

EFI_STATUS
NtfsEfiFreeMftRecord (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     Index
    );

/* ntfs_mft.c */
EFI_STATUS
NtfsEfiFixupRecord (
    IN PNTFS_EFI_VCB       Vcb,
    IN NTFS_RECORD_HEADER *Hdr
    );

/*
 * Sanity-check an index header's FirstEntryOffset/TotalSizeOfEntries/
 * AllocatedSize against the buffer it lives in, BEFORE deriving First/Last
 * walk pointers or using AllocatedSize as an insert room-check. All three are
 * untrusted on-disk data. Avail = bytes from Header to end of its buffer.
 */
BOOLEAN
NtfsEfiIndexHeaderOk (
    IN INDEX_HEADER_ATTRIBUTE *Header,
    IN UINT64                  Avail
    );

/* Same, for a whole INDX block read into a Vcb->BytesPerIndexRecord buffer. */
BOOLEAN
NtfsEfiIndexBlockOk (
    IN PNTFS_EFI_VCB Vcb,
    IN PINDEX_BUFFER Block
    );

/* Same, for a resident $INDEX_ROOT attribute record inside an MFT record. */
BOOLEAN
NtfsEfiIndexRootOk (
    IN PNTFS_ATTR_RECORD RootAttr
    );

EFI_STATUS
NtfsEfiReadFileRecord (
    IN  PNTFS_EFI_VCB    Vcb,
    IN  ULONGLONG        MFTIndex,
    OUT PFILE_RECORD_HEADER FileRecord   /* caller allocates BytesPerFileRecord */
    );

EFI_STATUS
NtfsEfiWriteFileRecord (
    IN PNTFS_EFI_VCB       Vcb,
    IN ULONGLONG           MFTIndex,
    IN PFILE_RECORD_HEADER FileRecord   /* BytesPerFileRecord bytes, currently fixed-up */
    );

EFI_STATUS
NtfsEfiWriteMultiSectorRecord (
    IN PNTFS_EFI_VCB       Vcb,
    IN PNTFS_ATTR_CTX      AttrCtx,
    IN UINT64              AttrOffset,
    IN NTFS_RECORD_HEADER *Hdr,
    IN ULONG               RecordLength
    );

/* ntfs_btree.c */
/* Collect ALL child MFT indices of a directory in-order into Out[0..Max);
 * returns count. One full B+tree walk (O(n)) - used to build the dir cache. */
ULONG
NtfsEfiCollectDir (
    IN  PNTFS_EFI_VCB Vcb,
    IN  ULONGLONG     DirMFTIndex,
    OUT ULONGLONG    *Out,
    IN  ULONG         Max
    );

ULONGLONG
NtfsEfiFindInDirectory (
    IN     PNTFS_EFI_VCB      Vcb,
    IN     ULONGLONG           DirMFTIndex,
    IN     CONST WCHAR        *Name,        /* component only, no backslashes */
    IN     UINTN               NameLen,
    IN     BOOLEAN             DirSearch,
    IN     BOOLEAN             CaseSensitive,
    IN OUT ULONG              *StartEntry   /* for sequential dir enumeration  */
    );

/* ntfs_symlink.c */
#define NTFS_MAX_PATH_CHARS 1024

BOOLEAN
NtfsEfiTryResolveSymlink (
    IN  PNTFS_EFI_VCB Vcb,
    IN  ULONGLONG     MFTIndex,
    OUT CONST WCHAR  **Target,      /* points into an internal static-lifetime buffer valid until next call */
    OUT BOOLEAN       *IsRelative
    );

/* ntfs_time.c */
VOID
NtfsEfiConvertTime (
    IN  UINT64    NtfsTime,
    OUT EFI_TIME *EfiTime
    );

UINT64
NtfsEfiConvertTimeToNtfs (
    IN CONST EFI_TIME *EfiTime
    );

/* ntfs_file.c */
VOID
NtfsEfiInitProtoTemplate (
    VOID
    );

PNTFS_EFI_FILE
NtfsEfiCreateHandle (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex
    );

/* shared MFT-record byte-shuffling primitives, also used by ntfs_create.c */
VOID
NtfsEfiShiftForward (
    IN PUCHAR Base,
    IN UINTN  Len,
    IN UINTN  Growth
    );

BOOLEAN
NtfsEfiGrowResidentInRecord (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER Rec,
    IN ULONG                AttrOffset,
    IN UINT64               NewValueLength
    );

/* ntfs_volume.c */
PNTFS_EFI_VCB
NtfsEfiVcbFromSfsp (
    IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfsp
    );

VOID
NtfsEfiUnmountVolume (
    IN PNTFS_EFI_VCB Vcb
    );

EFI_STATUS EFIAPI
NtfsEfiOpenVolume (
    IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *This,
    OUT EFI_FILE_PROTOCOL               **Root
    );

PNTFS_EFI_VCB
NtfsEfiMountVolume (
    IN EFI_DISK_IO_PROTOCOL  *DiskIo,
    IN EFI_BLOCK_IO_PROTOCOL *BlockIo
    );

EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *
NtfsEfiGetSfsp (
    IN PNTFS_EFI_VCB Vcb
    );

EFI_STATUS
NtfsSetVolumeDirty (
    IN PNTFS_EFI_VCB Vcb,
    IN BOOLEAN        Dirty
    );

VOID
NtfsMarkVolumeDirty (
    IN PNTFS_EFI_VCB Vcb
    );

/* ntfs_binding.c */
extern EFI_DRIVER_BINDING_PROTOCOL gNtfsDriverBinding;

#endif /* _NTFS_EFI_H_ */
