/**
 * ntfs_file.c - open-file handle factory, path lookup, and the
 * EFI_FILE_PROTOCOL method implementations (Open/Close/Delete/Read/Write/
 * GetPosition/SetPosition/GetInfo/SetInfo/Flush).
 */

#include "ntfs.h"

/* Preallocation quantum for non-resident growth. 1 MB minimised realloc calls
 * but bloated allocation ~2.2x on a big real-file copy (per-file slack that the
 * on-close trim did not fully reclaim at scale). 64 KB matches the typical copy
 * chunk, keeps slack tiny, and the run-merging append keeps large contiguous
 * files at one run regardless. 256 KB balances slack (~1.4x allocation) against
 * grow-call count (4x fewer than 64 KB). */
#define NTFS_WRITE_PREALLOC_BYTES (256U * 1024U)

static EFI_FILE_PROTOCOL g_FileProtoTemplate;  /* filled once at OpenVolume */

static EFI_STATUS EFIAPI NtfsEfiOpen (IN EFI_FILE_PROTOCOL *This, OUT EFI_FILE_PROTOCOL **NewHandle,
                    IN CHAR16 *FileName, IN UINT64 OpenMode, IN UINT64 Attributes);
static EFI_STATUS EFIAPI NtfsEfiClose (IN EFI_FILE_PROTOCOL *This);
static EFI_STATUS EFIAPI NtfsEfiDelete (IN EFI_FILE_PROTOCOL *This);
static EFI_STATUS EFIAPI NtfsEfiRead (IN EFI_FILE_PROTOCOL *This, IN OUT UINTN *BufferSize, OUT VOID *Buffer);
static EFI_STATUS EFIAPI NtfsEfiWrite (IN EFI_FILE_PROTOCOL *This, IN OUT UINTN *BufferSize, IN VOID *Buffer);
static EFI_STATUS EFIAPI NtfsEfiGetPosition (IN EFI_FILE_PROTOCOL *This, OUT UINT64 *Position);
static EFI_STATUS EFIAPI NtfsEfiSetPosition (IN EFI_FILE_PROTOCOL *This, IN UINT64 Position);
static EFI_STATUS EFIAPI NtfsEfiGetInfo (IN EFI_FILE_PROTOCOL *This, IN EFI_GUID *InformationTypeGuid,
                    IN OUT UINTN *BufferSize, OUT VOID *Buffer);
static EFI_STATUS EFIAPI NtfsEfiSetInfo (IN EFI_FILE_PROTOCOL *This, IN EFI_GUID *InformationTypeGuid,
                    IN UINTN BufferSize, IN VOID *Buffer);
static EFI_STATUS EFIAPI NtfsEfiFlush (IN EFI_FILE_PROTOCOL *This);

/* =========================================================================
 * Handle factory
 * ========================================================================= */

PNTFS_EFI_FILE
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
 * Path lookup
 * ========================================================================= */

/*
 * Split L"foo\bar\baz" into component L"foo" (Comp) and remainder L"bar\baz" (Rest).
 * EFI paths use backslash, but callers sometimes pass shell-style slash; treat
 * both as separators at the protocol boundary.
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
    while (Path[i] == L'\\' || Path[i] == L'/') i++;    /* skip leading separators */
    if (Path[i] == L'\0') return FALSE;
    *Comp = Path + i;
    while (Path[i] != L'\0' && Path[i] != L'\\' && Path[i] != L'/') i++;
    *CompLen = (Path + i) - *Comp;
    while (Path[i] == L'\\' || Path[i] == L'/') i++;    /* skip trailing separators */
    *Rest = Path + i;
    return TRUE;
}

#define NTFS_MAX_SYMLINK_HOPS 32

/* Parent directory MFT of a file, read from its own $FILE_NAME. Root's parent
 * is itself. Returns (ULONGLONG)-1 on failure. */
static ULONGLONG
NtfsEfiParentOf (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex
    )
{
    PFILE_RECORD_HEADER Rec;
    PNTFS_ATTR_CTX      FnCtx;
    ULONG               FnLen;
    PFILENAME_ATTRIBUTE FnAttr;
    ULONGLONG           Parent = (ULONGLONG)-1LL;

    if (MFTIndex == NTFS_FILE_ROOT) return NTFS_FILE_ROOT;

    Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) return (ULONGLONG)-1LL;
    if (!EFI_ERROR (NtfsEfiReadFileRecord (Vcb, MFTIndex, Rec))) {
        FnCtx = NtfsEfiFindAttribute (Vcb, Rec, AttributeFileName, NULL, 0, NULL);
        if (FnCtx != NULL) {
            FnLen = (ULONG)NtfsEfiAttrDataLength (FnCtx);
            FnAttr = AllocatePool (FnLen);
            if (FnAttr != NULL) {
                if (NtfsEfiReadAttr (Vcb, FnCtx, 0, (PCHAR)FnAttr, FnLen) == FnLen) {
                    Parent = FnAttr->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
                }
                FreePool (FnAttr);
            }
            NtfsEfiFreeAttrCtx (FnCtx);
        }
    }
    FreePool (Rec);
    return Parent;
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
    WCHAR        WorkBuf[NTFS_MAX_PATH_CHARS];
    UINTN        SymlinkHops = 0;

    /* mutable working copy: a symlink hit below splices its target (+ the
     * remaining path) back into this same buffer and the loop continues */
    {
        UINTN Len = StrLen (Path);
        if (Len >= NTFS_MAX_PATH_CHARS) Len = NTFS_MAX_PATH_CHARS - 1;
        CopyMem (WorkBuf, Path, Len * sizeof (WCHAR));
        WorkBuf[Len] = L'\0';
    }
    Path = WorkBuf;

    while (NtfsEfiSplitPath (Path, &Comp, &CompLen, &Rest)) {
        ULONG      Dummy = 0;
        ULONGLONG  Next;
        CONST WCHAR *Target;
        BOOLEAN    IsRelative;

        if (CompLen == 1 && Comp[0] == L'.') {
            Path = Rest;
            continue;
        }
        if (CompLen == 2 && Comp[0] == L'.' && Comp[1] == L'.') {
            Cur = NtfsEfiParentOf (Vcb, Cur);
            if (Cur == (ULONGLONG)-1LL) return (ULONGLONG)-1LL;
            Path = Rest;
            continue;
        }

        /*
         * Try an exact-case match first, then fall back to case-
         * insensitive. NTFS's per-directory "case-sensitive directories"
         * flag (added for WSL) postdates the reference NT source
         * available here, so we don't know which mode a given directory
         * is in - but exact-case-first is correct regardless: in a
         * case-sensitive directory with several case variants of a name,
         * only an exact match resolves it unambiguously anyway; in a
         * case-insensitive directory there is only one entry per
         * normalized name, so the result is identical either way.
         */
        Next = NtfsEfiFindInDirectory (Vcb, Cur, Comp, CompLen, FALSE, TRUE, &Dummy);
        if (Next == (ULONGLONG)-1LL && !CaseSensitive) {
            Next = NtfsEfiFindInDirectory (Vcb, Cur, Comp, CompLen, FALSE, FALSE, &Dummy);
        }
        if (Next == (ULONGLONG)-1LL) return (ULONGLONG)-1LL;

        if (NtfsEfiTryResolveSymlink (Vcb, Next, &Target, &IsRelative)) {
            WCHAR NewBuf[NTFS_MAX_PATH_CHARS];
            UINTN TLen, RLen, Pos;

            if (++SymlinkHops > NTFS_MAX_SYMLINK_HOPS) return (ULONGLONG)-1LL;

            TLen = StrLen (Target);
            RLen = StrLen (Rest);
            if (TLen >= NTFS_MAX_PATH_CHARS) return (ULONGLONG)-1LL;
            CopyMem (NewBuf, Target, TLen * sizeof (WCHAR));
            Pos = TLen;
            if (RLen > 0) {
                if (Pos + 1 + RLen >= NTFS_MAX_PATH_CHARS) return (ULONGLONG)-1LL;
                NewBuf[Pos++] = L'\\';
                CopyMem (NewBuf + Pos, Rest, RLen * sizeof (WCHAR));
                Pos += RLen;
            }
            NewBuf[Pos] = L'\0';
            CopyMem (WorkBuf, NewBuf, (Pos + 1) * sizeof (WCHAR));

            /* absolute target -> resume from root; relative -> stays
             * anchored at Cur (the symlink's own containing directory) */
            if (!IsRelative) Cur = NTFS_FILE_ROOT;
            Path = WorkBuf;
            continue;
        }

        Cur  = Next;
        Path = Rest;
    }
    return Cur;
}

