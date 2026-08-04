/**
 * ntfs_setinfo.c - everything EFI_FILE_PROTOCOL.SetInfo(EFI_FILE_INFO) can
 * change on an existing file, in one place:
 *   - timestamps + DOS attributes      NtfsEfiSetFileInfo
 *   - rename in place                  NtfsEfiRenameFile (wrapper)
 *   - cross-directory move             NtfsEfiMoveFile
 *   - shrink / truncate FileSize       NtfsEfiSetFileSize
 *   - release prealloc slack on close  NtfsEfiTrimAllocation
 *
 * The unifying problem all five solve: NTFS duplicates a file's times, sizes
 * and attribute bits in three places, and chkdsk cross-checks all three. Every
 * mutation here therefore lands in ALL of:
 *   - the file's own $STANDARD_INFORMATION,
 *   - the file's own $FILE_NAME,
 *   - the $FILE_NAME copy embedded in the parent directory's index entry
 *     (NtfsPatchParentIndexEntry walks $INDEX_ROOT and the INDX blocks).
 * A zero EFI_TIME (Year == 0) means "leave this timestamp unchanged", per the
 * UEFI spec.
 *
 * Limits: FileSize is shrink-only (growing is done by writing, which zero-fills
 * correctly); moves reject collisions and directory cycles (NtfsMoveWouldCycle);
 * the metadata files below NTFS_FILE_FIRST_USER_FILE are off limits.
 */

#include "ntfs.h"

/* fields to apply, gathered once from the incoming EFI_FILE_INFO */
typedef struct {
    BOOLEAN SetCreate;   UINT64 Create;
    BOOLEAN SetWrite;    UINT64 Write;     /* ModificationTime -> LastWrite         */
    BOOLEAN SetChange;   UINT64 Change;    /* MFT change time                       */
    BOOLEAN SetAccess;   UINT64 Access;
    ULONG   FileAttr;                       /* new NTFS FileAttribute value          */
    BOOLEAN SetSizes;    UINT64 Alloc; UINT64 Data;  /* rename: match file's sizes    */
    BOOLEAN KeepDirBit;                     /* preserve target's DIRECTORY bit as-is */
} NTFS_SETINFO_APPLY;

/* Patch a FILENAME_ATTRIBUTE's timestamps + attributes (and, for rename,
 * sizes) in place. */
static VOID
NtfsApplyToFileName (
    IN OUT PFILENAME_ATTRIBUTE Fn,
    IN     CONST NTFS_SETINFO_APPLY *A
    )
{
    if (A->SetCreate) Fn->CreationTime   = A->Create;
    if (A->SetWrite)  Fn->LastWriteTime  = A->Write;
    if (A->SetChange) Fn->ChangeTime     = A->Change;
    if (A->SetAccess) Fn->LastAccessTime = A->Access;
    if (A->SetSizes)  { Fn->AllocatedSize = A->Alloc; Fn->DataSize = A->Data; }
    if (A->KeepDirBit)
        Fn->FileAttributes = (Fn->FileAttributes & NTFS_FILE_TYPE_DIRECTORY) |
                             (A->FileAttr & ~NTFS_FILE_TYPE_DIRECTORY);
    else
        Fn->FileAttributes = A->FileAttr;
}

/* Locate the child's leaf index entry in the parent (root or INDX blocks)
 * and apply the timestamp/attribute changes to its embedded $FILE_NAME. */
