/**
 * ntfs_create.c - new (empty) file and directory creation: MFT record
 * allocation, $STANDARD_INFORMATION/$FILE_NAME/$DATA construction, and
 * insertion into the parent directory's index - plus the full B+tree insert
 * engine that keeps that index valid at any size.
 *
 * Directory index insert handles every shape NTFS.sys produces:
 *  - $INDEX_ROOT only (small directory): flat sorted array insert in place
 *    (NtfsInsertIndexEntrySmall).
 *  - $INDEX_ROOT outgrown: the root is converted to a single $INDEX_ALLOCATION
 *    INDX block (NtfsConvertRootToSingleIndexAllocation).
 *  - $INDEX_ALLOCATION present: recursive descent to the leaf, leaf/internal
 *    node splitting with separator promotion (NtfsBtreeInsertRec), and root
 *    push-down when the root itself overflows (NtfsBtreePushDownRoot), so the
 *    tree grows in depth exactly the way NTFS does.
 *  - index attributes relocated into an $ATTRIBUTE_LIST extension record:
 *    NtfsEfiResolveIndexHost hands the insert the record that actually owns
 *    $INDEX_ROOT:$I30 (see NtfsInsertIndexEntry).
 * $MFT itself grows on demand (NtfsGrowMft, ntfs_bitmap.c) when no free record
 * is left, so create is not bounded by the volume's initial $MFT size.
 *
 * Remaining restrictions:
 *  - no DOS 8.3 alias generated (single POSIX $FILE_NAME).
 *  - single path component only (caller resolves the parent first).
 *  - the new file's own attributes must fit its (fully resident) base MFT
 *    record - a fresh create never needs an extension record.
 *  - index attributes spread over several extension records are refused
 *    cleanly rather than edited.
 * Every failure path rolls back any MFT record already allocated
 * (NtfsRollbackNewRecord: clear FRH_IN_USE, bump sequence, zero LinkCount,
 * then release the $MFT $BITMAP bit), so a rejected create leaves neither
 * leaked space nor a chkdsk complaint.
 */

#include "ntfs.h"

#define NTFS_FILENAME_FIXED_SIZE 66   /* FILENAME_ATTRIBUTE up to Name[0] */
#define NTFS_ATTR_HEADER_RESIDENT_SIZE 24  /* Type..Instance(16) + Resident sub-header(8) */

static UINTN
NtfsBuildFileNameAttr (
    OUT PFILENAME_ATTRIBUTE Fn,
    IN  UINT64               ParentRef,
    IN  UINT64               NowNtfs,
    IN  CONST WCHAR         *Name,
    IN  UINTN                NameLen,
    IN  ULONG                FileAttributes
    )
{
    ZeroMem (Fn, NTFS_FILENAME_FIXED_SIZE + NameLen * sizeof (WCHAR));
    Fn->DirectoryFileReferenceNumber = ParentRef;
    Fn->CreationTime   = NowNtfs;
    Fn->ChangeTime     = NowNtfs;
    Fn->LastWriteTime  = NowNtfs;
    Fn->LastAccessTime = NowNtfs;
    Fn->AllocatedSize  = 0;
    Fn->DataSize       = 0;
    Fn->FileAttributes = FileAttributes;
    Fn->NameLength     = (UCHAR)NameLen;
    Fn->NameType       = NTFS_FILE_NAME_POSIX;
    CopyMem (Fn->Name, Name, NameLen * sizeof (WCHAR));
    return NTFS_FILENAME_FIXED_SIZE + NameLen * sizeof (WCHAR);
}

static INTN
NtfsCreateCompareNames (
    IN CONST WCHAR  *A,
    IN UINTN         ALen,
    IN CONST WCHAR  *B,
    IN UINTN         BLen,
    IN CONST USHORT *Upcase
    )
{
    UINTN i;
    UINTN MinLen = (ALen < BLen) ? ALen : BLen;

    for (i = 0; i < MinLen; i++) {
        WCHAR Ca = (WCHAR)Upcase[(USHORT)A[i]];
        WCHAR Cb = (WCHAR)Upcase[(USHORT)B[i]];
        if (Ca != Cb) return (INTN)Ca - (INTN)Cb;
    }
    if (ALen != BLen) return (ALen < BLen) ? -1 : 1;
    return 0;
}

static VOID
NtfsBuildIndexEntry (
    OUT PINDEX_ENTRY_ATTRIBUTE NewEntry,
    IN  UINT64                 ChildRef,
    IN  UINT64                 ParentRef,
    IN  UINT64                 NowNtfs,
    IN  CONST WCHAR           *Name,
    IN  UINTN                  NameLen,
    IN  UINT64                 AllocSize,
    IN  UINT64                 DataSize,
    IN  ULONG                  FileAttributes
    )
{
    UINT64 KeyLen = NTFS_FILENAME_FIXED_SIZE + NameLen * sizeof (WCHAR);
    UINT64 EntryLen = ROUND_UP (16 + KeyLen, 8);

    ZeroMem (NewEntry, (UINTN)EntryLen);
    NewEntry->Data.Directory.IndexedFile = ChildRef;
    NewEntry->Length    = (USHORT)EntryLen;
    NewEntry->KeyLength = (USHORT)KeyLen;
    NewEntry->Flags     = 0;
    NewEntry->Reserved  = 0;
    NtfsBuildFileNameAttr (&NewEntry->FileName, ParentRef, NowNtfs, Name, NameLen, FileAttributes);
    NewEntry->FileName.AllocatedSize = AllocSize;
    NewEntry->FileName.DataSize      = DataSize;
}

static EFI_STATUS
NtfsInsertIntoIndexEntries (
    IN OUT INDEX_HEADER_ATTRIBUTE *Header,
    IN     PINDEX_ENTRY_ATTRIBUTE  NewEntry,
    IN     CONST WCHAR            *Name,
    IN     UINTN                   NameLen,
    IN     CONST USHORT           *Upcase
    )
{
    PINDEX_ENTRY_ATTRIBUTE Entry;
    PINDEX_ENTRY_ATTRIBUTE Last;
    PINDEX_ENTRY_ATTRIBUTE InsertBefore;
    UINTN                  EntryLen = NewEntry->Length;
    UINTN                  InsertOffset;
    UINTN                  OldBytesToMove;

    Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Header + Header->FirstEntryOffset);
    Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Header + Header->TotalSizeOfEntries);
    InsertBefore = Last;

    while ((PUCHAR)Entry < (PUCHAR)Last) {
        if (Entry->Length == 0) return EFI_VOLUME_CORRUPTED;
        if (Entry->Flags & NTFS_INDEX_ENTRY_END) {
            if (Entry->Flags & NTFS_INDEX_ENTRY_NODE) return EFI_UNSUPPORTED;
            InsertBefore = Entry;
            break;
        }

        {
            INTN Cmp = NtfsCreateCompareNames (Name, NameLen,
                            Entry->FileName.Name, Entry->FileName.NameLength, Upcase);
            if (Cmp == 0) return EFI_ACCESS_DENIED;
            if (Cmp < 0) {
                if (Entry->Flags & NTFS_INDEX_ENTRY_NODE) return EFI_UNSUPPORTED;
                InsertBefore = Entry;
                break;
            }
        }

        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
    }

    if (Header->TotalSizeOfEntries + EntryLen > Header->AllocatedSize) {
        return EFI_UNSUPPORTED;
    }

    InsertOffset = (UINTN)((PUCHAR)InsertBefore - (PUCHAR)Header);
    OldBytesToMove = Header->TotalSizeOfEntries - InsertOffset;
    NtfsEfiShiftForward ((PUCHAR)Header + InsertOffset, OldBytesToMove, EntryLen);
    CopyMem ((PUCHAR)Header + InsertOffset, NewEntry, EntryLen);

    Header->TotalSizeOfEntries += (ULONG)EntryLen;
    return EFI_SUCCESS;
}

static EFI_STATUS
NtfsInsertIntoIndexAllocationNode (
    IN PNTFS_EFI_VCB          Vcb,
    IN PNTFS_ATTR_CTX         IndexAllocCtx,
    IN ULONGLONG              VCN,
    IN PINDEX_ENTRY_ATTRIBUTE NewEntry,
    IN CONST WCHAR           *Name,
    IN UINTN                  NameLen
    )
{
    PUCHAR                 IndexBuf;
    PINDEX_BUFFER          Block;
    PINDEX_ENTRY_ATTRIBUTE Entry;
    PINDEX_ENTRY_ATTRIBUTE Last;
    EFI_STATUS             Status = EFI_UNSUPPORTED;

    IndexBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (IndexBuf == NULL) return EFI_OUT_OF_RESOURCES;

    if (NtfsEfiReadAttr (Vcb, IndexAllocCtx, VCN * Vcb->BytesPerCluster,
                         (PCHAR)IndexBuf, Vcb->BytesPerIndexRecord) != Vcb->BytesPerIndexRecord) {
        FreePool (IndexBuf);
        return EFI_DEVICE_ERROR;
    }

    Block = (PINDEX_BUFFER)IndexBuf;
    /* NtfsInsertIntoIndexEntries below shifts bytes forward inside this buffer
     * using Header.AllocatedSize as its room check - all three header fields
     * are on-disk data, so they get validated before any of that */
    if (Block->Ntfs.Type != NRH_INDX_TYPE ||
        EFI_ERROR (NtfsEfiFixupRecord (Vcb, &Block->Ntfs)) ||
        !NtfsEfiIndexBlockOk (Vcb, Block)) {
        FreePool (IndexBuf);
        return EFI_VOLUME_CORRUPTED;
    }

    Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
    Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries);

    while ((PUCHAR)Entry < (PUCHAR)Last) {
        if (Entry->Length == 0) {
            Status = EFI_VOLUME_CORRUPTED;
            break;
        }

        if (Entry->Flags & NTFS_INDEX_ENTRY_END) {
            if (Entry->Flags & NTFS_INDEX_ENTRY_NODE) {
                ULONGLONG SubVCN = *(PULONGLONG)((PUCHAR)Entry + Entry->Length - sizeof (ULONGLONG));
                Status = NtfsInsertIntoIndexAllocationNode (Vcb, IndexAllocCtx, SubVCN,
                            NewEntry, Name, NameLen);
            } else {
                Status = NtfsInsertIntoIndexEntries (&Block->Header, NewEntry, Name, NameLen, Vcb->UpcaseTable);
                if (!EFI_ERROR (Status)) {
                    Status = NtfsEfiWriteMultiSectorRecord (Vcb, IndexAllocCtx,
                                VCN * Vcb->BytesPerCluster, &Block->Ntfs,
                                Vcb->BytesPerIndexRecord);
                }
            }
            break;
        }

        {
            INTN Cmp = NtfsCreateCompareNames (Name, NameLen,
                            Entry->FileName.Name, Entry->FileName.NameLength, Vcb->UpcaseTable);
            if (Cmp == 0) {
                Status = EFI_ACCESS_DENIED;
                break;
            }
            if (Cmp < 0) {
                if (Entry->Flags & NTFS_INDEX_ENTRY_NODE) {
                    ULONGLONG SubVCN = *(PULONGLONG)((PUCHAR)Entry + Entry->Length - sizeof (ULONGLONG));
                    Status = NtfsInsertIntoIndexAllocationNode (Vcb, IndexAllocCtx, SubVCN,
                                NewEntry, Name, NameLen);
                } else {
                    Status = NtfsInsertIntoIndexEntries (&Block->Header, NewEntry, Name, NameLen, Vcb->UpcaseTable);
                    if (!EFI_ERROR (Status)) {
                        Status = NtfsEfiWriteMultiSectorRecord (Vcb, IndexAllocCtx,
                                    VCN * Vcb->BytesPerCluster, &Block->Ntfs,
                                    Vcb->BytesPerIndexRecord);
                    }
                }
                break;
            }
        }

        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
    }

    FreePool (IndexBuf);
    return Status;
}

/* Cluster (VCN) -> LCN over a decoded run list. ClustersPerIndexRecord is 1
 * on the volumes we target, so leaf VCN == cluster. */
static INT64
NtfsVcnToLcn (
    IN PNTFS_ATTR_CTX Ctx,
    IN UINT64         Vcn
    )
{
    UINT64 Base = 0;
    ULONG  i;
    for (i = 0; i < Ctx->RunCount; i++) {
        if (Vcn < Base + Ctx->Runs[i].Len) {
            if (Ctx->Runs[i].LBN == -1LL) return -1;
            return Ctx->Runs[i].LBN + (INT64)(Vcn - Base);
        }
        Base += Ctx->Runs[i].Len;
    }
    return -1;
}

