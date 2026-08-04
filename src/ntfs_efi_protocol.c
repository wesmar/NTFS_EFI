/**
 * ntfs_efi_protocol.c
 *
 * Implements EFI_SIMPLE_FILE_SYSTEM_PROTOCOL and EFI_FILE_PROTOCOL for
 * the NTFS EFI driver. Self-contained: does not include ntfs.h or any
 * WDM headers. All NTFS on-disk structures are redefined here with UEFI
 * primitive types. Read-only; write path stubs return EFI_WRITE_PROTECTED.
 *
 * Depends on: ntfs_efi_blockdev.c (NtfsEfiReadDisk / NtfsEfiWriteDisk),
 *             UefiLib, MemoryAllocationLib, BaseMemoryLib, BaseLib.
 *
 * Call NtfsEfiGetSfsp() to obtain the EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*
 * to install on the volume device handle.
 */

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DriverBinding.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/FileSystemVolumeLabelInfo.h>

CONST UINT32 _gUefiDriverRevision = 0;
CHAR8       *gEfiCallerBaseName   = "ntfs";

EFI_GUID gEfiDriverBindingProtocolGuid          = EFI_DRIVER_BINDING_PROTOCOL_GUID;
EFI_GUID gEfiSimpleFileSystemProtocolGuid       = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiBlockIoProtocolGuid                = EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID gEfiDiskIoProtocolGuid                 = EFI_DISK_IO_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid                       = EFI_FILE_INFO_ID;
EFI_GUID gEfiFileSystemInfoGuid                 = EFI_FILE_SYSTEM_INFO_ID;
EFI_GUID gEfiFileSystemVolumeLabelInfoIdGuid    = EFI_FILE_SYSTEM_VOLUME_LABEL_ID;

EFI_STATUS EFIAPI
UefiMain (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    );

EFI_STATUS EFIAPI
UefiUnload (
    IN EFI_HANDLE ImageHandle
    )
{
    (VOID)ImageHandle;
    return EFI_SUCCESS;
}

VOID EFIAPI
ProcessLibraryConstructorList (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    (VOID)ImageHandle;
    (VOID)SystemTable;
}

VOID EFIAPI
ProcessLibraryDestructorList (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    (VOID)ImageHandle;
    (VOID)SystemTable;
}

EFI_STATUS EFIAPI
ProcessModuleEntryPointList (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    return UefiMain (ImageHandle, SystemTable);
}

BOOLEAN EFIAPI
DebugAssertEnabled (
    VOID
    )
{
    return FALSE;
}

BOOLEAN EFIAPI
DebugPrintEnabled (
    VOID
    )
{
    return FALSE;
}

BOOLEAN EFIAPI
DebugCodeEnabled (
    VOID
    )
{
    return FALSE;
}

BOOLEAN EFIAPI
DebugClearMemoryEnabled (
    VOID
    )
{
    return FALSE;
}

BOOLEAN EFIAPI
DebugPrintLevelEnabled (
    IN CONST UINTN ErrorLevel
    )
{
    (VOID)ErrorLevel;
    return FALSE;
}

VOID EFIAPI
DebugAssert (
    IN CONST CHAR8 *FileName,
    IN UINTN        LineNumber,
    IN CONST CHAR8 *Description
    )
{
    (VOID)FileName;
    (VOID)LineNumber;
    (VOID)Description;
}

VOID EFIAPI
DebugPrint (
    IN UINTN        ErrorLevel,
    IN CONST CHAR8 *Format,
    ...
    )
{
    (VOID)ErrorLevel;
    (VOID)Format;
}

VOID EFIAPI
DebugVPrint (
    IN UINTN        ErrorLevel,
    IN CONST CHAR8 *Format,
    IN VA_LIST      VaListMarker
    )
{
    (VOID)ErrorLevel;
    (VOID)Format;
    (VOID)VaListMarker;
}

VOID EFIAPI
DebugBPrint (
    IN UINTN        ErrorLevel,
    IN CONST CHAR8 *Format,
    IN BASE_LIST    BaseListMarker
    )
{
    (VOID)ErrorLevel;
    (VOID)Format;
    (VOID)BaseListMarker;
}

VOID *EFIAPI
DebugClearMemory (
    OUT VOID *Buffer,
    IN UINTN  Length
    )
{
    return SetMem (Buffer, Length, 0);
}

/* =========================================================================
 * Section 1 – primitive type aliases (WDM ↔ UEFI)
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
 * Section 2 – NTFS on-disk structures
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
    AttributeEnd                 = 0xFFFFFFFF
} ATTRIBUTE_TYPE;

typedef struct {
    ULONG     Type;
    USHORT    Length;
    UCHAR     NameLength;
    UCHAR     NameOffset;
    ULONGLONG StartingVCN;
    ULONGLONG MFTIndex;
    USHORT    Instance;
} NTFS_ATTR_LIST_ITEM, *PNTFS_ATTR_LIST_ITEM;

typedef struct {
    ULONGLONG CreationTime;
    ULONGLONG ChangeTime;
    ULONGLONG LastWriteTime;
    ULONGLONG LastAccessTime;
    ULONG     FileAttribute;
    ULONG     AlignmentOrReserved[3];
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
#define NTFS_FILE_ROOT            5ULL
#define NTFS_FILE_BITMAP          6ULL
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

/* =========================================================================
 * Section 3 – driver-internal structures
 * ========================================================================= */

/* Run-list entry: one extent (replaces LARGE_MCB entirely) */
typedef struct {
    UINT64  VBN;   /* virtual block number (cluster units), i.e. offset in file */
    INT64   LBN;   /* logical block number on disk; -1 = sparse/unallocated     */
    UINT64  Len;   /* length in clusters                                         */
} NTFS_RUN_ENTRY;

#define NTFS_MAX_RUNS 2048  /* max extents per attribute; 2048×8 bytes of disk = 16 GB frags */

/* Attribute context – replaces WDM NTFS_ATTR_CONTEXT (no LARGE_MCB) */
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
} NTFS_EFI_VCB, *PNTFS_EFI_VCB;

/* Open-file handle (EFI_FILE_PROTOCOL MUST be the first member) */
typedef struct _NTFS_EFI_FILE {
    EFI_FILE_PROTOCOL  Protocol;        /* vtable – populated in CreateHandle  */
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
    /* read/enum state */
    UINT64             Position;        /* byte offset for files              */
    ULONG              DirEnumEntry;    /* sequential counter for directories */
} NTFS_EFI_FILE, *PNTFS_EFI_FILE;

static PNTFS_EFI_VCB
NtfsEfiVcbFromSfsp (
    IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfsp
    )
{
    return (PNTFS_EFI_VCB)((PUCHAR)Sfsp - FIELD_OFFSET (NTFS_EFI_VCB, Sfsp));
}

/* =========================================================================
 * Section 4 – disk I/O (thin wrapper around EFI_DISK_IO_PROTOCOL)
 * ========================================================================= */

static EFI_STATUS
NtfsEfiReadDisk (
    IN  PNTFS_EFI_VCB Vcb,
    IN  UINT64        ByteOffset,
    IN  UINTN         Length,
    OUT VOID         *Buffer
    )
{
    return Vcb->DiskIo->ReadDisk (Vcb->DiskIo, Vcb->MediaId, ByteOffset, Length, Buffer);
}

/* =========================================================================
 * Section 5 – run-list management (replaces LARGE_MCB + FsRtl*)
 * ========================================================================= */

static PUCHAR
NtfsDecodeRunEntry (
    IN  PUCHAR    DataRun,
    OUT LONGLONG *OutOffset,    /* delta LCN; -1 = sparse                    */
    OUT UINT64   *OutLength     /* run length in clusters                    */
    )
{
    UCHAR LenBytes  = *DataRun & 0x0F;
    UCHAR OffBytes  = (*DataRun >> 4) & 0x0F;
    UINTN i;

    *OutLength = 0;
    *OutOffset = 0;
    DataRun++;

    for (i = 0; i < LenBytes; i++) {
        *OutLength |= ((UINT64)*DataRun) << (i * 8);
        DataRun++;
    }
    if (OffBytes == 0) {
        *OutOffset = -1LL; /* sparse run */
    } else {
        for (i = 0; i < OffBytes - 1; i++) {
            *OutOffset |= ((UINT64)*DataRun) << (i * 8);
            DataRun++;
        }
        /* sign-extend the most-significant byte */
        *OutOffset = (LONGLONG)((INT64)(CCHAR)(*DataRun) << (INT32)(i * 8)) + *OutOffset;
        DataRun++;
    }
    return DataRun;
}

/* Decode all data runs from an attribute record into a flat NTFS_RUN_ENTRY array. */
static EFI_STATUS
NtfsBuildRunList (
    IN  PNTFS_ATTR_RECORD  AttrRecord,
    OUT NTFS_RUN_ENTRY    *Runs,
    IN  ULONG              MaxRuns,
    OUT ULONG             *RunCount
    )
{
    PUCHAR   DataRun;
    LONGLONG DeltaLCN;
    UINT64   RunLen;
    INT64    CurrentLCN = 0;
    UINT64   CurrentVBN = AttrRecord->NonResident.LowestVCN;
    ULONG    Count      = 0;

    DataRun = (PUCHAR)AttrRecord + AttrRecord->NonResident.MappingPairsOffset;

    while (*DataRun != 0) {
        if (Count >= MaxRuns) {
            return EFI_BUFFER_TOO_SMALL;
        }
        DataRun = NtfsDecodeRunEntry (DataRun, &DeltaLCN, &RunLen);

        if (DeltaLCN == -1LL) {
            Runs[Count].LBN = -1LL; /* sparse */
        } else {
            CurrentLCN += DeltaLCN;
            Runs[Count].LBN = CurrentLCN;
        }
        Runs[Count].VBN = CurrentVBN;
        Runs[Count].Len = RunLen;
        CurrentVBN += RunLen;
        Count++;
    }
    *RunCount = Count;
    return EFI_SUCCESS;
}