static EFI_STATUS
NtfsPatchParentIndexEntry (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     ParentMFT,
    IN ULONGLONG     ChildMFT,
    IN CONST NTFS_SETINFO_APPLY *A
    )
{
    PFILE_RECORD_HEADER DirRec;
    ULONG               RootOffset = 0;
    PNTFS_ATTR_CTX      RootCtx;
    PNTFS_ATTR_CTX      AllocCtx;
    EFI_STATUS          Status = EFI_NOT_FOUND;

    DirRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (DirRec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, ParentMFT, DirRec))) {
        FreePool (DirRec);
        return EFI_DEVICE_ERROR;
    }

    RootCtx = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexRoot, L"$I30", 4, &RootOffset);
    if (RootCtx != NULL) {
        PNTFS_ATTR_RECORD RootAttr = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
        PUCHAR ValPtr = (PUCHAR)RootAttr + RootAttr->Resident.ValueOffset;
        PINDEX_ROOT_ATTRIBUTE IndexRoot = (PINDEX_ROOT_ATTRIBUTE)ValPtr;
        PINDEX_ENTRY_ATTRIBUTE Entry =
            (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
        PINDEX_ENTRY_ATTRIBUTE Last =
            (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.TotalSizeOfEntries);
        while ((PUCHAR)Entry < (PUCHAR)Last) {
            if (Entry->Length == 0) break;
            if (!(Entry->Flags & NTFS_INDEX_ENTRY_END) &&
                (Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK) == ChildMFT) {
                NtfsApplyToFileName (&Entry->FileName, A);
                Status = NtfsEfiWriteFileRecord (Vcb, ParentMFT, DirRec);
                NtfsEfiFreeAttrCtx (RootCtx);
                FreePool (DirRec);
                return Status;
            }
            if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;
            Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
        }
        NtfsEfiFreeAttrCtx (RootCtx);
    }

    AllocCtx = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexAllocation, L"$I30", 4, NULL);
    if (AllocCtx != NULL) {
        UINT64 AllocLen = NtfsEfiAttrDataLength (AllocCtx);
        UINT64 Vcn;
        PUCHAR Buf = AllocatePool (Vcb->BytesPerIndexRecord);
        if (Buf != NULL) {
            for (Vcn = 0; Vcn * Vcb->BytesPerIndexRecord < AllocLen; Vcn++) {
                PINDEX_BUFFER Block;
                PINDEX_ENTRY_ATTRIBUTE E, L2;
                if (NtfsEfiReadAttr (Vcb, AllocCtx, Vcn * Vcb->BytesPerIndexRecord,
                                     (PCHAR)Buf, Vcb->BytesPerIndexRecord) != Vcb->BytesPerIndexRecord)
                    continue;
                Block = (PINDEX_BUFFER)Buf;
                if (Block->Ntfs.Type != NRH_INDX_TYPE) continue;
                if (EFI_ERROR (NtfsEfiFixupRecord (Vcb, &Block->Ntfs))) continue;
                E  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
                L2 = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries);
                while ((PUCHAR)E < (PUCHAR)L2) {
                    if (E->Length == 0) break;
                    if (!(E->Flags & NTFS_INDEX_ENTRY_END) &&
                        (E->Data.Directory.IndexedFile & NTFS_MFT_MASK) == ChildMFT) {
                        NtfsApplyToFileName (&E->FileName, A);
                        Status = NtfsEfiWriteMultiSectorRecord (Vcb, AllocCtx,
                                    Vcn * Vcb->BytesPerIndexRecord, &Block->Ntfs,
                                    Vcb->BytesPerIndexRecord);
                        FreePool (Buf); NtfsEfiFreeAttrCtx (AllocCtx); FreePool (DirRec);
                        return Status;
                    }
                    if (E->Flags & NTFS_INDEX_ENTRY_END) break;
                    E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)E + E->Length);
                }
            }
            FreePool (Buf);
        }
        NtfsEfiFreeAttrCtx (AllocCtx);
    }

    FreePool (DirRec);
    return Status;
}