/* Initialise an empty INDX block buffer (header + a single END entry). */
static VOID
NtfsInitIndexBlock (
    IN PNTFS_EFI_VCB Vcb,
    IN PUCHAR        Buf,
    IN UINT64        Vcn
    )
{
    PINDEX_BUFFER          Block   = (PINDEX_BUFFER)Buf;
    USHORT                 UsaCount = (USHORT)(Vcb->BytesPerIndexRecord / Vcb->BytesPerSector) + 1;
    PINDEX_ENTRY_ATTRIBUTE End;

    ZeroMem (Buf, Vcb->BytesPerIndexRecord);
    Block->Ntfs.Type      = NRH_INDX_TYPE;
    Block->Ntfs.UsaOffset = 0x28;
    Block->Ntfs.UsaCount  = UsaCount;
    Block->Ntfs.Lsn       = 0;
    Block->VCN            = Vcn;
    Block->Header.FirstEntryOffset = (ULONG)ROUND_UP (
        (UINTN)((PUCHAR)Buf + Block->Ntfs.UsaOffset + UsaCount * sizeof (USHORT) -
                (PUCHAR)&Block->Header), ATTR_RECORD_ALIGNMENT);
    Block->Header.TotalSizeOfEntries = Block->Header.FirstEntryOffset + 16;
    Block->Header.AllocatedSize = Vcb->BytesPerIndexRecord -
        (ULONG)((PUCHAR)&Block->Header - (PUCHAR)Buf);
    Block->Header.Flags = 0;

    End = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
    End->Length = 16;
    End->KeyLength = 0;
    End->Flags = NTFS_INDEX_ENTRY_END;
}

/* Apply the update-sequence array and write an INDX block to a raw LCN. */
static EFI_STATUS
NtfsWriteIndexBlockAtLcn (
    IN PNTFS_EFI_VCB Vcb,
    IN PUCHAR        Buf,
    IN INT64         Lcn
    )
{
    PINDEX_BUFFER Block = (PINDEX_BUFFER)Buf;
    PUSHORT       Usa   = (PUSHORT)(Buf + Block->Ntfs.UsaOffset);
    USHORT        i;

    Usa[0]++;
    if (Usa[0] == 0 || Usa[0] == 0xFFFF) Usa[0] = 1;
    for (i = 1; i < Block->Ntfs.UsaCount; i++) {
        PUSHORT SectorEnd = (PUSHORT)(Buf + i * Vcb->BytesPerSector - sizeof (USHORT));
        Usa[i] = *SectorEnd;
        *SectorEnd = Usa[0];
    }
    return NtfsEfiWriteDisk (Vcb, (UINT64)Lcn * Vcb->BytesPerCluster,
                             Vcb->BytesPerIndexRecord, Buf);
}

/* Append one contiguous cluster run to a non-resident attribute's mapping
 * pairs, growing the MFT record. Mirrors ntfs_file.c's helper; caller frees
 * the clusters back on failure. */
/*
 * Extend a non-resident attribute by one already-allocated contiguous run,
 * MERGING it into the previous run when physically contiguous so the mapping
 * pairs stay compact (critical: without merging, a directory index or a
 * growing $MFT accrues one mapping pair per cluster and eventually overflows
 * the 1 KB MFT record - the "volume full at 107 MB free" bug). The whole
 * mapping-pair stream is re-encoded from the decoded run list. LastRealLCN is
 * unused now (kept for call-site compatibility). On failure (no record room
 * for a genuinely new pair) the caller frees the clusters back.
 */
BOOLEAN
NtfsAppendRunToAttr (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER Rec,
    IN ULONG                AttrOffset,
    IN UINT64               StartLCN,
    IN UINT64               RunClusters,
    IN INT64                LastRealLCN
    )
{
    PNTFS_ATTR_RECORD Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + AttrOffset);
    NTFS_RUN_ENTRY   *Runs;
    PUCHAR            MP;
    ULONG             RunCount = 0, i;
    UINTN             MPLen = 0;
    INT64             PrevLBN = 0;
    ULONG             OldAttrLen, NewAttrLen;
    LONG              GrowthBytes;
    PUCHAR            NextAttr;
    UINTN             TailLen;
    BOOLEAN           Ok = FALSE;

    (VOID)LastRealLCN;

    Runs = AllocatePool (NTFS_MAX_RUNS * sizeof (NTFS_RUN_ENTRY));
    MP   = AllocatePool (NTFS_MAX_RUNS * 9 + 8);
    if (Runs == NULL || MP == NULL) goto Cleanup;

    if (EFI_ERROR (NtfsBuildRunList (Attr, Runs, NTFS_MAX_RUNS, &RunCount))) goto Cleanup;

    /* merge with the tail run if contiguous, else append a new run */
    if (RunCount > 0 && Runs[RunCount - 1].LBN != -1LL &&
        (UINT64)(Runs[RunCount - 1].LBN + Runs[RunCount - 1].Len) == StartLCN) {
        Runs[RunCount - 1].Len += RunClusters;
    } else {
        if (RunCount >= NTFS_MAX_RUNS) goto Cleanup;
        Runs[RunCount].LBN = (INT64)StartLCN;
        Runs[RunCount].Len = RunClusters;
        RunCount++;
    }

    /* re-encode the whole mapping-pair stream */
    for (i = 0; i < RunCount; i++) {
        if (Runs[i].LBN == -1LL) goto Cleanup;   /* sparse not expected here */
        MPLen += NtfsEncodeRunEntry (MP + MPLen, Runs[i].Len, Runs[i].LBN - PrevLBN);
        PrevLBN = Runs[i].LBN;
    }
    MP[MPLen++] = 0;   /* terminator */

    OldAttrLen  = Attr->Length;
    NewAttrLen  = (ULONG)ROUND_UP (Attr->NonResident.MappingPairsOffset + MPLen, ATTR_RECORD_ALIGNMENT);
    GrowthBytes = (LONG)NewAttrLen - (LONG)OldAttrLen;
    if (GrowthBytes > 0 && (UINT64)GrowthBytes > (UINT64)Rec->BytesAllocated - Rec->BytesInUse) goto Cleanup;

    NextAttr = (PUCHAR)Attr + OldAttrLen;
    TailLen  = Rec->BytesInUse - (ULONG)(NextAttr - (PUCHAR)Rec);
    if (GrowthBytes != 0) CopyMem (NextAttr + GrowthBytes, NextAttr, TailLen);   /* memmove-safe */

    CopyMem ((PUCHAR)Attr + Attr->NonResident.MappingPairsOffset, MP, MPLen);
    Attr->Length                    = NewAttrLen;
    Attr->NonResident.HighestVCN   += RunClusters;
    Attr->NonResident.AllocatedSize += (LONGLONG)(RunClusters * Vcb->BytesPerCluster);
    Rec->BytesInUse                 = (ULONG)((LONG)Rec->BytesInUse + GrowthBytes);
    Ok = TRUE;

Cleanup:
    if (Runs) FreePool (Runs);
    if (MP)   FreePool (MP);
    return Ok;
}

/* Read the sub-node VCN a NODE index entry points at (last 8 bytes). */
#define NTFS_ENTRY_SUBVCN(e)  (*(UINT64 *)((PUCHAR)(e) + (e)->Length - sizeof (UINT64)))

/* Allocate one fresh INDX-block cluster and splice it into the directory's
 * $INDEX_ALLOCATION (mapping pairs + sizes) and $BITMAP:$I30. Chains the
 * mapping-pair delta base through *LastRealLCN across a whole insert. On any
 * failure the cluster is freed and the record left as it was. */
static EFI_STATUS
NtfsBtreeAllocBlock (
    IN     PNTFS_EFI_VCB       Vcb,
    IN OUT PFILE_RECORD_HEADER DirRec,
    IN     ULONG                AllocOffset,
    IN OUT INT64               *LastRealLCN,
    OUT    UINT64              *NewVCN,
    OUT    INT64               *NewLcn
    )
{
    UINT64            ClustersPer = Vcb->BytesPerIndexRecord / Vcb->BytesPerCluster;
    UINT64            StartLCN, Got;
    PNTFS_ATTR_RECORD AllocAttr = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + AllocOffset);
    ULONG             BmOff = 0;
    PNTFS_ATTR_CTX    BmProbe;
    PNTFS_ATTR_RECORD BmAttr;
    PUCHAR            BmVal;

    *NewVCN = (UINT64)AllocAttr->NonResident.HighestVCN + 1;
    if (EFI_ERROR (NtfsEfiAllocateClusters (Vcb, ClustersPer, &StartLCN, &Got)) || Got < ClustersPer) {
        if (Got) NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        return EFI_VOLUME_FULL;
    }
    *NewLcn = (INT64)StartLCN;
    if (!NtfsAppendRunToAttr (Vcb, DirRec, AllocOffset, StartLCN, ClustersPer, *LastRealLCN)) {
        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        return EFI_UNSUPPORTED;   /* record has no room for another mapping pair */
    }
    *LastRealLCN = (INT64)StartLCN;
    AllocAttr = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + AllocOffset);   /* stable: before $BITMAP */
    AllocAttr->NonResident.DataSize        = (LONGLONG)((*NewVCN + 1) * Vcb->BytesPerIndexRecord);
    AllocAttr->NonResident.InitializedSize = AllocAttr->NonResident.DataSize;

    /* the append shifted $BITMAP forward - re-find it */
    BmProbe = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeBitmap, L"$I30", 4, &BmOff);
    if (BmProbe == NULL) { NtfsEfiFreeClusters (Vcb, StartLCN, Got); return EFI_VOLUME_CORRUPTED; }
    NtfsEfiFreeAttrCtx (BmProbe);
    BmAttr = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + BmOff);
    if (BmAttr->IsNonResident || *NewVCN / 8 >= BmAttr->Resident.ValueLength) {
        /* grow the resident $BITMAP:$I30 to cover the new block's bit (64 more
         * bits per 8 bytes). $BITMAP is the last attribute before the record
         * END marker, so growing it only shifts that 4-byte terminator. A
         * non-resident index bitmap (enormous directories) is not handled. */
        ULONG NewBmValLen = (ULONG)ROUND_UP (*NewVCN / 8 + 1, 8);
        if (BmAttr->IsNonResident ||
            !NtfsEfiGrowResidentInRecord (Vcb, DirRec, BmOff, NewBmValLen)) {
            NtfsEfiFreeClusters (Vcb, StartLCN, Got);
            return EFI_UNSUPPORTED;
        }
        BmAttr = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + BmOff);   /* start unchanged */
    }
    BmVal = (PUCHAR)BmAttr + BmAttr->Resident.ValueOffset;
    BmVal[*NewVCN / 8] |= (UCHAR)(1U << (*NewVCN % 8));
    return EFI_SUCCESS;
}

/* Lay down a fresh INDX block for Vcn holding Ord[Start..Start+Count-1] then a
 * terminating END entry. EndSubVcn >= 0 makes the block an internal node whose
 * END carries that right-child pointer; < 0 makes it a leaf. */
static VOID
NtfsBuildBlockFromEntries (
    IN PNTFS_EFI_VCB           Vcb,
    IN PUCHAR                  Buf,
    IN UINT64                  Vcn,
    IN PINDEX_ENTRY_ATTRIBUTE *Ord,
    IN UINTN                   Start,
    IN UINTN                   Count,
    IN INT64                   EndSubVcn
    )
{
    PINDEX_BUFFER          Block = (PINDEX_BUFFER)Buf;
    PUCHAR                 p;
    PINDEX_ENTRY_ATTRIBUTE End;
    UINTN                  i;

    NtfsInitIndexBlock (Vcb, Buf, Vcn);
    p = (PUCHAR)&Block->Header + Block->Header.FirstEntryOffset;
    for (i = 0; i < Count; i++) {
        CopyMem (p, Ord[Start + i], Ord[Start + i]->Length);
        p += Ord[Start + i]->Length;
    }
    End = (PINDEX_ENTRY_ATTRIBUTE)p;
    ZeroMem (End, 24);
    End->KeyLength = 0;
    if (EndSubVcn >= 0) {
        End->Length = 24;
        End->Flags  = NTFS_INDEX_ENTRY_END | NTFS_INDEX_ENTRY_NODE;
        *(UINT64 *)((PUCHAR)End + 24 - sizeof (UINT64)) = (UINT64)EndSubVcn;
        p += 24;
        Block->Header.Flags = INDEX_NODE_LARGE;   /* internal node: has children */
    } else {
        End->Length = 16;
        End->Flags  = NTFS_INDEX_ENTRY_END;
        p += 16;
        Block->Header.Flags = 0;                  /* leaf node */
    }
    Block->Header.TotalSizeOfEntries = (ULONG)(p - (PUCHAR)&Block->Header);
}

