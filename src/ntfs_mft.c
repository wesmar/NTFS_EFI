/**
 * ntfs_mft.c - USA fixup (update sequence array) and ReadFileRecord.
 */

#include "ntfs.h"

/*
 * How big is the buffer a record header lives in, and are its update-sequence
 * -array fields inside it?
 *
 * UsaOffset and UsaCount come straight off the disk, and the fixup loops below
 * use BOTH as write bounds: they rewrite the last two bytes of every
 * BytesPerSector-sized chunk of the record and copy the saved values out of
 * USA[i]. A record claiming UsaCount == 0xFFFF therefore walks ~32 MB past a
 * 1 KiB heap allocation, writing as it goes - the one place in this driver
 * where a corrupt/hand-crafted record turns into heap CORRUPTION rather than a
 * bad read. (The old code even computed the record size and then threw it away
 * with (VOID)RecordSize, i.e. the check was meant to be here from the start.)
 *
 * The buffer size is not passed in, but it is always implied by the record
 * type: a FILE record is Vcb->BytesPerFileRecord, an INDX block is
 * Vcb->BytesPerIndexRecord - every caller allocates exactly that. Anything
 * else is not a fixed-up NTFS record at all and is refused.
 */
static BOOLEAN
NtfsUsaBounds (
    IN  PNTFS_EFI_VCB       Vcb,
    IN  NTFS_RECORD_HEADER *Hdr,
    OUT ULONG              *RecordSize
    )
{
    ULONG Size;

    if (Hdr->Type == NRH_FILE_TYPE)      Size = Vcb->BytesPerFileRecord;
    else if (Hdr->Type == NRH_INDX_TYPE) Size = Vcb->BytesPerIndexRecord;
    else                                 return FALSE;

    *RecordSize = Size;

    /* the USA itself must sit inside the record, after the header */
    if (Hdr->UsaOffset < sizeof (NTFS_RECORD_HEADER) ||
        (ULONG)Hdr->UsaOffset + (ULONG)Hdr->UsaCount * sizeof (USHORT) > Size) {
        return FALSE;
    }
    /* one USN slot per sector-sized chunk, plus USA[0] (the marker itself);
     * the last patched position is Hdr + (UsaCount-1)*BytesPerSector - 2 */
    if ((ULONG)(Hdr->UsaCount - 1) * Vcb->BytesPerSector > Size) {
        return FALSE;
    }
    return TRUE;
}

EFI_STATUS
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
    if (!NtfsUsaBounds (Vcb, Hdr, &RecordSize)) return EFI_VOLUME_CORRUPTED;

    USA       = (PUSHORT)((PUCHAR)Hdr + Hdr->UsaOffset);
    USN       = USA[0];

    for (i = 1; i < Hdr->UsaCount; i++) {
        SectorEnd = (PUSHORT)((PUCHAR)Hdr + i * Vcb->BytesPerSector - sizeof (USHORT));
        if (*SectorEnd != USN) return EFI_VOLUME_CORRUPTED;
        *SectorEnd = USA[i];
    }
    return EFI_SUCCESS;
}

/*
 * Validate the two offsets every index-entry walk in this driver derives its
 * start/end pointers from, against the buffer the header actually lives in.
 *
 * INDEX_HEADER_ATTRIBUTE.FirstEntryOffset / .TotalSizeOfEntries / .AllocatedSize
 * are on-disk data. Read paths use them as `First`/`Last` walk bounds, and the
 * INSERT paths (NtfsInsertIntoIndexEntries, NtfsSpliceInIndexBlock) use
 * AllocatedSize as the room-check for a shift-forward - so a bogus AllocatedSize
 * is an out-of-bounds heap WRITE, not just a wild read. NtfsEfiFixupRecord only
 * verifies the per-sector USN; it says nothing about these three. This is the
 * index-block equivalent of the AttributeOffset/BytesInUse check in
 * NtfsEfiReadFileRecord: one central place instead of ~15 call sites.
 *
 * Avail is the number of bytes from Header to the end of its buffer.
 */