EFI_STATUS
NtfsEfiSetFileInfo (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex,
    IN EFI_FILE_INFO *Info
    )
{
    PFILE_RECORD_HEADER Rec;
    PNTFS_ATTR_RECORD   Attr, LastPtr;
    NTFS_SETINFO_APPLY  A;
    ULONGLONG           ParentMFT = 0;
    BOOLEAN             HaveParent = FALSE;
    ULONG               OldAttr = 0;
    EFI_STATUS          Status;

    if (MFTIndex < NTFS_FILE_FIRST_USER_FILE) return EFI_ACCESS_DENIED;

    NtfsMarkVolumeDirty (Vcb);

    Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, MFTIndex, Rec))) {
        FreePool (Rec);
        return EFI_DEVICE_ERROR;
    }

    ZeroMem (&A, sizeof (A));
    A.SetCreate = (Info->CreateTime.Year       != 0);
    A.SetWrite  = (Info->ModificationTime.Year != 0);
    A.SetAccess = (Info->LastAccessTime.Year   != 0);
    if (A.SetCreate) A.Create = NtfsEfiConvertTimeToNtfs (&Info->CreateTime);
    if (A.SetWrite)  A.Write  = NtfsEfiConvertTimeToNtfs (&Info->ModificationTime);
    if (A.SetAccess) A.Access = NtfsEfiConvertTimeToNtfs (&Info->LastAccessTime);
    A.SetChange = A.SetWrite;   /* SetInfo maps ModificationTime -> ChangeTime too */
    A.Change    = A.Write;
    A.KeepDirBit = TRUE;        /* never toggle the DIRECTORY bit via SetInfo */

    /* current $STANDARD_INFORMATION.FileAttribute is the base; overlay the
     * four DOS bits the EFI Attribute field carries */
    {
        ULONG Off = 0;
        PNTFS_ATTR_CTX StdCtx = NtfsEfiFindAttrInRecord (Vcb, Rec,
                                    AttributeStandardInformation, NULL, 0, &Off);
        if (StdCtx != NULL) {
            PSTANDARD_INFORMATION Si =
                (PSTANDARD_INFORMATION)((PUCHAR)Rec + Off +
                    ((PNTFS_ATTR_RECORD)((PUCHAR)Rec + Off))->Resident.ValueOffset);
            OldAttr = Si->FileAttribute;
            NtfsEfiFreeAttrCtx (StdCtx);
        }
    }
    A.FileAttr = OldAttr & ~(NTFS_FILE_TYPE_READ_ONLY | NTFS_FILE_TYPE_HIDDEN |
                             NTFS_FILE_TYPE_SYSTEM | NTFS_FILE_TYPE_ARCHIVE);
    if (Info->Attribute & EFI_FILE_READ_ONLY) A.FileAttr |= NTFS_FILE_TYPE_READ_ONLY;
    if (Info->Attribute & EFI_FILE_HIDDEN)    A.FileAttr |= NTFS_FILE_TYPE_HIDDEN;
    if (Info->Attribute & EFI_FILE_SYSTEM)    A.FileAttr |= NTFS_FILE_TYPE_SYSTEM;
    if (Info->Attribute & EFI_FILE_ARCHIVE)   A.FileAttr |= NTFS_FILE_TYPE_ARCHIVE;

    /* walk the record: patch $STANDARD_INFORMATION and every $FILE_NAME */
    Attr    = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + Rec->AttributeOffset);
    LastPtr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + Rec->BytesInUse);
    while (Attr < LastPtr && Attr->Type != (ULONG)AttributeEnd) {
        if (Attr->Length == 0) break;
        if (Attr->Type == (ULONG)AttributeStandardInformation && !Attr->IsNonResident) {
            PSTANDARD_INFORMATION Si =
                (PSTANDARD_INFORMATION)((PUCHAR)Attr + Attr->Resident.ValueOffset);
            if (A.SetCreate) Si->CreationTime   = A.Create;
            if (A.SetWrite)  Si->LastWriteTime  = A.Write;
            if (A.SetWrite)  Si->ChangeTime     = A.Write;
            if (A.SetAccess) Si->LastAccessTime = A.Access;
            /* $STD_INFO keeps the DOS bits but never the DIRECTORY bit
             * (our create convention; matches Windows) */
            Si->FileAttribute = A.FileAttr & ~NTFS_FILE_TYPE_DIRECTORY;
        } else if (Attr->Type == (ULONG)AttributeFileName && !Attr->IsNonResident) {
            PFILENAME_ATTRIBUTE Fn =
                (PFILENAME_ATTRIBUTE)((PUCHAR)Attr + Attr->Resident.ValueOffset);
            if (!HaveParent) {
                ParentMFT  = Fn->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
                HaveParent = TRUE;
            }
            NtfsApplyToFileName (Fn, &A);
        }
        Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Attr + Attr->Length);
    }

    Status = NtfsEfiWriteFileRecord (Vcb, MFTIndex, Rec);
    FreePool (Rec);
    if (EFI_ERROR (Status)) return Status;

    if (HaveParent) {
        /* best effort: the file record is already consistent; keep the
         * parent index copy in sync so chkdsk sees matching duplicates */
        NtfsPatchParentIndexEntry (Vcb, ParentMFT, MFTIndex, &A);
    }
    return EFI_SUCCESS;
}

/*
 * Would moving MFTIndex under DestParentMFT create a directory cycle? True if
 * DestParent IS the moved node or one of its descendants - i.e. MFTIndex is an
 * ancestor of DestParent. Walk DestParent's chain of $FILE_NAME parent refs up
 * toward the root; a hit on MFTIndex means the move would detach a loop that
 * chkdsk would then have to reparent. Iteration-capped against corrupt chains.
 */
static BOOLEAN
NtfsMoveWouldCycle (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex,
    IN ULONGLONG     DestParentMFT
    )
{
    PFILE_RECORD_HEADER Rec;
    ULONGLONG           Cur = DestParentMFT;
    UINTN               Hops;
    BOOLEAN             Cycle = FALSE;

    if (DestParentMFT == MFTIndex) return TRUE;

    Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) return TRUE;   /* fail safe: refuse if we can't verify */

    for (Hops = 0; Hops < 4096 && Cur != NTFS_FILE_ROOT; Hops++) {
        PNTFS_ATTR_CTX      FnCtx;
        ULONG               FnOff = 0;
        PFILENAME_ATTRIBUTE Fn;

        if (Cur == MFTIndex) { Cycle = TRUE; break; }
        if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, Cur, Rec))) { Cycle = TRUE; break; }

        FnCtx = NtfsEfiFindAttrInRecord (Vcb, Rec, AttributeFileName, NULL, 0, &FnOff);
        if (FnCtx == NULL) { Cycle = TRUE; break; }   /* can't verify -> refuse */
        NtfsEfiFreeAttrCtx (FnCtx);

        Fn  = (PFILENAME_ATTRIBUTE)((PUCHAR)Rec + ((PNTFS_ATTR_RECORD)((PUCHAR)Rec + FnOff))->Resident.ValueOffset);
        Cur = Fn->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
    }

    FreePool (Rec);
    return Cycle;
}

