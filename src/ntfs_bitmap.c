/**
 * ntfs_bitmap.c - $Bitmap (MFT=6) cluster allocator. 1 bit per cluster,
 * 1=allocated. First-fit, with a rolling allocation cursor (Vcb->BitmapAllocHint)
 * so a long run of allocations does not re-scan already-allocated space from
 * bit 0 every single time (see NtfsEfiAllocateClusters below).
 */

#include "ntfs.h"

/* Scan [FromBit, ToBit) for the first free run of up to ClustersNeeded
 * contiguous clusters. Returns the run found (RunLen may be < ClustersNeeded,
 * a short/best-effort run) via *OutStart / *OutLen; RunLen==0 means nothing
 * free was found in this range at all.
 *
 * SCALE: a naive bit-at-a-time walk is O(bits) = up to ~256 million
 * iterations to cross the $Bitmap of a full 1 TB / 4 KiB-cluster volume,
 * PER allocation - a copy onto a large, nearly-full volume then spends
 * almost all its time here (looked like a hang / write failure on real
 * bare-metal 1 TB systems while small Hyper-V test volumes were fine). A
 * byte that reads 0xFF, or an 8-byte word that reads all-ones, is 8 / 64
 * fully-allocated clusters with no free bit - skip the whole chunk at once
 * (only while no partial free run is open, since any set bit breaks a run).
 * This makes crossing fully-allocated regions O(bytes/8) instead of O(bits)
 * and leaves the exact free-run accounting bit-accurate. */
static VOID
NtfsScanBitmapRange (
    IN  PUCHAR  BmBuf,
    IN  UINT64  FromBit,
    IN  UINT64  ToBit,
    IN  UINT64  ClustersNeeded,
    OUT UINT64 *OutStart,
    OUT UINT64 *OutLen
    )
{
    UINT64 RunStart  = (UINT64)-1;
    UINT64 RunLen    = 0;
    UINT64 BestStart = 0;
    UINT64 BestLen   = 0;
    UINT64 Bit       = FromBit;

    while (Bit < ToBit) {
        /* Fast-skip fully-allocated regions, but only when no free run is
         * currently open (RunLen==0) and we sit on a byte boundary - a set
         * bit anywhere in the skipped span would break a run, and RunLen==0
         * means there is no run to break. */
        if (RunLen == 0 && (Bit & 7) == 0) {
            /* whole all-ones 8-byte words first (64 allocated clusters each) */
            while (Bit + 64 <= ToBit &&
                   BmBuf[Bit/8+0]==0xFF && BmBuf[Bit/8+1]==0xFF &&
                   BmBuf[Bit/8+2]==0xFF && BmBuf[Bit/8+3]==0xFF &&
                   BmBuf[Bit/8+4]==0xFF && BmBuf[Bit/8+5]==0xFF &&
                   BmBuf[Bit/8+6]==0xFF && BmBuf[Bit/8+7]==0xFF) {
                Bit += 64;
            }
            /* then single all-ones bytes (8 allocated clusters each) */
            while (Bit + 8 <= ToBit && BmBuf[Bit/8] == 0xFF) {
                Bit += 8;
            }
            if (Bit >= ToBit) break;
        }

        {
            BOOLEAN Used = (BmBuf[Bit / 8] >> (Bit % 8)) & 1;
            if (!Used) {
                if (RunStart == (UINT64)-1) RunStart = Bit;
                RunLen++;
                if (RunLen >= ClustersNeeded) {
                    BestStart = RunStart;
                    BestLen   = RunLen;
                    break;
                }
            } else {
                /* Keep the longest run seen so far, not merely the last one.
                 * A caller that asks for a big chunk and settles for less -
                 * $MFT growth - would otherwise be handed whatever few free
                 * clusters happen to sit at the end of the range, while a
                 * much longer run earlier in it was scanned past and
                 * forgotten. */
                if (RunLen > BestLen) {
                    BestStart = RunStart;
                    BestLen   = RunLen;
                }
                RunStart = (UINT64)-1;
                RunLen   = 0;
            }
        }
        Bit++;
    }
    if (RunLen > BestLen) {
        BestStart = RunStart;
        BestLen   = RunLen;
    }
    *OutStart = (BestLen == 0) ? 0 : BestStart;
    *OutLen   = BestLen;
}