/* =========================================================================
 * EFI_FILE_PROTOCOL: Open
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

    Print (L"[ntfs] Open: Parent->MFTIndex=%ld FileName='%s' OpenMode=%lx\n",
        Parent->MFTIndex, FileName ? FileName : L"(null)", OpenMode);

    if (Path == NULL || NewHandle == NULL) return EFI_INVALID_PARAMETER;
    *NewHandle = NULL;

    /* resolve starting point */
    if (Path[0] == L'\\' || Path[0] == L'/') {
        StartMFT = NTFS_FILE_ROOT;
        while (*Path == L'\\' || *Path == L'/') Path++;
    } else {
        StartMFT = Parent->MFTIndex;
    }

    /* Empty path and "." open the current handle target. */
    if (Path[0] == L'\0' || (Path[0] == L'.' && Path[1] == L'\0')) {
        TargetMFT = Parent->MFTIndex;
    } else if (OpenMode & EFI_FILE_MODE_CREATE) {
        /*
         * Single-component create only for now: the caller must already
         * have the containing directory open/resolved (no multi-level
         * "create the whole path" support, no directory creation). EFI
         * semantics: CREATE means create-if-missing, open normally if it
         * already exists - $Bitmap/MFT-record/index-insert machinery
         * (ntfs_create.c) only runs on an actual miss.
         */
        UINTN i;
        for (i = 0; Path[i] != L'\0'; i++) {
            if (Path[i] == L'\\' || Path[i] == L'/') return EFI_UNSUPPORTED; /* deep create: later step */
        }
        if (i == 0) return EFI_INVALID_PARAMETER;

        TargetMFT = NtfsEfiLookupPath (Vcb, StartMFT, Path, FALSE);
        if (TargetMFT == (ULONGLONG)-1LL) {
            ULONGLONG  NewIdx;
            EFI_STATUS CrStatus = NtfsEfiCreateFile (Vcb, StartMFT, Path, i,
                                      (Attributes & EFI_FILE_DIRECTORY) != 0,
                                      &NewIdx);
            if (EFI_ERROR (CrStatus)) return CrStatus;
            TargetMFT = NewIdx;
        }
    } else {
        TargetMFT = NtfsEfiLookupPath (Vcb, StartMFT, Path, FALSE);
        if (TargetMFT == (ULONGLONG)-1LL) return EFI_NOT_FOUND;
    }

    Print (L"[ntfs] Open: resolved TargetMFT=%ld\n", TargetMFT);
    Handle = NtfsEfiCreateHandle (Vcb, TargetMFT);
    if (Handle == NULL) return EFI_OUT_OF_RESOURCES;

    Handle->OpenForWrite = (OpenMode & EFI_FILE_MODE_WRITE) != 0;

    *NewHandle = &Handle->Protocol;
    return EFI_SUCCESS;
}

/* =========================================================================
 * EFI_FILE_PROTOCOL: Close / Delete
 * ========================================================================= */

static EFI_STATUS EFIAPI
NtfsEfiClose (IN EFI_FILE_PROTOCOL *This)
{
    PNTFS_EFI_FILE F = (PNTFS_EFI_FILE)This;

    /* If this open grew the file, its $DATA was over-allocated to a prealloc
     * quantum; release the slack now (NTFS.sys does the same at cleanup). Best
     * effort - a failure here only leaves harmless extra allocation. */
    if (F->DidGrow)
        NtfsEfiTrimAllocation (F->Vcb, F->MFTIndex);

    if (F->DirCache != NULL) FreePool (F->DirCache);
    FreePool (This);   /* NTFS_EFI_FILE is a single flat allocation */
    return EFI_SUCCESS;
}

/*
 * EFI spec: Delete() always closes the handle, even when the delete itself
 * fails (then it returns EFI_WARN_DELETE_FAILURE instead of EFI_SUCCESS).
 * The actual on-disk work lives in ntfs_delete.c.
 */
static EFI_STATUS EFIAPI
NtfsEfiDelete (IN EFI_FILE_PROTOCOL *This)
{
    PNTFS_EFI_FILE F      = (PNTFS_EFI_FILE)This;
    PNTFS_EFI_VCB  Vcb    = F->Vcb;
    ULONGLONG      MFT    = F->MFTIndex;
    BOOLEAN        CanWrite = F->OpenForWrite;
    EFI_STATUS     Status;

    if (!CanWrite) {
        FreePool (This);
        return EFI_WARN_DELETE_FAILURE;
    }

    Status = NtfsEfiDeleteFile (Vcb, MFT);
    FreePool (This);
    return EFI_ERROR (Status) ? EFI_WARN_DELETE_FAILURE : EFI_SUCCESS;
}