BOOLEAN
NtfsEfiIndexHeaderOk (
    IN INDEX_HEADER_ATTRIBUTE *Header,
    IN UINT64                  Avail
    )
{
    if (Header == NULL) return FALSE;
    if (Avail < sizeof (INDEX_HEADER_ATTRIBUTE)) return FALSE;
    if ((UINT64)Header->FirstEntryOffset < sizeof (INDEX_HEADER_ATTRIBUTE)) return FALSE;
    if ((UINT64)Header->TotalSizeOfEntries > Avail) return FALSE;
    if ((UINT64)Header->FirstEntryOffset > (UINT64)Header->TotalSizeOfEntries) return FALSE;
    /*
     * AllocatedSize is deliberately NOT compared against Avail here. For a
     * resident $INDEX_ROOT it can legitimately exceed the value's own length
     * (Windows self-heal lowers TotalSizeOfEntries and leaves slack behind -
     * see the comment in NtfsInsertIndexEntrySmall), and nothing writes using
     * the root's AllocatedSize as a bound. Where it IS a write bound - INDX
     * blocks, via NtfsInsertIntoIndexEntries / NtfsSpliceInIndexBlock -
     * NtfsEfiIndexBlockOk below checks it.
     */
    return TRUE;
}

/*
 * Same check for a resident $INDEX_ROOT attribute inside an MFT record. The
 * attribute walk (NtfsEfiFindAttrInRecord) has already established that the
 * whole attribute - and hence its value - lies inside the record, so the
 * value's own declared length is the bound the index header must respect.
 */
BOOLEAN
NtfsEfiIndexRootOk (
    IN PNTFS_ATTR_RECORD RootAttr
    )
{
    PINDEX_ROOT_ATTRIBUTE Ir;
    UINT64                HeaderOffset = FIELD_OFFSET (INDEX_ROOT_ATTRIBUTE, Header);

    if (RootAttr->IsNonResident) return FALSE;   /* $INDEX_ROOT is always resident */
    if ((UINT64)RootAttr->Resident.ValueLength <= HeaderOffset) return FALSE;

    Ir = (PINDEX_ROOT_ATTRIBUTE)((PUCHAR)RootAttr + RootAttr->Resident.ValueOffset);
    return NtfsEfiIndexHeaderOk (&Ir->Header,
                                 (UINT64)RootAttr->Resident.ValueLength - HeaderOffset);
}

/* Same check for an INDX block read into a Vcb->BytesPerIndexRecord buffer. */
BOOLEAN
NtfsEfiIndexBlockOk (
    IN PNTFS_EFI_VCB Vcb,
    IN PINDEX_BUFFER Block
    )
{
    UINT64 HeaderOffset = (UINT64)((PUCHAR)&Block->Header - (PUCHAR)Block);
    UINT64 Avail;

    if (Vcb->BytesPerIndexRecord <= HeaderOffset) return FALSE;
    Avail = (UINT64)Vcb->BytesPerIndexRecord - HeaderOffset;

    if (!NtfsEfiIndexHeaderOk (&Block->Header, Avail)) return FALSE;
    /* insert/splice into an INDX block uses AllocatedSize as its room check */
    if ((UINT64)Block->Header.AllocatedSize > Avail) return FALSE;
    if ((UINT64)Block->Header.TotalSizeOfEntries > (UINT64)Block->Header.AllocatedSize) return FALSE;
    return TRUE;
}

/* Store a fixed-up record image into the MFT record cache (round-robin, or
 * overwrite the existing entry for this index). Only caches records that fit
 * the fixed-size slot. */
static VOID
NtfsMftCachePut (
    IN PNTFS_EFI_VCB       Vcb,
    IN ULONGLONG           MFTIndex,
    IN PFILE_RECORD_HEADER FileRecord
    )
{
    ULONG i, slot;
    if (Vcb->BytesPerFileRecord > NTFS_MFT_CACHE_RECSIZE) return;
    for (i = 0; i < NTFS_MFT_CACHE_ENTRIES; i++) {
        if (Vcb->MftCacheValid[i] && Vcb->MftCacheIndex[i] == MFTIndex) { slot = i; goto store; }
    }
    slot = Vcb->MftCacheNext;
    Vcb->MftCacheNext = (Vcb->MftCacheNext + 1) % NTFS_MFT_CACHE_ENTRIES;
store:
    Vcb->MftCacheIndex[slot] = MFTIndex;
    Vcb->MftCacheValid[slot] = TRUE;
    CopyMem (Vcb->MftCacheData[slot], FileRecord, Vcb->BytesPerFileRecord);
}

