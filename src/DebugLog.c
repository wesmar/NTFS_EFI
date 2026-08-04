#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include "DebugLog.h"

#if ENABLE_DEBUG_LOG

STATIC BOOLEAN            gDbgEnabled = FALSE;
STATIC EFI_FILE_PROTOCOL *gDbgRoot    = NULL;
STATIC EFI_FILE_PROTOCOL *gDbgFile    = NULL;
STATIC BOOLEAN            gDbgNoFlush = FALSE;

STATIC VOID DebugLog_OpenFresh(VOID)
{
    EFI_FILE_PROTOCOL *old = NULL;
    if (!gDbgRoot) return;

    if (!EFI_ERROR(gDbgRoot->Open(
            gDbgRoot, &old, L"\\EFI\\Boot\\ntfs.log",
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
        old->Delete(old);
    }

    (VOID)gDbgRoot->Open(
        gDbgRoot, &gDbgFile, L"\\EFI\\Boot\\ntfs.log",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
}

EFI_STATUS DebugLog_Init(IN EFI_HANDLE ImageHandle, IN BOOLEAN Enable)
{
    gDbgEnabled = FALSE;
    gDbgFile = NULL;
    gDbgRoot = NULL;

    if (!Enable) return EFI_SUCCESS;

    EFI_LOADED_IMAGE_PROTOCOL        *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *fs = NULL;

    EFI_STATUS st = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&li);
    if (EFI_ERROR(st) || !li) return st;

    st = (li->DeviceHandle != NULL)
        ? gBS->HandleProtocol(li->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&fs)
        : EFI_NOT_FOUND;
    if (!EFI_ERROR(st) && fs) {
        st = fs->OpenVolume(fs, &gDbgRoot);
    }
    /* Fallback: when the driver was LoadImage'd from a memory buffer (probe
     * self-load), DeviceHandle is NULL. Log to the first FAT volume that already
     * has an \EFI\Boot directory (our boot ESP). */
    if (EFI_ERROR(st) || !gDbgRoot) {
        EFI_HANDLE *H = NULL; UINTN HC = 0, i;
        gDbgRoot = NULL;
        if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &HC, &H))) {
            for (i = 0; i < HC; i++) {
                EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *cand = NULL; EFI_FILE_PROTOCOL *r = NULL, *d = NULL;
                if (EFI_ERROR(gBS->HandleProtocol(H[i], &gEfiSimpleFileSystemProtocolGuid, (VOID**)&cand))) continue;
                if (EFI_ERROR(cand->OpenVolume(cand, &r)) || !r) continue;
                if (!EFI_ERROR(r->Open(r, &d, L"\\EFI\\Boot", EFI_FILE_MODE_READ, 0))) {
                    d->Close(d); gDbgRoot = r; break;
                }
                r->Close(r);
            }
            gBS->FreePool(H);
        }
    }
    if (!gDbgRoot) return EFI_NOT_FOUND;

    DebugLog_OpenFresh();
    if (!gDbgFile) {
        gDbgRoot->Close(gDbgRoot);
        gDbgRoot = NULL;
        return EFI_DEVICE_ERROR;
    }

    gDbgEnabled = TRUE;
    DebugLog_Write("=== ntfs log start ===");
    return EFI_SUCCESS;
}

VOID DebugLog_Reopen(VOID)
{
    if (!gDbgEnabled || !gDbgRoot) return;
    gDbgFile = NULL;
    (VOID)gDbgRoot->Open(gDbgRoot, &gDbgFile, L"\\EFI\\Boot\\ntfs.log",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
}

VOID DebugLog_Close(VOID)
{
    if (gDbgFile) {
        gDbgFile->Flush(gDbgFile);
        gDbgFile->Close(gDbgFile);
        gDbgFile = NULL;
    }
    if (gDbgRoot) {
        gDbgRoot->Close(gDbgRoot);
        gDbgRoot = NULL;
    }
    gDbgEnabled = FALSE;
    gDbgNoFlush = FALSE;
}

VOID DebugLog_SetNoFlush(IN BOOLEAN NoFlush)
{
    gDbgNoFlush = NoFlush;
}

VOID DebugLog_Flush(VOID)
{
    if (gDbgEnabled && gDbgFile)
        gDbgFile->Flush(gDbgFile);
}

VOID DebugLog_Write(IN CONST CHAR8 *Text)
{
    if (!gDbgEnabled || !gDbgFile || !Text) return;

    CHAR8 line[512];
    UINTN pos = 0;
    while (Text[pos] && pos < sizeof(line) - 3) {
        line[pos] = Text[pos];
        pos++;
    }
    line[pos++] = '\r';
    line[pos++] = '\n';
    line[pos] = '\0';

    gDbgFile->SetPosition(gDbgFile, 0xFFFFFFFFFFFFFFFFULL);
    UINTN wr = pos;
    gDbgFile->Write(gDbgFile, &wr, line);
    if (!gDbgNoFlush)
        gDbgFile->Flush(gDbgFile);
}

VOID DebugLog_Print(IN CONST CHAR8 *Fmt, ...)
{
    if (!gDbgEnabled || !gDbgFile || !Fmt) return;

    CHAR8 buf[448];
    VA_LIST args;
    VA_START(args, Fmt);
    AsciiVSPrint(buf, sizeof(buf), Fmt, args);
    VA_END(args);
    DebugLog_Write(buf);
}

#endif