/*
 * Best-effort: finds the first free run of up to ClustersNeeded contiguous
 * clusters (may return fewer - caller decides whether a short allocation
 * is acceptable). Marks the returned run as allocated on disk before
 * returning, so a failed caller MUST roll back via NtfsEfiFreeClusters()
 * rather than silently leaking the allocation.
 *
 * Scan order: start at Vcb->BitmapAllocHint (end of the last allocation)
 * and scan forward to the end of the bitmap first; only if that comes up
 * short does it wrap around and scan [0, BitmapAllocHint) for space freed
 * behind the cursor (e.g. by a delete earlier in the same run). Without
 * this, a long sequential copy on a large/aged volume re-reads and
 * re-scans the *entire* $Bitmap from bit 0 on every single allocation,
 * which gets progressively slower as the low end of the disk fills up.
 */
EFI_STATUS
NtfsEfiAllocateClusters (
    IN  PNTFS_EFI_VCB Vcb,
    IN  UINT64        ClustersNeeded,
    OUT UINT64        *StartLCN,
    OUT UINT64        *GotClusters
    )
{
    PFILE_RECORD_HEADER BitmapRec;
    PNTFS_ATTR_CTX       BmCtx;
    UINT64                BmLen;
    UINT64                BmBits;
    PUCHAR                BmBuf;
    BOOLEAN               OwnBuf = FALSE;   /* TRUE only if BmBuf is our own alloc, not the VCB mirror */
    UINT64                RunStart = 0;
    UINT64                RunLen   = 0;
    UINT64                StartBit;
    EFI_STATUS            Status;

    *StartLCN = 0;
    *GotClusters = 0;

    BitmapRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (BitmapRec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, NTFS_FILE_BITMAP, BitmapRec))) {
        FreePool (BitmapRec);
        return EFI_DEVICE_ERROR;
    }

    BmCtx = NtfsEfiFindAttribute (Vcb, BitmapRec, AttributeData, NULL, 0, NULL);
    if (BmCtx == NULL) {
        FreePool (BitmapRec);
        return EFI_DEVICE_ERROR;
    }

    BmLen = NtfsEfiAttrDataLength (BmCtx);
    /*
     * Fast path: scan the in-RAM $Bitmap mirror seeded at mount instead of
     * re-reading the whole on-disk bitmap on every allocation. OwnBuf stays
     * NULL when we use the mirror, so the cleanup path frees only what it
     * actually allocated. Fallback (mirror absent / size drift): read on
     * demand exactly as before.
     */
    if (Vcb->VolBitmap != NULL && Vcb->VolBitmapLen == BmLen) {
        BmBuf  = Vcb->VolBitmap;
        OwnBuf = FALSE;
    } else {
        BmBuf = AllocatePool ((UINTN)BmLen);
        if (BmBuf == NULL) {
            NtfsEfiFreeAttrCtx (BmCtx);
            FreePool (BitmapRec);
            return EFI_OUT_OF_RESOURCES;
        }
        NtfsEfiReadAttr (Vcb, BmCtx, 0, (PCHAR)BmBuf, (ULONG)BmLen);
        OwnBuf = TRUE;
    }

    BmBits   = BmLen * 8;
    if (BmBits > Vcb->TotalClusters) BmBits = Vcb->TotalClusters;
    StartBit = Vcb->BitmapAllocHint;
    if (StartBit >= BmBits) StartBit = 0;

    /* Phase 1: hint -> end of bitmap. */
    NtfsScanBitmapRange (BmBuf, StartBit, BmBits, ClustersNeeded, &RunStart, &RunLen);

    /* Phase 2: only if phase 1 didn't already satisfy the request in full,
     * also try [0, StartBit) and keep whichever result is better - a full
     * match beats a short one, a longer short match beats a shorter one. */
    if (RunLen < ClustersNeeded && StartBit != 0) {
        UINT64 RunStart2, RunLen2;
        NtfsScanBitmapRange (BmBuf, 0, StartBit, ClustersNeeded, &RunStart2, &RunLen2);
        if (RunLen2 > RunLen) {
            RunStart = RunStart2;
            RunLen   = RunLen2;
        }
    }

    if (RunLen == 0) {
        Status = EFI_VOLUME_FULL;
        goto Done;
    }

    {
        UINT64 i;
        UINT64 FirstByte = RunStart / 8;
        UINT64 LastByte  = (RunStart + RunLen - 1) / 8;

        for (i = RunStart; i < RunStart + RunLen; i++) {
            BmBuf[i / 8] |= (UCHAR)(1U << (i % 8));
        }
        NtfsEfiWriteAttr (Vcb, BmCtx, FirstByte, (PCHAR)(BmBuf + FirstByte),
                          (ULONG)(LastByte - FirstByte + 1));
    }

    Vcb->FreeClusters = (Vcb->FreeClusters > RunLen) ? Vcb->FreeClusters - RunLen : 0;
    Vcb->BitmapAllocHint = RunStart + RunLen;   /* resume just past this run next time */
    *StartLCN    = RunStart;
    *GotClusters = RunLen;
    Status = EFI_SUCCESS;