/* Build the median separator promoted on a split: a NODE entry carrying
 * Src's key with sub-node pointer = LeftVCN. Src may be a leaf entry (no
 * sub-VCN yet, +8 bytes) or an internal entry (already sized for one). */
static ULONG
NtfsMakeSeparator (
    OUT PUCHAR                 SepBuf,
    IN  PINDEX_ENTRY_ATTRIBUTE Src,
    IN  BOOLEAN                SrcIsInternal,
    IN  UINT64                 LeftVCN
    )
{
    PINDEX_ENTRY_ATTRIBUTE Sep = (PINDEX_ENTRY_ATTRIBUTE)SepBuf;
    ULONG SepLen = SrcIsInternal ? Src->Length
                                 : (ULONG)ROUND_UP (Src->Length + sizeof (UINT64), ATTR_RECORD_ALIGNMENT);
    ZeroMem (SepBuf, SepLen);
    CopyMem (SepBuf, Src, Src->Length);
    Sep->Length = (USHORT)SepLen;
    Sep->Flags |= NTFS_INDEX_ENTRY_NODE;
    *(UINT64 *)(SepBuf + SepLen - sizeof (UINT64)) = LeftVCN;
    return SepLen;
}

/*
 * Recursive B-tree insert into the subtree rooted at NodeVCN (an INDX block).
 *   *DidSplit == FALSE: entry inserted, node rewritten in place, no promotion.
 *   *DidSplit == TRUE : NodeVCN split; SepOut/*SepLenOut hold the median as a
 *                       NODE separator (sub-VCN already = NodeVCN, the left
 *                       half) and *RightVCNOut is the new right sibling. The
 *                       caller must repoint whatever pointed at NodeVCN to the
 *                       right sibling and insert the separator before it.
 * Cluster allocations mutate DirRec (runlist/bitmap); the record is written
 * once by the top-level caller. INDX blocks are written here as they settle.
 */
static EFI_STATUS
NtfsBtreeInsertRec (
    IN     PNTFS_EFI_VCB          Vcb,
    IN OUT PFILE_RECORD_HEADER    DirRec,
    IN     ULONG                   AllocOffset,
    IN     PNTFS_ATTR_CTX          AllocCtx,
    IN     UINT64                  NodeVCN,
    IN     PINDEX_ENTRY_ATTRIBUTE  NewEntry,
    IN     CONST WCHAR            *Name,
    IN     UINTN                   NameLen,
    IN OUT INT64                  *LastRealLCN,
    IN     UINTN                   Depth,
    OUT    BOOLEAN                *DidSplit,
    OUT    PUCHAR                  SepOut,
    OUT    ULONG                  *SepLenOut,
    OUT    UINT64                 *RightVCNOut
    )
{
    PUCHAR                 NodeBuf = NULL, LeftBuf = NULL, RightBuf = NULL, WorkBuf = NULL;
    PINDEX_BUFFER          Blk;
    PINDEX_ENTRY_ATTRIBUTE E, EndE, Ord[512];
    UINTN                  NOrd, Half;
    INT64                  NodeLcn, NewLcn;
    UINT64                 NewVCN;
    BOOLEAN                IsInternal;
    EFI_STATUS             Status;

    *DidSplit = FALSE;
    /*
     * Height guard. Every level of this recursion carries an Ord[512] pointer
     * array (4 KiB) plus a ~600-byte separator buffer ON THE STACK, so the old
     * limit of 24 allowed ~120 KiB of stack - more than a DXE-phase driver can
     * assume it has. A B+tree of 4 KiB index blocks holds tens of entries per
     * node, so depth 12 already covers directories with billions of entries;
     * anything deeper is a corrupt/looping index, not a real directory.
     */
    if (Depth > 12) return EFI_UNSUPPORTED;

    NodeBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (NodeBuf == NULL) return EFI_OUT_OF_RESOURCES;
    if (NtfsEfiReadAttr (Vcb, AllocCtx, NodeVCN * Vcb->BytesPerCluster,
                         (PCHAR)NodeBuf, Vcb->BytesPerIndexRecord) != Vcb->BytesPerIndexRecord ||
        ((PINDEX_BUFFER)NodeBuf)->Ntfs.Type != NRH_INDX_TYPE ||
        EFI_ERROR (NtfsEfiFixupRecord (Vcb, &((PINDEX_BUFFER)NodeBuf)->Ntfs)) ||
        !NtfsEfiIndexBlockOk (Vcb, (PINDEX_BUFFER)NodeBuf)) {
        FreePool (NodeBuf);
        return EFI_VOLUME_CORRUPTED;
    }
    NodeLcn = NtfsVcnToLcn (AllocCtx, NodeVCN);
    if (NodeLcn < 0) { FreePool (NodeBuf); return EFI_VOLUME_CORRUPTED; }

    Blk  = (PINDEX_BUFFER)NodeBuf;
    E    = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Blk->Header + Blk->Header.FirstEntryOffset);
    EndE = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Blk->Header + Blk->Header.TotalSizeOfEntries);

    /* internal iff the END entry carries a sub-node pointer */
    {
        PINDEX_ENTRY_ATTRIBUTE T = E;
        IsInternal = FALSE;
        while ((PUCHAR)T < (PUCHAR)EndE) {
            if (T->Length == 0) { FreePool (NodeBuf); return EFI_VOLUME_CORRUPTED; }
            if (T->Flags & NTFS_INDEX_ENTRY_END) { IsInternal = (T->Flags & NTFS_INDEX_ENTRY_NODE) != 0; break; }
            T = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)T + T->Length);
        }
    }

    if (!IsInternal) {
        /* ---- leaf ---- */
        Status = NtfsInsertIntoIndexEntries (&Blk->Header, NewEntry, Name, NameLen, Vcb->UpcaseTable);
        if (Status == EFI_ACCESS_DENIED) { FreePool (NodeBuf); return Status; }
        if (!EFI_ERROR (Status)) {                       /* fit in place */
            Status = NtfsWriteIndexBlockAtLcn (Vcb, NodeBuf, NodeLcn);
            FreePool (NodeBuf);
            return Status;
        }
        /* full -> gather sorted [entries + NewEntry] and split */
        NOrd = 0;
        {
            BOOLEAN Inserted = FALSE;
            E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Blk->Header + Blk->Header.FirstEntryOffset);
            while ((PUCHAR)E < (PUCHAR)EndE) {
                if (E->Flags & NTFS_INDEX_ENTRY_END) break;
                if (!Inserted &&
                    NtfsCreateCompareNames (Name, NameLen, E->FileName.Name, E->FileName.NameLength, Vcb->UpcaseTable) < 0) {
                    Ord[NOrd++] = NewEntry; Inserted = TRUE;
                }
                if (NOrd >= 510) { FreePool (NodeBuf); return EFI_UNSUPPORTED; }
                Ord[NOrd++] = E;
                E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)E + E->Length);
            }
            if (!Inserted) Ord[NOrd++] = NewEntry;
        }
        if (NOrd < 2) { FreePool (NodeBuf); return EFI_UNSUPPORTED; }
        Half = NOrd / 2;

        Status = NtfsBtreeAllocBlock (Vcb, DirRec, AllocOffset, LastRealLCN, &NewVCN, &NewLcn);
        if (EFI_ERROR (Status)) { FreePool (NodeBuf); return Status; }

        *SepLenOut = NtfsMakeSeparator (SepOut, Ord[Half], FALSE, NodeVCN);
        LeftBuf  = AllocatePool (Vcb->BytesPerIndexRecord);
        RightBuf = AllocatePool (Vcb->BytesPerIndexRecord);
        if (LeftBuf == NULL || RightBuf == NULL) { Status = EFI_OUT_OF_RESOURCES; goto LeafCleanup; }
        NtfsBuildBlockFromEntries (Vcb, LeftBuf,  NodeVCN, Ord, 0,        Half,             -1);
        NtfsBuildBlockFromEntries (Vcb, RightBuf, NewVCN,  Ord, Half + 1, NOrd - Half - 1,  -1);
        Status = NtfsWriteIndexBlockAtLcn (Vcb, LeftBuf, NodeLcn);
        if (!EFI_ERROR (Status)) Status = NtfsWriteIndexBlockAtLcn (Vcb, RightBuf, NewLcn);
        if (!EFI_ERROR (Status)) { *DidSplit = TRUE; *RightVCNOut = NewVCN; }
LeafCleanup:
        if (LeftBuf)  FreePool (LeftBuf);
        if (RightBuf) FreePool (RightBuf);
        FreePool (NodeBuf);
        return Status;
    }

    /* ---- internal ---- find child slot, recurse, then maybe re-split ---- */
    {
        PINDEX_ENTRY_ATTRIBUTE Slot = NULL;
        UINT64                 ChildVCN = 0;
        BOOLEAN                ChildSplit = FALSE;
        UCHAR                  ChildSep[16 + NTFS_FILENAME_FIXED_SIZE + 512 + 8];
        ULONG                  ChildSepLen = 0;
        UINT64                 ChildRight = 0;

        E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Blk->Header + Blk->Header.FirstEntryOffset);
        while ((PUCHAR)E < (PUCHAR)EndE) {
            if (E->Flags & NTFS_INDEX_ENTRY_END) { Slot = E; ChildVCN = NTFS_ENTRY_SUBVCN (E); break; }
            {
                INTN Cmp = NtfsCreateCompareNames (Name, NameLen, E->FileName.Name, E->FileName.NameLength, Vcb->UpcaseTable);
                if (Cmp == 0) { FreePool (NodeBuf); return EFI_ACCESS_DENIED; }
                if (Cmp < 0)  { Slot = E; ChildVCN = NTFS_ENTRY_SUBVCN (E); break; }
            }
            E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)E + E->Length);
        }
        if (Slot == NULL) { FreePool (NodeBuf); return EFI_VOLUME_CORRUPTED; }

        Status = NtfsBtreeInsertRec (Vcb, DirRec, AllocOffset, AllocCtx, ChildVCN,
                     NewEntry, Name, NameLen, LastRealLCN, Depth + 1,
                     &ChildSplit, ChildSep, &ChildSepLen, &ChildRight);
        if (EFI_ERROR (Status) || !ChildSplit) { FreePool (NodeBuf); return Status; }

        /* child split: repoint Slot at ChildRight and insert ChildSep before it.
         * ChildSep's sub-VCN is already ChildVCN (the left half). */
        NTFS_ENTRY_SUBVCN (Slot) = ChildRight;

        if ((UINT64)Blk->Header.TotalSizeOfEntries + ChildSepLen <= Blk->Header.AllocatedSize) {
            UINTN  Off  = (UINTN)((PUCHAR)Slot - (PUCHAR)&Blk->Header);
            UINTN  Tail = Blk->Header.TotalSizeOfEntries - Off;
            NtfsEfiShiftForward ((PUCHAR)&Blk->Header + Off, Tail, ChildSepLen);
            CopyMem ((PUCHAR)&Blk->Header + Off, ChildSep, ChildSepLen);
            Blk->Header.TotalSizeOfEntries += ChildSepLen;
            Status = NtfsWriteIndexBlockAtLcn (Vcb, NodeBuf, NodeLcn);
            FreePool (NodeBuf);
            return Status;
        }

        /* this internal node overflows too -> gather [entries + ChildSep] and split */
        NOrd = 0;
        E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Blk->Header + Blk->Header.FirstEntryOffset);
        while ((PUCHAR)E < (PUCHAR)EndE) {
            if (E->Flags & NTFS_INDEX_ENTRY_END) break;
            if (E == Slot) { Ord[NOrd++] = (PINDEX_ENTRY_ATTRIBUTE)ChildSep; }
            if (NOrd >= 510) { FreePool (NodeBuf); return EFI_UNSUPPORTED; }
            Ord[NOrd++] = E;
            E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)E + E->Length);
        }
        /* Slot may be the END entry (ChildSep goes just before END) */
        if (Slot->Flags & NTFS_INDEX_ENTRY_END) Ord[NOrd++] = (PINDEX_ENTRY_ATTRIBUTE)ChildSep;

        if (NOrd < 2) { FreePool (NodeBuf); return EFI_UNSUPPORTED; }
        Half = NOrd / 2;

        Status = NtfsBtreeAllocBlock (Vcb, DirRec, AllocOffset, LastRealLCN, &NewVCN, &NewLcn);
        if (EFI_ERROR (Status)) { FreePool (NodeBuf); return Status; }

        {
            INT64  MedianOrigSub = (INT64)NTFS_ENTRY_SUBVCN (Ord[Half]);   /* left half's END pointer */
            INT64  OrigEndSub    = (INT64)NTFS_ENTRY_SUBVCN (EndE);         /* right half's END pointer */
            *SepLenOut = NtfsMakeSeparator (SepOut, Ord[Half], TRUE, NodeVCN);
            LeftBuf  = AllocatePool (Vcb->BytesPerIndexRecord);
            RightBuf = AllocatePool (Vcb->BytesPerIndexRecord);
            if (LeftBuf == NULL || RightBuf == NULL) { Status = EFI_OUT_OF_RESOURCES; goto IntCleanup; }
            NtfsBuildBlockFromEntries (Vcb, LeftBuf,  NodeVCN, Ord, 0,        Half,            MedianOrigSub);
            NtfsBuildBlockFromEntries (Vcb, RightBuf, NewVCN,  Ord, Half + 1, NOrd - Half - 1, OrigEndSub);
            Status = NtfsWriteIndexBlockAtLcn (Vcb, LeftBuf, NodeLcn);
            if (!EFI_ERROR (Status)) Status = NtfsWriteIndexBlockAtLcn (Vcb, RightBuf, NewLcn);
            if (!EFI_ERROR (Status)) { *DidSplit = TRUE; *RightVCNOut = NewVCN; }
IntCleanup:
            if (LeftBuf)  FreePool (LeftBuf);
            if (RightBuf) FreePool (RightBuf);
        }
        FreePool (NodeBuf);
        (VOID)WorkBuf;
        return Status;
    }
}