/*
 * Move (and/or rename) a file. If DestParentMFT == (ULONGLONG)-1 the file stays
 * in its current directory (pure rename); otherwise it is relinked under
 * DestParentMFT (cross-directory move). Steps, ordered so a failure leaves the
 * volume consistent:
 *   1. resolve the destination parent + reject collisions / directory cycles,
 *   2. rewrite the file's own $FILE_NAME (new name AND new parent ref) and
 *      write the record,
 *   3. unlink the old name(s) from the OLD parent index,
 *   4. insert a fresh entry under NewName in the DESTINATION parent,
 *   5. copy the file's duplicated info (times/sizes/attrs) into that entry so
 *      it byte-matches the file's own $FILE_NAME (chkdsk cross-check).
 * A directory carries no ".." entry in NTFS - its parent lives only in its own
 * $FILE_NAME ref - so moving a directory needs no child-side fixups beyond the
 * parent-ref rewrite in step 2.
 */
EFI_STATUS
NtfsEfiMoveFile (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex,
    IN ULONGLONG     DestParentMFT,
    IN CONST WCHAR  *NewName,
    IN UINTN         NewNameLen
    )
{
    PFILE_RECORD_HEADER Rec, ParentRec;
    ULONG               FnOff = 0;
    PNTFS_ATTR_CTX      FnCtx;
    PNTFS_ATTR_RECORD   FnAttr;
    PFILENAME_ATTRIBUTE Fn;
    ULONGLONG           OldParentMFT, Existing;
    UINT64              DestParentRef, ChildRef;
    BOOLEAN             IsDir, CrossDir;
    NTFS_SETINFO_APPLY  A;
    UINT64              SvCreate, SvWrite, SvChange, SvAccess, SvAlloc, SvData;
    ULONG               SvAttr, StartEntry = 0;
    ULONG               NewValLen, NewAttrLen, OldAttrLen, VOff;
    LONG                Delta;
    PUCHAR              NextAttr;
    UINTN               TailLen;
    EFI_STATUS          Status;

    if (MFTIndex < NTFS_FILE_FIRST_USER_FILE) return EFI_ACCESS_DENIED;
    if (NewNameLen == 0 || NewNameLen > 255) return EFI_INVALID_PARAMETER;

    NtfsMarkVolumeDirty (Vcb);

    Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, MFTIndex, Rec))) {
        FreePool (Rec);
        return EFI_DEVICE_ERROR;
    }

    FnCtx = NtfsEfiFindAttrInRecord (Vcb, Rec, AttributeFileName, NULL, 0, &FnOff);
    if (FnCtx == NULL) { FreePool (Rec); return EFI_VOLUME_CORRUPTED; }
    NtfsEfiFreeAttrCtx (FnCtx);

    FnAttr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + FnOff);
    VOff   = FnAttr->Resident.ValueOffset;
    Fn     = (PFILENAME_ATTRIBUTE)((PUCHAR)FnAttr + VOff);
    OldParentMFT = Fn->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
    IsDir        = (Rec->Flags & FRH_DIRECTORY) != 0;
    ChildRef     = MFTIndex | ((UINT64)Rec->SequenceNumber << 48);

    /* same-directory sentinel: keep the file's existing parent ref */
    if (DestParentMFT == (ULONGLONG)-1LL) {
        DestParentMFT = OldParentMFT;
        DestParentRef = Fn->DirectoryFileReferenceNumber;
        CrossDir      = FALSE;
    } else {
        CrossDir = (DestParentMFT != OldParentMFT);
    }

    /* snapshot the duplicated info (unchanged by move except the name/parent) */
    SvCreate = Fn->CreationTime; SvWrite = Fn->LastWriteTime;
    SvChange = Fn->ChangeTime;   SvAccess = Fn->LastAccessTime;
    SvAlloc  = Fn->AllocatedSize; SvData = Fn->DataSize; SvAttr = Fn->FileAttributes;

    /* 1a. for a real cross-directory move, validate the destination */
    if (CrossDir) {
        PFILE_RECORD_HEADER DestRec = AllocatePool (Vcb->BytesPerFileRecord);
        if (DestRec == NULL) { FreePool (Rec); return EFI_OUT_OF_RESOURCES; }
        if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, DestParentMFT, DestRec))) {
            FreePool (DestRec); FreePool (Rec); return EFI_DEVICE_ERROR;
        }
        if (!(DestRec->Flags & FRH_IN_USE) || !(DestRec->Flags & FRH_DIRECTORY)) {
            FreePool (DestRec); FreePool (Rec); return EFI_INVALID_PARAMETER;
        }
        DestParentRef = DestParentMFT | ((UINT64)DestRec->SequenceNumber << 48);
        FreePool (DestRec);

        if (IsDir && NtfsMoveWouldCycle (Vcb, MFTIndex, DestParentMFT)) {
            FreePool (Rec);
            return EFI_ACCESS_DENIED;   /* would detach a directory loop */
        }
    }

    /* 1b. reject a collision with a different existing file in the dest dir */
    Existing = NtfsEfiFindInDirectory (Vcb, DestParentMFT, NewName, NewNameLen, FALSE, FALSE, &StartEntry);
    if (Existing != (ULONGLONG)-1LL && Existing != MFTIndex) {
        FreePool (Rec);
        return EFI_ACCESS_DENIED;
    }

    /* 2. resize + rewrite the file's own $FILE_NAME (new name + new parent) */
    NewValLen  = (ULONG)OFFSET_OF (FILENAME_ATTRIBUTE, Name) + (ULONG)(NewNameLen * sizeof (WCHAR));
    OldAttrLen = FnAttr->Length;
    NewAttrLen = (ULONG)ROUND_UP (VOff + NewValLen, ATTR_RECORD_ALIGNMENT);
    Delta      = (LONG)NewAttrLen - (LONG)OldAttrLen;
    if (Delta > 0 && (UINT64)Delta > (UINT64)Rec->BytesAllocated - Rec->BytesInUse) {
        FreePool (Rec);
        return EFI_UNSUPPORTED;   /* record too full for the longer name */
    }
    NextAttr = (PUCHAR)FnAttr + OldAttrLen;
    TailLen  = Rec->BytesInUse - (ULONG)(NextAttr - (PUCHAR)Rec);
    if (Delta != 0) CopyMem (NextAttr + Delta, NextAttr, TailLen);   /* memmove-safe */

    FnAttr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + FnOff);
    Fn     = (PFILENAME_ATTRIBUTE)((PUCHAR)FnAttr + VOff);
    Fn->NameLength = (UCHAR)NewNameLen;
    Fn->DirectoryFileReferenceNumber = DestParentRef;
    CopyMem (Fn->Name, NewName, NewNameLen * sizeof (WCHAR));
    FnAttr->Resident.ValueLength = NewValLen;
    FnAttr->Length               = NewAttrLen;
    Rec->BytesInUse              = (ULONG)((LONG)Rec->BytesInUse + Delta);

    Status = NtfsEfiWriteFileRecord (Vcb, MFTIndex, Rec);
    FreePool (Rec);
    if (EFI_ERROR (Status)) return Status;

    /* 3. unlink the old name(s) from the OLD parent */
    NtfsEfiIndexRemoveByChild (Vcb, OldParentMFT, MFTIndex);

    /* 4. insert the new name into the DESTINATION parent */
    ParentRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (ParentRec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, DestParentMFT, ParentRec))) {
        FreePool (ParentRec);
        return EFI_DEVICE_ERROR;
    }
    Status = NtfsInsertIndexEntry (Vcb, ParentRec, DestParentMFT, ChildRef, DestParentRef,
                 SvWrite, NewName, NewNameLen, IsDir);
    if (!EFI_ERROR (Status)) {
        Status = NtfsEfiWriteFileRecord (Vcb, DestParentMFT, ParentRec);
    }
    FreePool (ParentRec);
    if (EFI_ERROR (Status)) return Status;

    /* 5. make the new index entry's duplicated info match the file exactly */
    ZeroMem (&A, sizeof (A));
    A.SetCreate = TRUE; A.Create = SvCreate;
    A.SetWrite  = TRUE; A.Write  = SvWrite;
    A.SetChange = TRUE; A.Change = SvChange;
    A.SetAccess = TRUE; A.Access = SvAccess;
    A.SetSizes  = TRUE; A.Alloc  = SvAlloc; A.Data = SvData;
    A.FileAttr  = SvAttr; A.KeepDirBit = FALSE;
    NtfsPatchParentIndexEntry (Vcb, DestParentMFT, MFTIndex, &A);

    return EFI_SUCCESS;
}