/* =========================================================================
 * EFI_FILE_PROTOCOL: Read
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
        /* -- regular file read --------------------------------------- */
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

        Print (L"[ntfs] Read: MFTIndex=%ld FileSize=%ld Position=%ld ToRead=%d IsNonResident=%d ValueLen/DataSize=%ld\n",
            F->MFTIndex, F->FileSize, F->Position, ToRead, DataCtx->pRecord->IsNonResident,
            DataCtx->pRecord->IsNonResident ? (UINT64)DataCtx->pRecord->NonResident.DataSize
                                            : (UINT64)DataCtx->pRecord->Resident.ValueLength);

        /*
         * LZNT1-compressed data is stored on disk in per-compression-unit
         * chunks that NtfsEfiReadAttr()'s plain run-list walk cannot
         * interpret directly (see ntfs_lznt1.c, ported from the real
         * base\ntos\rtl\lznt1.c).
         *
         * WOF/system-compression is a DIFFERENT scheme (real bytes live in
         * an alternate ":$WofCompressedData" stream selected by a reparse
         * point; XPRESS/LZX codec, not LZNT1) and is not handled here. Its
         * $DATA attribute is a red herring: CompressionUnit is set exactly
         * like classic-compressed files, but the *entire* run list is one
         * single all-sparse (LBN==-1) run spanning the whole file - there
         * is no real data in $DATA at all. Detect that specific shape and
         * fail cleanly instead of silently handing back all-zero content.
         */
        if (DataCtx->pRecord->IsNonResident && DataCtx->pRecord->NonResident.CompressionUnit != 0) {
            if (DataCtx->RunCount == 1 && DataCtx->Runs[0].LBN == -1LL) {
                NtfsEfiFreeAttrCtx (DataCtx);
                return EFI_UNSUPPORTED;
            }
            Read = NtfsEfiReadCompressedAttr (Vcb, DataCtx, F->Position, (PCHAR)Buffer, ToRead);
        } else {
            Read = NtfsEfiReadAttr (Vcb, DataCtx, F->Position, (PCHAR)Buffer, ToRead);
        }
        NtfsEfiFreeAttrCtx (DataCtx);

        F->Position += Read;
        *BufferSize  = Read;
        return EFI_SUCCESS;

    } else {
        /* -- directory read: one EFI_FILE_INFO per call, served O(1) from a
         * full in-order cache built on first call (was O(n^2) per-entry rescan). */
        ULONGLONG       ChildMFT;
        PNTFS_EFI_FILE  Child;
        EFI_FILE_INFO  *Info;
        UINTN           InfoSize;

        if (!F->DirCacheBuilt) {
            #define NTFS_DIR_CACHE_MAX (256U * 1024U)
            F->DirCache = AllocatePool (NTFS_DIR_CACHE_MAX * sizeof (ULONGLONG));
            F->DirCacheCount = F->DirCache ?
                NtfsEfiCollectDir (Vcb, F->MFTIndex, F->DirCache, NTFS_DIR_CACHE_MAX) : 0;
            F->DirCacheBuilt = TRUE;
        }
        if (F->DirCache == NULL || F->DirEnumEntry >= F->DirCacheCount) {
            *BufferSize = 0;
            return EFI_SUCCESS;
        }
        ChildMFT = F->DirCache[F->DirEnumEntry];

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

        /* map NTFS attributes -> EFI attributes */
        if (Child->NtfsAttribs & NTFS_FILE_TYPE_READ_ONLY) Info->Attribute |= EFI_FILE_READ_ONLY;
        if (Child->NtfsAttribs & NTFS_FILE_TYPE_HIDDEN)    Info->Attribute |= EFI_FILE_HIDDEN;
        if (Child->NtfsAttribs & NTFS_FILE_TYPE_SYSTEM)    Info->Attribute |= EFI_FILE_SYSTEM;
        if (Child->NtfsAttribs & NTFS_FILE_TYPE_ARCHIVE)   Info->Attribute |= EFI_FILE_ARCHIVE;
        if (Child->IsDirectory)                             Info->Attribute |= EFI_FILE_DIRECTORY;

        CopyMem (Info->FileName, Child->FileName,
                 (Child->FileNameChars + 1) * sizeof (CHAR16));

        *BufferSize = InfoSize;
        F->DirEnumEntry++;              /* advance to next cached entry        */
        FreePool (Child);
        return EFI_SUCCESS;
    }
}

/* =========================================================================
 * EFI_FILE_PROTOCOL: Write / SetPosition / GetPosition
 * ========================================================================= */

/*
 * The vendored EDK2 BaseMemoryLib header here only declares CopyMem (no
 * MoveMem/memmove) and CopyMem's overlap behavior isn't guaranteed, but
 * shifting the record's tail forward to make room for a grown attribute
 * is exactly the overlapping-forward-shift case. Copy backwards (highest
 * address first) so the not-yet-copied source bytes are never clobbered.
 */
VOID
NtfsEfiShiftForward (
    IN PUCHAR Base,
    IN UINTN  Len,
    IN UINTN  Growth
    )
{
    UINTN i;
    for (i = Len; i > 0; i--) {
        Base[Growth + i - 1] = Base[i - 1];
    }
}

static VOID
NtfsEfiShiftBackward (
    IN PUCHAR Base,
    IN UINTN  Len,
    IN UINTN  Shrink
    )
{
    UINTN i;
    for (i = 0; i < Len; i++) {
        Base[i] = Base[Shrink + i];
    }
}

/*
 * A file's size is duplicated in THREE places on disk: $DATA (the actual
 * bytes), the $FILE_NAME attribute in the file's own MFT record, and a
 * second copy of that same $FILE_NAME embedded inside the parent
 * directory's INDEX_ENTRY (that's what Explorer/`dir` read for a listing
 * without opening each file - and what chkdsk cross-checks). All three
 * must agree or chkdsk flags the directory link as stale/corrupt, exactly
 * as observed the first time this went untested: growth updated $DATA and
 * the file's own $FILE_NAME, but not the parent's index copy, and chkdsk
 * reported "unneeded link" + reported the file as 0 bytes/unreadable from
 * a real NTFS driver even though this driver's own (self-consistent, but
 * spec-incomplete) reads looked fine.
 *
 * Returns the packed parent MFT reference (record# | seq#<<48) it found
 * in the child's own $FILE_NAME, or 0 if none - used by the caller to
 * locate and patch the matching index entry next.
 */
static UINT64
NtfsSyncFileNameSize (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER Rec,
    IN UINT64               DataSize,
    IN UINT64               AllocSize
    )
{
    ULONG  FnOffset = 0;
    UINT64 ParentRef = 0;
    PNTFS_ATTR_CTX FnCtx = NtfsEfiFindAttrInRecord (Vcb, Rec, AttributeFileName, NULL, 0, &FnOffset);
    if (FnCtx != NULL) {
        PNTFS_ATTR_RECORD   FnAttr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + FnOffset);
        PFILENAME_ATTRIBUTE Fn     = (PFILENAME_ATTRIBUTE)((PUCHAR)FnAttr + FnAttr->Resident.ValueOffset);
        Fn->DataSize      = DataSize;
        Fn->AllocatedSize = AllocSize;
        ParentRef = Fn->DirectoryFileReferenceNumber;
        NtfsEfiFreeAttrCtx (FnCtx);
    }
    return ParentRef;
}

static BOOLEAN
NtfsPatchIndexAllocationEntrySize (
    IN PNTFS_EFI_VCB  Vcb,
    IN PNTFS_ATTR_CTX IndexAllocCtx,
    IN ULONGLONG      VCN,
    IN ULONGLONG      ChildMFT,
    IN UINT64         DataSize,
    IN UINT64         AllocSize
    )
{
    PUCHAR                 IndexBuf;
    PINDEX_BUFFER          Block;
    PINDEX_ENTRY_ATTRIBUTE Entry, Last;
    BOOLEAN                Patched = FALSE;

    IndexBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (IndexBuf == NULL) return FALSE;

    if (NtfsEfiReadAttr (Vcb, IndexAllocCtx, VCN * Vcb->BytesPerCluster,
                         (PCHAR)IndexBuf, Vcb->BytesPerIndexRecord) != Vcb->BytesPerIndexRecord) {
        FreePool (IndexBuf);
        return FALSE;
    }

    Block = (PINDEX_BUFFER)IndexBuf;
    if (Block->Ntfs.Type != NRH_INDX_TYPE ||
        EFI_ERROR (NtfsEfiFixupRecord (Vcb, &Block->Ntfs))) {
        FreePool (IndexBuf);
        return FALSE;
    }

    Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
    Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries);

    while ((PUCHAR)Entry < (PUCHAR)Last) {
        if (Entry->Length == 0) break;

        if (!(Entry->Flags & NTFS_INDEX_ENTRY_END) &&
            (Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK) == ChildMFT) {
            Entry->FileName.DataSize      = DataSize;
            Entry->FileName.AllocatedSize = AllocSize;
            Patched = !EFI_ERROR (NtfsEfiWriteMultiSectorRecord (Vcb, IndexAllocCtx,
                         VCN * Vcb->BytesPerCluster, &Block->Ntfs,
                         Vcb->BytesPerIndexRecord));
            break;
        }

        if (Entry->Flags & NTFS_INDEX_ENTRY_NODE) {
            ULONGLONG SubVCN = *(PULONGLONG)((PUCHAR)Entry + Entry->Length - sizeof (ULONGLONG));
            Patched = NtfsPatchIndexAllocationEntrySize (Vcb, IndexAllocCtx, SubVCN,
                          ChildMFT, DataSize, AllocSize);
            if (Patched) break;
        }

        if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;
        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
    }

    FreePool (IndexBuf);
    return Patched;
}

