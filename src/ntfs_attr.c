/**
 * ntfs_attr.c - attribute context lifecycle, ReadAttribute (no cache
 * manager, no MCB, direct DiskIo), and FindAttribute (in-record scan plus
 * $ATTRIBUTE_LIST follow for attributes split across multiple MFT records).
 */

#include "ntfs.h"

static PNTFS_ATTR_CTX
NtfsEfiAllocAttrCtx (VOID)
{
    return AllocateZeroPool (sizeof (NTFS_ATTR_CTX));
}

VOID
NtfsEfiFreeAttrCtx (PNTFS_ATTR_CTX Ctx)
{
    if (Ctx == NULL) return;
    if (Ctx->pRecord)  FreePool (Ctx->pRecord);
    if (Ctx->Runs)     FreePool (Ctx->Runs);
    FreePool (Ctx);
}

static PNTFS_ATTR_CTX
NtfsEfiPrepareAttrCtx (
    IN PNTFS_ATTR_RECORD  AttrRecord,
    IN ULONGLONG          FileMFTIndex
    )
{
    PNTFS_ATTR_CTX Ctx;

    /*
     * Callers have already checked that Length fits inside the source record
     * (NtfsEfiFindAttrInRecord). Re-check the value bounds a RESIDENT attribute
     * declares, because the heap copy taken below is exactly Length bytes and
     * NtfsEfiReadAttr's resident branch reads at ValueOffset+ValueLength inside
     * it - both fields are on-disk data and nothing else validates them.
     */
    if (AttrRecord->Length < NTFS_ATTR_MIN_HEADER) return NULL;
    if (!AttrRecord->IsNonResident &&
        (UINT64)AttrRecord->Resident.ValueOffset + AttrRecord->Resident.ValueLength
            > (UINT64)AttrRecord->Length) {
        return NULL;
    }

    Ctx = NtfsEfiAllocAttrCtx ();
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

UINT64
NtfsEfiAttrDataLength (PNTFS_ATTR_CTX Ctx)
{
    if (Ctx->pRecord->IsNonResident)
        return (UINT64)Ctx->pRecord->NonResident.DataSize;
    return Ctx->pRecord->Resident.ValueLength;
}

ULONG
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
        Print (L"[ntfs] ReadAttr(resident): pRecord=%p ValueOffset=%d ValLen=%ld ValPtr=%p bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            Ctx->pRecord, Ctx->pRecord->Resident.ValueOffset, ValLen, ValPtr,
            ((PUCHAR)ValPtr)[0], ((PUCHAR)ValPtr)[1], ((PUCHAR)ValPtr)[2], ((PUCHAR)ValPtr)[3],
            ((PUCHAR)ValPtr)[4], ((PUCHAR)ValPtr)[5], ((PUCHAR)ValPtr)[6], ((PUCHAR)ValPtr)[7]);
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

        /*
         * The cache only advances forward and is only valid when Offset
         * still falls within [CacheOffset, CacheOffset+RunLen) - callers
         * that read strictly sequentially (regular file data) keep hitting
         * this fast path. But this same AttrCtx is also reused for the
         * $INDEX_ALLOCATION random-access B+tree walk during directory
         * search, which jumps to arbitrary offsets, including backwards.
         * Without an explicit lower-bound check, a backward seek could
         * satisfy "Offset < CacheOffset + RunLen" against a stale, too-far
         * -advanced cache position, making RunOffsetBytes (Offset -
         * CacheOffset) underflow to a huge value and read garbage from the
         * wrong disk location entirely (observed as index nodes with a
         * corrupt signature). Invalidate the cache whenever Offset isn't
         * actually inside the cached run so the linear rescan below runs.
         */
        if (Ctx->CacheIdx < Ctx->RunCount) {
            RunLenBytes = Ctx->Runs[Ctx->CacheIdx].Len * Vcb->BytesPerCluster;
            if (Offset < Ctx->CacheOffset || Offset >= Ctx->CacheOffset + RunLenBytes) {
                Ctx->CacheIdx = Ctx->RunCount; /* force rescan below */
            }
        }
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
    if (AlreadyRead >= 8) {
        Print (L"[ntfs] ReadAttr(non-resident): AlreadyRead=%d bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            AlreadyRead,
            ((PUCHAR)Buffer)[0], ((PUCHAR)Buffer)[1], ((PUCHAR)Buffer)[2], ((PUCHAR)Buffer)[3],
            ((PUCHAR)Buffer)[4], ((PUCHAR)Buffer)[5], ((PUCHAR)Buffer)[6], ((PUCHAR)Buffer)[7]);
    }
    return AlreadyRead;
}