/*
 * Free the clusters of a non-resident attribute beyond KeepClusters and
 * re-encode the surviving mapping pairs in place, mirroring MS NTFS's
 * NtfsDeleteAllocation/NtfsTruncateAllocation. Adjusts HighestVCN, AllocatedSize,
 * the attribute Length and Rec->BytesInUse. DataSize / InitializedSize are left
 * to the caller's policy (a truncate lowers them; a prealloc trim keeps them).
 * KeepClusters == 0 leaves a valid empty non-resident attr (HighestVCN == -1).
 */
static EFI_STATUS
NtfsShrinkNonResidentRuns (
    IN     PNTFS_EFI_VCB       Vcb,
    IN OUT PFILE_RECORD_HEADER Rec,
    IN OUT PNTFS_ATTR_RECORD   Attr,
    IN     UINT64              KeepClusters
    )
{
    NTFS_RUN_ENTRY *Runs;
    PUCHAR  MP, MPDst, NextAttr;
    ULONG   RunCount = 0, Keep = 0, i;
    UINT64  Acc = 0;
    UINTN   MPLen = 0, TailLen;
    INT64   PrevLBN = 0;
    ULONG   OldAttrLen, NewAttrLen;
    LONG    GrowthBytes;

    Runs = AllocatePool (NTFS_MAX_RUNS * sizeof (NTFS_RUN_ENTRY));
    MP   = AllocatePool (NTFS_MAX_RUNS * 9 + 8);
    if (Runs == NULL || MP == NULL) {
        if (Runs) FreePool (Runs); if (MP) FreePool (MP);
        return EFI_OUT_OF_RESOURCES;
    }
    if (EFI_ERROR (NtfsBuildRunList (Attr, Runs, NTFS_MAX_RUNS, &RunCount))) {
        FreePool (Runs); FreePool (MP); return EFI_VOLUME_CORRUPTED;
    }

    /* keep clusters below the cut, free everything at or beyond it */
    for (i = 0; i < RunCount; i++) {
        UINT64 Start = Acc;
        UINT64 End   = Acc + Runs[i].Len;
        if (Start >= KeepClusters) {
            if (Runs[i].LBN != -1LL)
                NtfsEfiFreeClusters (Vcb, (UINT64)Runs[i].LBN, Runs[i].Len);
        } else if (End > KeepClusters) {
            UINT64 KeepLen = KeepClusters - Start;
            if (Runs[i].LBN != -1LL)
                NtfsEfiFreeClusters (Vcb, (UINT64)Runs[i].LBN + KeepLen, Runs[i].Len - KeepLen);
            Runs[Keep] = Runs[i];
            Runs[Keep].Len = KeepLen;
            Keep++;
        } else {
            Runs[Keep++] = Runs[i];
        }
        Acc = End;
    }

    /* re-encode the surviving runs (empty stream if KeepClusters == 0) */
    for (i = 0; i < Keep; i++) {
        if (Runs[i].LBN == -1LL) { FreePool (Runs); FreePool (MP); return EFI_UNSUPPORTED; }
        MPLen += NtfsEncodeRunEntry (MP + MPLen, Runs[i].Len, Runs[i].LBN - PrevLBN);
        PrevLBN = Runs[i].LBN;
    }
    MP[MPLen++] = 0;   /* terminator */

    OldAttrLen  = Attr->Length;
    NewAttrLen  = (ULONG)ROUND_UP (Attr->NonResident.MappingPairsOffset + MPLen, ATTR_RECORD_ALIGNMENT);
    GrowthBytes = (LONG)NewAttrLen - (LONG)OldAttrLen;   /* <= 0 when shrinking */

    NextAttr = (PUCHAR)Attr + OldAttrLen;
    TailLen  = Rec->BytesInUse - (ULONG)(NextAttr - (PUCHAR)Rec);
    if (GrowthBytes != 0) CopyMem (NextAttr + GrowthBytes, NextAttr, TailLen);   /* memmove-safe */

    MPDst = (PUCHAR)Attr + Attr->NonResident.MappingPairsOffset;
    CopyMem (MPDst, MP, MPLen);
    /* Zero the mapping-pairs slack after the new (shorter) stream. Shrinking
     * leaves the tail of the OLD, longer stream behind the terminator; chkdsk
     * reads those stale bytes as a bogus extra run and flags $DATA corrupt.
     * (Append never hit this - it only ever grows the stream.) */
    if ((PUCHAR)Attr + NewAttrLen > MPDst + MPLen)
        ZeroMem (MPDst + MPLen, (UINTN)(((PUCHAR)Attr + NewAttrLen) - (MPDst + MPLen)));

    Attr->Length                    = NewAttrLen;
    Attr->NonResident.HighestVCN    = (KeepClusters == 0) ? (LONGLONG)-1 : (LONGLONG)(KeepClusters - 1);
    Attr->NonResident.AllocatedSize = (LONGLONG)(KeepClusters * Vcb->BytesPerCluster);
    Rec->BytesInUse                 = (ULONG)((LONG)Rec->BytesInUse + GrowthBytes);

    FreePool (Runs);
    FreePool (MP);
    return EFI_SUCCESS;
}