/*
 * Insert a separator into the resident $INDEX_ROOT, repointing whatever
 * pointed at ChildVCN to ChildRight. Grows the root attribute. Returns
 * EFI_UNSUPPORTED (without touching the record) if the root has no room -
 * the caller then does a root push-down and retries against the new node.
 */
static EFI_STATUS
NtfsRootInsertSep (
    IN OUT PFILE_RECORD_HEADER Rec,
    IN     PNTFS_EFI_VCB       Vcb,
    IN     ULONG                RootOffset,
    IN     UINT64               ChildVCN,
    IN     UINT64               ChildRight,
    IN     PUCHAR               Sep,
    IN     ULONG                SepLen
    )
{
    PNTFS_ATTR_RECORD      RA = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + RootOffset);
    PUCHAR                 Val = (PUCHAR)RA + RA->Resident.ValueOffset;
    PINDEX_ROOT_ATTRIBUTE  Ir = (PINDEX_ROOT_ATTRIBUTE)Val;
    PINDEX_ENTRY_ATTRIBUTE R, Slot = NULL;
    UINT64                 InsertOff, NewValLen;

    R = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Ir->Header + Ir->Header.FirstEntryOffset);
    for (;;) {
        if (R->Length == 0) return EFI_VOLUME_CORRUPTED;
        if ((R->Flags & NTFS_INDEX_ENTRY_NODE) && NTFS_ENTRY_SUBVCN (R) == ChildVCN) { Slot = R; break; }
        if (R->Flags & NTFS_INDEX_ENTRY_END) break;
        R = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)R + R->Length);
    }
    if (Slot == NULL) return EFI_VOLUME_CORRUPTED;

    InsertOff = (UINT64)((PUCHAR)Slot - Val);
    NewValLen = 16 + (UINT64)Ir->Header.TotalSizeOfEntries + SepLen;
    if (!NtfsEfiGrowResidentInRecord (Vcb, Rec, RootOffset, NewValLen)) return EFI_UNSUPPORTED;

    RA  = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + RootOffset);
    Val = (PUCHAR)RA + RA->Resident.ValueOffset;
    Ir  = (PINDEX_ROOT_ATTRIBUTE)Val;
    NtfsEfiShiftForward (Val + InsertOff,
        (UINTN)((16 + Ir->Header.TotalSizeOfEntries) - InsertOff), (UINTN)SepLen);
    CopyMem (Val + InsertOff, Sep, SepLen);
    NTFS_ENTRY_SUBVCN ((PINDEX_ENTRY_ATTRIBUTE)(Val + InsertOff + SepLen)) = ChildRight;
    Ir->Header.TotalSizeOfEntries += SepLen;
    Ir->Header.AllocatedSize      += SepLen;
    return EFI_SUCCESS;
}

/*
 * Grow the tree by one level: move every root entry into a fresh internal
 * INDX node W and collapse the root to a single {END+NODE -> W}. The root is
 * copied out and shrunk in place BEFORE W's cluster is allocated, so the
 * mapping-pair append always has record room (the shrink frees ~all of the
 * root's separator bytes). Done proactively when the root is nearly full so
 * later separator promotions on this insert are guaranteed to fit.
 * *AllocOffset is refreshed (the shrink shifts $INDEX_ALLOCATION back).
 */
static EFI_STATUS
NtfsBtreePushDownRoot (
    IN     PNTFS_EFI_VCB       Vcb,
    IN OUT PFILE_RECORD_HEADER DirRec,
    IN     ULONG                RootOffset,
    IN OUT ULONG               *AllocOffset,
    IN OUT INT64               *LastRealLCN
    )
{
    PUCHAR                 WBuf, Temp;
    UINT64                 WVcn;
    INT64                  WLcn, RootEndSub;
    PINDEX_ROOT_ATTRIBUTE  Ir;
    PNTFS_ATTR_RECORD      RA;
    PUCHAR                 Val;
    PINDEX_ENTRY_ATTRIBUTE Ord[512], R, REnd;
    UINTN                  NOrd = 0, EntriesBytes;
    ULONG                  FEO;
    EFI_STATUS             Status;

    RA  = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
    Val = (PUCHAR)RA + RA->Resident.ValueOffset;
    Ir  = (PINDEX_ROOT_ATTRIBUTE)Val;
    FEO = Ir->Header.FirstEntryOffset;
    EntriesBytes = Ir->Header.TotalSizeOfEntries - FEO;

    Temp = AllocatePool (EntriesBytes);
    if (Temp == NULL) return EFI_OUT_OF_RESOURCES;
    CopyMem (Temp, (PUCHAR)&Ir->Header + FEO, EntriesBytes);
    R    = (PINDEX_ENTRY_ATTRIBUTE)Temp;
    REnd = (PINDEX_ENTRY_ATTRIBUTE)(Temp + EntriesBytes);
    while ((PUCHAR)R < (PUCHAR)REnd) {
        if (R->Length == 0) { FreePool (Temp); return EFI_VOLUME_CORRUPTED; }
        if (R->Flags & NTFS_INDEX_ENTRY_END) break;
        if (NOrd >= 510) { FreePool (Temp); return EFI_UNSUPPORTED; }
        Ord[NOrd++] = R;
        R = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)R + R->Length);
    }
    if ((PUCHAR)R >= (PUCHAR)REnd) { FreePool (Temp); return EFI_VOLUME_CORRUPTED; }
    RootEndSub = (INT64)NTFS_ENTRY_SUBVCN (R);

    /* shrink the root in place to {END+NODE->placeholder} */
    {
        ULONG NewValLen  = 16 + FEO + 24;
        ULONG OldAttrLen = RA->Length;
        ULONG NewAttrLen = (ULONG)ROUND_UP (RA->Resident.ValueOffset + NewValLen, ATTR_RECORD_ALIGNMENT);
        LONG  Delta      = (LONG)NewAttrLen - (LONG)OldAttrLen;
        PUCHAR NextAttr  = (PUCHAR)RA + OldAttrLen;
        UINTN  TailLen   = DirRec->BytesInUse - (ULONG)(NextAttr - (PUCHAR)DirRec);
        PINDEX_ENTRY_ATTRIBUTE NewEnd = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Ir->Header + FEO);
        ZeroMem (NewEnd, 24);
        NewEnd->Length = 24;
        NewEnd->Flags  = NTFS_INDEX_ENTRY_END | NTFS_INDEX_ENTRY_NODE;
        NTFS_ENTRY_SUBVCN (NewEnd) = 0;
        Ir->Header.TotalSizeOfEntries = FEO + 24;
        Ir->Header.AllocatedSize      = FEO + 24;
        RA->Resident.ValueLength = NewValLen;
        if (Delta != 0) CopyMem (NextAttr + Delta, NextAttr, TailLen);
        RA->Length = NewAttrLen;
        DirRec->BytesInUse = (ULONG)((LONG)DirRec->BytesInUse + Delta);
    }

    /* re-find $INDEX_ALLOCATION (the shrink shifted it back) */
    {
        ULONG          Fresh = 0;
        PNTFS_ATTR_CTX P = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexAllocation, L"$I30", 4, &Fresh);
        if (P == NULL) { FreePool (Temp); return EFI_VOLUME_CORRUPTED; }
        NtfsEfiFreeAttrCtx (P);
        *AllocOffset = Fresh;
    }

    Status = NtfsBtreeAllocBlock (Vcb, DirRec, *AllocOffset, LastRealLCN, &WVcn, &WLcn);
    if (EFI_ERROR (Status)) { FreePool (Temp); return Status; }

    WBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (WBuf == NULL) { FreePool (Temp); return EFI_OUT_OF_RESOURCES; }
    NtfsBuildBlockFromEntries (Vcb, WBuf, WVcn, Ord, 0, NOrd, RootEndSub);
    Status = NtfsWriteIndexBlockAtLcn (Vcb, WBuf, WLcn);
    FreePool (WBuf);
    FreePool (Temp);
    if (EFI_ERROR (Status)) return Status;

    /* point the root's END entry at W */
    RA  = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
    Val = (PUCHAR)RA + RA->Resident.ValueOffset;
    Ir  = (PINDEX_ROOT_ATTRIBUTE)Val;
    NTFS_ENTRY_SUBVCN ((PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Ir->Header + Ir->Header.FirstEntryOffset)) = WVcn;
    return EFI_SUCCESS;
}