/*
 * Patch the size fields of the $FILE_NAME copy embedded in the parent
 * directory's $INDEX_ROOT entry for ChildMFT. Same-size in-place edit -
 * no growth, no shifting, no room check needed (unlike NtfsInsertIndexEntry
 * in ntfs_create.c, which adds a whole new entry). Best-effort: handles
 * both resident $INDEX_ROOT and existing $INDEX_ALLOCATION nodes, but still
 * silently does nothing if the entry isn't found -
 * this must never turn a successful Write() into a failure over metadata
 * that will simply read as stale until the next chkdsk otherwise.
 */
static VOID
NtfsSyncParentIndexEntrySize (
    IN PNTFS_EFI_VCB Vcb,
    IN UINT64         ParentRef,
    IN ULONGLONG      ChildMFT,
    IN UINT64         DataSize,
    IN UINT64         AllocSize
    )
{
    ULONGLONG            ParentMFT = ParentRef & NTFS_MFT_MASK;
    PFILE_RECORD_HEADER  ParentRec;
    ULONG                RootOffset = 0;
    PNTFS_ATTR_CTX        RootCtx;
    PNTFS_ATTR_CTX        IndexAllocCtx;
    PNTFS_ATTR_RECORD     RootAttr;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    PINDEX_ENTRY_ATTRIBUTE Entry, Last;

    if (ParentRef == 0) return;

    ParentRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (ParentRec == NULL) return;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, ParentMFT, ParentRec))) {
        FreePool (ParentRec);
        return;
    }

    RootCtx = NtfsEfiFindAttrInRecord (Vcb, ParentRec, AttributeIndexRoot, L"$I30", 4, &RootOffset);
    if (RootCtx == NULL) {
        FreePool (ParentRec);
        return;
    }
    NtfsEfiFreeAttrCtx (RootCtx);

    IndexAllocCtx = NtfsEfiFindAttrInRecord (Vcb, ParentRec,
                        AttributeIndexAllocation, L"$I30", 4, NULL);

    RootAttr  = (PNTFS_ATTR_RECORD)((PUCHAR)ParentRec + RootOffset);
    IndexRoot = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)RootAttr + RootAttr->Resident.ValueOffset);
    Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
    Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.TotalSizeOfEntries);

    while ((PUCHAR)Entry < (PUCHAR)Last) {
        if (Entry->Length == 0) break;
        if (!(Entry->Flags & NTFS_INDEX_ENTRY_END) &&
            (Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK) == ChildMFT) {
            Entry->FileName.DataSize      = DataSize;
            Entry->FileName.AllocatedSize = AllocSize;
            if (EFI_ERROR (NtfsEfiWriteFileRecord (Vcb, ParentMFT, ParentRec))) {
                /* best-effort: leave it stale rather than fail the caller's Write() */
            }
            if (IndexAllocCtx) NtfsEfiFreeAttrCtx (IndexAllocCtx);
            FreePool (ParentRec);
            return;
        }

        if (IndexAllocCtx != NULL && (Entry->Flags & NTFS_INDEX_ENTRY_NODE)) {
            ULONGLONG SubVCN = *(PULONGLONG)((PUCHAR)Entry + Entry->Length - sizeof (ULONGLONG));
            if (NtfsPatchIndexAllocationEntrySize (Vcb, IndexAllocCtx, SubVCN,
                    ChildMFT, DataSize, AllocSize)) {
                NtfsEfiFreeAttrCtx (IndexAllocCtx);
                FreePool (ParentRec);
                return;
            }
        }

        if (Entry->Flags & NTFS_INDEX_ENTRY_END) {
            break;
        }
        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
    }

    if (IndexAllocCtx) NtfsEfiFreeAttrCtx (IndexAllocCtx);
    FreePool (ParentRec);
}

/*
 * Grow a resident attribute's value within its current MFT record, if
 * there's enough free space left in the record for the new length
 * (rounded up to ATTR_RECORD_ALIGNMENT). Shifts every attribute after it
 * forward by the growth amount and fixes up Rec->BytesInUse. No resident-
 * >non-resident conversion - that's a separate, larger step.
 *
 * Returns FALSE (leaving Rec untouched) if it doesn't fit.
 */
BOOLEAN
NtfsEfiGrowResidentInRecord (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER Rec,
    IN ULONG                AttrOffset,
    IN UINT64               NewValueLength
    )
{
    PNTFS_ATTR_RECORD Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + AttrOffset);
    ULONG  OldAttrLen  = Attr->Length;
    ULONG  NewAttrLen  = (ULONG)ROUND_UP (Attr->Resident.ValueOffset + NewValueLength,
                                          ATTR_RECORD_ALIGNMENT);
    LONG   GrowthBytes = (LONG)NewAttrLen - (LONG)OldAttrLen;
    UINT64 OldValueLength = Attr->Resident.ValueLength;
    PUCHAR NextAttr, TailSrc;
    UINTN  TailLen;

    if (GrowthBytes <= 0) {
        /* fits in existing padding already */
        Attr->Resident.ValueLength = (ULONG)NewValueLength;
        if (NewValueLength > OldValueLength) {
            ZeroMem ((PUCHAR)Attr + Attr->Resident.ValueOffset + OldValueLength,
                     (UINTN)(NewValueLength - OldValueLength));
        }
        return TRUE;
    }

    if ((UINT64)GrowthBytes > (UINT64)Rec->BytesAllocated - Rec->BytesInUse) {
        return FALSE;   /* MFT record has no room - deferred to a later round */
    }

    NextAttr = (PUCHAR)Attr + OldAttrLen;
    TailSrc  = NextAttr;
    TailLen  = Rec->BytesInUse - (ULONG)(TailSrc - (PUCHAR)Rec);
    NtfsEfiShiftForward (TailSrc, TailLen, (UINTN)GrowthBytes);

    ZeroMem ((PUCHAR)Attr + Attr->Resident.ValueOffset + OldValueLength,
             (UINTN)(NewValueLength - OldValueLength));

    Attr->Length                = NewAttrLen;
    Attr->Resident.ValueLength  = (ULONG)NewValueLength;
    Rec->BytesInUse            += (ULONG)GrowthBytes;
    return TRUE;
}

/*
 * Append one already-allocated, contiguous cluster run to a non-resident
 * attribute's mapping pairs, growing the MFT record the same way as
 * NtfsEfiGrowResidentInRecord() above. On failure the caller is responsible
 * for freeing the clusters back via NtfsEfiFreeClusters() - this function
 * only touches the mapping-pairs bytes, never the bitmap.
 */
static BOOLEAN
NtfsAppendRunInRecord (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER Rec,
    IN ULONG                AttrOffset,
    IN UINT64               StartLCN,
    IN UINT64               RunClusters,
    IN INT64                LastRealLCN
    )
{
    PNTFS_ATTR_RECORD Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + AttrOffset);
    UINTN  OldMPBytes = NtfsMappingPairsSize (Attr);      /* includes old terminator */
    UCHAR  EncodedRun[17];
    UINTN  EncodedLen = NtfsEncodeRunEntry (EncodedRun, RunClusters, (INT64)StartLCN - LastRealLCN);
    UINTN  NewMPBytes = (OldMPBytes - 1) + EncodedLen + 1; /* replace old term, add new one */
    ULONG  OldAttrLen = Attr->Length;
    ULONG  NewAttrLen = (ULONG)ROUND_UP (Attr->NonResident.MappingPairsOffset + NewMPBytes,
                                         ATTR_RECORD_ALIGNMENT);
    LONG   GrowthBytes = (LONG)NewAttrLen - (LONG)OldAttrLen;
    PUCHAR NextAttr, TailSrc;
    UINTN  TailLen;
    PUCHAR MPStart;

    if ((UINT64)GrowthBytes > (UINT64)Rec->BytesAllocated - Rec->BytesInUse) {
        return FALSE;
    }

    NextAttr = (PUCHAR)Attr + OldAttrLen;
    TailSrc  = NextAttr;
    TailLen  = Rec->BytesInUse - (ULONG)(TailSrc - (PUCHAR)Rec);
    if (GrowthBytes > 0) {
        NtfsEfiShiftForward (TailSrc, TailLen, (UINTN)GrowthBytes);
    }

    MPStart = (PUCHAR)Attr + Attr->NonResident.MappingPairsOffset;
    CopyMem (MPStart + (OldMPBytes - 1), EncodedRun, EncodedLen);
    MPStart[(OldMPBytes - 1) + EncodedLen] = 0;   /* new terminator */

    Attr->Length                    = NewAttrLen;
    Attr->NonResident.HighestVCN   += RunClusters;
    Attr->NonResident.AllocatedSize += (LONGLONG)(RunClusters * Vcb->BytesPerCluster);
    Rec->BytesInUse                += (ULONG)GrowthBytes;
    return TRUE;
}