Done:
    if (OwnBuf) FreePool (BmBuf);   /* never free the VCB-owned mirror */
    NtfsEfiFreeAttrCtx (BmCtx);
    FreePool (BitmapRec);
    return Status;
}

/* clusters per $MFT growth - 1 MiB, i.e. 1024 records at 1 KB each */
#define NTFS_MFT_GROW_CLUSTERS 256

static EFI_STATUS
NtfsGrowMft (
    IN PNTFS_EFI_VCB Vcb
    )
{
    UINT64             StartLCN, Got;
    EFI_STATUS          Status;
    PUCHAR              ZeroBuf;
    PNTFS_ATTR_RECORD   Attr;
    INT64               LastRealLCN = 0;
    ULONG               i;

    Print (L"[ntfs] NtfsGrowMft: growing Master File Table...\n");

    /*
     * Grow by a large chunk. Every growth that is not physically contiguous
     * with the previous $MFT extent costs one more mapping pair inside record
     * 0, and that record is 1 KB with no $ATTRIBUTE_LIST to spill into: at 16
     * clusters a chunk the table stops growing after about 7 000 records on a
     * fragmented volume, with the record's own attribute space, not the disk,
     * as the limit. The allocator hands back the longest contiguous run it can
     * find up to the amount asked for, so asking for more never fails - it
     * just returns a shorter run - while on a volume with free space it buys
     * 1024 records for the same single mapping pair.
     */
    Status = NtfsEfiAllocateClusters (Vcb, NTFS_MFT_GROW_CLUSTERS, &StartLCN, &Got);
    if (EFI_ERROR (Status)) return Status;
    if (Got < 1) {
        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        return EFI_VOLUME_FULL;
    }

    /* 1. Zero the newly allocated MFT cluster(s) on disk */
    ZeroBuf = AllocateZeroPool (Vcb->BytesPerCluster);
    if (ZeroBuf == NULL) {
        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        return EFI_OUT_OF_RESOURCES;
    }
    for (i = 0; i < (ULONG)Got; i++) {
        Status = NtfsEfiWriteDisk (Vcb, (StartLCN + i) * Vcb->BytesPerCluster,
                                   Vcb->BytesPerCluster, ZeroBuf);
        if (EFI_ERROR (Status)) break;
    }
    FreePool (ZeroBuf);
    if (EFI_ERROR (Status)) {
        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        return Status;
    }

    /* 2. Append the run to $MFT's $DATA in MFT record 0 */
    Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Vcb->MasterFileTable + Vcb->MftDataOffset);
    for (i = 0; i < Vcb->MFTContext->RunCount; i++) {
        if (Vcb->MFTContext->Runs[i].LBN != -1LL) {
            LastRealLCN = Vcb->MFTContext->Runs[i].LBN;
        }
    }

    if (!NtfsAppendRunToAttr (Vcb, Vcb->MasterFileTable, Vcb->MftDataOffset, StartLCN, Got, LastRealLCN)) {
        NtfsEfiFreeClusters (Vcb, StartLCN, Got);
        return EFI_VOLUME_FULL;
    }

    /* Update sizes */
    Attr = (PNTFS_ATTR_RECORD)((PUCHAR)Vcb->MasterFileTable + Vcb->MftDataOffset);
    Attr->NonResident.DataSize        = Attr->NonResident.AllocatedSize;
    Attr->NonResident.InitializedSize = Attr->NonResident.AllocatedSize;

    /* Write MFT record 0 back */
    Status = NtfsEfiWriteFileRecord (Vcb, NTFS_FILE_MFT, Vcb->MasterFileTable);
    if (EFI_ERROR (Status)) {
        return Status;
    }

    /* 3. Re-create Vcb->MFTContext */
    NtfsEfiFreeAttrCtx (Vcb->MFTContext);
    Vcb->MFTContext = NtfsEfiFindAttrInRecord (Vcb, Vcb->MasterFileTable, AttributeData, NULL, 0, &Vcb->MftDataOffset);
    if (Vcb->MFTContext == NULL) {
        Print (L"[ntfs] NtfsGrowMft: fatal - failed to recreate MFTContext!\n");
        return EFI_DEVICE_ERROR;
    }
    Vcb->MFTContext->FileMFTIndex = NTFS_FILE_MFT;

    Print (L"[ntfs] NtfsGrowMft: MFT grown successfully. New MftDataSize=%ld\n",
        (ULONG)NtfsEfiAttrDataLength (Vcb->MFTContext));

    return EFI_SUCCESS;
}

