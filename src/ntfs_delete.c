/**
 * ntfs_delete.c - file / empty-directory deletion.
 *
 * Inverse of ntfs_create.c. Deliberately restricted, cautious scope for
 * this round (mirrors the create restrictions):
 *
 *  - separator keys (B+tree internal nodes, NTFS_INDEX_ENTRY_NODE set) are
 *    handled via rebalance-on-delete: the in-order predecessor is extracted
 *    recursively from the child subtree (NtfsExtractMaxKey) and promoted
 *    into the separator slot. If the subtree is empty the separator is simply
 *    removed. Only if the replacement would overflow the host block is the
 *    operation refused (EFI_UNSUPPORTED).
 *  - only single-linked files (LinkCount == 1) are deleted; a file with
 *    extra hard links in other directories is refused so we never free a
 *    record another name still points at.
 *  - directories must be empty.
 *  - system records (< NTFS_FILE_FIRST_USER_FILE) are never touched.
 *
 * Ordering is chosen so a crash mid-delete can only ever LEAK space
 * (reclaimable by chkdsk), never leave a dangling reference:
 *   1. remove the name(s) from the parent index  (file becomes unreachable)
 *   2. free the $DATA / $INDEX_ALLOCATION / $BITMAP clusters
 *   3. clear FRH_IN_USE + bump the record sequence number, rewrite record
 *   4. release the record's bit in $MFT's $BITMAP
 * The parent index write in step 1 is the only structurally-visible change;
 * everything after it is cleanup of an already-orphaned object.
 */

#include "ntfs.h"

/* Shrink a resident attribute in-place by cutting RemoveLen bytes out of
 * its value at value-offset RemoveAt, then collapsing the record tail and
 * the attribute/record length fields. Exact inverse of the grow path used
 * by the $INDEX_ROOT insert. */
static VOID
NtfsShrinkResidentInRecord (
    IN OUT PFILE_RECORD_HEADER Rec,
    IN     ULONG                AttrOffset,
    IN     ULONG                RemoveAt,     /* offset within the attr value */
    IN     ULONG                RemoveLen
    )
{
    PNTFS_ATTR_RECORD Attr    = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + AttrOffset);
    PUCHAR            Val      = (PUCHAR)Attr + Attr->Resident.ValueOffset;
    ULONG             ValueLen = Attr->Resident.ValueLength;
    ULONG             NewValueLen = ValueLen - RemoveLen;
    ULONG             OldAttrLen  = Attr->Length;
    ULONG             NewAttrLen  = (ULONG)ROUND_UP (Attr->Resident.ValueOffset + NewValueLen,
                                                     ATTR_RECORD_ALIGNMENT);
    ULONG             Delta       = OldAttrLen - NewAttrLen;
    PUCHAR            TailSrc;
    UINTN             TailLen;

    /* drop the removed bytes out of the value (CopyMem is overlap-safe) */
    CopyMem (Val + RemoveAt, Val + RemoveAt + RemoveLen, (UINTN)(ValueLen - RemoveAt - RemoveLen));
    Attr->Resident.ValueLength = NewValueLen;

    /* collapse the record tail (attributes after this one) back by Delta */
    if (Delta > 0) {
        TailSrc = (PUCHAR)Attr + OldAttrLen;
        TailLen = Rec->BytesInUse - (ULONG)(TailSrc - (PUCHAR)Rec);
        CopyMem ((PUCHAR)Attr + NewAttrLen, TailSrc, TailLen);
        Attr->Length     = NewAttrLen;
        Rec->BytesInUse -= Delta;
    }
}

/* Replace OldLen bytes at value-offset At in a resident attribute with NewLen
 * bytes from Src, growing OR shrinking the attribute and the record in place.
 * Generalises NtfsShrinkResidentInRecord to a same-slot resize; caller must
 * have already checked the record has room when NewLen > OldLen. */
static VOID
NtfsSpliceResidentInRecord (
    IN OUT PFILE_RECORD_HEADER Rec,
    IN     ULONG                AttrOffset,
    IN     ULONG                At,        /* offset within the attr value */
    IN     ULONG                OldLen,
    IN     CONST UCHAR         *Src,
    IN     ULONG                NewLen
    )
{
    PNTFS_ATTR_RECORD Attr        = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + AttrOffset);
    PUCHAR            Val         = (PUCHAR)Attr + Attr->Resident.ValueOffset;
    ULONG             ValueLen    = Attr->Resident.ValueLength;
    ULONG             OldAttrLen  = Attr->Length;
    ULONG             NewValueLen = ValueLen - OldLen + NewLen;
    ULONG             NewAttrLen  = (ULONG)ROUND_UP (Attr->Resident.ValueOffset + NewValueLen,
                                                     ATTR_RECORD_ALIGNMENT);
    INTN              AttrDelta   = (INTN)NewAttrLen - (INTN)OldAttrLen;

    /* resize the hole inside the value, then drop the new bytes in
     * (CopyMem is overlap-safe, so a grow or shrink both work) */
    CopyMem (Val + At + NewLen, Val + At + OldLen,
             (UINTN)(ValueLen - At - OldLen));
    CopyMem (Val + At, Src, NewLen);
    Attr->Resident.ValueLength = NewValueLen;

    /* slide the record tail (attributes after this one) by AttrDelta */
    if (AttrDelta != 0) {
        PUCHAR TailSrc = (PUCHAR)Attr + OldAttrLen;
        UINTN  TailLen = Rec->BytesInUse - (ULONG)(TailSrc - (PUCHAR)Rec);
        CopyMem ((PUCHAR)Attr + NewAttrLen, TailSrc, TailLen);
        Attr->Length     = NewAttrLen;
        Rec->BytesInUse  = (ULONG)((INTN)Rec->BytesInUse + AttrDelta);
    }
}

/* Build a separator index entry into Dest from a predecessor key (leaf-form:
 * IndexedFile ref + FILENAME) plus a child VCN. Returns the entry length. */
static ULONG
NtfsBuildSeparatorEntry (
    OUT PUCHAR                 Dest,
    IN  PINDEX_ENTRY_ATTRIBUTE Key,
    IN  ULONGLONG              ChildVcn
    )
{
    ULONG                  Body = (ULONG)OFFSET_OF (INDEX_ENTRY_ATTRIBUTE, FileName) + Key->KeyLength;
    ULONG                  Len  = (ULONG)ROUND_UP (Body, 8) + (ULONG)sizeof (ULONGLONG);
    PINDEX_ENTRY_ATTRIBUTE R;
    ZeroMem (Dest, Len);
    CopyMem (Dest, Key, Body);
    R            = (PINDEX_ENTRY_ATTRIBUTE)Dest;
    R->Length    = (USHORT)Len;
    R->KeyLength = Key->KeyLength;
    R->Flags     = NTFS_INDEX_ENTRY_NODE;
    R->Reserved  = 0;
    *(PULONGLONG)(Dest + Len - sizeof (ULONGLONG)) = ChildVcn;
    return Len;
}