/* =========================================================================
 * Section 6 – attribute context lifecycle
 * ========================================================================= */

static PNTFS_ATTR_CTX
NtfsEfiAllocAttrCtx (VOID)
{
    return AllocateZeroPool (sizeof (NTFS_ATTR_CTX));
}

static VOID
NtfsEfiFreeAttrCtx (PNTFS_ATTR_CTX Ctx)
{
    if (Ctx == NULL) return;
    if (Ctx->pRecord)  FreePool (Ctx->pRecord);
    if (Ctx->Runs)     FreePool (Ctx->Runs);
    FreePool (Ctx);
}

static VOID
NtfsEfiUnmountVolume (
    IN PNTFS_EFI_VCB Vcb
    )
{
    if (Vcb == NULL) {
        return;
    }

    if (Vcb->MFTContext != NULL) {
        NtfsEfiFreeAttrCtx (Vcb->MFTContext);
    }
    if (Vcb->MasterFileTable != NULL) {
        FreePool (Vcb->MasterFileTable);
    }

    FreePool (Vcb);
}

static PNTFS_ATTR_CTX
NtfsEfiPrepareAttrCtx (
    IN PNTFS_ATTR_RECORD  AttrRecord,
    IN ULONGLONG          FileMFTIndex
    )
{
    PNTFS_ATTR_CTX Ctx = NtfsEfiAllocAttrCtx ();
    if (Ctx == NULL) return NULL;

    Ctx->pRecord = AllocatePool (AttrRecord->Length);
    if (Ctx->pRecord == NULL) {
        FreePool (Ctx);
        return NULL;
    }
    CopyMem (Ctx->pRecord, AttrRecord, AttrRecord->Length);
    Ctx->FileMFTIndex = FileMFTIndex;

    if (AttrRecord->IsNonResident) {
        Ctx->Runs = AllocatePool (sizeof (NTFS_RUN_ENTRY) * NTFS_MAX_RUNS);
        if (Ctx->Runs == NULL) {
            NtfsEfiFreeAttrCtx (Ctx);
            return NULL;
        }
        if (EFI_ERROR (NtfsBuildRunList (Ctx->pRecord, Ctx->Runs,
                                         NTFS_MAX_RUNS, &Ctx->RunCount))) {
            NtfsEfiFreeAttrCtx (Ctx);
            return NULL;
        }
    }
    return Ctx;
}

/* =========================================================================
 * Section 7 – ReadAttribute (no cache manager, no MCB, direct DiskIo)
 * ========================================================================= */

static UINT64
NtfsEfiAttrDataLength (PNTFS_ATTR_CTX Ctx)
{
    if (Ctx->pRecord->IsNonResident)
        return (UINT64)Ctx->pRecord->NonResident.DataSize;
    return Ctx->pRecord->Resident.ValueLength;
}

static ULONG
NtfsEfiReadAttr (
    IN  PNTFS_EFI_VCB  Vcb,
    IN  PNTFS_ATTR_CTX Ctx,
    IN  UINT64         Offset,
    OUT PCHAR          Buffer,
    IN  ULONG          Length
    )
{
    ULONG Remaining, AlreadyRead;

    if (!Ctx->pRecord->IsNonResident) {
        /* resident: data is inside the attribute record itself */
        UINT64 ValLen = Ctx->pRecord->Resident.ValueLength;
        PCHAR  ValPtr = (PCHAR)Ctx->pRecord + Ctx->pRecord->Resident.ValueOffset;
        if (Offset >= ValLen) return 0;
        if (Offset + Length > ValLen) Length = (ULONG)(ValLen - Offset);
        CopyMem (Buffer, ValPtr + Offset, Length);
        return Length;
    }

    /* non-resident: walk the run list */
    AlreadyRead = 0;
    Remaining   = Length;

    while (Remaining > 0) {
        ULONG  i;
        UINT64 RunOffsetBytes, RunLenBytes, TakeBytes;
        EFI_STATUS Status;

        /* update sequential cache: advance past exhausted run */
        while (Ctx->CacheIdx < Ctx->RunCount) {
            RunLenBytes = Ctx->Runs[Ctx->CacheIdx].Len * Vcb->BytesPerCluster;
            if (Offset < Ctx->CacheOffset + RunLenBytes) break;
            Ctx->CacheOffset += RunLenBytes;
            Ctx->CacheIdx++;
        }

        /* if cache missed (seek backwards or first access), linear scan */
        if (Ctx->CacheIdx >= Ctx->RunCount) {
            UINT64 Scan = 0;
            for (i = 0; i < Ctx->RunCount; i++) {
                UINT64 Bytes = Ctx->Runs[i].Len * Vcb->BytesPerCluster;
                if (Offset < Scan + Bytes) {
                    Ctx->CacheIdx    = i;
                    Ctx->CacheOffset = Scan;
                    break;
                }
                Scan += Bytes;
            }
            if (Ctx->CacheIdx >= Ctx->RunCount) break; /* past EOF */
        }

        RunOffsetBytes = Offset - Ctx->CacheOffset;
        RunLenBytes    = Ctx->Runs[Ctx->CacheIdx].Len * Vcb->BytesPerCluster;
        TakeBytes      = min (RunLenBytes - RunOffsetBytes, (UINT64)Remaining);

        if (Ctx->Runs[Ctx->CacheIdx].LBN == -1LL) {
            /* sparse: return zeros */
            ZeroMem (Buffer + AlreadyRead, (UINTN)TakeBytes);
        } else {
            UINT64 DiskByteOffset =
                (UINT64)Ctx->Runs[Ctx->CacheIdx].LBN * Vcb->BytesPerCluster
                + RunOffsetBytes;
            Status = NtfsEfiReadDisk (Vcb, DiskByteOffset, (UINTN)TakeBytes,
                                      Buffer + AlreadyRead);
            if (EFI_ERROR (Status)) break;
        }

        Offset      += TakeBytes;
        AlreadyRead += (ULONG)TakeBytes;
        Remaining   -= (ULONG)TakeBytes;

        /* advance cache to next run if this one is exhausted */
        if (RunOffsetBytes + TakeBytes == RunLenBytes) {
            Ctx->CacheOffset += RunLenBytes;
            Ctx->CacheIdx++;
        }
    }
    return AlreadyRead;
}

/* =========================================================================
 * Section 8 – USA fixup (update sequence array)
 * ========================================================================= */

static EFI_STATUS
NtfsEfiFixupRecord (
    IN PNTFS_EFI_VCB       Vcb,
    IN NTFS_RECORD_HEADER *Hdr
    )
{
    PUSHORT USA;
    PUSHORT SectorEnd;
    USHORT  USN;
    USHORT  i;
    ULONG   RecordSize;

    if (Hdr->UsaOffset == 0 || Hdr->UsaCount == 0) return EFI_SUCCESS;

    USA       = (PUSHORT)((PUCHAR)Hdr + Hdr->UsaOffset);
    USN       = USA[0];
    RecordSize = (Hdr->UsaCount - 1) * Vcb->BytesPerSector;

    for (i = 1; i < Hdr->UsaCount; i++) {
        SectorEnd = (PUSHORT)((PUCHAR)Hdr + i * Vcb->BytesPerSector - sizeof (USHORT));
        if (*SectorEnd != USN) return EFI_VOLUME_CORRUPTED;
        *SectorEnd = USA[i];
    }
    (VOID)RecordSize;
    return EFI_SUCCESS;
}

/* =========================================================================
 * Section 9 – ReadFileRecord
 * ========================================================================= */

static EFI_STATUS
NtfsEfiReadFileRecord (
    IN  PNTFS_EFI_VCB    Vcb,
    IN  ULONGLONG        MFTIndex,
    OUT PFILE_RECORD_HEADER FileRecord   /* caller allocates BytesPerFileRecord */
    )
{
    UINT64 ByteOffset = MFTIndex * Vcb->BytesPerFileRecord;
    ULONG  BytesRead  = NtfsEfiReadAttr (Vcb, Vcb->MFTContext, ByteOffset,
                                         (PCHAR)FileRecord, Vcb->BytesPerFileRecord);
    if (BytesRead != Vcb->BytesPerFileRecord) return EFI_DEVICE_ERROR;
    return NtfsEfiFixupRecord (Vcb, &FileRecord->Ntfs);
}

/* =========================================================================
 * Section 10 – FindAttribute (in-record scan + attribute-list follow)
 * ========================================================================= */