/*
 * Write NewData/NewAlloc into the size fields NTFS duplicates in every one of
 * the record's $FILE_NAME attributes, and return the parent directory MFT (from
 * the first $FILE_NAME) plus the file's current FileAttributes - so the caller
 * can mirror the identical sizes into the parent index entry. All three copies
 * (attribute, own $FILE_NAME, parent index) must agree or chkdsk flags the file.
 */
static ULONGLONG
NtfsRewriteOwnNameSizes (
    IN OUT PFILE_RECORD_HEADER Rec,
    IN     UINT64              NewData,
    IN     UINT64              NewAlloc,
    OUT    ULONG              *CurFileAttr
    )
{
    PNTFS_ATTR_RECORD W     = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + Rec->AttributeOffset);
    PNTFS_ATTR_RECORD WLast = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + Rec->BytesInUse);
    ULONGLONG         Parent = 0;
    BOOLEAN           HaveParent = FALSE;

    *CurFileAttr = 0;
    while (W < WLast && W->Type != (ULONG)AttributeEnd && W->Length != 0) {
        if (W->Type == (ULONG)AttributeFileName && !W->IsNonResident) {
            PFILENAME_ATTRIBUTE Wf = (PFILENAME_ATTRIBUTE)((PUCHAR)W + W->Resident.ValueOffset);
            Wf->AllocatedSize = NewAlloc;
            Wf->DataSize      = NewData;
            *CurFileAttr      = Wf->FileAttributes;
            if (!HaveParent) { Parent = Wf->DirectoryFileReferenceNumber & NTFS_MFT_MASK; HaveParent = TRUE; }
        }
        W = (PNTFS_ATTR_RECORD)((PUCHAR)W + W->Length);
    }
    return Parent;
}