/* Splice Repl/ReplLen bytes (ReplLen==0 => pure removal) over the entry at
 * OldEntry (length OldLen) inside an in-memory INDX block, fixing up
 * TotalSizeOfEntries. Returns FALSE only if a grow would overflow the block. */
static BOOLEAN
NtfsSpliceInIndexBlock (
    IN OUT PINDEX_BUFFER          Block,
    IN     PINDEX_ENTRY_ATTRIBUTE OldEntry,
    IN     ULONG                   OldLen,
    IN     CONST UCHAR            *Repl,
    IN     ULONG                   ReplLen
    )
{
    INTN   Delta = (INTN)ReplLen - (INTN)OldLen;
    PUCHAR After = (PUCHAR)OldEntry + OldLen;
    PUCHAR EndOf = (PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries;
    UINTN  Tail  = (UINTN)(EndOf - After);

    if (Delta > 0 &&
        (INTN)Block->Header.TotalSizeOfEntries + Delta > (INTN)Block->Header.AllocatedSize)
        return FALSE;

    CopyMem ((PUCHAR)OldEntry + ReplLen, After, Tail);
    if (ReplLen > 0) CopyMem ((PUCHAR)OldEntry, Repl, ReplLen);
    if (Delta < 0)
        ZeroMem ((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries + Delta, (UINTN)(-Delta));
    Block->Header.TotalSizeOfEntries =
        (ULONG)((INTN)Block->Header.TotalSizeOfEntries + Delta);
    return TRUE;
}

/* Clear the $I30 allocation-bitmap bit for a single INDX block at BlockVcn
 * (resident bitmap assumed - typical for <64 blocks). Does NOT write back
 * the directory record; caller is responsible for flushing DirRec.
 * Harmless if $BITMAP:$I30 is absent or the bit is already clear. */
static VOID
NtfsClearIndexBlockAlloc (
    IN PFILE_RECORD_HEADER DirRec,
    IN ULONGLONG           BlockVcn
    )
{
    PNTFS_ATTR_RECORD Attr = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + DirRec->AttributeOffset);
    while (Attr->Type != (ULONG)AttributeEnd && Attr->Length > 0) {
        if (Attr->Type == AttributeBitmap && !Attr->IsNonResident &&
            Attr->NameLength == 4 &&
            CompareMem ((PUCHAR)Attr + Attr->NameOffset, L"$I30", 8) == 0) {
            PUCHAR Bm    = (PUCHAR)Attr + Attr->Resident.ValueOffset;
            ULONG  BitNo = (ULONG)BlockVcn;
            if (BitNo / 8 < Attr->Resident.ValueLength)
                Bm[BitNo / 8] &= (UCHAR)~((UCHAR)1 << (BitNo % 8));
            return;
        }
        Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Attr + Attr->Length);
    }
}

/* Recursively free every INDX block reachable from Vcn (clearing their
 * $I30 bitmap bits in DirRec) and then free Vcn itself.  Used to clean up
 * INDX blocks that become unreachable when a separator is deleted.  Writes
 * back DirRec (once) so the bitmap changes are durable.  Bounded by Depth. */
static VOID
NtfsFreeIndexSubtree (
    IN  PNTFS_EFI_VCB       Vcb,
    IN  PNTFS_ATTR_CTX      Alloc,
    IN  PFILE_RECORD_HEADER DirRec,
    IN  ULONGLONG           DirMFT,
    IN  ULONGLONG           Vcn,
    IN  ULONG               Depth
    )
{
    PUCHAR                 Buf;
    PINDEX_BUFFER          Block;
    PINDEX_ENTRY_ATTRIBUTE E, Last;

    if (Depth > 32 || Alloc == NULL) goto Clear;
    Buf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (Buf == NULL) goto Clear;

    if (NtfsEfiReadAttr (Vcb, Alloc, Vcn * Vcb->BytesPerCluster, (PCHAR)Buf,
                         Vcb->BytesPerIndexRecord) != Vcb->BytesPerIndexRecord)
        goto FreeAndClear;
    Block = (PINDEX_BUFFER)Buf;
    if (Block->Ntfs.Type != NRH_INDX_TYPE ||
        EFI_ERROR (NtfsEfiFixupRecord (Vcb, &Block->Ntfs)))
        goto FreeAndClear;

    E    = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
    Last = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries);
    while ((PUCHAR)E < (PUCHAR)Last && E->Length > 0) {
        if (E->Flags & NTFS_INDEX_ENTRY_NODE) {
            ULONGLONG ChildVcn = *(PULONGLONG)((PUCHAR)E + E->Length - sizeof (ULONGLONG));
            NtfsFreeIndexSubtree (Vcb, Alloc, DirRec, DirMFT, ChildVcn, Depth + 1);
        }
        if (E->Flags & NTFS_INDEX_ENTRY_END) break;
        E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)E + E->Length);
    }

FreeAndClear:
    FreePool (Buf);
Clear:
    NtfsClearIndexBlockAlloc (DirRec, Vcn);
    NtfsEfiWriteFileRecord (Vcb, DirMFT, DirRec);
}

/* Extract (and delete from the tree) the maximum real key of the INDX subtree
 * rooted at Vcn - the in-order predecessor of the separator whose child this
 * subtree is.  Descends the rightmost path, but correctly falls back to a
 * node's own last key when a rightmost child subtree is empty (a state our
 * non-merging deletes create), and if that last key is itself a separator,
 * replaces it in place with ITS predecessor (bounded recursion).  Writes back
 * every block it modifies.  When a separator with an empty child is dropped,
 * NtfsFreeIndexSubtree is called so the child's bitmap bit is cleared.
 * Returns:
 *   EFI_SUCCESS   - Out holds the extracted key (leaf-form bytes)
 *   EFI_NOT_FOUND - the subtree is fully empty (no real key anywhere)
 *   other         - I/O / corruption error */