static PNTFS_ATTR_CTX
NtfsEfiFindAttrInRecord (
    IN  PNTFS_EFI_VCB        Vcb,
    IN  PFILE_RECORD_HEADER  FileRecord,
    IN  ATTRIBUTE_TYPE       Type,
    IN  PCWSTR               Name,           /* NULL or L"" = unnamed          */
    IN  USHORT               NameLength,     /* char count                     */
    OUT ULONG               *AttrOffset      /* may be NULL                    */
    );

static PNTFS_ATTR_CTX
NtfsEfiFindAttribute (
    IN  PNTFS_EFI_VCB        Vcb,
    IN  PFILE_RECORD_HEADER  FileRecord,
    IN  ATTRIBUTE_TYPE       Type,
    IN  PCWSTR               Name,
    IN  USHORT               NameLength,
    OUT ULONG               *AttrOffset
    )
{
    PNTFS_ATTR_CTX    Ctx;
    PNTFS_ATTR_CTX    ListCtx;

    /* primary scan */
    Ctx = NtfsEfiFindAttrInRecord (Vcb, FileRecord, Type, Name, NameLength, AttrOffset);
    if (Ctx != NULL) return Ctx;

    /* follow $ATTRIBUTE_LIST if present */
    ListCtx = NtfsEfiFindAttrInRecord (Vcb, FileRecord,
                                        AttributeAttributeList, NULL, 0, NULL);
    if (ListCtx == NULL) return NULL;

    {
        UINT64             ListLen  = NtfsEfiAttrDataLength (ListCtx);
        PUCHAR             ListBuf  = AllocatePool ((UINTN)ListLen);
        PNTFS_ATTR_LIST_ITEM Item;
        PNTFS_ATTR_LIST_ITEM ListEnd;

        if (ListBuf == NULL) { NtfsEfiFreeAttrCtx (ListCtx); return NULL; }
        NtfsEfiReadAttr (Vcb, ListCtx, 0, (PCHAR)ListBuf, (ULONG)ListLen);
        NtfsEfiFreeAttrCtx (ListCtx);

        Item    = (PNTFS_ATTR_LIST_ITEM)ListBuf;
        ListEnd = (PNTFS_ATTR_LIST_ITEM)(ListBuf + ListLen);

        while (Item < ListEnd && Item->Type != (ULONG)AttributeEnd) {
            if (Item->Type == (ULONG)Type && Item->NameLength == NameLength) {
                BOOLEAN NameMatch = TRUE;
                if (NameLength > 0) {
                    PWCHAR AttrName = (PWCHAR)((PUCHAR)Item + Item->NameOffset);
                    if (CompareMem (AttrName, Name, NameLength * sizeof (WCHAR)) != 0)
                        NameMatch = FALSE;
                }
                if (NameMatch) {
                    ULONGLONG      RemoteIdx = Item->MFTIndex & NTFS_MFT_MASK;
                    PFILE_RECORD_HEADER RemoteRec = AllocatePool (Vcb->BytesPerFileRecord);
                    if (RemoteRec == NULL) break;
                    if (!EFI_ERROR (NtfsEfiReadFileRecord (Vcb, RemoteIdx, RemoteRec))) {
                        Ctx = NtfsEfiFindAttrInRecord (Vcb, RemoteRec, Type,
                                                        Name, NameLength, AttrOffset);
                    }
                    FreePool (RemoteRec);
                    if (Ctx != NULL) { FreePool (ListBuf); return Ctx; }
                }
            }
            if (Item->Length == 0) break;
            Item = (PNTFS_ATTR_LIST_ITEM)((PUCHAR)Item + Item->Length);
        }
        FreePool (ListBuf);
    }
    return NULL;
}

static PNTFS_ATTR_CTX
NtfsEfiFindAttrInRecord (
    IN  PNTFS_EFI_VCB        Vcb,
    IN  PFILE_RECORD_HEADER  FileRecord,
    IN  ATTRIBUTE_TYPE       Type,
    IN  PCWSTR               Name,
    IN  USHORT               NameLength,
    OUT ULONG               *AttrOffset
    )
{
    PNTFS_ATTR_RECORD Attr    = (PNTFS_ATTR_RECORD)
                                ((PUCHAR)FileRecord + FileRecord->AttributeOffset);
    PNTFS_ATTR_RECORD LastPtr = (PNTFS_ATTR_RECORD)
                                ((PUCHAR)FileRecord + FileRecord->BytesInUse);

    while (Attr < LastPtr && Attr->Type != (ULONG)AttributeEnd) {
        if (Attr->Length == 0) break;

        if (Attr->Type == (ULONG)Type && Attr->NameLength == NameLength) {
            BOOLEAN NameMatch = TRUE;
            if (NameLength > 0) {
                PWCHAR AttrName = (PWCHAR)((PUCHAR)Attr + Attr->NameOffset);
                if (CompareMem (AttrName, Name, NameLength * sizeof (WCHAR)) != 0)
                    NameMatch = FALSE;
            }
            if (NameMatch) {
                PNTFS_ATTR_CTX Ctx = NtfsEfiPrepareAttrCtx (Attr,
                                        FileRecord->MFTRecordNumber);
                if (Ctx != NULL) {
                    if (AttrOffset != NULL)
                        *AttrOffset = (ULONG)((PUCHAR)Attr - (PUCHAR)FileRecord);
                    return Ctx;
                }
            }
        }
        Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Attr + Attr->Length);
    }
    return NULL;
}

/* =========================================================================
 * Section 11 – directory lookup (B-tree index traversal)
 * ========================================================================= */

/*
 * Case-insensitive wide-string comparison of exactly Len characters.
 * Returns 0 if equal, <0 / >0 otherwise (same contract as StrnCmp but ignores case).
 */
static INTN
NtfsEfiWcsniCmp (IN CONST WCHAR *A, IN CONST WCHAR *B, IN UINTN Len)
{
    UINTN i;
    for (i = 0; i < Len; i++) {
        WCHAR Ca = (A[i] >= L'a' && A[i] <= L'z') ? (WCHAR)(A[i] - 32) : A[i];
        WCHAR Cb = (B[i] >= L'a' && B[i] <= L'z') ? (WCHAR)(B[i] - 32) : B[i];
        if (Ca != Cb) return (INTN)Ca - (INTN)Cb;
    }
    return 0;
}

/*
 * Match a component name against an index entry.
 * DirSearch = FALSE → exact match (used during open-path lookup).
 * DirSearch = TRUE  → wildcard match not implemented; treated as exact.
 */
static BOOLEAN
NtfsEfiMatchEntry (
    IN CONST WCHAR              *Name,
    IN UINTN                     NameLen,
    IN PINDEX_ENTRY_ATTRIBUTE    Entry,
    IN BOOLEAN                   CaseSensitive
    )
{
    UINTN EntryLen = Entry->FileName.NameLength;
    if (EntryLen != NameLen) return FALSE;
    if (CaseSensitive)
        return (CompareMem (Name, Entry->FileName.Name, NameLen * sizeof (WCHAR)) == 0);
    return (NtfsEfiWcsniCmp (Name, Entry->FileName.Name, NameLen) == 0);
}

/* Scan a flat array of index entries (from INDEX_ROOT or INDEX_BUFFER).
 * Returns MFT index on success, MAX_UINT64 on miss.
 * *StartEntry / *CurrentEntry implement the sequential directory enumeration
 * position: during DirSearch each matching non-DOS entry increments *CurrentEntry.
 */
static ULONGLONG
NtfsEfiScanIndexBlock (
    IN     PINDEX_ENTRY_ATTRIBUTE  First,
    IN     PINDEX_ENTRY_ATTRIBUTE  Last,
    IN     CONST WCHAR            *Name,
    IN     UINTN                   NameLen,
    IN     BOOLEAN                 DirSearch,
    IN     BOOLEAN                 CaseSensitive,
    IN OUT ULONG                  *StartEntry,
    IN OUT ULONG                  *CurrentEntry
    );

/* Forward declaration for mutual recursion with sub-node traversal */
static ULONGLONG
NtfsEfiBrowseSubNode (
    IN     PNTFS_EFI_VCB      Vcb,
    IN     PNTFS_ATTR_CTX     IndexAllocCtx,
    IN     PUCHAR              Bitmap,
    IN     ULONG               BitmapBits,
    IN     ULONG               ClustPerBlock,
    IN     ULONGLONG           VCN,
    IN     CONST WCHAR        *Name,
    IN     UINTN               NameLen,
    IN     BOOLEAN             DirSearch,
    IN     BOOLEAN             CaseSensitive,
    IN OUT ULONG              *StartEntry,
    IN OUT ULONG              *CurrentEntry
    );