EFI_STATUS
NtfsEfiReadFileRecord (
    IN  PNTFS_EFI_VCB    Vcb,
    IN  ULONGLONG        MFTIndex,
    OUT PFILE_RECORD_HEADER FileRecord   /* caller allocates BytesPerFileRecord */
    )
{
    UINT64 ByteOffset = MFTIndex * Vcb->BytesPerFileRecord;
    ULONG  BytesRead, i;

    /* cache hit: return the fixed-up image directly, no DiskIo */
    for (i = 0; i < NTFS_MFT_CACHE_ENTRIES; i++) {
        if (Vcb->MftCacheValid[i] && Vcb->MftCacheIndex[i] == MFTIndex) {
            CopyMem (FileRecord, Vcb->MftCacheData[i], Vcb->BytesPerFileRecord);
            return EFI_SUCCESS;
        }
    }

    gNtfsRecordReads++;
    BytesRead = NtfsEfiReadAttr (Vcb, Vcb->MFTContext, ByteOffset,
                                 (PCHAR)FileRecord, Vcb->BytesPerFileRecord);
    if (BytesRead != Vcb->BytesPerFileRecord) return EFI_DEVICE_ERROR;
    if (EFI_ERROR (NtfsEfiFixupRecord (Vcb, &FileRecord->Ntfs))) return EFI_VOLUME_CORRUPTED;

    /*
     * Every attribute-walk loop in this driver (NtfsEfiFindAttrInRecord and
     * its equivalents in ntfs_delete.c / ntfs_setinfo.c) trusts
     * FileRecord->AttributeOffset and ->BytesInUse blindly to compute the
     * start/end pointers of the walk - neither is otherwise checked against
     * the actual size of this buffer (Vcb->BytesPerFileRecord). USA fixup
     * above only verifies the per-sector USN checksum; it says nothing
     * about whether these two header fields are sane. On a partially
     * recovered/corrupted volume (bad sector, torn write that still
     * happens to pass the USN check, hand-edited image) a wild value here
     * turns every subsequent attribute walk on this record into an
     * out-of-bounds heap read. Reject it here, once, centrally, instead of
     * trusting it at every one of those call sites.
     */
    if ((UINT64)FileRecord->AttributeOffset > Vcb->BytesPerFileRecord ||
        (UINT64)FileRecord->BytesInUse      > Vcb->BytesPerFileRecord ||
        (UINT64)FileRecord->BytesAllocated  > Vcb->BytesPerFileRecord ||
        FileRecord->BytesInUse < FileRecord->AttributeOffset) {
        return EFI_VOLUME_CORRUPTED;
    }

    NtfsMftCachePut (Vcb, MFTIndex, FileRecord);
    return EFI_SUCCESS;
}

/*
 * Inverse of NtfsEfiFixupRecord(): stash the real per-sector trailing bytes
 * back into the USA slots (refreshing them, in case the just-applied edit
 * touched one) and stamp the USN marker over the on-disk sector-end
 * position, exactly like a genuine NTFS driver does right before writing a
 * record out. Caller must call NtfsEfiFixupRecord() again immediately
 * after the disk write to restore the in-memory copy to its normal
 * (real-data-at-sector-ends) form.
 *
 * The USN is bumped every write (real NTFS does this too): the whole
 * point of the update-sequence-array scheme is to detect a torn write
 * (record partially flushed to disk, e.g. power loss mid-write) by
 * checking every sector-end still carries the SAME marker. Reusing the
 * old USN defeats that - a torn write mixing this generation's sector
 * ends with a stale previous generation's middle would look consistent.
 */
static BOOLEAN
NtfsEfiUnfixupRecord (
    IN PNTFS_EFI_VCB       Vcb,
    IN NTFS_RECORD_HEADER *Hdr
    )
{
    PUSHORT USA;
    PUSHORT SectorEnd;
    USHORT  USN;
    USHORT  i;
    ULONG   RecordSize;

    if (Hdr->UsaOffset == 0 || Hdr->UsaCount == 0) return TRUE;
    /* same bounds as the fixup direction - this one WRITES the sector ends */
    if (!NtfsUsaBounds (Vcb, Hdr, &RecordSize)) return FALSE;

    USA = (PUSHORT)((PUCHAR)Hdr + Hdr->UsaOffset);
    USN = (USHORT)(USA[0] + 1);
    if (USN == 0 || USN == 0xFFFF) USN = 1;   /* 0 means "no fixup"; 0xFFFF is reserved */
    USA[0] = USN;

    for (i = 1; i < Hdr->UsaCount; i++) {
        SectorEnd  = (PUSHORT)((PUCHAR)Hdr + i * Vcb->BytesPerSector - sizeof (USHORT));
        USA[i]     = *SectorEnd;   /* preserve the (possibly just-edited) real data */
        *SectorEnd = USN;
    }
    return TRUE;
}