static EFI_STATUS
NtfsExtractMaxKey (
    IN  PNTFS_EFI_VCB       Vcb,
    IN  PNTFS_ATTR_CTX      Alloc,
    IN  PFILE_RECORD_HEADER DirRec,
    IN  ULONGLONG           DirMFT,
    IN  ULONGLONG           Vcn,
    OUT PUCHAR              Out,       /* caller buffer >= BytesPerIndexRecord */
    OUT ULONG              *OutLen,
    IN  ULONG               Depth
    )
{
    PUCHAR                 Buf;
    PINDEX_BUFFER          Block;
    PINDEX_ENTRY_ATTRIBUTE E, First, Last, Prev, End;
    EFI_STATUS             Status = EFI_NOT_FOUND;
    ULONG                  KeyBytes;

    if (Depth > 32) return EFI_UNSUPPORTED;
    Buf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (Buf == NULL) return EFI_OUT_OF_RESOURCES;

    if (NtfsEfiReadAttr (Vcb, Alloc, Vcn * Vcb->BytesPerCluster, (PCHAR)Buf,
                         Vcb->BytesPerIndexRecord) != Vcb->BytesPerIndexRecord) {
        FreePool (Buf); return EFI_DEVICE_ERROR;
    }
    Block = (PINDEX_BUFFER)Buf;
    if (Block->Ntfs.Type != NRH_INDX_TYPE ||
        EFI_ERROR (NtfsEfiFixupRecord (Vcb, &Block->Ntfs))) {
        FreePool (Buf); return EFI_VOLUME_CORRUPTED;
    }

    First = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
    Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries);
    Prev  = NULL;
    E     = First;
    while ((PUCHAR)E < (PUCHAR)Last) {
        if (E->Length == 0) break;
        if (E->Flags & NTFS_INDEX_ENTRY_END) break;
        Prev = E;
        E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)E + E->Length);
    }
    if ((PUCHAR)E >= (PUCHAR)Last) { FreePool (Buf); return EFI_VOLUME_CORRUPTED; }
    End = E;

    /* 1. keys greater than everything here live in the rightmost child */
    if (End->Flags & NTFS_INDEX_ENTRY_NODE) {
        ULONGLONG RightVcn = *(PULONGLONG)((PUCHAR)End + End->Length - sizeof (ULONGLONG));
        Status = NtfsExtractMaxKey (Vcb, Alloc, DirRec, DirMFT, RightVcn, Out, OutLen, Depth + 1);
        if (Status != EFI_NOT_FOUND) { FreePool (Buf); return Status; }
        /* rightmost subtree empty: fall through to this node's own last key */
    }

    /* 2. this node's last real key is the maximum */
    if (Prev == NULL) { FreePool (Buf); return EFI_NOT_FOUND; }   /* empty node */

    KeyBytes = (ULONG)OFFSET_OF (INDEX_ENTRY_ATTRIBUTE, FileName) + Prev->KeyLength;
    CopyMem (Out, Prev, KeyBytes);
    ((PINDEX_ENTRY_ATTRIBUTE)Out)->Length = (USHORT)KeyBytes;
    ((PINDEX_ENTRY_ATTRIBUTE)Out)->Flags  = 0;    /* hand back a clean leaf-form key */
    *OutLen = KeyBytes;

    if (Prev->Flags & NTFS_INDEX_ENTRY_NODE) {
        /* Prev is itself a separator: replace it with ITS predecessor */
        ULONGLONG PrevChild = *(PULONGLONG)((PUCHAR)Prev + Prev->Length - sizeof (ULONGLONG));
        PUCHAR    Pk    = AllocatePool (Vcb->BytesPerIndexRecord);
        ULONG     PkLen = 0;
        if (Pk == NULL) { FreePool (Buf); return EFI_OUT_OF_RESOURCES; }
        Status = NtfsExtractMaxKey (Vcb, Alloc, DirRec, DirMFT, PrevChild, Pk, &PkLen, Depth + 1);
        if (Status == EFI_SUCCESS) {
            PUCHAR R = AllocatePool (Vcb->BytesPerIndexRecord);
            if (R == NULL) { FreePool (Pk); FreePool (Buf); return EFI_OUT_OF_RESOURCES; }
            {
                ULONG RLen = NtfsBuildSeparatorEntry (R, (PINDEX_ENTRY_ATTRIBUTE)Pk, PrevChild);
                if (!NtfsSpliceInIndexBlock (Block, Prev, Prev->Length, R, RLen))
                    Status = EFI_UNSUPPORTED;
            }
            FreePool (R);
        } else if (Status == EFI_NOT_FOUND) {
            /* Prev's subtree is empty: free all its reachable blocks (bitmap
             * bits cleared), then drop the separator. */
            NtfsFreeIndexSubtree (Vcb, Alloc, DirRec, DirMFT, PrevChild, Depth + 1);
            NtfsSpliceInIndexBlock (Block, Prev, Prev->Length, NULL, 0);
            Status = EFI_SUCCESS;
        }
        FreePool (Pk);
        if (EFI_ERROR (Status)) { FreePool (Buf); return Status; }
    } else {
        /* Prev is a leaf key: splice it out */
        NtfsSpliceInIndexBlock (Block, Prev, Prev->Length, NULL, 0);
        Status = EFI_SUCCESS;
    }

    {
        EFI_STATUS W = NtfsEfiWriteMultiSectorRecord (Vcb, Alloc,
                            Vcn * Vcb->BytesPerCluster, &Block->Ntfs, Vcb->BytesPerIndexRecord);
        if (EFI_ERROR (W)) Status = W;
    }
    FreePool (Buf);
    return Status;
}

/* Check if the INDX subtree rooted at Vcn contains any real keys.
 * Returns TRUE if empty (no real keys, only END entries), FALSE if any real key is found. */
static BOOLEAN
NtfsSubtreeIsEmpty (
    IN PNTFS_EFI_VCB  Vcb,
    IN PNTFS_ATTR_CTX Alloc,
    IN ULONGLONG      Vcn,
    IN ULONG          Depth
    )
{
    PUCHAR                 Buf;
    PINDEX_BUFFER          Block;
    PINDEX_ENTRY_ATTRIBUTE E, Last;
    BOOLEAN                Empty = TRUE;

    if (Depth > 32 || Alloc == NULL) return TRUE;
    Buf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (Buf == NULL) return TRUE;

    if (NtfsEfiReadAttr (Vcb, Alloc, Vcn * Vcb->BytesPerCluster, (PCHAR)Buf,
                         Vcb->BytesPerIndexRecord) != Vcb->BytesPerIndexRecord) {
        FreePool (Buf); return TRUE;
    }
    Block = (PINDEX_BUFFER)Buf;
    if (Block->Ntfs.Type != NRH_INDX_TYPE ||
        EFI_ERROR (NtfsEfiFixupRecord (Vcb, &Block->Ntfs))) {
        FreePool (Buf); return TRUE;
    }

    E    = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
    Last = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries);
    while ((PUCHAR)E < (PUCHAR)Last && E->Length > 0) {
        if (!(E->Flags & NTFS_INDEX_ENTRY_END)) {
            Empty = FALSE;
            break;
        }
        if (E->Flags & NTFS_INDEX_ENTRY_NODE) {
            ULONGLONG ChildVcn = *(PULONGLONG)((PUCHAR)E + E->Length - sizeof (ULONGLONG));
            if (!NtfsSubtreeIsEmpty (Vcb, Alloc, ChildVcn, Depth + 1)) {
                Empty = FALSE;
                break;
            }
        }
        if (E->Flags & NTFS_INDEX_ENTRY_END) break;
        E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)E + E->Length);
    }

    FreePool (Buf);
    return Empty;
}