static ULONGLONG
NtfsEfiFindInDirectory (
    IN     PNTFS_EFI_VCB      Vcb,
    IN     ULONGLONG           DirMFTIndex,
    IN     CONST WCHAR        *Name,        /* component only, no backslashes */
    IN     UINTN               NameLen,
    IN     BOOLEAN             DirSearch,
    IN     BOOLEAN             CaseSensitive,
    IN OUT ULONG              *StartEntry   /* for sequential dir enumeration  */
    )
{
    PFILE_RECORD_HEADER DirRecord;
    PNTFS_ATTR_CTX      IndexRootCtx;
    PNTFS_ATTR_CTX      IndexAllocCtx;
    PNTFS_ATTR_CTX      BitmapCtx;
    PUCHAR              IndexBuf;
    PUCHAR              BitmapBuf;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    PINDEX_ENTRY_ATTRIBUTE First, Last;
    ULONG               CurrentEntry = 0;
    ULONG               ClustPerBlock;
    ULONGLONG           Result = (ULONGLONG)-1LL;

    DirRecord = AllocatePool (Vcb->BytesPerFileRecord);
    if (DirRecord == NULL) return (ULONGLONG)-1LL;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, DirMFTIndex, DirRecord))) {
        FreePool (DirRecord); return (ULONGLONG)-1LL;
    }

    IndexRootCtx = NtfsEfiFindAttribute (Vcb, DirRecord,
                        AttributeIndexRoot, L"$I30", 4, NULL);
    if (IndexRootCtx == NULL) { FreePool (DirRecord); return (ULONGLONG)-1LL; }

    IndexBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (IndexBuf == NULL) {
        NtfsEfiFreeAttrCtx (IndexRootCtx); FreePool (DirRecord);
        return (ULONGLONG)-1LL;
    }
    NtfsEfiReadAttr (Vcb, IndexRootCtx, 0, (PCHAR)IndexBuf, Vcb->BytesPerIndexRecord);
    NtfsEfiFreeAttrCtx (IndexRootCtx);

    IndexRoot = (PINDEX_ROOT_ATTRIBUTE)IndexBuf;
    First     = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header
                    + IndexRoot->Header.FirstEntryOffset);
    Last      = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)IndexBuf
                    + IndexRoot->Header.TotalSizeOfEntries);

    /* Try to find in $INDEX_ROOT before touching $INDEX_ALLOCATION */
    IndexAllocCtx = NtfsEfiFindAttribute (Vcb, DirRecord,
                        AttributeIndexAllocation, L"$I30", 4, NULL);
    BitmapBuf = NULL;
    BitmapCtx = NULL;
    ClustPerBlock = Vcb->BytesPerIndexRecord / Vcb->BytesPerCluster;

    if (IndexAllocCtx != NULL) {
        UINT64 BitmapLen;
        BitmapCtx = NtfsEfiFindAttribute (Vcb, DirRecord,
                        AttributeBitmap, L"$I30", 4, NULL);
        if (BitmapCtx != NULL) {
            BitmapLen = NtfsEfiAttrDataLength (BitmapCtx);
            BitmapBuf = AllocateZeroPool ((UINTN)BitmapLen + sizeof (ULONG));
            if (BitmapBuf != NULL)
                NtfsEfiReadAttr (Vcb, BitmapCtx, 0, (PCHAR)BitmapBuf, (ULONG)BitmapLen);
            NtfsEfiFreeAttrCtx (BitmapCtx);
        }
    }

    Result = NtfsEfiScanIndexBlock (First, Last, Name, NameLen,
                                     DirSearch, CaseSensitive,
                                     StartEntry, &CurrentEntry);

    /* Walk sub-nodes referenced by $INDEX_ALLOCATION */
    if (Result == (ULONGLONG)-1LL && IndexAllocCtx != NULL) {
        PINDEX_ENTRY_ATTRIBUTE Entry = First;
        while ((PUCHAR)Entry < (PUCHAR)Last) {
            if (Entry->Length == 0) break;
            if ((Entry->Flags & NTFS_INDEX_ENTRY_NODE) &&
                !(Entry->Flags & NTFS_INDEX_ENTRY_END))
            {
                ULONGLONG SubVCN = *(PULONGLONG)
                    ((PUCHAR)Entry + Entry->Length - sizeof (ULONGLONG));
                ULONG BitmapBits = BitmapBuf ? (ULONG)(NtfsEfiAttrDataLength (IndexAllocCtx)
                                    / (ClustPerBlock * Vcb->BytesPerCluster)) : 0;
                Result = NtfsEfiBrowseSubNode (Vcb, IndexAllocCtx,
                            BitmapBuf, BitmapBits, ClustPerBlock,
                            SubVCN, Name, NameLen,
                            DirSearch, CaseSensitive,
                            StartEntry, &CurrentEntry);
                if (Result != (ULONGLONG)-1LL) break;
            }
            if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;
            Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
        }
    }

    if (BitmapBuf)     FreePool (BitmapBuf);
    if (IndexAllocCtx) NtfsEfiFreeAttrCtx (IndexAllocCtx);
    FreePool (IndexBuf);
    FreePool (DirRecord);
    return Result;
}