static EFI_STATUS
NtfsConvertResidentDataToNonResident (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER Rec,
    IN ULONG                AttrOffset,
    IN UINT64               TargetEnd,
    IN UINT64               WriteOffset,
    IN PCHAR                WriteBuffer,
    IN ULONG                WriteLength,
    OUT UINT64             *NewAllocSize
    )
{
    PNTFS_ATTR_RECORD Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + AttrOffset);
    UINT64            OldValueLength;
    PUCHAR            OldValue;
    UINT64            ClustersNeeded;
    UINT64            StartLCN;
    UINT64            Got;
    UINT64            AllocBytes;
    UCHAR             EncodedRun[17];
    UINTN             EncodedLen;
    ULONG             OldAttrLen;
    ULONG             NewAttrLen;
    USHORT            OldInstance;
    USHORT            OldFlags;
    LONG              DeltaBytes;
    PUCHAR            NextAttr;
    UINTN             TailLen;
    PUCHAR            MPStart;
    EFI_STATUS        Status;

    if (Attr->IsNonResident) return EFI_INVALID_PARAMETER;

    OldValueLength = Attr->Resident.ValueLength;
    OldInstance = Attr->Instance;
    OldFlags = Attr->Flags;
    OldValue = AllocatePool ((UINTN)OldValueLength);
    if (OldValue == NULL && OldValueLength != 0) return EFI_OUT_OF_RESOURCES;
    if (OldValueLength != 0) {
        CopyMem (OldValue, (PUCHAR)Attr + Attr->Resident.ValueOffset, (UINTN)OldValueLength);
    }

    ClustersNeeded = (TargetEnd + Vcb->BytesPerCluster - 1) / Vcb->BytesPerCluster;
    if (ClustersNeeded == 0) ClustersNeeded = 1;
    {
        UINT64 QuantumClusters = (NTFS_WRITE_PREALLOC_BYTES + Vcb->BytesPerCluster - 1) /
                                 Vcb->BytesPerCluster;
        if (ClustersNeeded < QuantumClusters) ClustersNeeded = QuantumClusters;
    }

    Status = NtfsEfiAllocateClusters (Vcb, ClustersNeeded, &StartLCN, &Got);
    if (EFI_ERROR (Status)) {
        if (OldValue) FreePool (OldValue);
        return Status;
    }
    if (Got < ClustersNeeded) {
        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        if (OldValue) FreePool (OldValue);
        return EFI_UNSUPPORTED;
    }

    EncodedLen = NtfsEncodeRunEntry (EncodedRun, ClustersNeeded, (INT64)StartLCN);
    OldAttrLen = Attr->Length;
    NewAttrLen = (ULONG)ROUND_UP (sizeof (NTFS_ATTR_RECORD) + EncodedLen + 1,
                                  ATTR_RECORD_ALIGNMENT);
    DeltaBytes = (LONG)NewAttrLen - (LONG)OldAttrLen;

    if (DeltaBytes > 0 &&
        (UINT64)DeltaBytes > (UINT64)Rec->BytesAllocated - Rec->BytesInUse) {
        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        if (OldValue) FreePool (OldValue);
        return EFI_UNSUPPORTED;
    }

    AllocBytes = ClustersNeeded * Vcb->BytesPerCluster;

    {
        /* Zero the freshly allocated clusters in as few DiskIo calls as
         * possible: one buffer, a large chunk, instead of one write per
         * cluster. */
        UINTN  ZeroBufSize = (UINTN)min (AllocBytes, (UINT64)(64 * 1024));
        PUCHAR ZeroBuf = AllocateZeroPool (ZeroBufSize);
        UINT64 Done = 0;
        if (ZeroBuf == NULL) {
            NtfsEfiFreeClusters (Vcb, StartLCN, Got);
            if (OldValue) FreePool (OldValue);
            return EFI_OUT_OF_RESOURCES;
        }
        while (Done < AllocBytes) {
            UINTN Chunk = (UINTN)min ((UINT64)ZeroBufSize, AllocBytes - Done);
            Status = NtfsEfiWriteDisk (Vcb,
                         StartLCN * Vcb->BytesPerCluster + Done,
                         Chunk, ZeroBuf);
            if (EFI_ERROR (Status)) break;
            Done += Chunk;
        }
        FreePool (ZeroBuf);
        if (EFI_ERROR (Status)) {
            NtfsEfiFreeClusters (Vcb, StartLCN, Got);
            if (OldValue) FreePool (OldValue);
            return Status;
        }
    }

    if (OldValueLength != 0) {
        Status = NtfsEfiWriteDisk (Vcb, StartLCN * Vcb->BytesPerCluster,
                                   (UINTN)OldValueLength, OldValue);
        if (EFI_ERROR (Status)) {
            NtfsEfiFreeClusters (Vcb, StartLCN, Got);
            if (OldValue) FreePool (OldValue);
            return Status;
        }
    }

    Status = NtfsEfiWriteDisk (Vcb, StartLCN * Vcb->BytesPerCluster + WriteOffset,
                               WriteLength, WriteBuffer);
    if (EFI_ERROR (Status)) {
        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        if (OldValue) FreePool (OldValue);
        return Status;
    }

    NextAttr = (PUCHAR)Attr + OldAttrLen;
    TailLen  = Rec->BytesInUse - (ULONG)(NextAttr - (PUCHAR)Rec);
    if (DeltaBytes > 0) {
        NtfsEfiShiftForward (NextAttr, TailLen, (UINTN)DeltaBytes);
    } else if (DeltaBytes < 0) {
        NtfsEfiShiftBackward (NextAttr + DeltaBytes, TailLen, (UINTN)(-DeltaBytes));
    }

    ZeroMem (Attr, NewAttrLen);
    Attr->Type = (ULONG)AttributeData;
    Attr->Length = NewAttrLen;
    Attr->IsNonResident = 1;
    Attr->NameLength = 0;
    Attr->NameOffset = 0;
    Attr->Flags = OldFlags;
    Attr->Instance = OldInstance;
    Attr->NonResident.LowestVCN = 0;
    Attr->NonResident.HighestVCN = ClustersNeeded - 1;
    Attr->NonResident.MappingPairsOffset = sizeof (NTFS_ATTR_RECORD);
    Attr->NonResident.CompressionUnit = 0;
    Attr->NonResident.AllocatedSize = (LONGLONG)AllocBytes;
    Attr->NonResident.DataSize = (LONGLONG)TargetEnd;
    Attr->NonResident.InitializedSize = (LONGLONG)TargetEnd;
    Attr->NonResident.CompressedSize = 0;

    MPStart = (PUCHAR)Attr + Attr->NonResident.MappingPairsOffset;
    CopyMem (MPStart, EncodedRun, EncodedLen);
    MPStart[EncodedLen] = 0;

    Rec->BytesInUse = (ULONG)((LONG)Rec->BytesInUse + DeltaBytes);
    *NewAllocSize = AllocBytes;

    if (OldValue) FreePool (OldValue);
    return EFI_SUCCESS;
}