/*
 * Write MFT record 0 back and keep the VCB's cached copy of it in step.
 *
 * NtfsGrowMft edits Vcb->MasterFileTable - the VCB's own image of record 0 -
 * and writes THAT to disk wholesale. Anything stored into record 0 from a
 * local copy without refreshing the cache is therefore undone by the next
 * growth. Every record-0 write in this file goes through here.
 */
static EFI_STATUS
NtfsWriteMftRec0 (
    IN PNTFS_EFI_VCB       Vcb,
    IN PFILE_RECORD_HEADER Rec
    )
{
    EFI_STATUS     Status;
    PNTFS_ATTR_CTX Ctx;
    ULONG          DataOffset = 0;

    Status = NtfsEfiWriteFileRecord (Vcb, NTFS_FILE_MFT, Rec);
    if (EFI_ERROR (Status)) return Status;

    CopyMem (Vcb->MasterFileTable, Rec, Vcb->BytesPerFileRecord);
    Ctx = NtfsEfiFindAttrInRecord (Vcb, Vcb->MasterFileTable, AttributeData, NULL, 0, &DataOffset);
    if (Ctx == NULL) return EFI_DEVICE_ERROR;
    Ctx->FileMFTIndex = NTFS_FILE_MFT;
    NtfsEfiFreeAttrCtx (Vcb->MFTContext);
    Vcb->MFTContext    = Ctx;
    Vcb->MftDataOffset = DataOffset;
    return EFI_SUCCESS;
}

/*
 * Grow $MFT's own $BITMAP so it has a bit for every record the table now
 * holds.
 *
 * On a small volume this attribute is resident and grows inside record 0. On
 * any real Windows install it is not: a 136 000-record MFT needs about 17 KB
 * of bits. The non-resident form is extended inside the clusters it already
 * owns, and only when those run out is one more run allocated and appended.
 * The bytes that newly fall inside the attribute are zeroed, because past the
 * old data size the clusters hold whatever was there before, and a stale 1 bit
 * would hand out a record that is already in use.
 */