/* If the index has no real entries but is still marked as having children,
 * collapse it back to a clean resident-only leaf node. This frees all remaining
 * empty INDX blocks, updates the INDEX_ROOT END entry to 16 bytes (no child VCN),
 * and clears the INDEX_NODE flag. Writes DirRec to disk if modified. */
static VOID
NtfsCollapseIndexToResident (
    IN     PNTFS_EFI_VCB       Vcb,
    IN OUT PFILE_RECORD_HEADER DirRec,
    IN     ULONGLONG           DirMFT
    )
{
    ULONG                  RootOffset = 0;
    PNTFS_ATTR_CTX         RootCtx;
    PNTFS_ATTR_RECORD      RootAttr;
    PUCHAR                 ValPtr;
    PINDEX_ROOT_ATTRIBUTE  IndexRoot;
    PINDEX_ENTRY_ATTRIBUTE FirstEntry;
    PNTFS_ATTR_CTX         AllocCtx;
    BOOLEAN                Modified = FALSE;

    RootCtx = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexRoot, L"$I30", 4, &RootOffset);
    if (RootCtx == NULL) return;

    RootAttr   = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
    ValPtr     = (PUCHAR)RootAttr + RootAttr->Resident.ValueOffset;
    IndexRoot  = (PINDEX_ROOT_ATTRIBUTE)ValPtr;
    FirstEntry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);

    if ((FirstEntry->Flags & NTFS_INDEX_ENTRY_END) &&
        (IndexRoot->Header.Flags & 1)) {

        ULONGLONG ChildVcn = *(PULONGLONG)((PUCHAR)FirstEntry + FirstEntry->Length - sizeof (ULONGLONG));
        
        /* 1. Check if the subtree is actually empty first! */
        AllocCtx = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexAllocation, L"$I30", 4, NULL);
        if (AllocCtx != NULL) {
            BOOLEAN SubtreeEmpty = NtfsSubtreeIsEmpty (Vcb, AllocCtx, ChildVcn, 0);
            if (!SubtreeEmpty) {
                NtfsEfiFreeAttrCtx (AllocCtx);
                NtfsEfiFreeAttrCtx (RootCtx);
                return;
            }
            
            /* Subtree is empty: free all its blocks and clear bitmap */
            NtfsFreeIndexSubtree (Vcb, AllocCtx, DirRec, DirMFT, ChildVcn, 0);
            NtfsEfiFreeAttrCtx (AllocCtx);
        }

        /* 2. Re-locate INDEX_ROOT */
        NtfsEfiFreeAttrCtx (RootCtx);
        RootCtx = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexRoot, L"$I30", 4, &RootOffset);
        if (RootCtx == NULL) return;
        RootAttr   = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
        ValPtr     = (PUCHAR)RootAttr + RootAttr->Resident.ValueOffset;
        IndexRoot  = (PINDEX_ROOT_ATTRIBUTE)ValPtr;
        FirstEntry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);

        /* 3. Shrink END entry from 24 to 16 bytes */
        if (FirstEntry->Length == 24) {
            ULONG RemoveAt = (ULONG)((PUCHAR)FirstEntry - ValPtr) + 16;
            NtfsShrinkResidentInRecord (DirRec, RootOffset, RemoveAt, 8);
            
            NtfsEfiFreeAttrCtx (RootCtx);
            RootCtx = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexRoot, L"$I30", 4, &RootOffset);
            if (RootCtx == NULL) return;
            RootAttr   = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
            ValPtr     = (PUCHAR)RootAttr + RootAttr->Resident.ValueOffset;
            IndexRoot  = (PINDEX_ROOT_ATTRIBUTE)ValPtr;
            FirstEntry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
        }

        FirstEntry->Length = 16;
        FirstEntry->Flags  = NTFS_INDEX_ENTRY_END;

        /* 4. Update INDEX_HEADER */
        IndexRoot->Header.Flags = 0;
        IndexRoot->Header.TotalSizeOfEntries = IndexRoot->Header.FirstEntryOffset + 16;
        IndexRoot->Header.AllocatedSize = IndexRoot->Header.TotalSizeOfEntries;
        Modified = TRUE;
    }

    NtfsEfiFreeAttrCtx (RootCtx);

    if (Modified) {
        NtfsEfiWriteFileRecord (Vcb, DirMFT, DirRec);
    }
}

/* Remove ONE leaf index entry (whichever one indexes ChildMFT) from the
 * parent directory. Returns EFI_SUCCESS if one was removed, EFI_NOT_FOUND
 * if none matched, EFI_UNSUPPORTED if the match is a B+tree separator. */
