/**
 * ntfs_diskio.c - thin wrapper around EFI_DISK_IO_PROTOCOL.
 */

#include "ntfs.h"

/* Perf counters: DiskIo round-trips + bytes moved. A deterministic metric
 * (unlike wall-clock, which QEMU/host load makes noisy). Reported at unmount.
 *
 * Deliberately unsynchronised (no RaiseTPL/atomics): this driver never
 * arms a timer or an async EFI_EVENT of its own, and every entry point is
 * a direct EFI_FILE_PROTOCOL call from the caller's own thread of
 * execution, so there is no concurrent writer to race against at the TPL
 * this code runs at. If a future version starts using EFI_EVENT callbacks
 * or gets called from a driver that raises TPL and reenters, these need
 * to move behind gBS->RaiseTPL (TPL_NOTIFY) or an atomic increment. */
UINT64 gNtfsReadCalls  = 0;
UINT64 gNtfsWriteCalls = 0;
UINT64 gNtfsReadBytes  = 0;
UINT64 gNtfsWriteBytes = 0;
UINT64 gNtfsRecordWrites = 0;   /* subset: MFT record writes */
UINT64 gNtfsRecordReads  = 0;   /* subset: MFT record reads  */
UINT64 gNtfsIndexWrites  = 0;   /* subset: INDX block writes */

EFI_STATUS
NtfsEfiReadDisk (
    IN  PNTFS_EFI_VCB Vcb,
    IN  UINT64        ByteOffset,
    IN  UINTN         Length,
    OUT VOID         *Buffer
    )
{
    gNtfsReadCalls++;
    gNtfsReadBytes += Length;
    return Vcb->DiskIo->ReadDisk (Vcb->DiskIo, Vcb->MediaId, ByteOffset, Length, Buffer);
}

EFI_STATUS
NtfsEfiWriteDisk (
    IN PNTFS_EFI_VCB Vcb,
    IN UINT64        ByteOffset,
    IN UINTN         Length,
    IN VOID         *Buffer
    )
{
    gNtfsWriteCalls++;
    gNtfsWriteBytes += Length;
    return Vcb->DiskIo->WriteDisk (Vcb->DiskIo, Vcb->MediaId, ByteOffset, Length, Buffer);
}