static EFI_STATUS
NtfsInsertIndexAllocationEntry (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER DirRec,
    IN ULONGLONG           DirRecMFT,   /* MFT index OF DirRec (may be an
                                         * $ATTRIBUTE_LIST extension record) */
    IN UINT64              ChildRef,
    IN UINT64              ParentRef,
    IN UINT64              NowNtfs,
    IN CONST WCHAR        *Name,
    IN UINTN               NameLen,
    IN BOOLEAN             IsDirectory
    )
{
    ULONG                  RootOffset = 0, AllocOffset = 0, BitmapOffset = 0;
    PNTFS_ATTR_CTX         RootCtx, AllocCtx;
    PINDEX_ROOT_ATTRIBUTE  IndexRoot;
    PINDEX_ENTRY_ATTRIBUTE Entry, Last;
    UCHAR                  NewEntryBuf[16 + NTFS_FILENAME_FIXED_SIZE + 512];
    PINDEX_ENTRY_ATTRIBUTE NewEntry = (PINDEX_ENTRY_ATTRIBUTE)NewEntryBuf;
    UINT64                 KeyLen = NTFS_FILENAME_FIXED_SIZE + NameLen * sizeof (WCHAR);
    UINT64                 RootChildVCN = 0;
    BOOLEAN                HaveTarget = FALSE, DidSplit = FALSE;
    UCHAR                  Sep[16 + NTFS_FILENAME_FIXED_SIZE + 512 + 8];
    ULONG                  SepLen = 0;
    UINT64                 RightVCN = 0;
    INT64                  LastRealLCN = 0;
    ULONG                  i;
    EFI_STATUS             Status = EFI_UNSUPPORTED;

    if (16 + KeyLen > sizeof (NewEntryBuf)) return EFI_UNSUPPORTED;

    RootCtx = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexRoot, L"$I30", 4, &RootOffset);
    if (RootCtx == NULL) return EFI_UNSUPPORTED;
    /* everything below derives entry pointers and shift lengths from this
     * header's on-disk offsets - reject an implausible one up front */
    if (!NtfsEfiIndexRootOk ((PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset))) {
        NtfsEfiFreeAttrCtx (RootCtx);
        return EFI_VOLUME_CORRUPTED;
    }
    NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexAllocation, L"$I30", 4, &AllocOffset);
    NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeBitmap, L"$I30", 4, &BitmapOffset);
    NtfsEfiFreeAttrCtx (RootCtx);
    AllocCtx = NtfsEfiFindAttribute (Vcb, DirRec, AttributeIndexAllocation, L"$I30", 4, NULL);
    if (AllocCtx == NULL || AllocOffset == 0 || BitmapOffset == 0) {
        if (AllocCtx) NtfsEfiFreeAttrCtx (AllocCtx);
        return EFI_UNSUPPORTED;
    }

    /*
     * Delete-all collapses a formerly large directory back to a resident
     * INDEX_ROOT, but deliberately keeps its INDEX_ALLOCATION mapping and
     * BITMAP attributes.  The clusters are still owned by that attribute;
     * only the allocation bits and the root's child pointer are cleared.
     *
     * On the next create ResolveIndexHost therefore correctly reports that
     * allocation attributes exist, while the resident root has no child to
     * descend into.  Treat that state as a reusable empty large index: reset
     * VCN 0 to an empty INDX block, restore bit 0 and grow the END entry back
     * to END|NODE -> VCN 0.  Without this transition every create after
     * "delete *" returned EFI_UNSUPPORTED until the directory was recreated.
     */
    {
        PNTFS_ATTR_RECORD      RA = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
        PINDEX_ROOT_ATTRIBUTE Ir = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)RA + RA->Resident.ValueOffset);

        if (!(Ir->Header.Flags & INDEX_ROOT_LARGE)) {
            PINDEX_ENTRY_ATTRIBUTE End;
            INT64                  Lcn0;
            PUCHAR                 EmptyBlock;
            PNTFS_ATTR_CTX         Probe;

            End = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Ir->Header + Ir->Header.FirstEntryOffset);
            if (!(End->Flags & NTFS_INDEX_ENTRY_END) || End->Length != 16 ||
                Ir->Header.TotalSizeOfEntries != Ir->Header.FirstEntryOffset + 16) {
                Status = EFI_VOLUME_CORRUPTED;
                goto Done;
            }

            Lcn0 = NtfsVcnToLcn (AllocCtx, 0);
            if (Lcn0 < 0) { Status = EFI_VOLUME_CORRUPTED; goto Done; }

            EmptyBlock = AllocatePool (Vcb->BytesPerIndexRecord);
            if (EmptyBlock == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
            NtfsInitIndexBlock (Vcb, EmptyBlock, 0);
            Status = NtfsWriteIndexBlockAtLcn (Vcb, EmptyBlock, Lcn0);
            FreePool (EmptyBlock);
            if (EFI_ERROR (Status)) goto Done;

            if (!NtfsEfiGrowResidentInRecord (Vcb, DirRec, RootOffset,
                                               RA->Resident.ValueLength + 8)) {
                Status = EFI_UNSUPPORTED;
                goto Done;
            }

            /* The root growth shifted both following attributes. */
            Probe = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexAllocation,
                                             L"$I30", 4, &AllocOffset);
            if (Probe == NULL) { Status = EFI_VOLUME_CORRUPTED; goto Done; }
            NtfsEfiFreeAttrCtx (Probe);
            Probe = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeBitmap,
                                             L"$I30", 4, &BitmapOffset);
            if (Probe == NULL) { Status = EFI_VOLUME_CORRUPTED; goto Done; }
            NtfsEfiFreeAttrCtx (Probe);

            RA  = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
            Ir  = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)RA + RA->Resident.ValueOffset);
            End = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Ir->Header + Ir->Header.FirstEntryOffset);
            ZeroMem (End, 24);
            End->Length = 24;
            End->Flags  = NTFS_INDEX_ENTRY_END | NTFS_INDEX_ENTRY_NODE;
            NTFS_ENTRY_SUBVCN (End) = 0;
            Ir->Header.Flags = INDEX_ROOT_LARGE;
            Ir->Header.TotalSizeOfEntries = Ir->Header.FirstEntryOffset + 24;
            Ir->Header.AllocatedSize      = Ir->Header.TotalSizeOfEntries;

            {
                PNTFS_ATTR_RECORD Bm = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + BitmapOffset);
                if (Bm->IsNonResident || Bm->Resident.ValueLength == 0) {
                    Status = EFI_UNSUPPORTED;
                    goto Done;
                }
                *((PUCHAR)Bm + Bm->Resident.ValueOffset) |= 1;
            }

            Status = NtfsEfiWriteFileRecord (Vcb, DirRecMFT, DirRec);
            if (EFI_ERROR (Status)) goto Done;

            NtfsEfiFreeAttrCtx (AllocCtx);
            AllocCtx = NtfsEfiFindAttribute (Vcb, DirRec, AttributeIndexAllocation,
                                             L"$I30", 4, NULL);
            if (AllocCtx == NULL) { Status = EFI_VOLUME_CORRUPTED; goto Done; }
        }
    }

    NtfsBuildIndexEntry (NewEntry, ChildRef, ParentRef, NowNtfs, Name, NameLen,
        0, 0, IsDirectory ? NTFS_FILE_TYPE_DIRECTORY : NTFS_FILE_TYPE_ARCHIVE);

    /* seed the mapping-pair delta base with the last real run's start LCN */
    for (i = 0; i < AllocCtx->RunCount; i++)
        if (AllocCtx->Runs[i].LBN != -1LL) LastRealLCN = AllocCtx->Runs[i].LBN;

    /* proactive height growth: if the root can't hold a separator for this
     * name (plus margin for the mapping-pair appends each split makes), push
     * it down FIRST so every promotion on the way down is guaranteed to fit
     * and no split stalls with a full record. */
    {
        PNTFS_ATTR_RECORD     RA = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
        PINDEX_ROOT_ATTRIBUTE Ir = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)RA + RA->Resident.ValueOffset);
        UINT64 SepNeed = ROUND_UP (16 + NTFS_FILENAME_FIXED_SIZE + NameLen * sizeof (WCHAR) + sizeof (UINT64),
                                   ATTR_RECORD_ALIGNMENT) + 96;
        /* Only push down when the root actually holds entries to move out. Once
         * the root has been reduced to just its END->child pointer, pushing
         * again would keep stacking empty levels (root has FIXED $SECURITY +
         * $TXF_DATA, so its slack can stay below the padded SepNeed forever) -
         * that stacked descent to Depth>24 was the bug. When the root is already
         * minimal we fall through: the insert descends into the child and any
         * promoted separator lands in the root, which has room for one. */
        if ((Ir->Header.Flags & INDEX_ROOT_LARGE) &&
            (UINT64)(DirRec->BytesAllocated - DirRec->BytesInUse) < SepNeed &&
            (UINT64)Ir->Header.TotalSizeOfEntries > (UINT64)Ir->Header.FirstEntryOffset + 24) {
            Status = NtfsBtreePushDownRoot (Vcb, DirRec, RootOffset, &AllocOffset, &LastRealLCN);
            if (EFI_ERROR (Status)) goto Done;
            /* push-down appended a run for W; re-decode the run list so the
             * descent can read the just-created node (its VCN is new) */
            NtfsEfiFreeAttrCtx (AllocCtx);
            AllocCtx = NtfsEfiFindAttribute (Vcb, DirRec, AttributeIndexAllocation, L"$I30", 4, NULL);
            if (AllocCtx == NULL) { Status = EFI_VOLUME_CORRUPTED; goto Done; }
        }
    }

    /* pick the root child subtree for Name (root is an internal node here) */
    {
        PNTFS_ATTR_RECORD RA = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
        IndexRoot = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)RA + RA->Resident.ValueOffset);
    }
    Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
    Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.TotalSizeOfEntries);
    while ((PUCHAR)Entry < (PUCHAR)Last) {
        if (Entry->Length == 0) { Status = EFI_VOLUME_CORRUPTED; goto Done; }
        if (Entry->Flags & NTFS_INDEX_ENTRY_END) {
            if (Entry->Flags & NTFS_INDEX_ENTRY_NODE) { RootChildVCN = NTFS_ENTRY_SUBVCN (Entry); HaveTarget = TRUE; }
            break;
        }
        {
            INTN Cmp = NtfsCreateCompareNames (Name, NameLen, Entry->FileName.Name, Entry->FileName.NameLength, Vcb->UpcaseTable);
            if (Cmp == 0) { Status = EFI_ACCESS_DENIED; goto Done; }
            if (Cmp < 0) {
                if (Entry->Flags & NTFS_INDEX_ENTRY_NODE) { RootChildVCN = NTFS_ENTRY_SUBVCN (Entry); HaveTarget = TRUE; }
                break;
            }
        }
        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
    }
    if (!HaveTarget) { Status = EFI_UNSUPPORTED; goto Done; }

    Status = NtfsBtreeInsertRec (Vcb, DirRec, AllocOffset, AllocCtx, RootChildVCN,
                 NewEntry, Name, NameLen, &LastRealLCN, 0,
                 &DidSplit, Sep, &SepLen, &RightVCN);
    if (EFI_ERROR (Status)) { Print (L"[allocins] btreeInsertRec -> %r\n", Status); goto Done; }

    if (DidSplit) {
        /* the root's direct child split - land the separator in the root.
         * The proactive push-down above guarantees it fits. */
        Status = NtfsRootInsertSep (DirRec, Vcb, RootOffset, RootChildVCN, RightVCN, Sep, SepLen);
        if (EFI_ERROR (Status)) { Print (L"[allocins] rootInsertSep -> %r\n", Status); goto Done; }
    }

    /* every split allocated clusters that live in DirRec - commit it once.
     * Use the caller-supplied index, not DirRec->MFTRecordNumber: that field is
     * untrusted on-disk data, and DirRec may be an extension record. */
    Status = NtfsEfiWriteFileRecord (Vcb, DirRecMFT, DirRec);

Done:
    if (EFI_ERROR (Status)) Print (L"[allocins] '%s' FINAL -> %r\n", Name, Status);
    NtfsEfiFreeAttrCtx (AllocCtx);
    return Status;
}

static EFI_STATUS
NtfsWriteNewIndexBufferDirect (
    IN PNTFS_EFI_VCB          Vcb,
    IN UINT64                 StartLCN,
    IN PINDEX_ENTRY_ATTRIBUTE FirstOld,
    IN PINDEX_ENTRY_ATTRIBUTE LastOld,
    IN PINDEX_ENTRY_ATTRIBUTE NewEntry,
    IN CONST WCHAR           *Name,
    IN UINTN                  NameLen
    )
{
    PUCHAR                 IndexBuf;
    PINDEX_BUFFER          Block;
    PINDEX_ENTRY_ATTRIBUTE Entry;
    EFI_STATUS             Status;
    PUSHORT                Usa;
    USHORT                 UsaCount;
    USHORT                 i;

    IndexBuf = AllocateZeroPool (Vcb->BytesPerIndexRecord);
    if (IndexBuf == NULL) return EFI_OUT_OF_RESOURCES;

    Block = (PINDEX_BUFFER)IndexBuf;
    Block->Ntfs.Type = NRH_INDX_TYPE;
    Block->Ntfs.UsaOffset = 0x28;
    UsaCount = (USHORT)(Vcb->BytesPerIndexRecord / Vcb->BytesPerSector) + 1;
    Block->Ntfs.UsaCount = UsaCount;
    Block->Ntfs.Lsn = 0;
    Block->VCN = 0;
    Block->Header.FirstEntryOffset = (ULONG)ROUND_UP (
        (UINTN)((PUCHAR)Block + Block->Ntfs.UsaOffset + UsaCount * sizeof (USHORT) -
                (PUCHAR)&Block->Header),
        ATTR_RECORD_ALIGNMENT);
    Block->Header.TotalSizeOfEntries = Block->Header.FirstEntryOffset + 16;
    Block->Header.AllocatedSize = Vcb->BytesPerIndexRecord -
        (ULONG)((PUCHAR)&Block->Header - (PUCHAR)Block);
    Block->Header.Flags = 0;

    Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
    Entry->Length = 16;
    Entry->KeyLength = 0;
    Entry->Flags = NTFS_INDEX_ENTRY_END;

    Entry = FirstOld;
    while ((PUCHAR)Entry < (PUCHAR)LastOld) {
        if (Entry->Length == 0) {
            FreePool (IndexBuf);
            return EFI_VOLUME_CORRUPTED;
        }
        if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;
        if (Entry->Flags & NTFS_INDEX_ENTRY_NODE) {
            FreePool (IndexBuf);
            return EFI_UNSUPPORTED;
        }

        Status = NtfsInsertIntoIndexEntries (&Block->Header, Entry,
                     Entry->FileName.Name, Entry->FileName.NameLength, Vcb->UpcaseTable);
        if (EFI_ERROR (Status)) {
            FreePool (IndexBuf);
            return Status;
        }
        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
    }

    Status = NtfsInsertIntoIndexEntries (&Block->Header, NewEntry, Name, NameLen, Vcb->UpcaseTable);
    if (EFI_ERROR (Status)) {
        FreePool (IndexBuf);
        return Status;
    }

    Usa = (PUSHORT)(IndexBuf + Block->Ntfs.UsaOffset);
    Usa[0] = 1;
    for (i = 1; i < UsaCount; i++) {
        PUSHORT SectorEnd = (PUSHORT)(IndexBuf + i * Vcb->BytesPerSector - sizeof (USHORT));
        Usa[i] = *SectorEnd;
        *SectorEnd = Usa[0];
    }

    Status = NtfsEfiWriteDisk (Vcb, StartLCN * Vcb->BytesPerCluster,
                               Vcb->BytesPerIndexRecord, IndexBuf);
    FreePool (IndexBuf);
    return Status;
}