static EFI_STATUS
NtfsRemoveOneDirEntryByChild (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     BaseMFT,
    IN ULONGLONG     ChildMFT
    )
{
    PFILE_RECORD_HEADER BaseRec;
    PFILE_RECORD_HEADER DirRec;
    ULONGLONG           DirMFT;
    NTFS_INDEX_HOST     Host;
    ULONG               RootOffset = 0;
    PNTFS_ATTR_CTX      RootCtx;
    PNTFS_ATTR_RECORD   RootAttr;
    PUCHAR              ValPtr;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    PINDEX_ENTRY_ATTRIBUTE Entry, Last;
    EFI_STATUS          Status = EFI_NOT_FOUND;
    PNTFS_ATTR_CTX      AllocCtx;

    BaseRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (BaseRec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, BaseMFT, BaseRec))) {
        FreePool (BaseRec);
        return EFI_DEVICE_ERROR;
    }

    /*
     * A big/fragmented directory keeps its $I30 index in an $ATTRIBUTE_LIST
     * extension record, not in its base record. Every step below edits one
     * record and writes it back by index, so work on whichever record actually
     * owns the index (same fix as the insert side in ntfs_create.c). Refuses
     * cleanly if the index attributes are spread over several records.
     */
    Status = NtfsEfiResolveIndexHost (Vcb, BaseRec, BaseMFT, &Host);
    if (EFI_ERROR (Status)) {
        FreePool (BaseRec);
        return Status;
    }
    DirRec = Host.Rec;
    DirMFT = Host.MFTIndex;
    Status = EFI_NOT_FOUND;

    /* --- 1. resident $INDEX_ROOT --- */
    RootCtx = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexRoot, L"$I30", 4, &RootOffset);
    if (RootCtx != NULL) {
        RootAttr  = (PNTFS_ATTR_RECORD)((PUCHAR)DirRec + RootOffset);
        ValPtr    = (PUCHAR)RootAttr + RootAttr->Resident.ValueOffset;
        IndexRoot = (PINDEX_ROOT_ATTRIBUTE)ValPtr;
        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
        Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.TotalSizeOfEntries);

        while ((PUCHAR)Entry < (PUCHAR)Last) {
            if (Entry->Length == 0) break;
            if (!(Entry->Flags & NTFS_INDEX_ENTRY_END) &&
                (Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK) == ChildMFT) {
                if (Entry->Flags & NTFS_INDEX_ENTRY_NODE) {
                    /* separator key: promote in-order predecessor from the
                     * child subtree using the recursive NtfsExtractMaxKey. */
                    PNTFS_ATTR_CTX A  = NtfsEfiFindAttrInRecord (Vcb, DirRec,
                                           AttributeIndexAllocation, L"$I30", 4, NULL);
                    PUCHAR    Pk      = AllocatePool (Vcb->BytesPerIndexRecord);
                    ULONG     PkLen   = 0;
                    ULONGLONG ChildVcn = *(PULONGLONG)((PUCHAR)Entry + Entry->Length
                                                        - sizeof (ULONGLONG));
                    if (Pk == NULL) {
                        if (A) NtfsEfiFreeAttrCtx (A);
                        Status = EFI_OUT_OF_RESOURCES; goto Done;
                    }
                    Status = NtfsExtractMaxKey (Vcb, A, DirRec, DirMFT, ChildVcn, Pk, &PkLen, 0);
                    if (Status == EFI_NOT_FOUND) {
                        /* child subtree empty: free its INDX blocks, then
                         * remove the separator */
                        NtfsFreeIndexSubtree (Vcb, A, DirRec, DirMFT, ChildVcn, 0);
                        {
                        ULONG RemoveAt  = (ULONG)((PUCHAR)Entry - ValPtr);
                        ULONG RemoveLen = Entry->Length;
                        IndexRoot->Header.TotalSizeOfEntries -= RemoveLen;
                        if (IndexRoot->Header.AllocatedSize >= RemoveLen)
                            IndexRoot->Header.AllocatedSize -= RemoveLen;
                        NtfsShrinkResidentInRecord (DirRec, RootOffset, RemoveAt, RemoveLen);
                        Status = NtfsEfiWriteFileRecord (Vcb, DirMFT, DirRec);
                        if (!EFI_ERROR (Status)) Status = EFI_SUCCESS;
                        }
                    } else if (!EFI_ERROR (Status)) {
                        /* build new separator from the extracted predecessor key */
                        PUCHAR R    = AllocatePool (Vcb->BytesPerIndexRecord);
                        if (R == NULL) {
                            FreePool (Pk);
                            if (A) NtfsEfiFreeAttrCtx (A);
                            Status = EFI_OUT_OF_RESOURCES; goto Done;
                        }
                        {
                            ULONG RLen   = NtfsBuildSeparatorEntry (R,
                                               (PINDEX_ENTRY_ATTRIBUTE)Pk, ChildVcn);
                            INTN  Delta  = (INTN)RLen - (INTN)Entry->Length;
                            if (Delta > 0 &&
                                DirRec->BytesInUse + (ULONG)Delta > Vcb->BytesPerFileRecord) {
                                Status = EFI_UNSUPPORTED; /* record full; bail */
                            } else {
                                ULONG RemoveAt = (ULONG)((PUCHAR)Entry - ValPtr);
                                NtfsSpliceResidentInRecord (DirRec, RootOffset, RemoveAt,
                                        Entry->Length, R, RLen);
                                IndexRoot->Header.TotalSizeOfEntries =
                                    (ULONG)((INTN)IndexRoot->Header.TotalSizeOfEntries + Delta);
                                IndexRoot->Header.AllocatedSize =
                                    (ULONG)((INTN)IndexRoot->Header.AllocatedSize + Delta);
                                Status = NtfsEfiWriteFileRecord (Vcb, DirMFT, DirRec);
                                if (!EFI_ERROR (Status)) Status = EFI_SUCCESS;
                            }
                        }
                        FreePool (R);
                    }
                    FreePool (Pk);
                    if (A) NtfsEfiFreeAttrCtx (A);
                    goto Done;
                }
                {
                    ULONG RemoveAt  = (ULONG)((PUCHAR)Entry - ValPtr);
                    ULONG RemoveLen = Entry->Length;
                    IndexRoot->Header.TotalSizeOfEntries -= RemoveLen;
                    if (IndexRoot->Header.AllocatedSize >= RemoveLen)
                        IndexRoot->Header.AllocatedSize -= RemoveLen;
                    NtfsShrinkResidentInRecord (DirRec, RootOffset, RemoveAt, RemoveLen);
                    Status = NtfsEfiWriteFileRecord (Vcb, DirMFT, DirRec);
                    if (!EFI_ERROR (Status)) Status = EFI_SUCCESS;
                }
                goto Done;
            }
            if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;
            Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
        }
    }

    /* --- 2. $INDEX_ALLOCATION (INDX) leaf blocks, scanned physically --- */
    AllocCtx = NtfsEfiFindAttrInRecord (Vcb, DirRec, AttributeIndexAllocation, L"$I30", 4, NULL);
    if (AllocCtx != NULL) {
        UINT64 AllocLen = NtfsEfiAttrDataLength (AllocCtx);
        UINT64 Vcn;
        PUCHAR Buf = AllocatePool (Vcb->BytesPerIndexRecord);
        if (Buf == NULL) { NtfsEfiFreeAttrCtx (AllocCtx); Status = EFI_OUT_OF_RESOURCES; goto Done; }

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
                    if (E->Flags & NTFS_INDEX_ENTRY_NODE) {
                        /* separator in an INDX block: promote in-order predecessor
                         * using the recursive NtfsExtractMaxKey. */
                        ULONGLONG ChildVcn = *(PULONGLONG)((PUCHAR)E + E->Length
                                                            - sizeof (ULONGLONG));
                        PUCHAR    Pk       = AllocatePool (Vcb->BytesPerIndexRecord);
                        ULONG     PkLen    = 0;
                        if (Pk == NULL) {
                            FreePool (Buf); NtfsEfiFreeAttrCtx (AllocCtx);
                            Status = EFI_OUT_OF_RESOURCES; goto Done;
                        }
                        /* re-find AllocCtx after potential stale-ctx risk: it was
                         * opened from DirRec above, still valid here (same Vcb). */
                        Status = NtfsExtractMaxKey (Vcb, AllocCtx, DirRec, DirMFT, ChildVcn, Pk, &PkLen, 0);
                        if (Status == EFI_NOT_FOUND) {
                            /* child subtree empty: free its INDX blocks, then
                             * splice out the separator */
                            NtfsFreeIndexSubtree (Vcb, AllocCtx, DirRec, DirMFT, ChildVcn, 0);
                            NtfsSpliceInIndexBlock (Block, E, E->Length, NULL, 0);
                            Status = NtfsEfiWriteMultiSectorRecord (Vcb, AllocCtx,
                                        Vcn * Vcb->BytesPerIndexRecord, &Block->Ntfs,
                                        Vcb->BytesPerIndexRecord);
                            if (!EFI_ERROR (Status)) Status = EFI_SUCCESS;
                        } else if (!EFI_ERROR (Status)) {
                            PUCHAR R    = AllocatePool (Vcb->BytesPerIndexRecord);
                            if (R == NULL) {
                                FreePool (Pk); FreePool (Buf);
                                NtfsEfiFreeAttrCtx (AllocCtx);
                                Status = EFI_OUT_OF_RESOURCES; goto Done;
                            }
                            {
                                ULONG RLen  = NtfsBuildSeparatorEntry (R,
                                                  (PINDEX_ENTRY_ATTRIBUTE)Pk, ChildVcn);
                                if (!NtfsSpliceInIndexBlock (Block, E, E->Length, R, RLen))
                                    Status = EFI_UNSUPPORTED; /* block full */
                                else {
                                    Status = NtfsEfiWriteMultiSectorRecord (Vcb, AllocCtx,
                                                Vcn * Vcb->BytesPerIndexRecord, &Block->Ntfs,
                                                Vcb->BytesPerIndexRecord);
                                    if (!EFI_ERROR (Status)) Status = EFI_SUCCESS;
                                }
                            }
                            FreePool (R);
                        }
                        FreePool (Pk);
                        FreePool (Buf); NtfsEfiFreeAttrCtx (AllocCtx);
                        goto Done;
                    }
                    {
                        ULONG RemoveLen = E->Length;
                        PUCHAR After    = (PUCHAR)E + RemoveLen;
                        UINTN  MoveLen  = (UINTN)((PUCHAR)L2 - After);
                        CopyMem ((PUCHAR)E, After, MoveLen);
                        Block->Header.TotalSizeOfEntries -= RemoveLen;
                        ZeroMem ((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries, RemoveLen);
                        Status = NtfsEfiWriteMultiSectorRecord (Vcb, AllocCtx,
                                    Vcn * Vcb->BytesPerIndexRecord, &Block->Ntfs,
                                    Vcb->BytesPerIndexRecord);
                        if (!EFI_ERROR (Status)) Status = EFI_SUCCESS;
                    }
                    FreePool (Buf); NtfsEfiFreeAttrCtx (AllocCtx);
                    goto Done;
                }
                if (E->Flags & NTFS_INDEX_ENTRY_END) break;
                E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)E + E->Length);
            }
        }
        FreePool (Buf);
        NtfsEfiFreeAttrCtx (AllocCtx);
    }