static EFI_STATUS
NtfsGrowMftBitmap (
    IN     PNTFS_EFI_VCB       Vcb,
    IN OUT PFILE_RECORD_HEADER MftRec0,
    IN     ULONG               BmOffset,
    IN     UINT64              OldLen,
    IN     UINT64              NeedLen
    )
{
    PNTFS_ATTR_RECORD BmAttr = (PNTFS_ATTR_RECORD)((PUCHAR)MftRec0 + BmOffset);
    EFI_STATUS        Status;

    if (NeedLen <= OldLen) return EFI_SUCCESS;

    if (!BmAttr->IsNonResident) {
        if (!NtfsEfiGrowResidentInRecord (Vcb, MftRec0, BmOffset, NeedLen)) {
            return NTFS_REFUSE (EFI_VOLUME_FULL);   /* record cannot hold more bits */
        }
        BmAttr = (PNTFS_ATTR_RECORD)((PUCHAR)MftRec0 + BmOffset);
        ZeroMem ((PUCHAR)BmAttr + BmAttr->Resident.ValueOffset + OldLen,
                 (UINTN)(NeedLen - OldLen));
        return NtfsWriteMftRec0 (Vcb, MftRec0);
    }

    if (NeedLen > (UINT64)BmAttr->NonResident.AllocatedSize) {
        UINT64 NeedClusters =
            (NeedLen - (UINT64)BmAttr->NonResident.AllocatedSize +
             Vcb->BytesPerCluster - 1) / Vcb->BytesPerCluster;
        UINT64 StartLCN = 0, Got = 0;

        Status = NtfsEfiAllocateClusters (Vcb, NeedClusters, &StartLCN, &Got);
        if (EFI_ERROR (Status)) return Status;
        if (Got < NeedClusters) {
            if (Got) NtfsEfiFreeClusters (Vcb, StartLCN, Got);
            return NTFS_REFUSE (EFI_VOLUME_FULL);
        }
        if (!NtfsAppendRunToAttr (Vcb, MftRec0, BmOffset, StartLCN, Got, 0)) {
            NtfsEfiFreeClusters (Vcb, StartLCN, Got);
            return NTFS_REFUSE (EFI_OUT_OF_RESOURCES);   /* no room for another mapping pair */
        }
        BmAttr = (PNTFS_ATTR_RECORD)((PUCHAR)MftRec0 + BmOffset);
    }

    BmAttr->NonResident.DataSize        = (LONGLONG)NeedLen;
    BmAttr->NonResident.InitializedSize = (LONGLONG)NeedLen;
    Status = NtfsWriteMftRec0 (Vcb, MftRec0);
    if (EFI_ERROR (Status)) return Status;

    {
        PNTFS_ATTR_CTX Fresh = NtfsEfiFindAttrInRecord (Vcb, MftRec0, AttributeBitmap, NULL, 0, NULL);
        PUCHAR         Zeros;
        UINT64         ZeroLen = NeedLen - OldLen;

        if (Fresh == NULL) return EFI_DEVICE_ERROR;
        Zeros = AllocateZeroPool ((UINTN)ZeroLen);
        if (Zeros == NULL) {
            NtfsEfiFreeAttrCtx (Fresh);
            return EFI_OUT_OF_RESOURCES;
        }
        if (NtfsEfiWriteAttr (Vcb, Fresh, OldLen, (PCHAR)Zeros, (ULONG)ZeroLen) != (ULONG)ZeroLen) {
            Status = EFI_DEVICE_ERROR;
        }
        FreePool (Zeros);
        NtfsEfiFreeAttrCtx (Fresh);
    }
    return Status;
}

/* First clear bit in [NTFS_FILE_FIRST_USER_FILE, Limit), or FALSE when every
 * one of them is taken. Records below NTFS_FILE_FIRST_USER_FILE belong to
 * volume metadata even where their bit happens to read as free. */
