/**
 * ntfs_entry.c - module entry point and the AutoGen-replacement plumbing
 * (ProcessLibraryConstructorList/ProcessModuleEntryPointList/UefiUnload)
 * that UefiApplicationEntryPoint.lib expects to find, since this project's
 * plain-MSVC build doesn't run the edk2 build system's AutoGen step.
 */

#include "ntfs.h"

EFI_STATUS EFIAPI
UefiMain (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    );

EFI_STATUS EFIAPI
UefiUnload (
    IN EFI_HANDLE ImageHandle
    )
{
    (VOID)ImageHandle;
    DebugLog_Close ();
    return EFI_SUCCESS;
}

VOID EFIAPI
ProcessLibraryConstructorList (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    (VOID)ImageHandle;
    (VOID)SystemTable;
}

VOID EFIAPI
ProcessLibraryDestructorList (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    (VOID)ImageHandle;
    (VOID)SystemTable;
}

EFI_STATUS EFIAPI
ProcessModuleEntryPointList (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    return UefiMain (ImageHandle, SystemTable);
}

EFI_STATUS EFIAPI
UefiMain (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    /*
     * ProcessLibraryConstructorList() above is a stub, so the real
     * UefiBootServicesTableLib constructor (which normally populates
     * gBS/gST/gImageHandle) never runs. Without this, gBS is NULL and
     * gBS->InstallMultipleProtocolInterfaces() below jumps through a
     * garbage pointer read from low memory (IVT/BDA) instead of faulting
     * cleanly - observed as a #UD in the legacy VGA segment (0xB0000).
     */
    gImageHandle = ImageHandle;
    gST          = SystemTable;
    gBS          = SystemTable->BootServices;
    gRT          = SystemTable->RuntimeServices;

    DebugLog_Init (ImageHandle, TRUE);
    DebugLog_SetNoFlush (TRUE);   /* buffer; flush explicitly (per-line flush hangs QEMU vvfat) */

    gNtfsDriverBinding.ImageHandle = ImageHandle;
    gNtfsDriverBinding.DriverBindingHandle = ImageHandle;

    return gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiDriverBindingProtocolGuid,
                  &gNtfsDriverBinding,
                  NULL
                  );
}