static ULONGLONG
NtfsEfiScanIndexBlock (
    IN     PINDEX_ENTRY_ATTRIBUTE  First,
    IN     PINDEX_ENTRY_ATTRIBUTE  Last,
    IN     CONST WCHAR            *Name,
    IN     UINTN                   NameLen,
    IN     BOOLEAN                 DirSearch,
    IN     BOOLEAN                 CaseSensitive,
    IN OUT ULONG                  *StartEntry,
    IN OUT ULONG                  *CurrentEntry
    )
{
    PINDEX_ENTRY_ATTRIBUTE Entry = First;

    while ((PUCHAR)Entry < (PUCHAR)Last) {
        if (Entry->Length < sizeof (INDEX_ENTRY_ATTRIBUTE)) break;
        if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;

        if ((Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE
            && Entry->FileName.NameType != NTFS_FILE_NAME_DOS)
        {
            if (DirSearch) {
                /* sequential enumeration: return the StartEntry-th match */
                if (*CurrentEntry >= *StartEntry) {
                    /* wildcard '*' — always matches for simple enum */
                    (*StartEntry)++;
                    (*CurrentEntry)++;
                    return (Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
                }
                (*CurrentEntry)++;
            } else {
                /* exact / case-insensitive lookup during Open() */
                if (NtfsEfiMatchEntry (Name, NameLen, Entry, CaseSensitive))
                    return (Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
            }
        }
        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
    }
    return (ULONGLONG)-1LL;
}

static ULONGLONG
NtfsEfiBrowseSubNode (
    IN     PNTFS_EFI_VCB      Vcb,
    IN     PNTFS_ATTR_CTX     IndexAllocCtx,
    IN     PUCHAR              Bitmap,
    IN     ULONG               BitmapBits,
    IN     ULONG               ClustPerBlock,
    IN     ULONGLONG           VCN,
    IN     CONST WCHAR        *Name,
    IN     UINTN               NameLen,
    IN     BOOLEAN             DirSearch,
    IN     BOOLEAN             CaseSensitive,
    IN OUT ULONG              *StartEntry,
    IN OUT ULONG              *CurrentEntry
    )
{
    ULONG               NodeNumber = (ULONG)(VCN / ClustPerBlock);
    PUCHAR              IndexBuf;
    PINDEX_BUFFER       Block;
    PINDEX_ENTRY_ATTRIBUTE First, Last, Entry;
    ULONGLONG           Result;

    /* validate against bitmap */
    if (Bitmap != NULL && BitmapBits > 0 && NodeNumber < BitmapBits) {
        if (!((Bitmap[NodeNumber / 8] >> (NodeNumber % 8)) & 1))
            return (ULONGLONG)-1LL;
    }

    IndexBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (IndexBuf == NULL) return (ULONGLONG)-1LL;

    if (NtfsEfiReadAttr (Vcb, IndexAllocCtx,
                          VCN * Vcb->BytesPerCluster,
                          (PCHAR)IndexBuf,
                          Vcb->BytesPerIndexRecord)
        != Vcb->BytesPerIndexRecord) {
        FreePool (IndexBuf); return (ULONGLONG)-1LL;
    }
    Block = (PINDEX_BUFFER)IndexBuf;
    if (Block->Ntfs.Type != NRH_INDX_TYPE) {
        FreePool (IndexBuf); return (ULONGLONG)-1LL;
    }
    NtfsEfiFixupRecord (Vcb, &((PFILE_RECORD_HEADER)Block)->Ntfs);

    First = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
    Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries);

    Result = NtfsEfiScanIndexBlock (First, Last, Name, NameLen,
                                     DirSearch, CaseSensitive,
                                     StartEntry, CurrentEntry);

    /* recurse into sub-nodes within this block */
    if (Result == (ULONGLONG)-1LL && (Block->Header.Flags & INDEX_NODE_LARGE)) {
        Entry = First;
        while ((PUCHAR)Entry < (PUCHAR)Last) {
            if (Entry->Length == 0) break;
            if ((Entry->Flags & NTFS_INDEX_ENTRY_NODE) &&
                !(Entry->Flags & NTFS_INDEX_ENTRY_END))
            {
                ULONGLONG SubVCN = *(PULONGLONG)
                    ((PUCHAR)Entry + Entry->Length - sizeof (ULONGLONG));
                Result = NtfsEfiBrowseSubNode (Vcb, IndexAllocCtx,
                            Bitmap, BitmapBits, ClustPerBlock,
                            SubVCN, Name, NameLen,
                            DirSearch, CaseSensitive,
                            StartEntry, CurrentEntry);
                if (Result != (ULONGLONG)-1LL) break;
            }
            if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;
            Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
        }
    }
    FreePool (IndexBuf);
    return Result;
}

/* =========================================================================
 * Section 12 – NTFS → EFI_TIME conversion
 * ========================================================================= */

/* 100 ns intervals between 1601-01-01 and 1970-01-01 */
#define NTFS_EPOCH_DELTA  116444736000000000ULL

static VOID
NtfsEfiConvertTime (
    IN  UINT64    NtfsTime,
    OUT EFI_TIME *EfiTime
    )
{
    UINT64 UnixSec, Rem;
    UINT32 Year, Month, Day, Hour, Minute, Second;
    UINT32 DaysInYear, DaysInMonth;
    /* days per month in a non-leap year */
    UINT8  DPM[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    BOOLEAN Leap;
    UINT32 Days;

    ZeroMem (EfiTime, sizeof (EFI_TIME));
    if (NtfsTime < NTFS_EPOCH_DELTA) return;

    Rem        = (NtfsTime - NTFS_EPOCH_DELTA);
    EfiTime->Nanosecond = (UINT32)((Rem % 10000000ULL) * 100ULL);
    UnixSec    = Rem / 10000000ULL;

    Second     = (UINT32)(UnixSec % 60); UnixSec /= 60;
    Minute     = (UINT32)(UnixSec % 60); UnixSec /= 60;
    Hour       = (UINT32)(UnixSec % 24); UnixSec /= 24;

    /* Days since 1970-01-01 */
    Days = (UINT32)UnixSec;
    Year = 1970;
    for (;;) {
        Leap = ((Year % 4 == 0 && Year % 100 != 0) || (Year % 400 == 0));
        DaysInYear = Leap ? 366 : 365;
        if (Days < DaysInYear) break;
        Days -= DaysInYear;
        Year++;
    }
    Leap = ((Year % 4 == 0 && Year % 100 != 0) || (Year % 400 == 0));
    DPM[1] = Leap ? 29 : 28;
    Month  = 0;
    for (;;) {
        DaysInMonth = DPM[Month];
        if (Days < DaysInMonth) break;
        Days -= DaysInMonth;
        Month++;
    }
    Day = Days + 1;

    EfiTime->Year   = (UINT16)Year;
    EfiTime->Month  = (UINT8)(Month + 1);
    EfiTime->Day    = (UINT8)Day;
    EfiTime->Hour   = (UINT8)Hour;
    EfiTime->Minute = (UINT8)Minute;
    EfiTime->Second = (UINT8)Second;
    EfiTime->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
}

/* =========================================================================
 * Section 13 – handle factory helpers
 * ========================================================================= */

static EFI_FILE_PROTOCOL g_FileProtoTemplate;  /* filled once at OpenVolume */

static PNTFS_EFI_FILE
NtfsEfiCreateHandle (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex
    )
{
    PNTFS_EFI_FILE         Handle;
    PFILE_RECORD_HEADER    Rec;
    PNTFS_ATTR_CTX         StdCtx;
    PNTFS_ATTR_CTX         FnCtx;
    PSTANDARD_INFORMATION  StdInfo;
    PFILENAME_ATTRIBUTE    FnAttr;
    PNTFS_ATTR_CTX         DataCtx;

    Handle = AllocateZeroPool (sizeof (NTFS_EFI_FILE));
    if (Handle == NULL) return NULL;

    Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) { FreePool (Handle); return NULL; }
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, MFTIndex, Rec))) {
        FreePool (Rec); FreePool (Handle); return NULL;
    }

    Handle->Vcb      = Vcb;
    Handle->MFTIndex = MFTIndex;
    CopyMem (&Handle->Protocol, &g_FileProtoTemplate, sizeof (EFI_FILE_PROTOCOL));

    /* determine if directory */
    Handle->IsDirectory = (Rec->Flags & FRH_DIRECTORY) != 0;

    /* extract $STANDARD_INFORMATION timestamps & attributes */
    StdCtx = NtfsEfiFindAttribute (Vcb, Rec, AttributeStandardInformation, NULL, 0, NULL);
    if (StdCtx != NULL) {
        StdInfo = AllocatePool (sizeof (STANDARD_INFORMATION));
        if (StdInfo != NULL) {
            NtfsEfiReadAttr (Vcb, StdCtx, 0, (PCHAR)StdInfo, sizeof (STANDARD_INFORMATION));
            Handle->CreationTime  = StdInfo->CreationTime;
            Handle->ChangeTime    = StdInfo->ChangeTime;
            Handle->LastWriteTime = StdInfo->LastWriteTime;
            Handle->LastAccessTime= StdInfo->LastAccessTime;
            Handle->NtfsAttribs   = StdInfo->FileAttribute;
            FreePool (StdInfo);
        }
        NtfsEfiFreeAttrCtx (StdCtx);
    }

    /* prefer WIN32 name; fall back to POSIX */
    FnCtx = NtfsEfiFindAttribute (Vcb, Rec, AttributeFileName, NULL, 0, NULL);
    if (FnCtx != NULL) {
        ULONG FnLen = (ULONG)NtfsEfiAttrDataLength (FnCtx);
        FnAttr = AllocatePool (FnLen);
        if (FnAttr != NULL) {
            NtfsEfiReadAttr (Vcb, FnCtx, 0, (PCHAR)FnAttr, FnLen);
            Handle->FileNameChars = FnAttr->NameLength;
            if (Handle->FileNameChars > 255) Handle->FileNameChars = 255;
            CopyMem (Handle->FileName, FnAttr->Name,
                     Handle->FileNameChars * sizeof (WCHAR));
            Handle->FileName[Handle->FileNameChars] = L'\0';
            /* use $FILE_NAME sizes as fallback for directories */
            if (Handle->IsDirectory) {
                Handle->FileSize  = FnAttr->DataSize;
                Handle->AllocSize = FnAttr->AllocatedSize;
            }
            FreePool (FnAttr);
        }
        NtfsEfiFreeAttrCtx (FnCtx);
    }

    /* for files: get real size from $DATA */
    if (!Handle->IsDirectory) {
        DataCtx = NtfsEfiFindAttribute (Vcb, Rec, AttributeData, NULL, 0, NULL);
        if (DataCtx != NULL) {
            Handle->FileSize  = NtfsEfiAttrDataLength (DataCtx);
            Handle->AllocSize = DataCtx->pRecord->IsNonResident
                ? (UINT64)DataCtx->pRecord->NonResident.AllocatedSize
                : ROUND_UP (DataCtx->pRecord->Resident.ValueLength, Vcb->BytesPerCluster);
            NtfsEfiFreeAttrCtx (DataCtx);
        }
    }

    FreePool (Rec);
    return Handle;
}

/* =========================================================================
 * Section 14 – path lookup
 * ========================================================================= */

/*
 * Split L"foo\bar\baz" into component L"foo" (Comp) and remainder L"bar\baz" (Rest).
 * Leading backslash is consumed.  Returns TRUE if a component was found.
 */
static BOOLEAN
NtfsEfiSplitPath (
    IN  CONST WCHAR  *Path,
    OUT CONST WCHAR **Comp,
    OUT UINTN        *CompLen,
    OUT CONST WCHAR **Rest
    )
{
    UINTN i = 0;
    while (Path[i] == L'\\') i++;    /* skip leading separators              */
    if (Path[i] == L'\0') return FALSE;
    *Comp = Path + i;
    while (Path[i] != L'\0' && Path[i] != L'\\') i++;
    *CompLen = (Path + i) - *Comp;
    while (Path[i] == L'\\') i++;    /* skip trailing separators             */
    *Rest = Path + i;
    return TRUE;
}

static ULONGLONG
NtfsEfiLookupPath (
    IN PNTFS_EFI_VCB  Vcb,
    IN ULONGLONG      StartMFT,
    IN CONST WCHAR   *Path,
    IN BOOLEAN        CaseSensitive
    )
{
    CONST WCHAR *Comp, *Rest;
    UINTN        CompLen;
    ULONGLONG    Cur = StartMFT;

    while (NtfsEfiSplitPath (Path, &Comp, &CompLen, &Rest)) {
        ULONG Dummy = 0;
        Cur = NtfsEfiFindInDirectory (Vcb, Cur, Comp, CompLen,
                                       FALSE, CaseSensitive, &Dummy);
        if (Cur == (ULONGLONG)-1LL) return (ULONGLONG)-1LL;
        Path = Rest;
    }
    return Cur;
}

/* =========================================================================
 * Section 15 – EFI_FILE_PROTOCOL: Open
 * ========================================================================= */

static EFI_STATUS EFIAPI
NtfsEfiOpen (
    IN  EFI_FILE_PROTOCOL  *This,
    OUT EFI_FILE_PROTOCOL **NewHandle,
    IN  CHAR16             *FileName,
    IN  UINT64              OpenMode,
    IN  UINT64              Attributes
    )
{
    PNTFS_EFI_FILE Parent = (PNTFS_EFI_FILE)This;
    PNTFS_EFI_VCB  Vcb    = Parent->Vcb;
    ULONGLONG      TargetMFT;
    PNTFS_EFI_FILE Handle;
    ULONGLONG      StartMFT;
    CONST WCHAR   *Path    = FileName;

    /* write modes are not supported */
    if (OpenMode & (EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE))
        return EFI_WRITE_PROTECTED;

    /* resolve starting point */
    if (Path[0] == L'\\') {
        StartMFT = NTFS_FILE_ROOT;
        while (*Path == L'\\') Path++;
    } else {
        StartMFT = Parent->MFTIndex;
    }

    /* "." → self, ".." → not implemented (no parent pointer cached) */
    if (Path[0] == L'\0' || (Path[0] == L'.' && Path[1] == L'\0')) {
        TargetMFT = Parent->MFTIndex;
    } else {
        TargetMFT = NtfsEfiLookupPath (Vcb, StartMFT, Path, FALSE);
        if (TargetMFT == (ULONGLONG)-1LL) return EFI_NOT_FOUND;
    }

    Handle = NtfsEfiCreateHandle (Vcb, TargetMFT);
    if (Handle == NULL) return EFI_OUT_OF_RESOURCES;

    *NewHandle = &Handle->Protocol;
    return EFI_SUCCESS;
}