/*
 * Trim prealloc slack: after a growing write leaves AllocatedSize rounded up to
 * a preallocation quantum past the actual DataSize, release the tail clusters so
 * the file occupies only what it needs - exactly what NTFS.sys does at cleanup
 * (NtfsTruncateAllocation to ValidDataLength). No-op for resident data or when
 * there is no slack. DataSize / InitializedSize are unchanged.
 */
EFI_STATUS
NtfsEfiTrimAllocation (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex
    )
{
    PFILE_RECORD_HEADER Rec;
    PNTFS_ATTR_CTX      DataCtx;
    PNTFS_ATTR_RECORD   Attr;
    ULONG               DataOff = 0, CurFileAttr = 0;
    UINT64              DataSize, AllocSize, NeedClusters, HaveClusters, NewAlloc;
    ULONGLONG           ParentMFT;
    NTFS_SETINFO_APPLY  A;
    EFI_STATUS          Status;

    if (MFTIndex < NTFS_FILE_FIRST_USER_FILE) return EFI_SUCCESS;

    Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, MFTIndex, Rec))) { FreePool (Rec); return EFI_DEVICE_ERROR; }

    DataCtx = NtfsEfiFindAttrInRecord (Vcb, Rec, AttributeData, NULL, 0, &DataOff);
    if (DataCtx == NULL) { FreePool (Rec); return EFI_SUCCESS; }   /* nothing to trim */
    NtfsEfiFreeAttrCtx (DataCtx);
    Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + DataOff);
    if (!Attr->IsNonResident) { FreePool (Rec); return EFI_SUCCESS; }

    DataSize  = (UINT64)Attr->NonResident.DataSize;
    AllocSize = (UINT64)Attr->NonResident.AllocatedSize;
    NeedClusters = (DataSize + Vcb->BytesPerCluster - 1) / Vcb->BytesPerCluster;
    HaveClusters = AllocSize / Vcb->BytesPerCluster;
    if (NeedClusters >= HaveClusters) { FreePool (Rec); return EFI_SUCCESS; }   /* no slack */

    NtfsMarkVolumeDirty (Vcb);

    Status = NtfsShrinkNonResidentRuns (Vcb, Rec, Attr, NeedClusters);
    if (EFI_ERROR (Status)) { FreePool (Rec); return Status; }

    NewAlloc  = NeedClusters * Vcb->BytesPerCluster;
    ParentMFT = NtfsRewriteOwnNameSizes (Rec, DataSize, NewAlloc, &CurFileAttr);

    Status = NtfsEfiWriteFileRecord (Vcb, MFTIndex, Rec);
    FreePool (Rec);
    if (EFI_ERROR (Status)) return Status;

    ZeroMem (&A, sizeof (A));
    A.SetSizes = TRUE; A.Alloc = NewAlloc; A.Data = DataSize;
    A.FileAttr = CurFileAttr; A.KeepDirBit = TRUE;
    NtfsPatchParentIndexEntry (Vcb, ParentMFT, MFTIndex, &A);
    return EFI_SUCCESS;
}

/*
 * Truncate a file's unnamed $DATA to NewSize (SetInfo with a smaller FileSize).
 * Shrink only - a request to GROW past the current size is refused
 * (EFI_UNSUPPORTED): apps extend a file by writing, and growing here would need
 * on-disk zero-fill of the newly exposed range, which is out of this step's
 * scope. Handles both resident (cut the value in place) and non-resident (free
 * the tail clusters, re-encode the truncated mapping-pair stream) $DATA, then
 * syncs the file's own $FILE_NAME and the parent index copy so chkdsk's size
 * cross-checks match. NewSize == current is a no-op success.
 */