/*
 * Write support. Phase 1 (in-place overwrite of already-allocated bytes)
 * plus phase 2 (grow the file): Position + *BufferSize may now exceed the
 * current FileSize. Growth still requires Position <= FileSize (append or
 * overwrite-tail; arbitrary sparse seek-writes past EOF are not yet
 * supported). $STANDARD_INFORMATION timestamps are intentionally left
 * untouched (a stale mtime is harmless; a half-written attribute isn't).
 *
 * Growth may span MULTIPLE new runs per Write() call: if the free-space
 * search hands back a run shorter than requested, more runs are appended
 * until the request is satisfied. Every cluster allocated in the call is
 * logged; a failure at any step (volume full, or the MFT record running
 * out of room for another mapping pair) rolls the whole allocation back
 * and leaves the on-disk record untouched (Rec is only written after all
 * runs are in place), so a partial write never leaks clusters or corrupts
 * the record.
 *
 * Only attributes resident directly in the primary MFT record are
 * supported (no $ATTRIBUTE_LIST follow) - every synthetic test file used
 * to validate this is small enough to qualify.
 */
static EFI_STATUS EFIAPI
NtfsEfiWrite (
    IN     EFI_FILE_PROTOCOL *This,
    IN OUT UINTN             *BufferSize,
    IN     VOID              *Buffer
    )
{
    PNTFS_EFI_FILE       F   = (PNTFS_EFI_FILE)This;
    PNTFS_EFI_VCB        Vcb = F->Vcb;
    PFILE_RECORD_HEADER  Rec;
    ULONG                ToWrite, Written;
    UINT64               TargetEnd;

    if (F->IsDirectory) {
        *BufferSize = 0;
        return EFI_UNSUPPORTED;
    }
    if (!F->OpenForWrite) {
        *BufferSize = 0;
        return EFI_ACCESS_DENIED;
    }
    if (F->Position > F->FileSize) {
        UINT64 GapSize = F->Position - F->FileSize;
        UINT64 OriginalPosition = F->Position;
        PUCHAR ZeroBuf;
        UINTN  WriteSize;
        EFI_STATUS Status;

        ZeroBuf = AllocateZeroPool (4096);
        if (ZeroBuf == NULL) return EFI_OUT_OF_RESOURCES;

        F->Position = F->FileSize;
        while (GapSize > 0) {
            WriteSize = (GapSize > 4096) ? 4096 : (UINTN)GapSize;
            Status = NtfsEfiWrite (This, &WriteSize, ZeroBuf);
            if (EFI_ERROR (Status)) {
                FreePool (ZeroBuf);
                F->Position = OriginalPosition;
                return Status;
            }
            GapSize -= WriteSize;
        }
        FreePool (ZeroBuf);
        F->Position = OriginalPosition;
    }
    ToWrite   = (ULONG)*BufferSize;
    TargetEnd = F->Position + ToWrite;
    if (ToWrite == 0) return EFI_SUCCESS;

    NtfsMarkVolumeDirty (Vcb);

    Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, F->MFTIndex, Rec))) {
        FreePool (Rec);
        return EFI_DEVICE_ERROR;
    }

    {
        ULONG          AttrOffset = 0;
        PNTFS_ATTR_CTX DataCtx = NtfsEfiFindAttrInRecord (Vcb, Rec, AttributeData,
                                        NULL, 0, &AttrOffset);
        UINT64         NewAllocSize = 0;
        BOOLEAN        RecordDirty  = FALSE;

        if (DataCtx == NULL) {
            FreePool (Rec);
            *BufferSize = 0;
            return EFI_UNSUPPORTED;
        }

        if (!DataCtx->pRecord->IsNonResident) {
            /* resident $DATA lives inside Rec itself */
            PNTFS_ATTR_RECORD AttrInRec;
            PCHAR             ValPtr;

            if (TargetEnd > DataCtx->pRecord->Resident.ValueLength) {
                if (!NtfsEfiGrowResidentInRecord (Vcb, Rec, AttrOffset, TargetEnd)) {
                    UINT64 ParentRef;
                    EFI_STATUS ConvStatus;

                    ConvStatus = NtfsConvertResidentDataToNonResident (Vcb, Rec, AttrOffset,
                                     TargetEnd, F->Position, (PCHAR)Buffer, ToWrite,
                                     &NewAllocSize);
                    NtfsEfiFreeAttrCtx (DataCtx);

                    if (EFI_ERROR (ConvStatus)) {
                        FreePool (Rec);
                        *BufferSize = 0;
                        return ConvStatus;
                    }

                    ParentRef = NtfsSyncFileNameSize (Vcb, Rec, TargetEnd, NewAllocSize);

                    if (EFI_ERROR (NtfsEfiWriteFileRecord (Vcb, F->MFTIndex, Rec))) {
                        FreePool (Rec);
                        *BufferSize = 0;
                        return EFI_DEVICE_ERROR;
                    }

                    NtfsSyncParentIndexEntrySize (Vcb, ParentRef, F->MFTIndex,
                                                  TargetEnd, NewAllocSize);
                    Written = ToWrite;
                    RecordDirty = TRUE;
                    goto WriteDone;
                }
                RecordDirty = TRUE;
            }

            AttrInRec = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + AttrOffset);
            ValPtr    = (PCHAR)AttrInRec + AttrInRec->Resident.ValueOffset;
            CopyMem (ValPtr + F->Position, Buffer, ToWrite);
            NtfsEfiFreeAttrCtx (DataCtx);

            {
                UINT64 FileNameAllocSz = TargetEnd;
                UINT64 IndexAllocSz = ROUND_UP (TargetEnd, ATTR_RECORD_ALIGNMENT);
                UINT64 ParentRef = 0;
                if (RecordDirty) {
                    ParentRef = NtfsSyncFileNameSize (Vcb, Rec, TargetEnd, FileNameAllocSz);
                }

                if (EFI_ERROR (NtfsEfiWriteFileRecord (Vcb, F->MFTIndex, Rec))) {
                    FreePool (Rec);
                    *BufferSize = 0;
                    return EFI_DEVICE_ERROR;
                }

                if (RecordDirty) {
                    NtfsSyncParentIndexEntrySize (Vcb, ParentRef, F->MFTIndex, TargetEnd, IndexAllocSz);
                }
            }
            Written      = ToWrite;
            NewAllocSize = F->AllocSize;   /* unchanged: resident "alloc size" tracks record slack, not reported precisely */

        } else if (DataCtx->pRecord->NonResident.CompressionUnit != 0) {
            /* compressed files: a byte-range write would require
             * re-encoding the whole compression unit - not yet supported */
            NtfsEfiFreeAttrCtx (DataCtx);
            FreePool (Rec);
            *BufferSize = 0;
            return EFI_UNSUPPORTED;

        } else {
            UINT64 OldAllocSize = (UINT64)DataCtx->pRecord->NonResident.AllocatedSize;
            UINT64 OldDataSize  = (UINT64)DataCtx->pRecord->NonResident.DataSize;

            if (TargetEnd > OldAllocSize) {
                UINT64 GrowBytes      = TargetEnd - OldAllocSize;
                UINT64 ClustersNeeded = (GrowBytes + Vcb->BytesPerCluster - 1) / Vcb->BytesPerCluster;
                UINT64 QuantumClusters = (NTFS_WRITE_PREALLOC_BYTES + Vcb->BytesPerCluster - 1) /
                                         Vcb->BytesPerCluster;
                UINT64 Remaining;
                UINT64 NextVBN;
                INT64  LastRealLCN = 0;
                ULONG  i;
                /*
                 * Multi-run growth: the free-space search may hand back a run
                 * shorter than requested. Instead of failing (as the earlier,
                 * cautious version did), append as many contiguous runs as it
                 * takes to satisfy the request, then fall back only if the MFT
                 * record fills up or the volume runs out of space. Every
                 * cluster allocated in THIS call is logged so a failure at any
                 * later step rolls the whole allocation back - Rec is not
                 * written to disk until success, so its in-memory mapping-pair
                 * mutations are simply discarded on the failure path.
                 */
                NTFS_RUN_ENTRY *AllocLog;
                ULONG           AllocLogCount = 0;
                EFI_STATUS      GrowStatus = EFI_SUCCESS;

                if (ClustersNeeded < QuantumClusters) ClustersNeeded = QuantumClusters;

                for (i = 0; i < DataCtx->RunCount; i++) {
                    if (DataCtx->Runs[i].LBN != -1LL) LastRealLCN = DataCtx->Runs[i].LBN;
                }

                AllocLog = AllocatePool (NTFS_MAX_RUNS * sizeof (NTFS_RUN_ENTRY));
                if (AllocLog == NULL) {
                    NtfsEfiFreeAttrCtx (DataCtx);
                    FreePool (Rec);
                    *BufferSize = 0;
                    return EFI_OUT_OF_RESOURCES;
                }

                NextVBN   = OldAllocSize / Vcb->BytesPerCluster;
                Remaining = ClustersNeeded;

                while (Remaining > 0) {
                    UINT64 StartLCN, Got;

                    if (DataCtx->RunCount >= NTFS_MAX_RUNS ||
                        AllocLogCount >= NTFS_MAX_RUNS) {
                        GrowStatus = EFI_UNSUPPORTED;   /* run list exhausted */
                        break;
                    }

                    if (EFI_ERROR (NtfsEfiAllocateClusters (Vcb, Remaining, &StartLCN, &Got)) ||
                        Got == 0) {
                        GrowStatus = EFI_VOLUME_FULL;
                        break;
                    }

                    /* NtfsAppendRunToAttr MERGES a run contiguous with the tail
                     * instead of appending a new mapping pair - so a large file
                     * laid down in contiguous prealloc chunks stays 1 run rather
                     * than accreting one pair per MB and overflowing the record
                     * (~127 MB was the observed cliff). */
                    if (!NtfsAppendRunToAttr (Vcb, Rec, AttrOffset, StartLCN, Got, LastRealLCN)) {
                        /* MFT record has no room for another mapping pair */
                        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
                        GrowStatus = EFI_UNSUPPORTED;
                        break;
                    }

                    AllocLog[AllocLogCount].LBN = (INT64)StartLCN;
                    AllocLog[AllocLogCount].Len = Got;
                    AllocLogCount++;

                    /* extend the in-memory run list too so the write below
                     * (same DataCtx) can reach the newly-appended clusters */
                    DataCtx->Runs[DataCtx->RunCount].VBN = NextVBN;
                    DataCtx->Runs[DataCtx->RunCount].LBN = (INT64)StartLCN;
                    DataCtx->Runs[DataCtx->RunCount].Len = Got;
                    DataCtx->RunCount++;

                    NextVBN     += Got;
                    LastRealLCN  = (INT64)StartLCN;
                    Remaining    -= Got;
                }

                if (EFI_ERROR (GrowStatus)) {
                    /* roll back every cluster allocated in this Write() call;
                     * the partially-mutated Rec is discarded unwritten */
                    ULONG r;
                    for (r = 0; r < AllocLogCount; r++) {
                        NtfsEfiFreeClusters (Vcb, (UINT64)AllocLog[r].LBN, AllocLog[r].Len);
                    }
                    FreePool (AllocLog);
                    NtfsEfiFreeAttrCtx (DataCtx);
                    FreePool (Rec);
                    *BufferSize = 0;
                    return GrowStatus;
                }

                FreePool (AllocLog);
                RecordDirty = TRUE;
                F->DidGrow  = TRUE;   /* prealloc slack -> trim on Close */
            }

            Written = NtfsEfiWriteAttr (Vcb, DataCtx, F->Position, (PCHAR)Buffer, ToWrite);

            if (TargetEnd > OldDataSize) {
                RecordDirty = TRUE;
            }

            if (RecordDirty) {
                PNTFS_ATTR_RECORD AttrInRec = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + AttrOffset);
                UINT64 ParentRef;
                AttrInRec->NonResident.DataSize        = (LONGLONG)TargetEnd;
                AttrInRec->NonResident.InitializedSize = (LONGLONG)TargetEnd;
                NewAllocSize = (UINT64)AttrInRec->NonResident.AllocatedSize;

                ParentRef = NtfsSyncFileNameSize (Vcb, Rec, TargetEnd, NewAllocSize);

                if (EFI_ERROR (NtfsEfiWriteFileRecord (Vcb, F->MFTIndex, Rec))) {
                    NtfsEfiFreeAttrCtx (DataCtx);
                    FreePool (Rec);
                    *BufferSize = 0;
                    return EFI_DEVICE_ERROR;
                }

                NtfsSyncParentIndexEntrySize (Vcb, ParentRef, F->MFTIndex, TargetEnd, NewAllocSize);
            } else {
                NewAllocSize = OldAllocSize;
            }
            NtfsEfiFreeAttrCtx (DataCtx);
        }