Done:
    if (RootCtx != NULL) NtfsEfiFreeAttrCtx (RootCtx);
    if (!EFI_ERROR (Status)) {
        NtfsCollapseIndexToResident (Vcb, DirRec, DirMFT);
    }
    if (Host.Own) FreePool (Host.Rec);
    FreePool (BaseRec);
    return Status;
}

/* Free every cluster referenced by any non-resident attribute in Rec. */
static VOID
NtfsFreeAllAttributeClusters (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER Rec
    )
{
    PNTFS_ATTR_RECORD Attr    = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + Rec->AttributeOffset);
    PNTFS_ATTR_RECORD LastPtr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + Rec->BytesInUse);
    NTFS_RUN_ENTRY   *Runs    = AllocatePool (NTFS_MAX_RUNS * sizeof (NTFS_RUN_ENTRY));

    if (Runs == NULL) return;

    while (Attr < LastPtr && Attr->Type != (ULONG)AttributeEnd) {
        if (Attr->Length == 0) break;
        if (Attr->IsNonResident) {
            ULONG RunCount = 0, i;
            if (!EFI_ERROR (NtfsBuildRunList (Attr, Runs, NTFS_MAX_RUNS, &RunCount))) {
                for (i = 0; i < RunCount; i++) {
                    if (Runs[i].LBN != -1LL && Runs[i].Len != 0) {
                        NtfsEfiFreeClusters (Vcb, (UINT64)Runs[i].LBN, Runs[i].Len);
                    }
                }
            }
        }
        Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Attr + Attr->Length);
    }
    FreePool (Runs);
}

/*
 * A fragmented / attribute-heavy file spills attributes that don't fit its base
 * 1 KB MFT record into EXTENSION records, listed in its $ATTRIBUTE_LIST (each
 * item's MFTIndex names the record holding that attribute). NtfsFreeAll-
 * AttributeClusters above only walks the BASE record, so without this the
 * extension records stay marked in-use (chkdsk: "Deleting orphan file record
 * segment ...") and any $DATA clusters described by run-lists that live in those
 * extension records stay marked allocated (chkdsk: "free space marked as
 * allocated in the volume bitmap"). Free each distinct extension record's
 * clusters, then release the record itself (in-use flag + $MFT bitmap bit).
 */
