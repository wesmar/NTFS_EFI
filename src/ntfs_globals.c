/**
 * ntfs_globals.c - GUIDs, PCD/debug-lib stubs, and the Print() replacement
 * that a normal edk2 build gets for free from the AutoGen/PCD database,
 * which this project's plain-MSVC build (build.ps1 -> cl.exe/link.exe
 * directly, no edk2 build system) does not run.
 */

#include "ntfs.h"

CONST UINT32 _gUefiDriverRevision = 0;
CHAR8       *gEfiCallerBaseName   = "ntfs";

/*
 * BasePrintLib.lib was built expecting these two PCD tokens from an
 * AutoGen module. Default values match MdePkg.dec
 * (PcdMaximumUnicodeStringLength / PcdMaximumAsciiStringLength = 1000000).
 */
UINT32 _PCD_GET_MODE_32_PcdMaximumUnicodeStringLength = 1000000;
UINT32 _PCD_GET_MODE_32_PcdMaximumAsciiStringLength   = 1000000;

VOID
NtfsEfiDebugPrint (
    IN CONST CHAR16 *Fmt,
    ...
    )
{
    CHAR16  Buf[256];
    VA_LIST Marker;

    VA_START (Marker, Fmt);
    UnicodeVSPrint (Buf, sizeof (Buf), Fmt, Marker);
    VA_END (Marker);

    // gST->ConOut->OutputString (gST->ConOut, Buf);
#if ENABLE_DEBUG_LOG
    /* Hot-path spam prefix filter: skip the ubiquitous "[ntfs]" trace lines so
     * a big run doesn't drown the log (and hang on flush); keep diagnostic tags
     * like "[btree]" / "[allocins]". Remove this filter for full tracing. */
    if (!(Buf[0]==L'['&&Buf[1]==L'n'&&Buf[2]==L't'&&Buf[3]==L'f'&&Buf[4]==L's'&&Buf[5]==L']')) {
        CHAR8 AsciiBuf[256];
        UINTN j = 0;
        while (Buf[j] && j < sizeof (AsciiBuf) - 1) {
            AsciiBuf[j] = (CHAR8)Buf[j];
            j++;
        }
        AsciiBuf[j] = '\0';
        DebugLog_Write (AsciiBuf);
    }
#endif
}

EFI_GUID gEfiDriverBindingProtocolGuid          = EFI_DRIVER_BINDING_PROTOCOL_GUID;
EFI_GUID gEfiSimpleFileSystemProtocolGuid       = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiBlockIoProtocolGuid                = EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID gEfiDiskIoProtocolGuid                 = EFI_DISK_IO_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid                       = EFI_FILE_INFO_ID;
EFI_GUID gEfiFileSystemInfoGuid                 = EFI_FILE_SYSTEM_INFO_ID;
EFI_GUID gEfiFileSystemVolumeLabelInfoIdGuid    = EFI_FILE_SYSTEM_VOLUME_LABEL_ID;
EFI_GUID gEfiLoadedImageProtocolGuid =
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

BOOLEAN EFIAPI
DebugAssertEnabled (
    VOID
    )
{
    return FALSE;
}

BOOLEAN EFIAPI
DebugPrintEnabled (
    VOID
    )
{
    return FALSE;
}

BOOLEAN EFIAPI
DebugCodeEnabled (
    VOID
    )
{
    return FALSE;
}

BOOLEAN EFIAPI
DebugClearMemoryEnabled (
    VOID
    )
{
    return FALSE;
}

BOOLEAN EFIAPI
DebugPrintLevelEnabled (
    IN CONST UINTN ErrorLevel
    )
{
    (VOID)ErrorLevel;
    return FALSE;
}

VOID EFIAPI
DebugAssert (
    IN CONST CHAR8 *FileName,
    IN UINTN        LineNumber,
    IN CONST CHAR8 *Description
    )
{
    (VOID)FileName;
    (VOID)LineNumber;
    (VOID)Description;
}

VOID EFIAPI
DebugPrint (
    IN UINTN        ErrorLevel,
    IN CONST CHAR8 *Format,
    ...
    )
{
    (VOID)ErrorLevel;
    (VOID)Format;
}

VOID EFIAPI
DebugVPrint (
    IN UINTN        ErrorLevel,
    IN CONST CHAR8 *Format,
    IN VA_LIST      VaListMarker
    )
{
    (VOID)ErrorLevel;
    (VOID)Format;
    (VOID)VaListMarker;
}

VOID EFIAPI
DebugBPrint (
    IN UINTN        ErrorLevel,
    IN CONST CHAR8 *Format,
    IN BASE_LIST    BaseListMarker
    )
{
    (VOID)ErrorLevel;
    (VOID)Format;
    (VOID)BaseListMarker;
}

VOID *EFIAPI
DebugClearMemory (
    OUT VOID *Buffer,
    IN UINTN  Length
    )
{
    return SetMem (Buffer, Length, 0);
}