/* =========================================================================
 * Section 16 – EFI_FILE_PROTOCOL: Close / Delete
 * ========================================================================= */

static EFI_STATUS EFIAPI
NtfsEfiClose (IN EFI_FILE_PROTOCOL *This)
{
    FreePool (This);   /* NTFS_EFI_FILE is a single flat allocation */
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
NtfsEfiDelete (IN EFI_FILE_PROTOCOL *This)
{
    FreePool (This);
    return EFI_WARN_DELETE_FAILURE;  /* read-only volume */
}

/* =========================================================================
 * Section 17 – EFI_FILE_PROTOCOL: Read
 * ========================================================================= */

static EFI_STATUS EFIAPI
NtfsEfiRead (
    IN     EFI_FILE_PROTOCOL *This,
    IN OUT UINTN             *BufferSize,
    OUT    VOID              *Buffer
    )
{
    PNTFS_EFI_FILE   F   = (PNTFS_EFI_FILE)This;
    PNTFS_EFI_VCB    Vcb = F->Vcb;

    if (!F->IsDirectory) {
        /* ── regular file read ────────────────────────────────────────── */
        PFILE_RECORD_HEADER Rec;
        PNTFS_ATTR_CTX      DataCtx;
        ULONG               ToRead, Read;

        if (F->Position >= F->FileSize) {
            *BufferSize = 0;
            return EFI_SUCCESS;
        }
        ToRead = (ULONG)min ((UINT64)*BufferSize, F->FileSize - F->Position);

        Rec = AllocatePool (Vcb->BytesPerFileRecord);
        if (Rec == NULL) return EFI_OUT_OF_RESOURCES;
        if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, F->MFTIndex, Rec))) {
            FreePool (Rec); return EFI_DEVICE_ERROR;
        }
        DataCtx = NtfsEfiFindAttribute (Vcb, Rec, AttributeData, NULL, 0, NULL);
        FreePool (Rec);
        if (DataCtx == NULL) return EFI_NOT_FOUND;

        Read = NtfsEfiReadAttr (Vcb, DataCtx, F->Position, (PCHAR)Buffer, ToRead);
        NtfsEfiFreeAttrCtx (DataCtx);

        F->Position += Read;
        *BufferSize  = Read;
        return EFI_SUCCESS;

    } else {
        /* ── directory read: one EFI_FILE_INFO per call ───────────────── */
        ULONGLONG       ChildMFT;
        PNTFS_EFI_FILE  Child;
        EFI_FILE_INFO  *Info;
        UINTN           InfoSize;
        ULONG           StartEntry = F->DirEnumEntry;

        ChildMFT = NtfsEfiFindInDirectory (Vcb, F->MFTIndex,
                        NULL, 0, TRUE, FALSE, &StartEntry);
        if (ChildMFT == (ULONGLONG)-1LL) {
            *BufferSize = 0;
            return EFI_SUCCESS;
        }

        Child = NtfsEfiCreateHandle (Vcb, ChildMFT);
        if (Child == NULL) return EFI_OUT_OF_RESOURCES;

        InfoSize = SIZE_OF_EFI_FILE_INFO
                   + (Child->FileNameChars + 1) * sizeof (CHAR16);

        if (*BufferSize < InfoSize) {
            *BufferSize = InfoSize;
            FreePool (Child);
            return EFI_BUFFER_TOO_SMALL;
        }

        Info = (EFI_FILE_INFO*)Buffer;
        ZeroMem (Info, InfoSize);
        Info->Size         = InfoSize;
        Info->FileSize     = Child->FileSize;
        Info->PhysicalSize = Child->AllocSize;
        NtfsEfiConvertTime (Child->CreationTime,   &Info->CreateTime);
        NtfsEfiConvertTime (Child->LastAccessTime, &Info->LastAccessTime);
        NtfsEfiConvertTime (Child->LastWriteTime,  &Info->ModificationTime);

        /* map NTFS attributes → EFI attributes */
        if (Child->NtfsAttribs & NTFS_FILE_TYPE_READ_ONLY) Info->Attribute |= EFI_FILE_READ_ONLY;
        if (Child->NtfsAttribs & NTFS_FILE_TYPE_HIDDEN)    Info->Attribute |= EFI_FILE_HIDDEN;
        if (Child->NtfsAttribs & NTFS_FILE_TYPE_SYSTEM)    Info->Attribute |= EFI_FILE_SYSTEM;
        if (Child->NtfsAttribs & NTFS_FILE_TYPE_ARCHIVE)   Info->Attribute |= EFI_FILE_ARCHIVE;
        if (Child->IsDirectory)                             Info->Attribute |= EFI_FILE_DIRECTORY;

        CopyMem (Info->FileName, Child->FileName,
                 (Child->FileNameChars + 1) * sizeof (CHAR16));

        *BufferSize = InfoSize;
        F->DirEnumEntry = StartEntry;   /* advance to next entry              */
        FreePool (Child);
        return EFI_SUCCESS;
    }
}

/* =========================================================================
 * Section 18 – EFI_FILE_PROTOCOL: Write / SetPosition / GetPosition
 * ========================================================================= */