static BOOLEAN
NtfsFirstFreeBit (
    IN  CONST UCHAR *BmBuf,
    IN  UINT64       Limit,
    OUT UINT64      *FreeBit
    )
{
    UINT64 Bit;

    for (Bit = NTFS_FILE_FIRST_USER_FILE; Bit < Limit; Bit++) {
        if (!((BmBuf[Bit / 8] >> (Bit % 8)) & 1)) {
            *FreeBit = Bit;
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * $MFT (record 0) has its own unnamed $BITMAP attribute tracking which MFT
 * record slots are in use - separate from the volume-wide $Bitmap (record 6,
 * cluster allocation) handled above.
 *
 * Two passes at most: look for a free record bit, and if there is none, grow
 * the MFT (which appends fresh, all-zero records) and its bitmap, then look
 * again. Record 0 is re-read at the top of each pass and after the growth, so
 * no edit is ever made to a copy that NtfsGrowMft has since superseded.
 */
EFI_STATUS
NtfsEfiAllocateMftRecord (
    IN  PNTFS_EFI_VCB Vcb,
    OUT ULONGLONG    *NewIndex
    )
{
    PFILE_RECORD_HEADER MftRec0;
    ULONG               BmOffset = 0;
    PNTFS_ATTR_CTX      BmCtx;
    UINT64              BmLen;
    UINT64              MftRecordCount;
    UINT64              Bit = 0;
    UINT64              Limit;
    EFI_STATUS          Status;
    BOOLEAN             Resident;
    BOOLEAN             Found;
    UINTN               Attempt;

    *NewIndex = 0;

    MftRec0 = AllocatePool (Vcb->BytesPerFileRecord);
    if (MftRec0 == NULL) return EFI_OUT_OF_RESOURCES;

    for (Attempt = 0; Attempt < 2; Attempt++) {
        if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, NTFS_FILE_MFT, MftRec0))) {
            FreePool (MftRec0);
            return EFI_DEVICE_ERROR;
        }

        /*
         * NtfsEfiWriteAttr refuses resident writes - they would land in a
         * disconnected heap copy and never reach disk, see its comment in
         * ntfs_attr.c - so a resident bitmap is patched inside MftRec0's own
         * attribute value and the whole record written back, exactly like
         * every other resident-attribute write in this driver.
         */
        BmCtx = NtfsEfiFindAttrInRecord (Vcb, MftRec0, AttributeBitmap, NULL, 0, &BmOffset);
        if (BmCtx == NULL) {
            FreePool (MftRec0);
            return EFI_DEVICE_ERROR;
        }
        Resident       = !BmCtx->pRecord->IsNonResident;
        BmLen          = NtfsEfiAttrDataLength (BmCtx);
        MftRecordCount = NtfsEfiAttrDataLength (Vcb->MFTContext) / Vcb->BytesPerFileRecord;
        Limit          = (BmLen * 8 < MftRecordCount) ? BmLen * 8 : MftRecordCount;

        if (Resident) {
            PNTFS_ATTR_RECORD BmAttr = (PNTFS_ATTR_RECORD)((PUCHAR)MftRec0 + BmOffset);
            PUCHAR            BmBuf  = (PUCHAR)BmAttr + BmAttr->Resident.ValueOffset;

            Found = NtfsFirstFreeBit (BmBuf, Limit, &Bit);
            NtfsEfiFreeAttrCtx (BmCtx);
            if (Found) {
                BmBuf[Bit / 8] |= (UCHAR)(1U << (Bit % 8));
                Status = NtfsWriteMftRec0 (Vcb, MftRec0);
                if (!EFI_ERROR (Status)) *NewIndex = Bit;
                FreePool (MftRec0);
                return Status;
            }
        } else {
            PUCHAR BmBuf = AllocatePool ((UINTN)BmLen);

            if (BmBuf == NULL) {
                NtfsEfiFreeAttrCtx (BmCtx);
                FreePool (MftRec0);
                return EFI_OUT_OF_RESOURCES;
            }
            if (NtfsEfiReadAttr (Vcb, BmCtx, 0, (PCHAR)BmBuf, (ULONG)BmLen) != (ULONG)BmLen) {
                FreePool (BmBuf);
                NtfsEfiFreeAttrCtx (BmCtx);
                FreePool (MftRec0);
                return EFI_VOLUME_CORRUPTED;
            }
            Found = NtfsFirstFreeBit (BmBuf, Limit, &Bit);
            FreePool (BmBuf);
            if (Found) {
                UCHAR Byte = 0;

                if (NtfsEfiReadAttr (Vcb, BmCtx, Bit / 8, (PCHAR)&Byte, 1) != 1) {
                    NtfsEfiFreeAttrCtx (BmCtx);
                    FreePool (MftRec0);
                    return EFI_VOLUME_CORRUPTED;
                }
                Byte |= (UCHAR)(1U << (Bit % 8));
                Status = (NtfsEfiWriteAttr (Vcb, BmCtx, Bit / 8, (PCHAR)&Byte, 1) == 1)
                             ? EFI_SUCCESS : EFI_DEVICE_ERROR;
                NtfsEfiFreeAttrCtx (BmCtx);
                if (!EFI_ERROR (Status)) *NewIndex = Bit;
                FreePool (MftRec0);
                return Status;
            }
            NtfsEfiFreeAttrCtx (BmCtx);
        }

        if (Attempt > 0) break;     /* grew once already and still nothing */

        Status = NtfsGrowMft (Vcb);
        if (EFI_ERROR (Status)) {
            FreePool (MftRec0);
            return Status;
        }

        /* NtfsGrowMft rewrote record 0 from the VCB's copy: re-read it before
         * touching the bitmap, or the growth would be thrown away. */
        if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, NTFS_FILE_MFT, MftRec0))) {
            FreePool (MftRec0);
            return EFI_DEVICE_ERROR;
        }
        BmCtx = NtfsEfiFindAttrInRecord (Vcb, MftRec0, AttributeBitmap, NULL, 0, &BmOffset);
        if (BmCtx == NULL) {
            FreePool (MftRec0);
            return EFI_DEVICE_ERROR;
        }
        BmLen          = NtfsEfiAttrDataLength (BmCtx);
        MftRecordCount = NtfsEfiAttrDataLength (Vcb->MFTContext) / Vcb->BytesPerFileRecord;
        NtfsEfiFreeAttrCtx (BmCtx);

        if (MftRecordCount > BmLen * 8) {
            UINT64 NeedLen = ROUND_UP ((MftRecordCount + 7) / 8, 8);

            Status = NtfsGrowMftBitmap (Vcb, MftRec0, BmOffset, BmLen, NeedLen);
            if (EFI_ERROR (Status)) {
                FreePool (MftRec0);
                return Status;
            }
        }
    }

    FreePool (MftRec0);
    return NTFS_REFUSE (EFI_VOLUME_FULL);
}