static EFI_STATUS
NtfsConvertRootToSingleIndexAllocation (
    IN PNTFS_EFI_VCB          Vcb,
    IN PFILE_RECORD_HEADER    DirRec,
    IN ULONG                  RootOffset,
    IN PINDEX_ENTRY_ATTRIBUTE NewEntry,
    IN CONST WCHAR           *Name,
    IN UINTN                  NameLen
    )
{
    PNTFS_ATTR_RECORD     RootAttr;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    PINDEX_ENTRY_ATTRIBUTE FirstOld;
    PINDEX_ENTRY_ATTRIBUTE LastOld;
    PNTFS_ATTR_RECORD     NextAttr;
    UINT64                StartLCN;
    UINT64                Got;
    EFI_STATUS            Status;
    ULONG                 RootValueLen;
    ULONG                 RootAttrLen;
    PUCHAR                Cur;
    WCHAR                 AttrName[4] = { L'$', L'I', L'3', L'0' };
    UCHAR                 Run[17];
    UINTN                 RunLen;
    USHORT                InstanceNo;

    RootAttr = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
    IndexRoot = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)RootAttr + RootAttr->Resident.ValueOffset);
    FirstOld = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
    LastOld  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.TotalSizeOfEntries);

    if (IndexRoot->Header.Flags & INDEX_ROOT_LARGE) { Print (L"[convert] already-large\n"); return EFI_UNSUPPORTED; }

    NextAttr = (PNTFS_ATTR_RECORD)((PUCHAR)RootAttr + RootAttr->Length);
    if (NextAttr->Type != (ULONG)AttributeEnd) {
        Print (L"[convert] root-not-last type=%x\n", NextAttr->Type);
        return EFI_UNSUPPORTED;
    }

    Status = NtfsEfiAllocateClusters (Vcb,
                 Vcb->BytesPerIndexRecord / Vcb->BytesPerCluster,
                 &StartLCN, &Got);
    if (EFI_ERROR (Status)) return Status;
    if (Got < Vcb->BytesPerIndexRecord / Vcb->BytesPerCluster) {
        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        return EFI_UNSUPPORTED;
    }

    Status = NtfsWriteNewIndexBufferDirect (Vcb, StartLCN,
                 FirstOld, LastOld, NewEntry, Name, NameLen);
    if (EFI_ERROR (Status)) {
        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        return Status;
    }

    RootValueLen = sizeof (INDEX_ROOT_ATTRIBUTE) + 24;
    RootAttrLen = (ULONG)ROUND_UP (RootAttr->Resident.ValueOffset + RootValueLen,
                                   ATTR_RECORD_ALIGNMENT);

    RunLen = NtfsEncodeRunEntry (Run,
                 Vcb->BytesPerIndexRecord / Vcb->BytesPerCluster,
                 (INT64)StartLCN);

    {
        ULONG IndexAllocLen = (ULONG)ROUND_UP (sizeof (NTFS_ATTR_RECORD) +
                              sizeof (AttrName) + RunLen + 1, ATTR_RECORD_ALIGNMENT);
        ULONG BitmapLen = (ULONG)ROUND_UP (32 + 8, ATTR_RECORD_ALIGNMENT);
        ULONG NeedBytes = RootAttrLen + IndexAllocLen + BitmapLen + sizeof (ULONG);

        if (RootOffset + NeedBytes > DirRec->BytesAllocated) {
            Print (L"[convert] no-room need=%d off=%d balloc=%d biu=%d\n",
                (UINT32)NeedBytes, (UINT32)RootOffset, (UINT32)DirRec->BytesAllocated, (UINT32)DirRec->BytesInUse);
            NtfsEfiFreeClusters (Vcb, StartLCN, Got);
            return EFI_UNSUPPORTED;
        }
    }

    InstanceNo = DirRec->NextAttributeNumber;

    ZeroMem ((PUCHAR)RootAttr + RootAttr->Resident.ValueOffset, RootValueLen);
    IndexRoot = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)RootAttr + RootAttr->Resident.ValueOffset);
    IndexRoot->AttributeType = AttributeFileName;
    IndexRoot->CollationRule = 1;
    IndexRoot->SizeOfEntry = Vcb->BytesPerIndexRecord;
    IndexRoot->ClustersPerIndexRecord = (UCHAR)(Vcb->BytesPerIndexRecord / Vcb->BytesPerCluster);
    IndexRoot->Header.FirstEntryOffset = 16;
    IndexRoot->Header.TotalSizeOfEntries = 40;
    IndexRoot->Header.AllocatedSize = 40;
    IndexRoot->Header.Flags = INDEX_ROOT_LARGE;

    {
        PINDEX_ENTRY_ATTRIBUTE EndEntry =
            (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
        EndEntry->Length = 24;
        EndEntry->KeyLength = 0;
        EndEntry->Flags = NTFS_INDEX_ENTRY_END | NTFS_INDEX_ENTRY_NODE;
        *(PULONGLONG)((PUCHAR)EndEntry + EndEntry->Length - sizeof (ULONGLONG)) = 0;
    }

    RootAttr->Resident.ValueLength = RootValueLen;
    RootAttr->Length = RootAttrLen;

    Cur = (PUCHAR)RootAttr + RootAttrLen;

    /*
     * Zero everything from here to the end of the record BEFORE laying down
     * the new $INDEX_ALLOCATION/$BITMAP attributes. The only thing that
     * lived past $INDEX_ROOT was the end marker (verified above), so this is
     * safe - and it is required: otherwise the $BITMAP:$I30 value (of which
     * we only ever set bit 0 below) keeps leftover record bytes in bits
     * 1..63, which chkdsk reads as phantom allocated INDX blocks and
     * "corrects" as an $I30 index error. It also clears the dead padding
     * past BytesInUse so a fresh record is byte-clean like Windows writes.
     */
    ZeroMem (Cur, DirRec->BytesAllocated - (ULONG)((PUCHAR)Cur - (PUCHAR)DirRec));

    {
        PNTFS_ATTR_RECORD A = (PNTFS_ATTR_RECORD)Cur;
        ULONG MpOff = (ULONG)ROUND_UP (sizeof (NTFS_ATTR_RECORD) + sizeof (AttrName),
                                       ATTR_RECORD_ALIGNMENT);
        A->Type = (ULONG)AttributeIndexAllocation;
        A->IsNonResident = 1;
        A->NameLength = 4;
        A->NameOffset = sizeof (NTFS_ATTR_RECORD);
        A->Flags = 0;
        A->Instance = InstanceNo++;
        A->NonResident.LowestVCN = 0;
        A->NonResident.HighestVCN = (Vcb->BytesPerIndexRecord / Vcb->BytesPerCluster) - 1;
        A->NonResident.MappingPairsOffset = (USHORT)MpOff;
        A->NonResident.CompressionUnit = 0;
        A->NonResident.AllocatedSize = Vcb->BytesPerIndexRecord;
        A->NonResident.DataSize = Vcb->BytesPerIndexRecord;
        A->NonResident.InitializedSize = Vcb->BytesPerIndexRecord;
        A->Length = (ULONG)ROUND_UP (MpOff + RunLen + 1, ATTR_RECORD_ALIGNMENT);
        CopyMem ((PUCHAR)A + A->NameOffset, AttrName, sizeof (AttrName));
        CopyMem ((PUCHAR)A + A->NonResident.MappingPairsOffset, Run, RunLen);
        *((PUCHAR)A + A->NonResident.MappingPairsOffset + RunLen) = 0;
        Cur += A->Length;
    }

    {
        PNTFS_ATTR_RECORD A = (PNTFS_ATTR_RECORD)Cur;
        A->Type = (ULONG)AttributeBitmap;
        A->IsNonResident = 0;
        A->NameLength = 4;
        A->NameOffset = NTFS_ATTR_HEADER_RESIDENT_SIZE;
        A->Flags = 0;
        A->Instance = InstanceNo++;
        A->Resident.ValueLength = 8;
        A->Resident.ValueOffset = 32;
        A->Resident.Flags = 0;
        A->Length = (ULONG)ROUND_UP (A->Resident.ValueOffset + A->Resident.ValueLength,
                                     ATTR_RECORD_ALIGNMENT);
        CopyMem ((PUCHAR)A + A->NameOffset, AttrName, sizeof (AttrName));
        *((PUCHAR)A + A->Resident.ValueOffset) = 1;
        Cur += A->Length;
    }

    *(PULONG)Cur = (ULONG)AttributeEnd;
    Cur += sizeof (ULONG);
    DirRec->BytesInUse = (ULONG)ROUND_UP ((UINTN)(Cur - (PUCHAR)DirRec), ATTR_RECORD_ALIGNMENT);
    DirRec->NextAttributeNumber = InstanceNo;
    return EFI_SUCCESS;
}

/*
 * Build a brand-new file record (STD_INFO + FILE_NAME + empty DATA +
 * end marker) and write it out via the existing Unfixup/WriteAttr/Fixup
 * pipeline (NtfsEfiWriteFileRecord already does the USA dance).
 */