/*
 * In-place overwrite only: Offset..Offset+Length must already lie inside
 * an allocated (non-sparse) run. No allocation, no sparse-run materialize,
 * no resident->non-resident conversion - those are size-changing
 * operations that belong to a later, more careful round (they touch
 * $Bitmap and possibly $MFT record layout, not just data bytes).
 */
ULONG
NtfsEfiWriteAttr (
    IN PNTFS_EFI_VCB  Vcb,
    IN PNTFS_ATTR_CTX Ctx,
    IN UINT64         Offset,
    IN PCHAR          Buffer,
    IN ULONG          Length
    )
{
    ULONG Remaining, Written;

    if (!Ctx->pRecord->IsNonResident) {
        /*
         * Ctx->pRecord is a heap COPY of the attribute record (see
         * NtfsEfiPrepareAttrCtx) - it's what ReadAttr's resident branch
         * reads from, but it is NOT the live MFT record buffer, so a
         * write here would vanish silently instead of reaching disk.
         * Every current resident-attribute writer (NtfsEfiWrite in
         * ntfs_file.c) already knows this and patches the real record
         * buffer directly instead of going through this function. Refuse
         * rather than pretend to succeed, so a future caller that reaches
         * this path finds out immediately instead of losing data quietly.
         */
        return 0;
    }

    Remaining = Length;
    Written   = 0;

    while (Remaining > 0) {
        ULONG  i;
        UINT64 Scan = 0;
        UINT64 RunOffsetBytes, RunLenBytes, TakeBytes;
        EFI_STATUS Status;
        BOOLEAN Found = FALSE;

        /* linear scan every call - writes are rare/small in this first
         * cautious round, no need for the sequential-read cache here */
        for (i = 0; i < Ctx->RunCount; i++) {
            UINT64 Bytes = Ctx->Runs[i].Len * Vcb->BytesPerCluster;
            if (Offset < Scan + Bytes) { Found = TRUE; break; }
            Scan += Bytes;
        }
        if (!Found) break;   /* past allocated extent - no growth */

        RunOffsetBytes = Offset - Scan;
        RunLenBytes    = Ctx->Runs[i].Len * Vcb->BytesPerCluster;
        TakeBytes      = min (RunLenBytes - RunOffsetBytes, (UINT64)Remaining);

        if (Ctx->Runs[i].LBN == -1LL) {
            /* sparse run: materializing it means allocating real clusters -
             * a later round, not a safe in-place overwrite */
            break;
        }

        {
            UINT64 DiskByteOffset =
                (UINT64)Ctx->Runs[i].LBN * Vcb->BytesPerCluster + RunOffsetBytes;
            Status = NtfsEfiWriteDisk (Vcb, DiskByteOffset, (UINTN)TakeBytes,
                                       Buffer + Written);
            if (EFI_ERROR (Status)) break;
        }

        Offset    += TakeBytes;
        Written   += (ULONG)TakeBytes;
        Remaining -= (ULONG)TakeBytes;
    }
    return Written;
}