static VOID
NtfsFreeAttributeListRecords (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER Rec,
    IN ULONGLONG           BaseMFT
    )
{
    PNTFS_ATTR_CTX       ListCtx;
    UINT64               ListLen;
    PUCHAR               ListBuf;
    PNTFS_ATTR_LIST_ITEM Item, End;
    ULONGLONG            Seen[128];
    ULONG                NSeen = 0, i;
    PFILE_RECORD_HEADER  Ext;

    ListCtx = NtfsEfiFindAttrInRecord (Vcb, Rec, AttributeAttributeList, NULL, 0, NULL);
    if (ListCtx == NULL) return;                 /* no $ATTRIBUTE_LIST: nothing to do */

    ListLen = NtfsEfiAttrDataLength (ListCtx);
    ListBuf = AllocatePool ((UINTN)ListLen);
    if (ListBuf == NULL) { NtfsEfiFreeAttrCtx (ListCtx); return; }
    NtfsEfiReadAttr (Vcb, ListCtx, 0, (PCHAR)ListBuf, (ULONG)ListLen);
    NtfsEfiFreeAttrCtx (ListCtx);

    /* collect the distinct extension records (skip the base itself) */
    Item = (PNTFS_ATTR_LIST_ITEM)ListBuf;
    End  = (PNTFS_ATTR_LIST_ITEM)(ListBuf + ListLen);
    while (Item < End && Item->Type != (ULONG)AttributeEnd) {
        ULONGLONG Idx;
        if (Item->Length == 0) break;
        Idx = Item->MFTIndex & NTFS_MFT_MASK;
        if (Idx != BaseMFT) {
            BOOLEAN Dup = FALSE;
            for (i = 0; i < NSeen; i++) if (Seen[i] == Idx) { Dup = TRUE; break; }
            if (!Dup && NSeen < (ULONG)(sizeof (Seen) / sizeof (Seen[0]))) Seen[NSeen++] = Idx;
        }
        Item = (PNTFS_ATTR_LIST_ITEM)((PUCHAR)Item + Item->Length);
    }
    FreePool (ListBuf);

    Ext = AllocatePool (Vcb->BytesPerFileRecord);
    if (Ext == NULL) return;
    for (i = 0; i < NSeen; i++) {
        if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, Seen[i], Ext))) continue;
        if (!(Ext->Flags & FRH_IN_USE)) continue;      /* already free */
        NtfsFreeAllAttributeClusters (Vcb, Ext);
        Ext->Flags &= (USHORT)~FRH_IN_USE;
        Ext->SequenceNumber++;
        if (Ext->SequenceNumber == 0) Ext->SequenceNumber = 1;
        Ext->LinkCount = 0;
        NtfsEfiWriteFileRecord (Vcb, Seen[i], Ext);
        NtfsEfiFreeMftRecord (Vcb, Seen[i]);
    }
    FreePool (Ext);
}

/*
 * TRUE if the directory holds no live child entries.
 *
 * FAIL-CLOSED: a directory whose $I30 index we cannot resolve is reported
 * NON-empty, so rmdir refuses it. Reporting "empty" on an unreadable index
 * would delete a directory that still has children and orphan every one of
 * them - a base-record-only lookup used to do exactly that for any directory
 * big enough to keep its index in an $ATTRIBUTE_LIST extension record.
 */
static BOOLEAN
NtfsDirectoryIsEmpty (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER BaseRec,
    IN ULONGLONG           BaseMFT
    )
{
    ULONG                  RootOffset = 0;
    PNTFS_ATTR_CTX         RootCtx;
    PNTFS_ATTR_CTX         AllocCtx;
    BOOLEAN                Empty = TRUE;
    NTFS_INDEX_HOST        Host;
    PFILE_RECORD_HEADER    Rec;

    if (EFI_ERROR (NtfsEfiResolveIndexHost (Vcb, BaseRec, BaseMFT, &Host))) return FALSE;
    Rec = Host.Rec;

    RootCtx = NtfsEfiFindAttrInRecord (Vcb, Rec, AttributeIndexRoot, L"$I30", 4, &RootOffset);
    if (RootCtx != NULL) {
        PNTFS_ATTR_RECORD RootAttr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + RootOffset);
        PUCHAR ValPtr = (PUCHAR)RootAttr + RootAttr->Resident.ValueOffset;
        PINDEX_ROOT_ATTRIBUTE IndexRoot = (PINDEX_ROOT_ATTRIBUTE)ValPtr;
        PINDEX_ENTRY_ATTRIBUTE Entry =
            (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.FirstEntryOffset);
        PINDEX_ENTRY_ATTRIBUTE Last =
            (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header + IndexRoot->Header.TotalSizeOfEntries);
        while ((PUCHAR)Entry < (PUCHAR)Last) {
            if (Entry->Length == 0) break;
            if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;
            Empty = FALSE;   /* a real child entry */
            break;
        }
        NtfsEfiFreeAttrCtx (RootCtx);
    }

    if (!Empty) { if (Host.Own) FreePool (Host.Rec); return FALSE; }

    /* any INDX block with a non-END entry means non-empty too */
    AllocCtx = NtfsEfiFindAttrInRecord (Vcb, Rec, AttributeIndexAllocation, L"$I30", 4, NULL);
    if (AllocCtx != NULL) {
        UINT64 AllocLen = NtfsEfiAttrDataLength (AllocCtx);
        UINT64 Vcn;
        PUCHAR Buf = AllocatePool (Vcb->BytesPerIndexRecord);
        if (Buf != NULL) {
            for (Vcn = 0; Empty && Vcn * Vcb->BytesPerIndexRecord < AllocLen; Vcn++) {
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
                    if (E->Flags & NTFS_INDEX_ENTRY_END) break;
                    Empty = FALSE;
                    break;
                }
            }
            FreePool (Buf);
        }
        NtfsEfiFreeAttrCtx (AllocCtx);
    }
    if (Host.Own) FreePool (Host.Rec);
    return Empty;
}

/* Public: unlink every leaf index entry for ChildMFT from DirMFT (a WIN32
 * name plus a DOS 8.3 alias both point at the same child). Used by rename. */
EFI_STATUS
NtfsEfiIndexRemoveByChild (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     DirMFT,
    IN ULONGLONG     ChildMFT
    )
{
    EFI_STATUS Status = NtfsRemoveOneDirEntryByChild (Vcb, DirMFT, ChildMFT);
    ULONG      Guard;
    if (EFI_ERROR (Status)) return Status;
    for (Guard = 0; Guard < 8; Guard++) {
        EFI_STATUS More = NtfsRemoveOneDirEntryByChild (Vcb, DirMFT, ChildMFT);
        if (More == EFI_NOT_FOUND) break;
        if (EFI_ERROR (More)) break;
    }
    return EFI_SUCCESS;
}