WriteDone:
        if (RecordDirty && TargetEnd > F->FileSize) {
            F->FileSize  = TargetEnd;
            F->AllocSize = NewAllocSize;
        }
    }

    FreePool (Rec);
    F->Position += Written;
    *BufferSize  = Written;
    return (Written == ToWrite) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
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
        /* EFI spec: 0 rewinds directory enumeration.
         *
         * Drop the cached listing too, so the rewound enumeration re-reads the
         * directory as it is NOW. Read() builds the child-MFT list once per
         * handle for O(1) enumeration; keeping it across a rewind made the
         * classic "list a directory, delete entries, list again" loop replay a
         * stale snapshot - the caller then tried to open entries that no longer
         * exist and, worse, could miss entries added meanwhile. Rewinding is the
         * one point where a caller explicitly asks to start over, so it is the
         * right place to refresh. */
        if (Position == 0 && F->DirCacheBuilt) {
            if (F->DirCache != NULL) { FreePool (F->DirCache); F->DirCache = NULL; }
            F->DirCacheCount = 0;
            F->DirCacheBuilt = FALSE;
        }
        F->DirEnumEntry = (ULONG)(Position == 0 ? 0 : Position);
    } else {
        F->Position = (Position == 0xFFFFFFFFFFFFFFFFULL)
                      ? F->FileSize : Position;
    }
    return EFI_SUCCESS;
}