static EFI_STATUS EFIAPI
NtfsEfiWrite (
    IN     EFI_FILE_PROTOCOL *This,
    IN OUT UINTN             *BufferSize,
    IN     VOID              *Buffer
    )
{
    (VOID)This; (VOID)Buffer;
    *BufferSize = 0;
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI
NtfsEfiGetPosition (
    IN  EFI_FILE_PROTOCOL *This,
    OUT UINT64            *Position
    )
{
    PNTFS_EFI_FILE F = (PNTFS_EFI_FILE)This;
    *Position = F->IsDirectory ? F->DirEnumEntry : F->Position;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
NtfsEfiSetPosition (
    IN EFI_FILE_PROTOCOL *This,
    IN UINT64             Position
    )
{
    PNTFS_EFI_FILE F = (PNTFS_EFI_FILE)This;
    if (F->IsDirectory) {
        /* EFI spec: 0 rewinds directory enumeration */
        F->DirEnumEntry = (ULONG)(Position == 0 ? 0 : Position);
    } else {
        F->Position = (Position == 0xFFFFFFFFFFFFFFFFULL)
                      ? F->FileSize : Position;
    }
    return EFI_SUCCESS;
}

/* =========================================================================
 * Section 19 – EFI_FILE_PROTOCOL: GetInfo / SetInfo / Flush
 * ========================================================================= */

static EFI_STATUS EFIAPI
NtfsEfiGetInfo (
    IN  EFI_FILE_PROTOCOL *This,
    IN  EFI_GUID          *InformationTypeGuid,
    IN  OUT UINTN         *BufferSize,
    OUT VOID              *Buffer
    )
{
    PNTFS_EFI_FILE F   = (PNTFS_EFI_FILE)This;
    PNTFS_EFI_VCB  Vcb = F->Vcb;

    if (CompareGuid (InformationTypeGuid, &gEfiFileInfoGuid)) {
        EFI_FILE_INFO *Info;
        UINTN          Needed = SIZE_OF_EFI_FILE_INFO
                                + (F->FileNameChars + 1) * sizeof (CHAR16);
        if (*BufferSize < Needed) { *BufferSize = Needed; return EFI_BUFFER_TOO_SMALL; }

        Info = (EFI_FILE_INFO*)Buffer;
        ZeroMem (Info, Needed);
        Info->Size         = Needed;
        Info->FileSize     = F->FileSize;
        Info->PhysicalSize = F->AllocSize;
        NtfsEfiConvertTime (F->CreationTime,   &Info->CreateTime);
        NtfsEfiConvertTime (F->LastAccessTime, &Info->LastAccessTime);
        NtfsEfiConvertTime (F->LastWriteTime,  &Info->ModificationTime);

        if (F->NtfsAttribs & NTFS_FILE_TYPE_READ_ONLY) Info->Attribute |= EFI_FILE_READ_ONLY;
        if (F->NtfsAttribs & NTFS_FILE_TYPE_HIDDEN)    Info->Attribute |= EFI_FILE_HIDDEN;
        if (F->NtfsAttribs & NTFS_FILE_TYPE_SYSTEM)    Info->Attribute |= EFI_FILE_SYSTEM;
        if (F->NtfsAttribs & NTFS_FILE_TYPE_ARCHIVE)   Info->Attribute |= EFI_FILE_ARCHIVE;
        if (F->IsDirectory)                             Info->Attribute |= EFI_FILE_DIRECTORY;

        CopyMem (Info->FileName, F->FileName, (F->FileNameChars + 1) * sizeof (CHAR16));
        *BufferSize = Needed;
        return EFI_SUCCESS;

    } else if (CompareGuid (InformationTypeGuid, &gEfiFileSystemInfoGuid)) {
        EFI_FILE_SYSTEM_INFO *SysInfo;
        UINTN                 Needed = SIZE_OF_EFI_FILE_SYSTEM_INFO
                                       + (Vcb->VolumeLabelLen + 1) * sizeof (CHAR16);
        if (*BufferSize < Needed) { *BufferSize = Needed; return EFI_BUFFER_TOO_SMALL; }

        SysInfo = (EFI_FILE_SYSTEM_INFO*)Buffer;
        ZeroMem (SysInfo, Needed);
        SysInfo->Size         = Needed;
        SysInfo->ReadOnly     = TRUE;
        SysInfo->BlockSize    = Vcb->BytesPerCluster;
        CopyMem (SysInfo->VolumeLabel, Vcb->VolumeLabel,
                 (Vcb->VolumeLabelLen + 1) * sizeof (CHAR16));
        *BufferSize = Needed;
        return EFI_SUCCESS;

    } else if (CompareGuid (InformationTypeGuid, &gEfiFileSystemVolumeLabelInfoIdGuid)) {
        EFI_FILE_SYSTEM_VOLUME_LABEL *Label;
        UINTN Needed = SIZE_OF_EFI_FILE_SYSTEM_VOLUME_LABEL
                       + (Vcb->VolumeLabelLen + 1) * sizeof (CHAR16);
        if (*BufferSize < Needed) { *BufferSize = Needed; return EFI_BUFFER_TOO_SMALL; }

        Label = (EFI_FILE_SYSTEM_VOLUME_LABEL*)Buffer;
        CopyMem (Label->VolumeLabel, Vcb->VolumeLabel,
                 (Vcb->VolumeLabelLen + 1) * sizeof (CHAR16));
        *BufferSize = Needed;
        return EFI_SUCCESS;
    }

    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
NtfsEfiSetInfo (
    IN EFI_FILE_PROTOCOL *This,
    IN EFI_GUID          *InformationTypeGuid,
    IN UINTN              BufferSize,
    IN VOID              *Buffer
    )
{
    (VOID)This; (VOID)InformationTypeGuid; (VOID)BufferSize; (VOID)Buffer;
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI
NtfsEfiFlush (IN EFI_FILE_PROTOCOL *This)
{
    (VOID)This;
    return EFI_SUCCESS;   /* read-only: nothing to flush */
}

/* =========================================================================
 * Section 20 – EFI_SIMPLE_FILE_SYSTEM_PROTOCOL: OpenVolume
 * ========================================================================= */

static VOID
NtfsEfiInitProtoTemplate (VOID)
{
    g_FileProtoTemplate.Revision    = EFI_FILE_PROTOCOL_REVISION;
    g_FileProtoTemplate.Open        = NtfsEfiOpen;
    g_FileProtoTemplate.Close       = NtfsEfiClose;
    g_FileProtoTemplate.Delete      = NtfsEfiDelete;
    g_FileProtoTemplate.Read        = NtfsEfiRead;
    g_FileProtoTemplate.Write       = NtfsEfiWrite;
    g_FileProtoTemplate.GetPosition = NtfsEfiGetPosition;
    g_FileProtoTemplate.SetPosition = NtfsEfiSetPosition;
    g_FileProtoTemplate.GetInfo     = NtfsEfiGetInfo;
    g_FileProtoTemplate.SetInfo     = NtfsEfiSetInfo;
    g_FileProtoTemplate.Flush       = NtfsEfiFlush;
    g_FileProtoTemplate.OpenEx      = NULL;
    g_FileProtoTemplate.ReadEx      = NULL;
    g_FileProtoTemplate.WriteEx     = NULL;
    g_FileProtoTemplate.FlushEx     = NULL;
}

EFI_STATUS EFIAPI
NtfsEfiOpenVolume (
    IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *This,
    OUT EFI_FILE_PROTOCOL               **Root
    )
{
    /*
     * The EFI_SIMPLE_FILE_SYSTEM_PROTOCOL instance is embedded inside
     * NTFS_EFI_VCB. Cast using the containing-record trick.
     */
    PNTFS_EFI_VCB        Vcb = NtfsEfiVcbFromSfsp (This);
    PNTFS_EFI_FILE       RootHandle;

    NtfsEfiInitProtoTemplate ();

    RootHandle = NtfsEfiCreateHandle (Vcb, NTFS_FILE_ROOT);
    if (RootHandle == NULL) return EFI_OUT_OF_RESOURCES;

    /* Ensure the root handle has a proper display name */
    RootHandle->FileName[0]  = L'\\';
    RootHandle->FileName[1]  = L'\0';
    RootHandle->FileNameChars = 1;
    RootHandle->IsDirectory   = TRUE;

    *Root = &RootHandle->Protocol;
    return EFI_SUCCESS;
}

/* =========================================================================
 * Section 21 – Volume mount (called from DriverBinding.Start)
 * ========================================================================= */

/*
 * Allocate and initialise a NTFS_EFI_VCB from DiskIo + BlockIo.
 * Returns NULL on failure.
 *
 */
PNTFS_EFI_VCB
NtfsEfiMountVolume (
    IN EFI_DISK_IO_PROTOCOL  *DiskIo,
    IN EFI_BLOCK_IO_PROTOCOL *BlockIo
    )
{
    NTFS_BOOT_SECTOR  Boot;
    PNTFS_EFI_VCB     Vcb = NULL;
    PFILE_RECORD_HEADER MftRec = NULL;
    PNTFS_ATTR_CTX    DataCtx = NULL;
    LONGLONG          ClustersPerMftRecord;
    ULONG             MftDataOffset;
    EFI_STATUS        Status;

    /* read the boot sector (LBA 0 = byte 0) */
    Status = DiskIo->ReadDisk (DiskIo, BlockIo->Media->MediaId,
                                0, sizeof (NTFS_BOOT_SECTOR), &Boot);
    if (EFI_ERROR (Status)) return NULL;

    /* minimal NTFS signature check */
    if (CompareMem (Boot.OEMID, "NTFS    ", 8) != 0) return NULL;

    Vcb = AllocateZeroPool (sizeof (NTFS_EFI_VCB));
    if (Vcb == NULL) return NULL;

    Vcb->DiskIo  = DiskIo;
    Vcb->BlockIo = BlockIo;
    Vcb->MediaId = BlockIo->Media->MediaId;

    /* parse geometry */
    Vcb->BytesPerSector  = Boot.BPB.BytesPerSector;
    Vcb->SectorsPerCluster = Boot.BPB.SectorsPerCluster;
    Vcb->BytesPerCluster   = Vcb->BytesPerSector * Vcb->SectorsPerCluster;
    Vcb->SerialNumber      = Boot.EBPB.SerialNumber;

    ClustersPerMftRecord = Boot.EBPB.ClustersPerMftRecord;
    if (ClustersPerMftRecord < 0) {
        /* negative → 2^|ClustersPerMftRecord| bytes */
        Vcb->BytesPerFileRecord = 1U << (ULONG)(-ClustersPerMftRecord);
    } else {
        Vcb->BytesPerFileRecord = (ULONG)ClustersPerMftRecord * Vcb->BytesPerCluster;
    }

    {
        LONGLONG ClustersPerIndex = Boot.EBPB.ClustersPerIndexRecord;
        if (ClustersPerIndex < 0) {
            Vcb->BytesPerIndexRecord = 1U << (ULONG)(-ClustersPerIndex);
        } else {
            Vcb->BytesPerIndexRecord = (ULONG)ClustersPerIndex * Vcb->BytesPerCluster;
        }
    }

    /* ── bootstrap $MFT ─────────────────────────────────────────────────── */

    /*
     * The first file record of $MFT is at a fixed disk location.
     * We build a minimal attribute context for it manually so
     * NtfsEfiReadFileRecord() can use ReadAttr to load further records.
     */
    MftRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (MftRec == NULL) { FreePool (Vcb); return NULL; }

    {
        UINT64 MftByteOffset = Boot.EBPB.MftLocation * Vcb->BytesPerCluster;
        Status = DiskIo->ReadDisk (DiskIo, Vcb->MediaId,
                                    MftByteOffset,
                                    Vcb->BytesPerFileRecord,
                                    MftRec);
        if (EFI_ERROR (Status)) goto Fail;
        if (EFI_ERROR (NtfsEfiFixupRecord (Vcb, &MftRec->Ntfs))) goto Fail;
    }

    /* find the unnamed $DATA attribute of $MFT */
    DataCtx = NtfsEfiFindAttrInRecord (Vcb, MftRec, AttributeData, NULL, 0, &MftDataOffset);
    if (DataCtx == NULL) goto Fail;
    DataCtx->FileMFTIndex = NTFS_FILE_MFT;

    Vcb->MasterFileTable = MftRec;
    Vcb->MFTContext      = DataCtx;
    Vcb->MftDataOffset   = MftDataOffset;

    /* ── load volume name ──────────────────────────────────────────────── */
    {
        PFILE_RECORD_HEADER VolumeRec = AllocatePool (Vcb->BytesPerFileRecord);
        if (VolumeRec != NULL) {
            if (!EFI_ERROR (NtfsEfiReadFileRecord (Vcb, 3ULL /* $Volume */, VolumeRec))) {
                PNTFS_ATTR_CTX VnCtx = NtfsEfiFindAttribute (Vcb, VolumeRec,
                                            AttributeVolumeName, NULL, 0, NULL);
                if (VnCtx != NULL) {
                    ULONG LabelBytes = (ULONG)NtfsEfiAttrDataLength (VnCtx);
                    ULONG LabelChars = LabelBytes / sizeof (WCHAR);
                    if (LabelChars > 127) LabelChars = 127;
                    NtfsEfiReadAttr (Vcb, VnCtx, 0, (PCHAR)Vcb->VolumeLabel, LabelBytes);
                    Vcb->VolumeLabel[LabelChars] = L'\0';
                    Vcb->VolumeLabelLen = (USHORT)LabelChars;
                    NtfsEfiFreeAttrCtx (VnCtx);
                }
            }
            FreePool (VolumeRec);
        }
    }

    /* install EFI_SIMPLE_FILE_SYSTEM_PROTOCOL */
    Vcb->Sfsp.Revision  = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    Vcb->Sfsp.OpenVolume = NtfsEfiOpenVolume;

    return Vcb;

Fail:
    if (DataCtx != NULL) {
        NtfsEfiFreeAttrCtx (DataCtx);
    }
    if (MftRec != NULL) {
        FreePool (MftRec);
    }
    if (Vcb != NULL) {
        FreePool (Vcb);
    }
    return NULL;
}

/* =========================================================================
 * Section 22 – public accessor
 * ========================================================================= */

EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *
NtfsEfiGetSfsp (IN PNTFS_EFI_VCB Vcb)
{
    return &Vcb->Sfsp;
}

/* =========================================================================
 * Section 23 – EFI_DRIVER_BINDING_PROTOCOL
 * ========================================================================= */

static EFI_STATUS EFIAPI NtfsEfiBindingSupported (
    IN EFI_DRIVER_BINDING_PROTOCOL *This,
    IN EFI_HANDLE                   ControllerHandle,
    IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
    );

static EFI_STATUS EFIAPI NtfsEfiBindingStart (
    IN EFI_DRIVER_BINDING_PROTOCOL *This,
    IN EFI_HANDLE                   ControllerHandle,
    IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
    );

static EFI_STATUS EFIAPI NtfsEfiBindingStop (
    IN EFI_DRIVER_BINDING_PROTOCOL *This,
    IN EFI_HANDLE                   ControllerHandle,
    IN UINTN                        NumberOfChildren,
    IN EFI_HANDLE                  *ChildHandleBuffer OPTIONAL
    );

static EFI_DRIVER_BINDING_PROTOCOL gNtfsDriverBinding = {
    NtfsEfiBindingSupported,
    NtfsEfiBindingStart,
    NtfsEfiBindingStop,
    0x10,
    NULL,
    NULL
};

static BOOLEAN
NtfsEfiIsNtfsVolume (
    IN EFI_DISK_IO_PROTOCOL  *DiskIo,
    IN EFI_BLOCK_IO_PROTOCOL *BlockIo
    )
{
    NTFS_BOOT_SECTOR Boot;

    if (DiskIo == NULL || BlockIo == NULL || BlockIo->Media == NULL) {
        return FALSE;
    }
    if (!BlockIo->Media->MediaPresent) {
        return FALSE;
    }

    if (EFI_ERROR (DiskIo->ReadDisk (
            DiskIo,
            BlockIo->Media->MediaId,
            0,
            sizeof (Boot),
            &Boot)))
    {
        return FALSE;
    }

    return (BOOLEAN)(CompareMem (Boot.OEMID, "NTFS    ", 8) == 0);
}

static EFI_STATUS EFIAPI
NtfsEfiBindingSupported (
    IN EFI_DRIVER_BINDING_PROTOCOL *This,
    IN EFI_HANDLE                   ControllerHandle,
    IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
    )
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfsp;
    EFI_DISK_IO_PROTOCOL            *DiskIo;
    EFI_BLOCK_IO_PROTOCOL           *BlockIo;
    EFI_STATUS                       Status;

    (VOID)This;
    (VOID)RemainingDevicePath;

    Status = gBS->HandleProtocol (
                    ControllerHandle,
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Sfsp
                    );
    if (!EFI_ERROR (Status) && Sfsp->OpenVolume == NtfsEfiOpenVolume) {
        return EFI_ALREADY_STARTED;
    }

    Status = gBS->HandleProtocol (
                    ControllerHandle,
                    &gEfiDiskIoProtocolGuid,
                    (VOID **)&DiskIo
                    );
    if (EFI_ERROR (Status)) {
        return Status;
    }

    Status = gBS->HandleProtocol (
                    ControllerHandle,
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo
                    );
    if (EFI_ERROR (Status)) {
        return Status;
    }

    return NtfsEfiIsNtfsVolume (DiskIo, BlockIo) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
NtfsEfiBindingStart (
    IN EFI_DRIVER_BINDING_PROTOCOL *This,
    IN EFI_HANDLE                   ControllerHandle,
    IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
    )
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfsp;
    EFI_DISK_IO_PROTOCOL            *DiskIo;
    EFI_BLOCK_IO_PROTOCOL           *BlockIo;
    PNTFS_EFI_VCB                    Vcb;
    EFI_STATUS                       Status;

    (VOID)RemainingDevicePath;

    Status = gBS->HandleProtocol (
                    ControllerHandle,
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Sfsp
                    );
    if (!EFI_ERROR (Status) && Sfsp->OpenVolume == NtfsEfiOpenVolume) {
        return EFI_ALREADY_STARTED;
    }

    Status = gBS->OpenProtocol (
                    ControllerHandle,
                    &gEfiDiskIoProtocolGuid,
                    (VOID **)&DiskIo,
                    This->DriverBindingHandle,
                    ControllerHandle,
                    EFI_OPEN_PROTOCOL_BY_DRIVER
                    );
    if (EFI_ERROR (Status)) {
        return Status;
    }

    Status = gBS->OpenProtocol (
                    ControllerHandle,
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo,
                    This->DriverBindingHandle,
                    ControllerHandle,
                    EFI_OPEN_PROTOCOL_BY_DRIVER
                    );
    if (EFI_ERROR (Status)) {
        gBS->CloseProtocol (
               ControllerHandle,
               &gEfiDiskIoProtocolGuid,
               This->DriverBindingHandle,
               ControllerHandle
               );
        return Status;
    }

    Vcb = NtfsEfiMountVolume (DiskIo, BlockIo);
    if (Vcb == NULL) {
        gBS->CloseProtocol (
               ControllerHandle,
               &gEfiBlockIoProtocolGuid,
               This->DriverBindingHandle,
               ControllerHandle
               );
        gBS->CloseProtocol (
               ControllerHandle,
               &gEfiDiskIoProtocolGuid,
               This->DriverBindingHandle,
               ControllerHandle
               );
        return EFI_UNSUPPORTED;
    }

    Status = gBS->InstallMultipleProtocolInterfaces (
                    &ControllerHandle,
                    &gEfiSimpleFileSystemProtocolGuid,
                    &Vcb->Sfsp,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
        NtfsEfiUnmountVolume (Vcb);
        gBS->CloseProtocol (
               ControllerHandle,
               &gEfiBlockIoProtocolGuid,
               This->DriverBindingHandle,
               ControllerHandle
               );
        gBS->CloseProtocol (
               ControllerHandle,
               &gEfiDiskIoProtocolGuid,
               This->DriverBindingHandle,
               ControllerHandle
               );
    }

    return Status;
}

static EFI_STATUS EFIAPI
NtfsEfiBindingStop (
    IN EFI_DRIVER_BINDING_PROTOCOL *This,
    IN EFI_HANDLE                   ControllerHandle,
    IN UINTN                        NumberOfChildren,
    IN EFI_HANDLE                  *ChildHandleBuffer OPTIONAL
    )
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfsp;
    PNTFS_EFI_VCB                    Vcb;
    EFI_STATUS                       Status;

    (VOID)NumberOfChildren;
    (VOID)ChildHandleBuffer;

    Status = gBS->HandleProtocol (
                    ControllerHandle,
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Sfsp
                    );
    if (EFI_ERROR (Status) || Sfsp->OpenVolume != NtfsEfiOpenVolume) {
        return EFI_SUCCESS;
    }

    Vcb = NtfsEfiVcbFromSfsp (Sfsp);

    Status = gBS->UninstallMultipleProtocolInterfaces (
                    ControllerHandle,
                    &gEfiSimpleFileSystemProtocolGuid,
                    Sfsp,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
        return Status;
    }

    gBS->CloseProtocol (
           ControllerHandle,
           &gEfiBlockIoProtocolGuid,
           This->DriverBindingHandle,
           ControllerHandle
           );
    gBS->CloseProtocol (
           ControllerHandle,
           &gEfiDiskIoProtocolGuid,
           This->DriverBindingHandle,
           ControllerHandle
           );

    NtfsEfiUnmountVolume (Vcb);
    return EFI_SUCCESS;
}

/* =========================================================================
 * Section 24 – module entry point
 * ========================================================================= */

EFI_STATUS EFIAPI
UefiMain (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    (VOID)SystemTable;

    gNtfsDriverBinding.ImageHandle = ImageHandle;
    gNtfsDriverBinding.DriverBindingHandle = ImageHandle;

    return gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiDriverBindingProtocolGuid,
                  &gNtfsDriverBinding,
                  NULL
                  );
}