EFI_STATUS
NtfsEfiWriteMultiSectorRecord (
    IN PNTFS_EFI_VCB       Vcb,
    IN PNTFS_ATTR_CTX      AttrCtx,
    IN UINT64              AttrOffset,
    IN NTFS_RECORD_HEADER *Hdr,
    IN ULONG               RecordLength
    )
{
    ULONG Written;

    gNtfsIndexWrites++;
    if (!NtfsEfiUnfixupRecord (Vcb, Hdr)) return EFI_VOLUME_CORRUPTED;
    Written = NtfsEfiWriteAttr (Vcb, AttrCtx, AttrOffset, (PCHAR)Hdr, RecordLength);
    NtfsEfiFixupRecord (Vcb, Hdr);

    return (Written == RecordLength) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/*
 * $MFTMirr (MFT record 1) holds byte-identical backup copies of the first
 * four MFT records (0=$MFT, 1=$MFTMirr, 2=$LogFile, 3=$Volume) in its $DATA.
 * chkdsk cross-checks the two on every scan, so whenever we write one of
 * those primary records (the volume dirty flag writes $Volume (3); $MFT
 * growth writes $MFT (0)) the mirror copy must be updated in lock-step or
 * the volume reads as corrupt. RawRec must be the on-disk (un-fixed-up)
 * image, i.e. the exact bytes just written to $MFT. Best effort: the
 * primary write already succeeded; a mirror failure is logged, not fatal.
 */
static VOID
NtfsEfiSyncMftMirror (
    IN PNTFS_EFI_VCB       Vcb,
    IN ULONGLONG           MFTIndex,
    IN PFILE_RECORD_HEADER RawRec   /* un-fixed-up, as written to $MFT */
    )
{
    PFILE_RECORD_HEADER MirrRec;
    PNTFS_ATTR_CTX      DataCtx;

    if (MFTIndex > 3) return;

    MirrRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (MirrRec == NULL) return;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, NTFS_FILE_MFTMIRR, MirrRec))) {
        FreePool (MirrRec);
        return;
    }
    DataCtx = NtfsEfiFindAttribute (Vcb, MirrRec, AttributeData, NULL, 0, NULL);
    if (DataCtx != NULL) {
        NtfsEfiWriteAttr (Vcb, DataCtx, MFTIndex * Vcb->BytesPerFileRecord,
                          (PCHAR)RawRec, Vcb->BytesPerFileRecord);
        NtfsEfiFreeAttrCtx (DataCtx);
    }
    FreePool (MirrRec);
}

EFI_STATUS
NtfsEfiWriteFileRecord (
    IN PNTFS_EFI_VCB       Vcb,
    IN ULONGLONG           MFTIndex,
    IN PFILE_RECORD_HEADER FileRecord   /* BytesPerFileRecord bytes, currently fixed-up */
    )
{
    UINT64 ByteOffset = MFTIndex * Vcb->BytesPerFileRecord;
    ULONG  Written;

    gNtfsRecordWrites++;
    if (!NtfsEfiUnfixupRecord (Vcb, &FileRecord->Ntfs)) return EFI_VOLUME_CORRUPTED;
    Written = NtfsEfiWriteAttr (Vcb, Vcb->MFTContext, ByteOffset,
                                (PCHAR)FileRecord, Vcb->BytesPerFileRecord);
    /* keep $MFTMirr in lock-step for the mirrored primaries (0-3) */
    if (Written == Vcb->BytesPerFileRecord && MFTIndex <= 3) {
        NtfsEfiSyncMftMirror (Vcb, MFTIndex, FileRecord);
    }
    /* restore in-memory record to normal (fixed-up) form regardless */
    NtfsEfiFixupRecord (Vcb, &FileRecord->Ntfs);

    if (Written != Vcb->BytesPerFileRecord) return EFI_DEVICE_ERROR;
    /* refresh the cache with the just-written (fixed-up) image so subsequent
     * reads see the new content without a DiskIo round-trip */
    NtfsMftCachePut (Vcb, MFTIndex, FileRecord);
    return EFI_SUCCESS;
}