EFI_STATUS
NtfsEfiFreeMftRecord (
    IN PNTFS_EFI_VCB Vcb,
    IN ULONGLONG     Index
    )
{
    PFILE_RECORD_HEADER MftRec0;
    ULONG                 BmOffset = 0;
    PNTFS_ATTR_CTX        BmCtx;
    EFI_STATUS            Status = EFI_SUCCESS;

    MftRec0 = AllocatePool (Vcb->BytesPerFileRecord);
    if (MftRec0 == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, NTFS_FILE_MFT, MftRec0))) {
        FreePool (MftRec0);
        return EFI_DEVICE_ERROR;
    }

    BmCtx = NtfsEfiFindAttrInRecord (Vcb, MftRec0, AttributeBitmap, NULL, 0, &BmOffset);
    if (BmCtx == NULL) {
        FreePool (MftRec0);
        return EFI_DEVICE_ERROR;
    }

    if (!BmCtx->pRecord->IsNonResident) {
        PNTFS_ATTR_RECORD BmAttr = (PNTFS_ATTR_RECORD)((PUCHAR)MftRec0 + BmOffset);
        PUCHAR            BmBuf  = (PUCHAR)BmAttr + BmAttr->Resident.ValueOffset;
        /* Index comes from a file handle, i.e. ultimately from a directory entry
         * on disk: keep the bit clear inside the attribute's own value */
        if (Index / 8 >= BmAttr->Resident.ValueLength) {
            NtfsEfiFreeAttrCtx (BmCtx);
            FreePool (MftRec0);
            return EFI_VOLUME_CORRUPTED;
        }
        BmBuf[Index / 8] &= (UCHAR)~(1U << (Index % 8));
        NtfsEfiFreeAttrCtx (BmCtx);
        Status = NtfsWriteMftRec0 (Vcb, MftRec0);
    } else {
        UCHAR Byte;
        NtfsEfiReadAttr (Vcb, BmCtx, Index / 8, (PCHAR)&Byte, 1);
        Byte &= (UCHAR)~(1U << (Index % 8));
        NtfsEfiWriteAttr (Vcb, BmCtx, Index / 8, (PCHAR)&Byte, 1);
        NtfsEfiFreeAttrCtx (BmCtx);
    }

    FreePool (MftRec0);
    return Status;
}