EFI_STATUS
NtfsEfiSetFileSize (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex,
    IN UINT64        NewSize
    )
{
    PFILE_RECORD_HEADER Rec;
    PNTFS_ATTR_CTX      DataCtx;
    PNTFS_ATTR_RECORD   Attr;
    ULONG               DataOff = 0;
    ULONGLONG           ParentMFT;
    UINT64              NewData, NewAlloc;
    NTFS_SETINFO_APPLY  A;
    EFI_STATUS          Status;

    if (MFTIndex < NTFS_FILE_FIRST_USER_FILE) return EFI_ACCESS_DENIED;

    Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, MFTIndex, Rec))) {
        FreePool (Rec);
        return EFI_DEVICE_ERROR;
    }
    if (Rec->Flags & FRH_DIRECTORY) { FreePool (Rec); return EFI_UNSUPPORTED; }

    DataCtx = NtfsEfiFindAttrInRecord (Vcb, Rec, AttributeData, NULL, 0, &DataOff);
    if (DataCtx == NULL) { FreePool (Rec); return EFI_VOLUME_CORRUPTED; }
    NtfsEfiFreeAttrCtx (DataCtx);
    Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + DataOff);

    /* parent for the index dup-info sync (from the file's own $FILE_NAME) */
    {
        ULONG FnOff = 0;
        PNTFS_ATTR_CTX FnCtx = NtfsEfiFindAttrInRecord (Vcb, Rec, AttributeFileName, NULL, 0, &FnOff);
        if (FnCtx == NULL) { FreePool (Rec); return EFI_VOLUME_CORRUPTED; }
        NtfsEfiFreeAttrCtx (FnCtx);
        ParentMFT = ((PFILENAME_ATTRIBUTE)((PUCHAR)Rec +
            ((PNTFS_ATTR_RECORD)((PUCHAR)Rec + FnOff))->Resident.ValueOffset))
            ->DirectoryFileReferenceNumber & NTFS_MFT_MASK;
    }

    NtfsMarkVolumeDirty (Vcb);

    if (!Attr->IsNonResident) {
        /* ---- resident $DATA ---- */
        UINT64 Cur = Attr->Resident.ValueLength;
        ULONG  VOff = Attr->Resident.ValueOffset;
        ULONG  OldAttrLen, NewAttrLen;
        LONG   Delta;
        PUCHAR NextAttr;
        UINTN  TailLen;

        if (NewSize == Cur) { FreePool (Rec); return EFI_SUCCESS; }
        if (NewSize > Cur)  { FreePool (Rec); return EFI_UNSUPPORTED; }

        OldAttrLen = Attr->Length;
        NewAttrLen = (ULONG)ROUND_UP (VOff + NewSize, ATTR_RECORD_ALIGNMENT);
        Delta      = (LONG)NewAttrLen - (LONG)OldAttrLen;
        NextAttr   = (PUCHAR)Attr + OldAttrLen;
        TailLen    = Rec->BytesInUse - (ULONG)(NextAttr - (PUCHAR)Rec);
        if (Delta != 0) CopyMem (NextAttr + Delta, NextAttr, TailLen);   /* memmove-safe */

        Attr->Resident.ValueLength = (ULONG)NewSize;
        Attr->Length               = NewAttrLen;
        Rec->BytesInUse            = (ULONG)((LONG)Rec->BytesInUse + Delta);

        NewData  = NewSize;
        NewAlloc = ROUND_UP (NewSize, Vcb->BytesPerCluster);
    } else {
        /* ---- non-resident $DATA: free the tail clusters, re-encode ---- */
        UINT64 OldData = (UINT64)Attr->NonResident.DataSize;
        UINT64 OldInit = (UINT64)Attr->NonResident.InitializedSize;
        UINT64 NewClusters = (NewSize + Vcb->BytesPerCluster - 1) / Vcb->BytesPerCluster;

        if (NewSize == OldData) { FreePool (Rec); return EFI_SUCCESS; }
        if (NewSize > OldData)  { FreePool (Rec); return EFI_UNSUPPORTED; }

        Status = NtfsShrinkNonResidentRuns (Vcb, Rec, Attr, NewClusters);
        if (EFI_ERROR (Status)) { FreePool (Rec); return Status; }

        Attr->NonResident.DataSize        = (LONGLONG)NewSize;
        Attr->NonResident.InitializedSize = (LONGLONG)((OldInit < NewSize) ? OldInit : NewSize);

        NewData  = NewSize;
        NewAlloc = NewClusters * Vcb->BytesPerCluster;
    }

    /* mirror the new sizes into the file's own $FILE_NAME(s) and the parent
     * index entry (FileAttr carried through unchanged) so chkdsk's three-way
     * size cross-check agrees. */
    {
        ULONG CurFileAttr = 0;
        (VOID)NtfsRewriteOwnNameSizes (Rec, NewData, NewAlloc, &CurFileAttr);

        Status = NtfsEfiWriteFileRecord (Vcb, MFTIndex, Rec);
        FreePool (Rec);
        if (EFI_ERROR (Status)) return Status;

        ZeroMem (&A, sizeof (A));
        A.SetSizes = TRUE; A.Alloc = NewAlloc; A.Data = NewData;
        A.FileAttr = CurFileAttr; A.KeepDirBit = TRUE;
        NtfsPatchParentIndexEntry (Vcb, ParentMFT, MFTIndex, &A);
    }
    return EFI_SUCCESS;
}

/* Rename within the same directory: thin wrapper over the general move path. */
EFI_STATUS
NtfsEfiRenameFile (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex,
    IN CONST WCHAR  *NewName,
    IN UINTN         NewNameLen
    )
{
    return NtfsEfiMoveFile (Vcb, MFTIndex, (ULONGLONG)-1LL, NewName, NewNameLen);
}
