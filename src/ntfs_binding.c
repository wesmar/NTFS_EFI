/**
 * ntfs_binding.c - EFI_DRIVER_BINDING_PROTOCOL (Supported/Start/Stop).
 */

#include "ntfs.h"

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

EFI_DRIVER_BINDING_PROTOCOL gNtfsDriverBinding = {
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
    EFI_STATUS        RdStatus;

    if (DiskIo == NULL || BlockIo == NULL || BlockIo->Media == NULL) {
        Print (L"[ntfs] IsNtfsVolume: null DiskIo/BlockIo/Media\n");
        return FALSE;
    }
    if (!BlockIo->Media->MediaPresent) {
        Print (L"[ntfs] IsNtfsVolume: MediaPresent=FALSE\n");
        return FALSE;
    }

    RdStatus = DiskIo->ReadDisk (
            DiskIo,
            BlockIo->Media->MediaId,
            0,
            sizeof (Boot),
            &Boot);
    if (EFI_ERROR (RdStatus)) {
        Print (L"[ntfs] IsNtfsVolume: ReadDisk failed %r\n", RdStatus);
        return FALSE;
    }

    Print (L"[ntfs] IsNtfsVolume: OEMID='%c%c%c%c%c%c%c%c'\n",
        Boot.OEMID[0], Boot.OEMID[1], Boot.OEMID[2], Boot.OEMID[3],
        Boot.OEMID[4], Boot.OEMID[5], Boot.OEMID[6], Boot.OEMID[7]);

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
        Print (L"[ntfs] Supported(%p): no DiskIo (%r)\n", ControllerHandle, Status);
        return Status;
    }

    Status = gBS->HandleProtocol (
                    ControllerHandle,
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo
                    );
    if (EFI_ERROR (Status)) {
        Print (L"[ntfs] Supported(%p): no BlockIo (%r)\n", ControllerHandle, Status);
        return Status;
    }

    Print (L"[ntfs] Supported(%p): checking NTFS...\n", ControllerHandle);
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
        Print (L"[ntfs] Start(%p): OpenProtocol DiskIo failed %r\n", ControllerHandle, Status);
        return Status;
    }

    /*
     * BlockIo is only used here to read Media (BlockSize/MediaId/
     * MediaPresent) - actual I/O goes through DiskIo. DiskIoDxe already
     * holds BlockIo open BY_DRIVER on this same handle to produce DiskIo,
     * so requesting BY_DRIVER here too is unnecessary and gets rejected
     * with EFI_ACCESS_DENIED. GET_PROTOCOL is the correct attribute.
     */
    Status = gBS->OpenProtocol (
                    ControllerHandle,
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo,
                    This->DriverBindingHandle,
                    ControllerHandle,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
        Print (L"[ntfs] Start(%p): OpenProtocol BlockIo failed %r\n", ControllerHandle, Status);
        gBS->CloseProtocol (
               ControllerHandle,
               &gEfiDiskIoProtocolGuid,
               This->DriverBindingHandle,
               ControllerHandle
               );
        return Status;
    }

    Print (L"[ntfs] Start(%p): mounting...\n", ControllerHandle);
    Vcb = NtfsEfiMountVolume (DiskIo, BlockIo);
    if (Vcb == NULL) {
        Print (L"[ntfs] Start(%p): NtfsEfiMountVolume returned NULL\n", ControllerHandle);
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
    Print (L"[ntfs] Start(%p): mounted OK, installing SFSP...\n", ControllerHandle);

    Status = gBS->InstallMultipleProtocolInterfaces (
                    &ControllerHandle,
                    &gEfiSimpleFileSystemProtocolGuid,
                    &Vcb->Sfsp,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
        Print (L"[ntfs] Start(%p): InstallMultipleProtocolInterfaces failed %r\n", ControllerHandle, Status);
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
    } else {
        Print (L"[ntfs] Start(%p): SFSP installed, SUCCESS\n", ControllerHandle);
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