/* =========================================================================
 * EFI_FILE_PROTOCOL: GetInfo / SetInfo / Flush
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
        SysInfo->ReadOnly     = FALSE;
        SysInfo->BlockSize    = Vcb->BytesPerCluster;
        SysInfo->VolumeSize   = Vcb->TotalClusters * Vcb->BytesPerCluster;
        SysInfo->FreeSpace    = Vcb->FreeClusters  * Vcb->BytesPerCluster;
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
    PNTFS_EFI_FILE F   = (PNTFS_EFI_FILE)This;
    PNTFS_EFI_VCB  Vcb = F->Vcb;

    if (CompareGuid (InformationTypeGuid, &gEfiFileInfoGuid)) {
        EFI_FILE_INFO *Info = (EFI_FILE_INFO *)Buffer;
        EFI_STATUS     Status;

        if (BufferSize < SIZE_OF_EFI_FILE_INFO) return EFI_BAD_BUFFER_SIZE;
        if (!F->OpenForWrite) return EFI_ACCESS_DENIED;

        /* rename / move: FileName is the file's new name, optionally prefixed
         * with a destination directory path. A path with separators is a
         * cross-directory move - absolute (leading '\') anchors at the volume
         * root, otherwise it is resolved relative to the file's current
         * directory. A bare name (no separator) is an in-place rename. */
        {
            UINTN   NewLen = StrLen (Info->FileName);
            INTN    LastSep = -1;
            UINTN   k;

            for (k = 0; k < NewLen; k++)
                if (Info->FileName[k] == L'\\' || Info->FileName[k] == L'/')
                    LastSep = (INTN)k;

            if (NewLen > 0) {
                CONST WCHAR *FinalName = Info->FileName + (LastSep + 1);
                UINTN        FinalLen  = NewLen - (UINTN)(LastSep + 1);
                ULONGLONG    DestParent = (ULONGLONG)-1LL;   /* same-dir default */
                BOOLEAN      NameChanged;

                if (FinalLen == 0 || FinalLen > 255) return EFI_INVALID_PARAMETER;

                if (LastSep >= 0) {
                    /* resolve the destination directory portion */
                    WCHAR     DirBuf[NTFS_MAX_PATH_CHARS];
                    ULONGLONG Anchor;
                    UINTN     DirLen = (UINTN)LastSep;   /* chars before final comp */

                    if (DirLen >= NTFS_MAX_PATH_CHARS) return EFI_INVALID_PARAMETER;
                    CopyMem (DirBuf, Info->FileName, DirLen * sizeof (WCHAR));
                    DirBuf[DirLen] = L'\0';

                    if (Info->FileName[0] == L'\\' || Info->FileName[0] == L'/') {
                        Anchor = NTFS_FILE_ROOT;
                    } else {
                        Anchor = NtfsEfiParentOf (Vcb, F->MFTIndex);
                        if (Anchor == (ULONGLONG)-1LL) return EFI_DEVICE_ERROR;
                    }

                    /* dir portion is only separators (e.g. "\name") -> root */
                    {
                        UINTN j = 0;
                        while (DirBuf[j] == L'\\' || DirBuf[j] == L'/') j++;
                        if (DirBuf[j] == L'\0') DestParent = NTFS_FILE_ROOT;
                        else {
                            DestParent = NtfsEfiLookupPath (Vcb, Anchor, DirBuf, FALSE);
                            if (DestParent == (ULONGLONG)-1LL) return EFI_NOT_FOUND;
                        }
                    }
                }

                NameChanged =
                    (LastSep >= 0) ||   /* an explicit path is always a move request */
                    (FinalLen != F->FileNameChars) ||
                    (CompareMem (FinalName, F->FileName, FinalLen * sizeof (CHAR16)) != 0);

                if (NameChanged) {
                    Status = NtfsEfiMoveFile (Vcb, F->MFTIndex, DestParent, FinalName, FinalLen);
                    if (EFI_ERROR (Status)) return Status;
                    CopyMem (F->FileName, FinalName, FinalLen * sizeof (CHAR16));
                    F->FileName[FinalLen] = L'\0';
                    F->FileNameChars      = FinalLen;
                }
            }
        }

        /* resize: a changed FileSize resizes the file. Shrink -> free the tail
         * via NtfsEfiSetFileSize. Grow -> reuse the tested seek-past-EOF write
         * path (zero-fills the exposed range, allocates/converts as needed) by
         * seeking to the new end and writing zero bytes. Directories carry no
         * $DATA, so only touch it for regular files. */
        if (!F->IsDirectory && Info->FileSize != F->FileSize) {
            if (Info->FileSize < F->FileSize) {
                Status = NtfsEfiSetFileSize (Vcb, F->MFTIndex, Info->FileSize);
                if (EFI_ERROR (Status)) return Status;
            } else {
                UINT64 SavedPos = F->Position;
                UINT8  Dummy    = 0;
                UINTN  Zero     = 0;
                F->Position = Info->FileSize;                 /* gap-fill target */
                Status = NtfsEfiWrite (This, &Zero, &Dummy);  /* triggers zero-fill to Position */
                F->Position = SavedPos;
                if (EFI_ERROR (Status)) return Status;
            }
            F->FileSize = Info->FileSize;
            if (F->Position > F->FileSize) F->Position = F->FileSize;
        }

        Status = NtfsEfiSetFileInfo (Vcb, F->MFTIndex, Info);
        if (EFI_ERROR (Status)) return Status;

        /* refresh cached fields so a following GetInfo reflects the change */
        if (Info->CreateTime.Year != 0)
            F->CreationTime  = NtfsEfiConvertTimeToNtfs (&Info->CreateTime);
        if (Info->ModificationTime.Year != 0) {
            F->LastWriteTime = NtfsEfiConvertTimeToNtfs (&Info->ModificationTime);
            F->ChangeTime    = F->LastWriteTime;
        }
        if (Info->LastAccessTime.Year != 0)
            F->LastAccessTime = NtfsEfiConvertTimeToNtfs (&Info->LastAccessTime);
        F->NtfsAttribs &= ~(NTFS_FILE_TYPE_READ_ONLY | NTFS_FILE_TYPE_HIDDEN |
                            NTFS_FILE_TYPE_SYSTEM | NTFS_FILE_TYPE_ARCHIVE);
        if (Info->Attribute & EFI_FILE_READ_ONLY) F->NtfsAttribs |= NTFS_FILE_TYPE_READ_ONLY;
        if (Info->Attribute & EFI_FILE_HIDDEN)    F->NtfsAttribs |= NTFS_FILE_TYPE_HIDDEN;
        if (Info->Attribute & EFI_FILE_SYSTEM)    F->NtfsAttribs |= NTFS_FILE_TYPE_SYSTEM;
        if (Info->Attribute & EFI_FILE_ARCHIVE)   F->NtfsAttribs |= NTFS_FILE_TYPE_ARCHIVE;
        return EFI_SUCCESS;
    }

    /* $FILE_SYSTEM_INFO (volume relabel) and others: not yet supported */
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI
NtfsEfiFlush (IN EFI_FILE_PROTOCOL *This)
{
    PNTFS_EFI_FILE F   = (PNTFS_EFI_FILE)This;
    PNTFS_EFI_VCB  Vcb = (F != NULL) ? F->Vcb : NULL;

    /*
     * Honour the EFI_FILE_PROTOCOL.Flush contract: push every buffered write
     * on this volume through to the physical medium. Metadata mutations
     * (write/create/delete/setinfo) already reach the driver's DiskIo
     * immediately, but on real firmware DiskIo/BlockIo may sit in a
     * write-back cache that is only guaranteed to drain on FlushBlocks - so
     * without this a caller that does "delete then power off" (e.g. the EC
     * file manager, which has no reason to unmount between operations) could
     * lose the change on bare metal even though it looked done. This used to
     * be a no-op left over from the read-only era; that made every app-level
     * Flush() silently non-durable. The unmount path (NtfsEfiUnmountVolume)
     * still flushes too; this just lets a long-running app force durability
     * without tearing the volume down.
     */
    if (Vcb != NULL && Vcb->BlockIo != NULL && Vcb->BlockIo->FlushBlocks != NULL) {
        return Vcb->BlockIo->FlushBlocks (Vcb->BlockIo);
    }
    return EFI_SUCCESS;
}

/* =========================================================================
 * Protocol vtable template, filled once by NtfsEfiOpenVolume() (ntfs_volume.c)
 * ========================================================================= */

VOID
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