static EFI_STATUS
NtfsCreateFileRecord (
    IN  PNTFS_EFI_VCB Vcb,
    IN  ULONGLONG      NewIndex,
    IN  UINT64         ParentRef,
    IN  UINT64         NowNtfs,
    IN  CONST WCHAR   *Name,
    IN  UINTN          NameLen,
    IN  ULONG          InheritSecurityId,
    IN  BOOLEAN        IsDirectory,
    OUT USHORT        *OutSequenceNumber
    )
{
    PFILE_RECORD_HEADER Rec;
    PUSHORT              Usa;
    USHORT                UsaCount;
    ULONG                 AttrOff;
    USHORT                Seq = 1;
    EFI_STATUS            Status;

    Rec = AllocateZeroPool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) return EFI_OUT_OF_RESOURCES;

    /*
     * If this record slot was used before (freed file), preserve+bump its
     * stored sequence number so stale directory-entry/handle references
     * to the old file reliably miss (NTFS reference = record# + seq#).
     * A never-used slot reads as zeroed/garbage FILE0 signature mismatch,
     * in which case we just start at 1.
     */
    {
        PFILE_RECORD_HEADER Old = AllocatePool (Vcb->BytesPerFileRecord);
        if (Old != NULL) {
            UINT64 ByteOffset = NewIndex * Vcb->BytesPerFileRecord;
            if (NtfsEfiReadAttr (Vcb, Vcb->MFTContext, ByteOffset, (PCHAR)Old, Vcb->BytesPerFileRecord)
                    == Vcb->BytesPerFileRecord &&
                Old->Ntfs.Type == NRH_FILE_TYPE) {
                Seq = (USHORT)(Old->SequenceNumber + 1);
                if (Seq == 0) Seq = 1;   /* 0 is not a valid sequence number */
            }
            FreePool (Old);
        }
    }

    Rec->Ntfs.Type      = NRH_FILE_TYPE;
    Rec->Ntfs.UsaOffset = 0x30;   /* matches this volume's on-disk records (48) */
    UsaCount             = (USHORT)(Vcb->BytesPerFileRecord / Vcb->BytesPerSector) + 1;
    Rec->Ntfs.UsaCount  = UsaCount;
    Rec->Ntfs.Lsn       = 0;
    Rec->SequenceNumber = Seq;
    Rec->LinkCount      = 1;   /* one name; NTFS directories are NOT '.'/'..' link-counted like Unix */
    Rec->Flags          = IsDirectory ? (FRH_IN_USE | FRH_DIRECTORY) : FRH_IN_USE;
    Rec->BaseFileRecord = 0;
    Rec->MFTRecordNumber = (ULONG)NewIndex;

    Usa    = (PUSHORT)((PUCHAR)Rec + Rec->Ntfs.UsaOffset);
    Usa[0] = 1;   /* USN - NtfsEfiWriteFileRecord's unfixup step fills the rest */

    AttrOff = (ULONG)ROUND_UP (Rec->Ntfs.UsaOffset + UsaCount * sizeof (USHORT), ATTR_RECORD_ALIGNMENT);
    Rec->AttributeOffset = (USHORT)AttrOff;
    Rec->BytesAllocated  = Vcb->BytesPerFileRecord;

    {
        PUCHAR Cur = (PUCHAR)Rec + AttrOff;
        USHORT InstanceNo = 0;
        ULONG  StandardFileAttributes = IsDirectory ? 0 : NTFS_FILE_TYPE_ARCHIVE;
        ULONG  FileNameFileAttributes = IsDirectory ? NTFS_FILE_TYPE_DIRECTORY : NTFS_FILE_TYPE_ARCHIVE;

        /* $STANDARD_INFORMATION */
        {
            PNTFS_ATTR_RECORD A = (PNTFS_ATTR_RECORD)Cur;
            PSTANDARD_INFORMATION Si = (PSTANDARD_INFORMATION)(Cur + NTFS_ATTR_HEADER_RESIDENT_SIZE);
            ULONG ValLen = sizeof (STANDARD_INFORMATION);
            A->Type = (ULONG)AttributeStandardInformation;
            A->IsNonResident = 0;
            A->NameLength = 0;
            A->NameOffset = 0;   /* real NTFS convention: 0 when NameLength==0 */
            A->Flags = 0;
            A->Instance = InstanceNo++;
            A->Resident.ValueLength = ValLen;
            A->Resident.ValueOffset = NTFS_ATTR_HEADER_RESIDENT_SIZE;
            A->Resident.Flags = 0;
            A->Length = (ULONG)ROUND_UP (NTFS_ATTR_HEADER_RESIDENT_SIZE + ValLen, ATTR_RECORD_ALIGNMENT);
            ZeroMem (Si, sizeof (STANDARD_INFORMATION));
            Si->CreationTime   = NowNtfs;
            Si->ChangeTime     = NowNtfs;
            Si->LastWriteTime  = NowNtfs;
            Si->LastAccessTime = NowNtfs;
            Si->FileAttribute  = StandardFileAttributes;
            Si->SecurityId     = InheritSecurityId;
            Cur += A->Length;
        }

        /* $FILE_NAME */
        {
            PNTFS_ATTR_RECORD A = (PNTFS_ATTR_RECORD)Cur;
            PFILENAME_ATTRIBUTE Fn = (PFILENAME_ATTRIBUTE)(Cur + NTFS_ATTR_HEADER_RESIDENT_SIZE);
            ULONG ValLen = (ULONG)NtfsBuildFileNameAttr (Fn, ParentRef, NowNtfs, Name, NameLen,
                               FileNameFileAttributes);
            A->Type = (ULONG)AttributeFileName;
            A->IsNonResident = 0;
            A->NameLength = 0;
            A->NameOffset = 0;   /* real NTFS convention: 0 when NameLength==0 */
            A->Flags = 0;
            A->Instance = InstanceNo++;
            A->Resident.ValueLength = ValLen;
            A->Resident.ValueOffset = NTFS_ATTR_HEADER_RESIDENT_SIZE;
            A->Resident.Flags = 1;   /* $FILE_NAME is indexed in parent directories */
            A->Length = (ULONG)ROUND_UP (NTFS_ATTR_HEADER_RESIDENT_SIZE + ValLen, ATTR_RECORD_ALIGNMENT);
            Cur += A->Length;
        }

        if (!IsDirectory) {
            /* $DATA - empty, resident */
            PNTFS_ATTR_RECORD A = (PNTFS_ATTR_RECORD)Cur;
            A->Type = (ULONG)AttributeData;
            A->IsNonResident = 0;
            A->NameLength = 0;
            A->NameOffset = NTFS_ATTR_HEADER_RESIDENT_SIZE;
            A->Flags = 0;
            A->Instance = InstanceNo++;
            A->Resident.ValueLength = 0;
            A->Resident.ValueOffset = NTFS_ATTR_HEADER_RESIDENT_SIZE;
            A->Resident.Flags = 0;
            A->Length = (ULONG)ROUND_UP (NTFS_ATTR_HEADER_RESIDENT_SIZE, ATTR_RECORD_ALIGNMENT);
            Cur += A->Length;
        } else {
            /* $INDEX_ROOT:$I30 - empty directory, resident */
            PNTFS_ATTR_RECORD A = (PNTFS_ATTR_RECORD)Cur;
            WCHAR             AttrName[4] = { L'$', L'I', L'3', L'0' };
            PINDEX_ROOT_ATTRIBUTE Ir = (PINDEX_ROOT_ATTRIBUTE)(Cur + 32);
            PINDEX_ENTRY_ATTRIBUTE EndEntry;
            ULONG ValLen = sizeof (INDEX_ROOT_ATTRIBUTE) + 16;

            A->Type = (ULONG)AttributeIndexRoot;
            A->IsNonResident = 0;
            A->NameLength = 4;
            A->NameOffset = NTFS_ATTR_HEADER_RESIDENT_SIZE;
            A->Flags = 0;
            A->Instance = InstanceNo++;
            CopyMem (Cur + A->NameOffset, AttrName, sizeof (AttrName));
            A->Resident.ValueLength = ValLen;
            A->Resident.ValueOffset = 32;
            A->Resident.Flags = 0;
            A->Length = (ULONG)ROUND_UP (A->Resident.ValueOffset + ValLen, ATTR_RECORD_ALIGNMENT);

            ZeroMem (Ir, ValLen);
            Ir->AttributeType = (ULONG)AttributeFileName;
            Ir->CollationRule = 1;
            Ir->SizeOfEntry = Vcb->BytesPerIndexRecord;
            Ir->ClustersPerIndexRecord = (UCHAR)(Vcb->BytesPerIndexRecord / Vcb->BytesPerCluster);
            Ir->Header.FirstEntryOffset = 16;
            Ir->Header.TotalSizeOfEntries = 32;
            Ir->Header.AllocatedSize = 32;
            Ir->Header.Flags = 0;

            EndEntry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Ir->Header + Ir->Header.FirstEntryOffset);
            EndEntry->Length = 16;
            EndEntry->KeyLength = 0;
            EndEntry->Flags = NTFS_INDEX_ENTRY_END;

            Cur += A->Length;
        }

        Rec->NextAttributeNumber = InstanceNo;

        /* end marker */
        *(PULONG)Cur = (ULONG)AttributeEnd;
        Cur += sizeof (ULONG);
        Rec->BytesInUse = (ULONG)ROUND_UP ((UINTN)(Cur - (PUCHAR)Rec), ATTR_RECORD_ALIGNMENT);

        if (Rec->BytesInUse > Rec->BytesAllocated) {
            FreePool (Rec);
            return EFI_UNSUPPORTED;   /* shouldn't happen: fixed-size attrs, huge record slack */
        }
    }

    Status = NtfsEfiWriteFileRecord (Vcb, NewIndex, Rec);
    if (!EFI_ERROR (Status)) {
        *OutSequenceNumber = Seq;
    }
    FreePool (Rec);
    return Status;
}

/*
 * Insert one new leaf entry (Flags=0, no NODE/END) into DirRec's resident
 * $INDEX_ROOT, keeping the flat array sorted by NTFS collation (case-
 * insensitive) so the existing directed B+tree search continues to work.
 * Fails (without modifying DirRec) if the grown $INDEX_ROOT would not fit
 * in the parent's MFT record - the caller then converts to a large index.
 *
 * SMALL-INDEX path only: DirRec must already be the record that owns
 * $INDEX_ROOT:$I30 and must have no $INDEX_ALLOCATION (see the
 * NtfsInsertIndexEntry wrapper below, which resolves both). Does NOT commit
 * DirRec - the caller writes it back.
 */