/*
 * Effective hard-link count: the number of $FILE_NAME attributes that are
 * genuine names, i.e. NOT a DOS 8.3 alias. A file with a long name that needs
 * an 8.3 short name carries TWO $FILE_NAME attributes (one WIN32, one DOS) and
 * therefore a raw Rec->LinkCount of 2, even though it is a single logical file
 * with a single hard link (fsutil hardlink list shows 1). Counting the DOS
 * alias as a separate link made the delete gate below refuse every long-named
 * file that has an 8.3 alias (e.g. every ".ses"/"MpSigStub.log" in
 * \Windows\Temp). The alias is removed together with its long name by the
 * multi-entry unlink loop, so it is never orphaned - don't count it here.
 * A WIN32_AND_DOS (combined) name is a single attribute and counts once.
 */
static ULONG
NtfsCountRealNames (
    IN PFILE_RECORD_HEADER Rec
    )
{
    PNTFS_ATTR_RECORD Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + Rec->AttributeOffset);
    PNTFS_ATTR_RECORD Last = (PNTFS_ATTR_RECORD)((PUCHAR)Rec + Rec->BytesInUse);
    ULONG             Count = 0;

    while (Attr < Last && Attr->Type != (ULONG)AttributeEnd) {
        if (Attr->Length == 0) break;
        if (Attr->Type == (ULONG)AttributeFileName && !Attr->IsNonResident) {
            PFILENAME_ATTRIBUTE Fn =
                (PFILENAME_ATTRIBUTE)((PUCHAR)Attr + Attr->Resident.ValueOffset);
            if (Fn->NameType != NTFS_FILE_NAME_DOS) Count++;
        }
        Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Attr + Attr->Length);
    }
    return Count;
}

EFI_STATUS
NtfsEfiDeleteFile (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     MFTIndex
    )
{
    PFILE_RECORD_HEADER Rec;
    PNTFS_ATTR_CTX      FnCtx;
    ULONGLONG           ParentMFT;
    BOOLEAN             IsDir;
    EFI_STATUS          Status;
    ULONG               Guard;

    if (MFTIndex < NTFS_FILE_FIRST_USER_FILE) return EFI_ACCESS_DENIED;

    NtfsMarkVolumeDirty (Vcb);

    Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, MFTIndex, Rec))) {
        FreePool (Rec);
        return EFI_DEVICE_ERROR;
    }
    if (!(Rec->Flags & FRH_IN_USE)) {
        FreePool (Rec);
        return EFI_NOT_FOUND;   /* already free */
    }

    IsDir = (Rec->Flags & FRH_DIRECTORY) != 0;

    /* Files: single-linked only, so we never orphan another directory's
     * hard link to the same record. Directories cannot be hard-linked in
     * NTFS (LinkCount there is a fixed convention, not a real link count),
     * so the gate does not apply to them. */
    if (!IsDir && NtfsCountRealNames (Rec) > 1) {
        Print (L"[delete] refused MFT=%ld : LinkCount=%d, real (non-DOS) names > 1 "
               L"(genuinely hard-linked from another directory - e.g. WinSxS/"
               L"component-store style links - deleting this name would orphan the "
               L"others)\n", MFTIndex, Rec->LinkCount);
        FreePool (Rec);
        return EFI_UNSUPPORTED;
    }

    if (IsDir && !NtfsDirectoryIsEmpty (Vcb, Rec, MFTIndex)) {
        Print (L"[delete] refused MFT=%ld : directory not empty (caller must delete "
               L"children first - this driver never recurses)\n", MFTIndex);
        FreePool (Rec);
        return EFI_ACCESS_DENIED;   /* directory not empty */
    }

    /* parent directory from $FILE_NAME.DirectoryFileReferenceNumber. Use
     * NtfsEfiFindAttribute (follows $ATTRIBUTE_LIST) not ...InRecord: a
     * fragmented / stream-heavy file spills $FILE_NAME into an EXTENSION
     * record, and searching only the base record would wrongly report the
     * file as corrupt and refuse the delete. */
    FnCtx = NtfsEfiFindAttribute (Vcb, Rec, AttributeFileName, NULL, 0, NULL);
    if (FnCtx == NULL) {
        FreePool (Rec);
        return EFI_VOLUME_CORRUPTED;
    }
    {
        FILENAME_ATTRIBUTE Fn;
        NtfsEfiReadAttr (Vcb, FnCtx, 0, (PCHAR)&Fn, sizeof (Fn));
        ParentMFT = Fn.DirectoryFileReferenceNumber & NTFS_MFT_MASK;
        NtfsEfiFreeAttrCtx (FnCtx);
    }

    /* step 1: unlink every name (WIN32 + DOS alias) from the parent index.
     * Do a probe pass first: if the entry is a B+tree separator we bail out
     * BEFORE freeing anything, leaving the volume untouched. */
    Status = NtfsRemoveOneDirEntryByChild (Vcb, ParentMFT, MFTIndex);
    if (Status == EFI_UNSUPPORTED) {
        Print (L"[delete] refused MFT=%ld under parent=%ld : name is a B+tree "
               L"separator key with a subtree (rebalance-on-delete not implemented) "
               L"- volume untouched\n", MFTIndex, ParentMFT);
        FreePool (Rec);
        return EFI_UNSUPPORTED;   /* separator key - nothing changed yet */
    }
    if (EFI_ERROR (Status)) {
        FreePool (Rec);
        return Status;            /* NOT_FOUND / device error - nothing freed */
    }
    /* remove any remaining aliases (bounded) */
    for (Guard = 0; Guard < 8; Guard++) {
        EFI_STATUS More = NtfsRemoveOneDirEntryByChild (Vcb, ParentMFT, MFTIndex);
        if (More == EFI_NOT_FOUND) break;
        if (EFI_ERROR (More)) break;   /* best effort: the file is already unreachable */
    }

    /* step 2: free data / index / bitmap clusters (base record + any
     * $ATTRIBUTE_LIST extension records and their clusters) */
    NtfsFreeAllAttributeClusters (Vcb, Rec);
    NtfsFreeAttributeListRecords (Vcb, Rec, MFTIndex);

    /* step 3: mark the record free, bump sequence so stale refs are caught */
    Rec->Flags &= (USHORT)~FRH_IN_USE;
    Rec->SequenceNumber++;
    if (Rec->SequenceNumber == 0) Rec->SequenceNumber = 1;
    Rec->LinkCount = 0;
    NtfsEfiWriteFileRecord (Vcb, MFTIndex, Rec);

    /* step 4: release the record's $MFT bitmap bit */
    NtfsEfiFreeMftRecord (Vcb, MFTIndex);

    FreePool (Rec);
    return EFI_SUCCESS;
}