PNTFS_ATTR_CTX
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

    (VOID)Vcb;

    /*
     * Attr->Length is a 32-bit field read straight off the disk, and it is used
     * BOTH to step to the next attribute and (in NtfsEfiPrepareAttrCtx below) as
     * an AllocatePool+CopyMem size. Checking only for 0 - enough to stop the
     * walk looping forever - still let a record whose attribute claimed a length
     * larger than the record itself copy far past the end of the record buffer.
     * This function is the central attribute lookup (every file operation goes
     * through it), so validate here, once: minimum header size, and the whole
     * attribute (plus its name) inside the record's in-use area. Same shape as
     * the check NtfsEfiSelectNameInRecord in ntfs_file.c already does.
     */
    while ((PUCHAR)Attr + NTFS_ATTR_MIN_HEADER <= (PUCHAR)LastPtr &&
           Attr->Type != (ULONG)AttributeEnd) {
        if (Attr->Length < NTFS_ATTR_MIN_HEADER ||
            (PUCHAR)Attr + Attr->Length > (PUCHAR)LastPtr) {
            break;
        }
        if (Attr->NameLength != 0 &&
            (UINT64)Attr->NameOffset + (UINT64)Attr->NameLength * sizeof (WCHAR)
                > (UINT64)Attr->Length) {
            break;
        }

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

PNTFS_ATTR_CTX
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
        PUCHAR             ListBuf;
        PUCHAR             ListEnd;
        PNTFS_ATTR_LIST_ITEM Item;

        /* ListLen is the attribute's declared data size, i.e. on-disk data:
         * bound it before it becomes an AllocatePool size, exactly as
         * NtfsEfiResolveIndexHost below already does. */
        if (ListLen < sizeof (NTFS_ATTR_LIST_ITEM) || ListLen > (UINT64)(1024 * 1024)) {
            NtfsEfiFreeAttrCtx (ListCtx);
            return NULL;
        }
        ListBuf = AllocatePool ((UINTN)ListLen);
        if (ListBuf == NULL) { NtfsEfiFreeAttrCtx (ListCtx); return NULL; }
        /* a short read would leave the tail of ListBuf uninitialised and the
         * loop below would then parse whatever the pool handed us */
        if (NtfsEfiReadAttr (Vcb, ListCtx, 0, (PCHAR)ListBuf, (ULONG)ListLen) != (ULONG)ListLen) {
            FreePool (ListBuf);
            NtfsEfiFreeAttrCtx (ListCtx);
            return NULL;
        }
        NtfsEfiFreeAttrCtx (ListCtx);

        Item    = (PNTFS_ATTR_LIST_ITEM)ListBuf;
        ListEnd = ListBuf + ListLen;

        /* every field read below must be inside the buffer: the whole fixed
         * part of the item, its Length (which advances the walk), and the name
         * bytes at NameOffset */
        while ((PUCHAR)Item + sizeof (NTFS_ATTR_LIST_ITEM) <= ListEnd &&
               Item->Type != (ULONG)AttributeEnd) {
            if (Item->Length < sizeof (NTFS_ATTR_LIST_ITEM) ||
                (PUCHAR)Item + Item->Length > ListEnd) {
                break;
            }
            if (Item->Type == (ULONG)Type && Item->NameLength == NameLength) {
                BOOLEAN NameMatch = TRUE;
                if (NameLength > 0) {
                    PWCHAR AttrName = (PWCHAR)((PUCHAR)Item + Item->NameOffset);
                    if ((PUCHAR)Item + Item->NameOffset +
                            (UINTN)NameLength * sizeof (WCHAR) > ListEnd) {
                        NameMatch = FALSE;
                    } else if (CompareMem (AttrName, Name, NameLength * sizeof (WCHAR)) != 0) {
                        NameMatch = FALSE;
                    }
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
            Item = (PNTFS_ATTR_LIST_ITEM)((PUCHAR)Item + Item->Length);
        }
        FreePool (ListBuf);
    }
    return NULL;
}

/*
 * Resolve which MFT record actually owns a directory's $I30 index attributes.
 *
 * A large or heavily fragmented directory overflows its base MFT record, and
 * Windows then relocates whole attributes into $ATTRIBUTE_LIST extension
 * records. Measured on a real near-full volume: the root's base record kept
 * only $STANDARD_INFORMATION + $FILE_NAME while $INDEX_ROOT, $INDEX_ALLOCATION
 * and $BITMAP (all :$I30) had moved together into one extension record.
 *
 * Every index-mutating path in this driver edits ONE record in memory and
 * writes that record back, so they only need to be handed the record that owns
 * the index instead of the base record. That is what this resolves.
 *
 * REFUSES (EFI_UNSUPPORTED, nothing modified, nothing allocated) when the $I30
 * attributes are not all in a single record - editing a partial view of the
 * index would corrupt the directory, and a clean refusal lets the caller roll
 * back instead.
 *
 * On success Host->Rec owns $INDEX_ROOT:$I30, Host->MFTIndex is its record
 * number (pass THIS to NtfsEfiWriteFileRecord - never the record's own
 * MFTRecordNumber field, which is untrusted on-disk data), Host->HasAlloc says
 * whether the directory already has $INDEX_ALLOCATION:$I30 (large index), and
 * Host->Own is TRUE when Rec is a separately allocated extension-record buffer
 * the caller must FreePool. When Own is FALSE, Rec aliases BaseRec.
 */
EFI_STATUS
NtfsEfiResolveIndexHost (
    IN  PNTFS_EFI_VCB        Vcb,
    IN  PFILE_RECORD_HEADER  BaseRec,
    IN  ULONGLONG            BaseMFT,
    OUT PNTFS_INDEX_HOST     Host
    )
{
    PNTFS_ATTR_LIST_ITEM Item;
    PUCHAR               ListEnd;
    UINT64               ListLen;
    PUCHAR               ListBuf;
    PNTFS_ATTR_CTX       ListCtx;
    ULONGLONG RootMFT   = (ULONGLONG)-1LL;
    ULONGLONG AllocMFT  = (ULONGLONG)-1LL;
    ULONGLONG BitmapMFT = (ULONGLONG)-1LL;

    Host->Rec = NULL; Host->MFTIndex = 0; Host->Own = FALSE;
    Host->RootRec = NULL; Host->RootMFTIndex = 0; Host->RootOwn = FALSE;
    Host->HasAlloc = FALSE;

    ListCtx = NtfsEfiFindAttrInRecord (Vcb, BaseRec, AttributeAttributeList, NULL, 0, NULL);
    if (ListCtx == NULL) {
        /* no $ATTRIBUTE_LIST: everything this file has is in the base record */
        PNTFS_ATTR_CTX Ctx = NtfsEfiFindAttrInRecord (Vcb, BaseRec, AttributeIndexRoot, L"$I30", 4, NULL);
        if (Ctx == NULL) return EFI_UNSUPPORTED;      /* not a directory */
        NtfsEfiFreeAttrCtx (Ctx);
        Ctx = NtfsEfiFindAttrInRecord (Vcb, BaseRec, AttributeIndexAllocation, L"$I30", 4, NULL);
        if (Ctx != NULL) { Host->HasAlloc = TRUE; NtfsEfiFreeAttrCtx (Ctx); }
        Host->Rec     = BaseRec; Host->MFTIndex     = BaseMFT; Host->Own     = FALSE;
        Host->RootRec = BaseRec; Host->RootMFTIndex = BaseMFT; Host->RootOwn = FALSE;
        return EFI_SUCCESS;
    }

    ListLen = NtfsEfiAttrDataLength (ListCtx);
    if (ListLen < sizeof (NTFS_ATTR_LIST_ITEM) || ListLen > (UINT64)(1024 * 1024)) {
        NtfsEfiFreeAttrCtx (ListCtx);
        return EFI_VOLUME_CORRUPTED;
    }
    ListBuf = AllocatePool ((UINTN)ListLen);
    if (ListBuf == NULL) { NtfsEfiFreeAttrCtx (ListCtx); return EFI_OUT_OF_RESOURCES; }
    if (NtfsEfiReadAttr (Vcb, ListCtx, 0, (PCHAR)ListBuf, (ULONG)ListLen) != (ULONG)ListLen) {
        FreePool (ListBuf);
        NtfsEfiFreeAttrCtx (ListCtx);
        return EFI_VOLUME_CORRUPTED;
    }
    NtfsEfiFreeAttrCtx (ListCtx);

    Item    = (PNTFS_ATTR_LIST_ITEM)ListBuf;
    ListEnd = ListBuf + ListLen;

    while ((PUCHAR)Item + sizeof (NTFS_ATTR_LIST_ITEM) <= ListEnd &&
           Item->Type != (ULONG)AttributeEnd) {
        if (Item->Length < sizeof (NTFS_ATTR_LIST_ITEM)) break;   /* malformed */

        if (Item->Type == (ULONG)AttributeIndexRoot ||
            Item->Type == (ULONG)AttributeIndexAllocation ||
            Item->Type == (ULONG)AttributeBitmap) {
            if (Item->NameLength == 4 &&
                (PUCHAR)Item + Item->NameOffset + 4 * sizeof (WCHAR) <= ListEnd &&
                CompareMem ((PWCHAR)((PUCHAR)Item + Item->NameOffset),
                            L"$I30", 4 * sizeof (WCHAR)) == 0) {
                ULONGLONG At = Item->MFTIndex & NTFS_MFT_MASK;
                if (Item->Type == (ULONG)AttributeIndexRoot)       RootMFT   = At;
                if (Item->Type == (ULONG)AttributeIndexAllocation) AllocMFT  = At;
                if (Item->Type == (ULONG)AttributeBitmap)          BitmapMFT = At;
            }
        }
        Item = (PNTFS_ATTR_LIST_ITEM)((PUCHAR)Item + Item->Length);
    }
    FreePool (ListBuf);

    if (RootMFT == (ULONGLONG)-1LL) return EFI_UNSUPPORTED;  /* no $I30 index */

    {
        ULONGLONG HostIdx;

        if (AllocMFT != (ULONGLONG)-1LL) {
            if (BitmapMFT != (ULONGLONG)-1LL && BitmapMFT != AllocMFT) {
                /* the mapping pairs and the allocation bitmap must be editable
                 * together, in one record, or an insert cannot stay consistent */
                return EFI_UNSUPPORTED;
            }
            HostIdx = AllocMFT;
            Host->HasAlloc = TRUE;
        } else {
            HostIdx = RootMFT;
            Host->HasAlloc = FALSE;
        }

        /* the allocation/bitmap host first */
        if (HostIdx == BaseMFT) {
            Host->Rec = BaseRec; Host->MFTIndex = BaseMFT; Host->Own = FALSE;
        } else {
            PFILE_RECORD_HEADER Rec = AllocatePool (Vcb->BytesPerFileRecord);
            if (Rec == NULL) return EFI_OUT_OF_RESOURCES;
            if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, HostIdx, Rec))) {
                FreePool (Rec);
                return EFI_VOLUME_CORRUPTED;
            }
            Host->Rec = Rec; Host->MFTIndex = HostIdx; Host->Own = TRUE;
        }

        /*
         * Then the $INDEX_ROOT host. Alias the buffer we already have whenever it
         * is the same record - two buffers for one record would each be written
         * back whole, and the later write would drop the earlier one's edits.
         */
        if (RootMFT == HostIdx) {
            Host->RootRec = Host->Rec; Host->RootMFTIndex = Host->MFTIndex; Host->RootOwn = FALSE;
        } else if (RootMFT == BaseMFT) {
            Host->RootRec = BaseRec; Host->RootMFTIndex = BaseMFT; Host->RootOwn = FALSE;
        } else {
            PFILE_RECORD_HEADER RRec = AllocatePool (Vcb->BytesPerFileRecord);
            if (RRec == NULL) {
                if (Host->Own) FreePool (Host->Rec);
                Host->Rec = NULL; Host->Own = FALSE;
                return EFI_OUT_OF_RESOURCES;
            }
            if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, RootMFT, RRec))) {
                FreePool (RRec);
                if (Host->Own) FreePool (Host->Rec);
                Host->Rec = NULL; Host->Own = FALSE;
                return EFI_VOLUME_CORRUPTED;
            }
            Host->RootRec = RRec; Host->RootMFTIndex = RootMFT; Host->RootOwn = TRUE;
        }

        /* the record we were pointed at must really carry the attribute claimed */
        {
            PNTFS_ATTR_CTX Chk = NtfsEfiFindAttrInRecord (Vcb, Host->RootRec,
                                     AttributeIndexRoot, L"$I30", 4, NULL);
            if (Chk == NULL) {                       /* list disagrees with record */
                if (Host->RootOwn) FreePool (Host->RootRec);
                if (Host->Own)     FreePool (Host->Rec);
                Host->Rec = NULL; Host->RootRec = NULL;
                Host->Own = FALSE; Host->RootOwn = FALSE;
                return EFI_VOLUME_CORRUPTED;
            }
            NtfsEfiFreeAttrCtx (Chk);
        }
        return EFI_SUCCESS;
    }
}