static EFI_STATUS
NtfsInsertIndexEntrySmall (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER DirRec,
    IN UINT64               ChildRef,     /* new file's MFT record# | (seq#<<48) */
    IN UINT64               ParentRef,
    IN UINT64               NowNtfs,
    IN CONST WCHAR         *Name,
    IN UINTN                NameLen,
    IN BOOLEAN              IsDirectory
    )
{
    ULONG              RootOffset = 0;
    PNTFS_ATTR_CTX     RootCtx = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexRoot,
                                       L"$I30", 4, &RootOffset);
    PNTFS_ATTR_RECORD  RootAttr;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    PINDEX_ENTRY_ATTRIBUTE Entry, InsertBefore, Last;
    UINT64             KeyLen  = NTFS_FILENAME_FIXED_SIZE + NameLen * sizeof (WCHAR);
    UINT64             EntryLen = ROUND_UP (16 + KeyLen, 8);
    UINT64             OldValueLen, NewValueLen, InsertOffset;
    UCHAR              NewEntryBuf[16 + NTFS_FILENAME_FIXED_SIZE + 512];
    PINDEX_ENTRY_ATTRIBUTE NewEntry = (PINDEX_ENTRY_ATTRIBUTE)NewEntryBuf;
    PUCHAR             ValPtr;

    if (RootCtx == NULL) return EFI_UNSUPPORTED;   /* resolver guarantees otherwise */

    /* EntryLen (the 8-aligned entry size) is what gets zeroed and copied below,
     * so that - not 16+KeyLen+16 - is the buffer requirement. The old check
     * added a spurious extra 16 bytes and rejected creates with names of ~247
     * chars and up even though the entry fit. */
    if (EntryLen > sizeof (NewEntryBuf)) {
        NtfsEfiFreeAttrCtx (RootCtx);
        return EFI_UNSUPPORTED;   /* pathological name length */
    }
    if (!NtfsEfiIndexRootOk ((PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset))) {
        NtfsEfiFreeAttrCtx (RootCtx);
        return EFI_VOLUME_CORRUPTED;
    }

    RootAttr  = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
    ValPtr    = (PUCHAR)RootAttr + RootAttr->Resident.ValueOffset;
    IndexRoot = (PINDEX_ROOT_ATTRIBUTE)ValPtr;

    Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
    Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.TotalSizeOfEntries);

    InsertBefore = Last;
    while ((PUCHAR)Entry < (PUCHAR)Last) {
        if (Entry->Length == 0) break;
        if (!(Entry->Flags & NTFS_INDEX_ENTRY_END)) {
            /*
             * Collate with the volume's own $UpCase table, exactly like every
             * other insert and every lookup in this driver (NtfsCreateCompareNames
             * above, NtfsEfiCompareNames in ntfs_btree.c).
             *
             * This loop used to fold only a-z by hand. For ASCII that agrees with
             * $UpCase, but not beyond it: $UpCase folds 'ą' to 'Ą', 'ł' to 'Ł',
             * Cyrillic 'а' to 'А' - the ASCII fold leaves them distinct. So this
             * one path ordered - and compared for equality - by a different rule
             * than the path that later searches the same index. A small directory
             * could end up holding both "ą.txt" and "Ą.txt" as separate entries
             * while Windows and chkdsk see one single name there.
             */
            INTN  Cmp = NtfsCreateCompareNames (Name, NameLen,
                            Entry->FileName.Name, Entry->FileName.NameLength,
                            Vcb->UpcaseTable);
            if (Cmp == 0) {
                NtfsEfiFreeAttrCtx (RootCtx);
                return EFI_ACCESS_DENIED;   /* name already exists */
            }
            if (Cmp < 0) { InsertBefore = Entry; break; }
        } else {
            InsertBefore = Entry;
            break;
        }
        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
    }

    /*
     * Use the LOGICAL entries boundary (16-byte INDEX_ROOT_ATTRIBUTE
     * prefix + Header.TotalSizeOfEntries), not the attribute's raw
     * Resident.ValueLength, as "where the real content ends". A real
     * NTFS driver (or this one, on an earlier insert) can leave
     * ValueLength LARGER than TotalSizeOfEntries - observed in practice
     * after Windows self-heal logically shrinks TotalSizeOfEntries
     * (removing a bad link) without shrinking the attribute's value
     * length, leaving stale garbage bytes as slack. Trusting
     * ValueLength here would shift-forward that garbage on top of the
     * next real entry (Foo.txt's fields were seen corrupted this way -
     * its AllocatedSize field got overwritten with leftover bytes from
     * a since-removed, larger index state). Basing everything on the
     * logical boundary instead also has the side effect of compacting
     * away that slack on every insert.
     */
    OldValueLen  = 16 + IndexRoot->Header.TotalSizeOfEntries;
    NewValueLen  = OldValueLen + EntryLen;
    InsertOffset = (UINT64)((PUCHAR)InsertBefore - ValPtr);

    if (!NtfsEfiGrowResidentInRecord (Vcb, DirRec, RootOffset, NewValueLen)) {
        EFI_STATUS ConvertStatus;

        ZeroMem (NewEntry, (UINTN)EntryLen);   /* whole entry incl. 8-align padding */
        NewEntry->Data.Directory.IndexedFile = ChildRef;
        NewEntry->Length    = (USHORT)EntryLen;
        NewEntry->KeyLength = (USHORT)KeyLen;
        NewEntry->Flags     = 0;
        NewEntry->Reserved  = 0;
        NtfsBuildFileNameAttr (&NewEntry->FileName, ParentRef, NowNtfs, Name, NameLen,
            IsDirectory ? NTFS_FILE_TYPE_DIRECTORY : NTFS_FILE_TYPE_ARCHIVE);

        ConvertStatus = NtfsConvertRootToSingleIndexAllocation (Vcb, DirRec,
                            RootOffset, NewEntry, Name, NameLen);
        NtfsEfiFreeAttrCtx (RootCtx);
        return ConvertStatus;
    }

    /* re-fetch pointers: growth may have shifted attributes AFTER $INDEX_ROOT,
     * but $INDEX_ROOT's own Attr/value position is unchanged */
    RootAttr  = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
    ValPtr    = (PUCHAR)RootAttr + RootAttr->Resident.ValueOffset;
    IndexRoot = (PINDEX_ROOT_ATTRIBUTE)ValPtr;

    NtfsEfiShiftForward (ValPtr + InsertOffset, (UINTN)(OldValueLen - InsertOffset), (UINTN)EntryLen);

    ZeroMem (NewEntry, (UINTN)EntryLen);   /* whole entry incl. 8-align padding */
    NewEntry->Data.Directory.IndexedFile = ChildRef;
    NewEntry->Length    = (USHORT)EntryLen;
    NewEntry->KeyLength = (USHORT)KeyLen;
    NewEntry->Flags     = 0;
    NewEntry->Reserved  = 0;
    NtfsBuildFileNameAttr (&NewEntry->FileName, ParentRef, NowNtfs, Name, NameLen,
        IsDirectory ? NTFS_FILE_TYPE_DIRECTORY : NTFS_FILE_TYPE_ARCHIVE);
    CopyMem (ValPtr + InsertOffset, NewEntry, (UINTN)EntryLen);

    IndexRoot->Header.TotalSizeOfEntries += (ULONG)EntryLen;
    IndexRoot->Header.AllocatedSize      += (ULONG)EntryLen;

    NtfsEfiFreeAttrCtx (RootCtx);
    return EFI_SUCCESS;
}

/*
 * Insert a directory entry, transparently handling directories whose index
 * attributes were relocated into an $ATTRIBUTE_LIST extension record.
 *
 * The index machinery below edits exactly one MFT record, so the only thing
 * needed for big/fragmented directories is to hand it the record that actually
 * owns $INDEX_ROOT:$I30 rather than the base record (that mismatch used to fail
 * every create/mkdir/move into such a directory with EFI_UNSUPPORTED - the
 * bare-metal "grown volume" bug). NtfsEfiResolveIndexHost finds it, and refuses
 * cleanly if the index attributes are spread over several records.
 *
 * Commit rules: the large-index path commits the host record itself; the small
 * path does not, so we commit it here when it is an extension record. When the
 * host IS the base record we leave the write-back to the caller, unchanged.
 */
EFI_STATUS
NtfsInsertIndexEntry (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER DirRec,
    IN ULONGLONG            DirMFT,
    IN UINT64               ChildRef,
    IN UINT64               ParentRef,
    IN UINT64               NowNtfs,
    IN CONST WCHAR         *Name,
    IN UINTN                NameLen,
    IN BOOLEAN              IsDirectory
    )
{
    NTFS_INDEX_HOST Host;
    EFI_STATUS      Status = NtfsEfiResolveIndexHost (Vcb, DirRec, DirMFT, &Host);

    if (EFI_ERROR (Status)) return Status;

    if (Host.HasAlloc) {
        Status = NtfsInsertIndexAllocationEntry (Vcb, Host.Rec, Host.MFTIndex,
                     ChildRef, ParentRef, NowNtfs, Name, NameLen, IsDirectory);
    } else {
        Status = NtfsInsertIndexEntrySmall (Vcb, Host.Rec, ChildRef, ParentRef,
                     NowNtfs, Name, NameLen, IsDirectory);
        if (!EFI_ERROR (Status) && Host.Own) {
            Status = NtfsEfiWriteFileRecord (Vcb, Host.MFTIndex, Host.Rec);
        }
    }

    if (Host.Own) FreePool (Host.Rec);
    return Status;
}

/*
 * Roll back a just-allocated child MFT record when a later step of a create
 * (the parent index insert, or the parent record write-back) fails. Calling
 * NtfsEfiFreeMftRecord alone only clears the $MFT $BITMAP bit and leaves the
 * on-disk record still flagged FRH_IN_USE - chkdsk then reports it as an
 * orphaned file AND an "MFT BITMAP attribute is incorrect" mismatch (i.e. a
 * failed create silently corrupts the volume). Mirror the delete path: clear
 * FRH_IN_USE + bump the sequence number + zero the link count on disk, THEN
 * release the bitmap bit. A freshly created record is fully resident, so it has
 * no external clusters to free here.
 */
static VOID
NtfsRollbackNewRecord (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     NewIndex
    )
{
    PFILE_RECORD_HEADER Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec != NULL) {
        if (!EFI_ERROR (NtfsEfiReadFileRecord (Vcb, NewIndex, Rec))) {
            Rec->Flags &= (USHORT)~FRH_IN_USE;
            Rec->SequenceNumber++;
            if (Rec->SequenceNumber == 0) Rec->SequenceNumber = 1;
            Rec->LinkCount = 0;
            NtfsEfiWriteFileRecord (Vcb, NewIndex, Rec);
        }
        FreePool (Rec);
    }
    NtfsEfiFreeMftRecord (Vcb, NewIndex);
}

EFI_STATUS
NtfsEfiCreateFile (
    IN  PNTFS_EFI_VCB Vcb,
    IN  ULONGLONG      ParentMFT,
    IN  CONST WCHAR   *Name,
    IN  UINTN          NameLen,
    IN  BOOLEAN        IsDirectory,
    OUT ULONGLONG     *NewMFTIndex
    )
{
    PFILE_RECORD_HEADER ParentRec;
    ULONGLONG            NewIndex;
    UINT64                NowNtfs;
    UINT64                ParentRef;
    USHORT                ChildSeq;
    ULONG                 InheritSecurityId;
    EFI_STATUS            Status;
    EFI_TIME              NowEfi;

    if (NameLen == 0 || NameLen > 255) return EFI_INVALID_PARAMETER;

    NtfsMarkVolumeDirty (Vcb);

    ParentRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (ParentRec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, ParentMFT, ParentRec))) {
        FreePool (ParentRec);
        return EFI_DEVICE_ERROR;
    }
    if (!(ParentRec->Flags & FRH_DIRECTORY)) {
        FreePool (ParentRec);
        return EFI_INVALID_PARAMETER;
    }

    {
        ULONG DummyStart = 0;
        if (NtfsEfiFindInDirectory (Vcb, ParentMFT, Name, NameLen, FALSE, FALSE, &DummyStart) != (ULONGLONG)-1LL) {
            FreePool (ParentRec);
            return EFI_ACCESS_DENIED;   /* already exists */
        }
    }
    ZeroMem (&NowEfi, sizeof (NowEfi));
    if (gRT != NULL) gRT->GetTime (&NowEfi, NULL);
    NowNtfs = NtfsEfiConvertTimeToNtfs (&NowEfi);

    ParentRef = (UINT64)ParentMFT | ((UINT64)ParentRec->SequenceNumber << 48);

    /*
     * A new file needs a real SecurityId referencing an existing $Secure
     * entry - SecurityId==0 reads as "no valid security descriptor" and
     * NTFS.sys treats the whole file as corrupt (confirmed the hard way).
     * Implementing $Secure lookups/allocation is out of scope for this
     * round, so inherit the parent directory's own SecurityId instead -
     * same approach real filesystems use as the default when no explicit
     * ACL is requested, and guarantees a valid, already-existing entry.
     */
    {
        PNTFS_ATTR_CTX ParentStdCtx = NtfsEfiFindAttrInRecord (Vcb, ParentRec,
                                            AttributeStandardInformation, NULL, 0, NULL);
        InheritSecurityId = 0;
        if (ParentStdCtx != NULL) {
            if (!ParentStdCtx->pRecord->IsNonResident &&
                ParentStdCtx->pRecord->Resident.ValueLength >= sizeof (STANDARD_INFORMATION)) {
                PSTANDARD_INFORMATION ParentSi = (PSTANDARD_INFORMATION)
                    ((PUCHAR)ParentStdCtx->pRecord + ParentStdCtx->pRecord->Resident.ValueOffset);
                InheritSecurityId = ParentSi->SecurityId;
            }
            NtfsEfiFreeAttrCtx (ParentStdCtx);
        }
    }

    Status = NtfsEfiAllocateMftRecord (Vcb, &NewIndex);
    if (EFI_ERROR (Status)) {
        FreePool (ParentRec);
        return Status;
    }

    Status = NtfsCreateFileRecord (Vcb, NewIndex, ParentRef, NowNtfs, Name, NameLen,
                 InheritSecurityId, IsDirectory, &ChildSeq);
    if (EFI_ERROR (Status)) {
        Print (L"[create] '%s' record -> %r\n", Name, Status);
        NtfsEfiFreeMftRecord (Vcb, NewIndex);
        FreePool (ParentRec);
        return Status;
    }

    {
        UINT64 ChildRef = (UINT64)NewIndex | ((UINT64)ChildSeq << 48);
        Status = NtfsInsertIndexEntry (Vcb, ParentRec, ParentMFT, ChildRef, ParentRef,
                     NowNtfs, Name, NameLen, IsDirectory);
    }
    if (EFI_ERROR (Status)) {
        Print (L"[create] '%s' insert -> %r (parentMFT=%ld)\n", Name, Status, ParentMFT);
        /* the child record was already written to disk; a bitmap-only free would
         * leave it FRH_IN_USE and unreferenced (chkdsk: orphan + MFT bitmap
         * mismatch). Fully roll it back so a failed create leaves NO corruption. */
        NtfsRollbackNewRecord (Vcb, NewIndex);
        FreePool (ParentRec);
        return Status;
    }

    Status = NtfsEfiWriteFileRecord (Vcb, ParentMFT, ParentRec);
    FreePool (ParentRec);
    if (EFI_ERROR (Status)) {
        NtfsRollbackNewRecord (Vcb, NewIndex);
        return Status;
    }

    *NewMFTIndex = NewIndex;
    return EFI_SUCCESS;
}