EFI_STATUS
NtfsEfiFreeClusters (
    IN PNTFS_EFI_VCB Vcb,
    IN UINT64        StartLCN,
    IN UINT64        Count
    )
{
    PFILE_RECORD_HEADER BitmapRec;
    PNTFS_ATTR_CTX       BmCtx;
    UINT64                FirstByte = StartLCN / 8;
    UINT64                LastByte  = (StartLCN + Count - 1) / 8;
    PUCHAR                Patch;
    UINT64                i;
    EFI_STATUS            Status = EFI_SUCCESS;

    if (Count == 0) return EFI_SUCCESS;

    BitmapRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (BitmapRec == NULL) return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, NTFS_FILE_BITMAP, BitmapRec))) {
        FreePool (BitmapRec);
        return EFI_DEVICE_ERROR;
    }

    BmCtx = NtfsEfiFindAttribute (Vcb, BitmapRec, AttributeData, NULL, 0, NULL);
    if (BmCtx == NULL) {
        FreePool (BitmapRec);
        return EFI_DEVICE_ERROR;
    }

    /*
     * Clear the freed bits and write only the affected byte-range back. When
     * the in-RAM $Bitmap mirror is live, clear the bits directly IN the mirror
     * (keeping it coherent with disk - otherwise freed clusters would look
     * allocated to the mirror-scanning allocator and leak for the rest of the
     * mount) and write that slice straight from it, skipping the read-back
     * entirely. Otherwise fall back to read-modify-write of a scratch patch.
     */
    if (Vcb->VolBitmap != NULL && Vcb->VolBitmapLen > LastByte) {
        for (i = StartLCN; i < StartLCN + Count; i++) {
            Vcb->VolBitmap[i / 8] &= (UCHAR)~(1U << (i % 8));
        }
        NtfsEfiWriteAttr (Vcb, BmCtx, FirstByte, (PCHAR)(Vcb->VolBitmap + FirstByte),
                          (ULONG)(LastByte - FirstByte + 1));
        Patch = NULL;
    } else {
        Patch = AllocatePool ((UINTN)(LastByte - FirstByte + 1));
        if (Patch == NULL) {
            NtfsEfiFreeAttrCtx (BmCtx);
            FreePool (BitmapRec);
            return EFI_OUT_OF_RESOURCES;
        }
        NtfsEfiReadAttr (Vcb, BmCtx, FirstByte, (PCHAR)Patch, (ULONG)(LastByte - FirstByte + 1));
        for (i = StartLCN; i < StartLCN + Count; i++) {
            Patch[i / 8 - FirstByte] &= (UCHAR)~(1U << (i % 8));
        }
        NtfsEfiWriteAttr (Vcb, BmCtx, FirstByte, (PCHAR)Patch, (ULONG)(LastByte - FirstByte + 1));
    }

    Vcb->FreeClusters += Count;

    /* If we just freed a *sizeable* chunk at or before the allocation
     * cursor, pull the cursor back to StartLCN so the next allocation
     * reuses it promptly instead of only finding it after a full
     * wrap-around scan. Gated by a minimum size (one write-side
     * preallocation quantum's worth of clusters, mirroring
     * NTFS_WRITE_PREALLOC_BYTES in ntfs_file.c) so that many small,
     * scattered frees - e.g. deleting a directory tree full of tiny
     * files at random offsets - don't yank the cursor back to a low
     * address on every single one and re-introduce the "rescan from
     * near zero" cost the cursor exists to avoid. A genuinely large
     * free (freeing a big file, or a whole cleaned-up directory's
     * worth of contiguous space) still gets picked up immediately. */
    {
        #define NTFS_BITMAP_RETREAT_THRESHOLD_BYTES (256ULL * 1024ULL)
        UINT64 RetreatThresholdClusters =
            (NTFS_BITMAP_RETREAT_THRESHOLD_BYTES + Vcb->BytesPerCluster - 1) /
            Vcb->BytesPerCluster;
        if (StartLCN < Vcb->BitmapAllocHint && Count >= RetreatThresholdClusters) {
            Vcb->BitmapAllocHint = StartLCN;
        }
        #undef NTFS_BITMAP_RETREAT_THRESHOLD_BYTES
    }

    if (Patch != NULL) FreePool (Patch);   /* NULL when the mirror path was used */
    NtfsEfiFreeAttrCtx (BmCtx);
    FreePool (BitmapRec);
    return Status;
}
