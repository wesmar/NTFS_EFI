/**
 * ntfs_volume.c - EFI_SIMPLE_FILE_SYSTEM_PROTOCOL.OpenVolume, volume
 * mount/unmount (called from EFI_DRIVER_BINDING_PROTOCOL.Start/Stop in
 * ntfs_binding.c), and the VCB<->SFSP containing-record cast.
 */

#include "ntfs.h"

PNTFS_EFI_VCB
NtfsEfiVcbFromSfsp (
    IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfsp
    )
{
    return (PNTFS_EFI_VCB)((PUCHAR)Sfsp - FIELD_OFFSET (NTFS_EFI_VCB, Sfsp));
}

VOID
NtfsEfiUnmountVolume (
    IN PNTFS_EFI_VCB Vcb
    )
{
    if (Vcb == NULL) {
        return;
    }

    if (Vcb->VolumeDirtySet) {
        NtfsSetVolumeDirty (Vcb, FALSE);
        Vcb->VolumeDirtySet = FALSE;
    }

    /* Push every buffered write through to the physical medium before we let
     * go of the volume. DiskIo->WriteDisk may sit in the BlockIo cache; a
     * small final write (e.g. a truncate's single MFT record) can otherwise
     * be lost on reset while a large one flushes itself. Flush guarantees
     * durability of ALL metadata we wrote this session. */
    if (Vcb->BlockIo != NULL && Vcb->BlockIo->FlushBlocks != NULL) {
        Vcb->BlockIo->FlushBlocks (Vcb->BlockIo);
    }

    DebugLog_Flush ();   /* persist buffered diagnostics before teardown */

    /* Report the DiskIo perf counters straight to ConOut (the serial console in
     * the headless test), so test-qemu's log captures a deterministic metric. */
    {
        CHAR16 Line[160];
        UnicodeSPrint (Line, sizeof (Line),
            L"==NTFS-PERF== reads=%lu (%lu B, rec=%lu) writes=%lu (%lu B, rec=%lu, indx=%lu)\r\n",
            gNtfsReadCalls, gNtfsReadBytes, gNtfsRecordReads,
            gNtfsWriteCalls, gNtfsWriteBytes, gNtfsRecordWrites, gNtfsIndexWrites);
        if (gST != NULL && gST->ConOut != NULL)
            gST->ConOut->OutputString (gST->ConOut, Line);
    }

    if (Vcb->MFTContext != NULL) {
        NtfsEfiFreeAttrCtx (Vcb->MFTContext);
    }
    if (Vcb->MasterFileTable != NULL) {
        FreePool (Vcb->MasterFileTable);
    }
    if (Vcb->UpcaseTable != NULL) {
        FreePool (Vcb->UpcaseTable);
    }
    if (Vcb->VolBitmap != NULL) {
        FreePool (Vcb->VolBitmap);
    }

    FreePool (Vcb);
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

/*
 * Allocate and initialise a NTFS_EFI_VCB from DiskIo + BlockIo.
 * Returns NULL on failure.
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
    if (EFI_ERROR (Status)) {
        Print (L"[ntfs] MountVolume: boot sector ReadDisk failed %r\n", Status);
        return NULL;
    }

    /* minimal NTFS signature check */
    if (CompareMem (Boot.OEMID, "NTFS    ", 8) != 0) {
        Print (L"[ntfs] MountVolume: OEMID mismatch\n");
        return NULL;
    }

    Vcb = AllocateZeroPool (sizeof (NTFS_EFI_VCB));
    if (Vcb == NULL) {
        Print (L"[ntfs] MountVolume: AllocateZeroPool(Vcb) failed\n");
        return NULL;
    }

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
        /* negative -> 2^|ClustersPerMftRecord| bytes */
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

    /* -- boot-sector geometry sanity ------------------------------------- *
     * This is a pre-boot rescue driver: it can be pointed at a damaged or
     * hand-edited volume. Every size below is later used as a DIVISOR
     * (cluster<->byte, VCN<->offset, record indexing). A zero or non-power-
     * of-two field would turn the first such division into a divide-by-zero
     * fault or a wild offset. Validate once, here, and refuse the volume
     * cleanly (return NULL -> firmware simply gets no NTFS FS on this
     * handle) instead of faulting deep in a later code path. All fields are
     * powers of two on every real NTFS volume. */
    #define NTFS_IS_POW2(x) ((x) != 0 && (((x) & ((x) - 1)) == 0))
    if (!NTFS_IS_POW2 (Vcb->BytesPerSector)      || Vcb->BytesPerSector  > 4096 ||
        !NTFS_IS_POW2 (Vcb->SectorsPerCluster)   ||
        !NTFS_IS_POW2 (Vcb->BytesPerCluster)     ||
        !NTFS_IS_POW2 (Vcb->BytesPerFileRecord)  ||
        !NTFS_IS_POW2 (Vcb->BytesPerIndexRecord)) {
        Print (L"[ntfs] MountVolume: implausible geometry bps=%d spc=%d bpc=%d bpfr=%d bpir=%d - refusing\n",
            Vcb->BytesPerSector, Vcb->SectorsPerCluster, Vcb->BytesPerCluster,
            Vcb->BytesPerFileRecord, Vcb->BytesPerIndexRecord);
        FreePool (Vcb);
        return NULL;
    }
    /*
     * The USA (update-sequence-array) fixup in ntfs_mft.c walks one
     * fix-up slot per BytesPerSector-sized chunk of a record and rewrites
     * the last two bytes of each such chunk. That is only in-bounds when a
     * record is at least one sector long. On a true 4Kn volume (logical
     * sector 4096) with the usual 1 KiB file record the record would be
     * SMALLER than a sector and the fixup would read/write past the record
     * buffer. This driver's fixup does not implement the sub-sector-record
     * case; refuse rather than corrupt. 512e drives report a 512-byte
     * logical sector here and are unaffected (1 KiB record >= 512).
     */
    if (Vcb->BytesPerFileRecord < Vcb->BytesPerSector ||
        Vcb->BytesPerIndexRecord < Vcb->BytesPerSector) {
        Print (L"[ntfs] MountVolume: record(%d/%d) < sector(%d) (4Kn) not supported - refusing\n",
            Vcb->BytesPerFileRecord, Vcb->BytesPerIndexRecord, Vcb->BytesPerSector);
        FreePool (Vcb);
        return NULL;
    }
    /*
     * This driver addresses $INDEX_ALLOCATION sub-nodes in CLUSTER units
     * (offset = VCN * BytesPerCluster) and allocates whole clusters per
     * index block (ClustersPer = BytesPerIndexRecord / BytesPerCluster).
     * That is only valid when an index record spans >= 1 whole cluster.
     * When the cluster is LARGER than the (typically 4 KiB) index record -
     * volumes formatted with a large allocation unit, or > 16 TB volumes
     * whose default cluster exceeds 4 KiB - NTFS stores the index VCN in
     * SECTOR units and packs several index blocks per cluster (see the
     * reference driver's bytes_per_index_record < cluster branch). This
     * driver does not implement that addressing: ClustersPer would compute
     * to 0 (divide-by-zero in VCN/ClustPerBlock) and every index read would
     * land at the wrong offset. Refuse rather than silently misread every
     * directory. The common 4 KiB-cluster / 4 KiB-index case is unaffected.
     */
    if (Vcb->BytesPerCluster > Vcb->BytesPerIndexRecord) {
        Print (L"[ntfs] MountVolume: cluster(%d) > index record(%d) not supported - refusing\n",
            Vcb->BytesPerCluster, Vcb->BytesPerIndexRecord);
        FreePool (Vcb);
        return NULL;
    }
    #undef NTFS_IS_POW2

    /* -- bootstrap $MFT --------------------------------------------------- */

    /*
     * The first file record of $MFT is at a fixed disk location.
     * We build a minimal attribute context for it manually so
     * NtfsEfiReadFileRecord() can use ReadAttr to load further records.
     */
    MftRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (MftRec == NULL) {
        Print (L"[ntfs] MountVolume: AllocatePool(MftRec) failed\n");
        FreePool (Vcb);
        return NULL;
    }

    {
        UINT64 MftByteOffset = Boot.EBPB.MftLocation * Vcb->BytesPerCluster;
        Print (L"[ntfs] MountVolume: BytesPerCluster=%d BytesPerFileRecord=%d MftLocation=%ld MftByteOffset=%ld\n",
            Vcb->BytesPerCluster, Vcb->BytesPerFileRecord, Boot.EBPB.MftLocation, MftByteOffset);
        Status = DiskIo->ReadDisk (DiskIo, Vcb->MediaId,
                                    MftByteOffset,
                                    Vcb->BytesPerFileRecord,
                                    MftRec);
        if (EFI_ERROR (Status)) {
            Print (L"[ntfs] MountVolume: $MFT record0 ReadDisk failed %r\n", Status);
            goto Fail;
        }
        Status = NtfsEfiFixupRecord (Vcb, &MftRec->Ntfs);
        if (EFI_ERROR (Status)) {
            Print (L"[ntfs] MountVolume: NtfsEfiFixupRecord($MFT) failed %r\n", Status);
            goto Fail;
        }
    }

    /* find the unnamed $DATA attribute of $MFT */
    DataCtx = NtfsEfiFindAttrInRecord (Vcb, MftRec, AttributeData, NULL, 0, &MftDataOffset);
    if (DataCtx == NULL) {
        Print (L"[ntfs] MountVolume: FindAttrInRecord($MFT, $DATA) failed\n");
        goto Fail;
    }
    DataCtx->FileMFTIndex = NTFS_FILE_MFT;

    Vcb->MasterFileTable = MftRec;
    Vcb->MFTContext      = DataCtx;
    Vcb->MftDataOffset   = MftDataOffset;

    /* -- load $UpCase (MFT #10): the on-disk Unicode case-folding table ----- */
    {
        PFILE_RECORD_HEADER UpRec;

        Vcb->UpcaseTable = AllocatePool (NTFS_UPCASE_ENTRIES * sizeof (USHORT));
        if (Vcb->UpcaseTable == NULL) {
            Print (L"[ntfs] MountVolume: AllocatePool(UpcaseTable) failed\n");
            goto Fail;
        }

        /* Safety-net default: identity, with ASCII a-z folded to A-Z. Gets
         * overwritten below by the real table when $UpCase reads cleanly -
         * kept so a missing/damaged $UpCase never yields a NULL deref or an
         * all-zero table (which would make every name compare equal). */
        {
            ULONG c;
            for (c = 0; c < NTFS_UPCASE_ENTRIES; c++) {
                Vcb->UpcaseTable[c] = (USHORT)((c >= L'a' && c <= L'z') ? (c - 32) : c);
            }
        }

        UpRec = AllocatePool (Vcb->BytesPerFileRecord);
        if (UpRec != NULL) {
            if (!EFI_ERROR (NtfsEfiReadFileRecord (Vcb, NTFS_FILE_UPCASE, UpRec))) {
                PNTFS_ATTR_CTX UpCtx = NtfsEfiFindAttribute (Vcb, UpRec,
                                            AttributeData, NULL, 0, NULL);
                if (UpCtx != NULL) {
                    UINT64 UpLen  = NtfsEfiAttrDataLength (UpCtx);
                    ULONG  ToRead = (ULONG)min (UpLen,
                                        (UINT64)(NTFS_UPCASE_ENTRIES * sizeof (USHORT)));
                    NtfsEfiReadAttr (Vcb, UpCtx, 0, (PCHAR)Vcb->UpcaseTable, ToRead);
                    NtfsEfiFreeAttrCtx (UpCtx);
                    Print (L"[ntfs] MountVolume: UpCase table loaded, %d bytes\n", ToRead);
                } else {
                    Print (L"[ntfs] MountVolume: $UpCase has no $DATA - using ASCII fallback\n");
                }
            } else {
                Print (L"[ntfs] MountVolume: $UpCase record read failed - using ASCII fallback\n");
            }
            FreePool (UpRec);
        }
    }

    /* -- load volume name --------------------------------------------------- */
    {
        PFILE_RECORD_HEADER VolumeRec = AllocatePool (Vcb->BytesPerFileRecord);
        if (VolumeRec != NULL) {
            if (!EFI_ERROR (NtfsEfiReadFileRecord (Vcb, 3ULL /* $Volume */, VolumeRec))) {
                PNTFS_ATTR_CTX VnCtx = NtfsEfiFindAttribute (Vcb, VolumeRec,
                                            AttributeVolumeName, NULL, 0, NULL);
                if (VnCtx != NULL) {
                    /*
                     * LabelBytes is $VOLUME_NAME's declared value length, i.e.
                     * on-disk data - it must be clamped to the DESTINATION size
                     * before the read, not just when placing the terminator.
                     * Vcb->VolumeLabel is a fixed WCHAR[128] inside the VCB, and
                     * a label attribute claiming more (up to the ~1 KB an MFT
                     * record can hold) overwrote the VCB fields that follow it -
                     * MasterFileTable and MFTContext are the very next members,
                     * so a hand-edited/corrupt $Volume record could plant
                     * arbitrary pointers into the mounted volume state.
                     */
                    ULONG LabelBytes = (ULONG)NtfsEfiAttrDataLength (VnCtx);
                    ULONG LabelChars;
                    ULONG MaxBytes = (ULONG)(sizeof (Vcb->VolumeLabel) - sizeof (WCHAR));
                    if (LabelBytes > MaxBytes) LabelBytes = MaxBytes;
                    LabelChars = LabelBytes / sizeof (WCHAR);
                    NtfsEfiReadAttr (Vcb, VnCtx, 0, (PCHAR)Vcb->VolumeLabel, LabelBytes);
                    Vcb->VolumeLabel[LabelChars] = L'\0';
                    Vcb->VolumeLabelLen = (USHORT)LabelChars;
                    NtfsEfiFreeAttrCtx (VnCtx);
                }
            }
            FreePool (VolumeRec);
        }
    }

    /* -- free space + $Bitmap mirror: $Bitmap (MFT=6) is 1 bit per cluster,
     * 1=allocated. Read the whole thing ONCE here, both to compute free space
     * and to seed Vcb->VolBitmap - the in-RAM mirror the cluster allocator
     * uses instead of re-reading the on-disk bitmap on every allocation. -- */
    {
        static CONST UCHAR PopCount4[16] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };
        PFILE_RECORD_HEADER BitmapRec = AllocatePool (Vcb->BytesPerFileRecord);
        if (BitmapRec != NULL) {
            if (!EFI_ERROR (NtfsEfiReadFileRecord (Vcb, NTFS_FILE_BITMAP, BitmapRec))) {
                PNTFS_ATTR_CTX BmCtx = NtfsEfiFindAttribute (Vcb, BitmapRec,
                                            AttributeData, NULL, 0, NULL);
                if (BmCtx != NULL) {
                    UINT64 BmLen         = NtfsEfiAttrDataLength (BmCtx);
                    UINT64 UsedClusters  = 0;

                    Vcb->VolBitmap = AllocatePool ((UINTN)BmLen);
                    if (Vcb->VolBitmap != NULL) {
                        ULONG Got = NtfsEfiReadAttr (Vcb, BmCtx, 0, (PCHAR)Vcb->VolBitmap, (ULONG)BmLen);
                        if ((UINT64)Got == BmLen) {
                            UINT64 i;
                            Vcb->VolBitmapLen = BmLen;
                            for (i = 0; i < BmLen; i++) {
                                UsedClusters += PopCount4[Vcb->VolBitmap[i] & 0x0F]
                                              + PopCount4[(Vcb->VolBitmap[i] >> 4) & 0x0F];
                            }
                        } else {
                            /* short read - discard the mirror; allocator falls
                             * back to on-demand reads. Still leave free-space
                             * uncounted-safe below. */
                            FreePool (Vcb->VolBitmap);
                            Vcb->VolBitmap = NULL;
                        }
                    }

                    Vcb->TotalClusters = BmLen * 8;
                    Vcb->FreeClusters  = (Vcb->TotalClusters > UsedClusters)
                                         ? (Vcb->TotalClusters - UsedClusters) : 0;
                    Print (L"[ntfs] MountVolume: TotalClusters=%ld FreeClusters=%ld VolBitmap=%p\n",
                        Vcb->TotalClusters, Vcb->FreeClusters, Vcb->VolBitmap);
                    NtfsEfiFreeAttrCtx (BmCtx);
                }
            }
            FreePool (BitmapRec);
        }
    }

    /* NOTE: no fast-startup / hibernation read-only guard here by design.
     * This is a pre-boot rescue driver; writing to a Windows volume that is
     * "off" (including after a Fast Startup hybrid shutdown) is its actual
     * job and works in practice from a USB boot. The theoretical risk is
     * narrow and only bites if Windows later RESUMES from an active
     * hiberfil.sys and flushes its stale cached NTFS metadata over external
     * changes - but Windows discards the hibernation image and cold-boots
     * when it detects the volume changed underneath it, so real corruption
     * is rare. Deciding that trade-off belongs to the operator, not a
     * blanket refusal, so the driver stays writable (same posture as
     * ntfs-3g's non-default force mount). */

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
        if (Vcb->UpcaseTable != NULL) {
            FreePool (Vcb->UpcaseTable);
        }
        FreePool (Vcb);
    }
    return NULL;
}

EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *
NtfsEfiGetSfsp (IN PNTFS_EFI_VCB Vcb)
{
    return &Vcb->Sfsp;
}

EFI_STATUS
NtfsSetVolumeDirty (
    IN PNTFS_EFI_VCB Vcb,
    IN BOOLEAN        Dirty
    )
{
    PFILE_RECORD_HEADER VolumeRec;
    PNTFS_ATTR_CTX      ViCtx = NULL;
    ULONG               AttrOffset = 0;
    EFI_STATUS          Status;

    VolumeRec = AllocatePool (Vcb->BytesPerFileRecord);
    if (VolumeRec == NULL) return EFI_OUT_OF_RESOURCES;

    Status = NtfsEfiReadFileRecord (Vcb, 3ULL /* $Volume */, VolumeRec);
    if (EFI_ERROR (Status)) {
        FreePool (VolumeRec);
        return Status;
    }

    ViCtx = NtfsEfiFindAttrInRecord (Vcb, VolumeRec, AttributeVolumeInformation, NULL, 0, &AttrOffset);
    if (ViCtx == NULL) {
        FreePool (VolumeRec);
        return EFI_NOT_FOUND;
    }

    {
        PNTFS_ATTR_RECORD Attr = (PNTFS_ATTR_RECORD)((PUCHAR)VolumeRec + AttrOffset);
        /* the dirty flag is the USHORT at value offset 10 (NTFS_VOLUME_INFORMATION
         * .VolumeFlags) - only touch it when the value really is that long */
        if (!Attr->IsNonResident &&
            Attr->Resident.ValueLength >= sizeof (NTFS_VOLUME_INFORMATION)) {
            PUCHAR Val = (PUCHAR)Attr + Attr->Resident.ValueOffset;
            PUSHORT FlagsPtr = (PUSHORT)(Val + 10);
            USHORT OldFlags = *FlagsPtr;
            USHORT NewFlags = OldFlags;

            if (Dirty) {
                NewFlags |= VOLUME_DIRTY;
            } else {
                NewFlags &= ~VOLUME_DIRTY;
            }

            if (OldFlags != NewFlags) {
                *FlagsPtr = NewFlags;
                NtfsEfiFreeAttrCtx (ViCtx);
                Status = NtfsEfiWriteFileRecord (Vcb, 3ULL, VolumeRec);
                FreePool (VolumeRec);
                Print (L"[ntfs] SetVolumeDirty: dirty=%d, flags: 0x%04x -> 0x%04x, status=%r\n",
                    Dirty, OldFlags, NewFlags, Status);
                return Status;
            }
        }
    }

    NtfsEfiFreeAttrCtx (ViCtx);
    FreePool (VolumeRec);
    return EFI_SUCCESS;
}

VOID
NtfsMarkVolumeDirty (
    IN PNTFS_EFI_VCB Vcb
    )
{
    if (Vcb != NULL && !Vcb->VolumeDirtySet) {
        Vcb->VolumeDirtySet = TRUE;
        NtfsSetVolumeDirty (Vcb, TRUE);
    }
}
