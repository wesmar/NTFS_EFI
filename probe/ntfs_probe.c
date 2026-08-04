/**
 * ntfs_probe.c - standalone diagnostic EFI application.
 *
 * Bypasses the UEFI Shell entirely (its 'cp'/'type' commands re-resolve
 * multi-component paths one directory at a time and, for paths deeper than
 * one level, re-issue the final Open() against the volume root instead of
 * the parent directory handle - a Shell-side bug, not a filesystem driver
 * concern). This app calls EFI_FILE_PROTOCOL.Open() with the full path in
 * a single call, exactly like a real bootloader would, against every
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL handle in the system, and reports
 * PASS/FAIL directly to the console (captured via -serial - writing
 * results through the FAT ESP was tried first but QEMU's vvfat write-back
 * proved unreliable in earlier testing, independent of driver correctness).
 */

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/FileInfo.h>

CONST UINT32 _gUefiDriverRevision = 0;
CHAR8       *gEfiCallerBaseName   = "ntfs_probe";

/* EFI_FILE_INFO_ID - provide the symbol ourselves (no GUID lib linked) */
EFI_GUID gEfiFileInfoGuid = { 0x09576e92, 0x6d3f, 0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

UINT32 _PCD_GET_MODE_32_PcdMaximumUnicodeStringLength = 1000000;
UINT32 _PCD_GET_MODE_32_PcdMaximumAsciiStringLength   = 1000000;

static VOID
ProbePrint (
    IN CONST CHAR16 *Fmt,
    ...
    )
{
    CHAR16  Buf[512];
    VA_LIST Marker;

    VA_START (Marker, Fmt);
    UnicodeVSPrint (Buf, sizeof (Buf), Fmt, Marker);
    VA_END (Marker);

    gST->ConOut->OutputString (gST->ConOut, Buf);
}

EFI_STATUS EFIAPI
UefiUnload (
    IN EFI_HANDLE ImageHandle
    )
{
    (VOID)ImageHandle;
    return EFI_SUCCESS;
}

/* running totals for the real-file copy test */
static UINTN  gCopyFiles = 0;
static UINTN  gCopyDirs  = 0;
static UINT64 gCopyBytes = 0;
static UINTN  gSrcFileFail = 0;   /* source Open(file) failed */
static UINTN  gSrcDirFail  = 0;   /* source Open(dir) failed  */
static UINTN  gDstDirFail   = 0;  /* dest Open(dir,CREATE) failed */
static EFI_FILE_PROTOCOL *gBigDst = NULL;   /* target root, for progress dumps */
static CHAR8 gLastName[160] = {0};          /* most-recent file, for crash triage */

/* Overwrite _PROG.txt on BIGDST with the current copy counters. Called every
 * few thousand files so that if the run dies without a serial (Hyper-V) the
 * last checkpoint survives. */
static VOID
DumpProgress (VOID)
{
    EFI_FILE_PROTOCOL *PF;
    if (gBigDst == NULL) return;
    if (EFI_ERROR (gBigDst->Open (gBigDst, &PF, L"_PROG.txt",
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) return;
    {
        CHAR8 b[256]; UINTN n;
        AsciiSPrint (b, sizeof (b), "files=%u dirs=%u bytes=%lu dstDirFail=%u last=%a\n",
            (UINT32)gCopyFiles, (UINT32)gCopyDirs, gCopyBytes, (UINT32)gDstDirFail, gLastName);
        for (n = 0; b[n]; n++) {}
        PF->SetPosition (PF, 0);
        PF->Write (PF, &n, b);
        PF->Close (PF);
    }
}

/*
 * Recursively copy a directory tree from SrcDir (a source volume, here the
 * FAT ESP that the host staged real \Windows\System32 files onto) into
 * DstDir (our NTFS volume, written entirely through ntfs.efi): create each
 * subdirectory, stream each file's bytes, then propagate timestamps and DOS
 * attributes via SetInfo - exactly what a real recursive copy does.
 */
static EFI_STATUS
CopyTree (
    IN EFI_FILE_PROTOCOL *SrcDir,
    IN EFI_FILE_PROTOCOL *DstDir,
    IN UINTN              Depth
    )
{
    UINT8 *InfoBuf;   /* on the HEAP: a 2 KB stack buffer x deep recursion (real
                       * source trees nest 30+ dirs) overflowed the small UEFI
                       * stack and killed the copy mid-run around ~8.5k files. */
    UINTN  InfoSize;
    EFI_STATUS RetSt = EFI_SUCCESS;

    if (Depth > 64) return EFI_SUCCESS;   /* guard against pathological depth */

    { VOID *p = NULL; if (EFI_ERROR (gBS->AllocatePool (EfiBootServicesData, 2048, &p)) || p == NULL) return EFI_OUT_OF_RESOURCES; InfoBuf = (UINT8 *)p; }

    SrcDir->SetPosition (SrcDir, 0);
    for (;;) {
        EFI_FILE_INFO     *Fi = (EFI_FILE_INFO *)InfoBuf;
        EFI_FILE_PROTOCOL *SrcChild, *DstChild;
        EFI_STATUS         St;

        InfoSize = 2048;   /* InfoBuf is now a heap pointer - sizeof would be 8! */
        St = SrcDir->Read (SrcDir, &InfoSize, InfoBuf);
        /* The 2048 B buffer covers any legal 255-char name, so
         * EFI_BUFFER_TOO_SMALL cannot happen here - the old 512 B buffer
         * returned it on long names and the `break` below truncated the
         * whole copy. */
        if (EFI_ERROR (St) || InfoSize == 0) break;

        if (Fi->FileName[0] == L'.' &&
            (Fi->FileName[1] == L'\0' ||
             (Fi->FileName[1] == L'.' && Fi->FileName[2] == L'\0'))) {
            continue;   /* skip "." and ".." */
        }

        if (Fi->Attribute & EFI_FILE_DIRECTORY) {
            if (EFI_ERROR (SrcDir->Open (SrcDir, &SrcChild, Fi->FileName,
                    EFI_FILE_MODE_READ, 0))) { gSrcDirFail++; continue; }
            St = DstDir->Open (DstDir, &DstChild, Fi->FileName,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                    EFI_FILE_DIRECTORY);
            if (EFI_ERROR (St)) {
                gDstDirFail++;
                ProbePrint (L"    MKDIR FAIL '%s': %r\r\n", Fi->FileName, St);
                SrcChild->Close (SrcChild);
                continue;
            }
            gCopyDirs++;
            CopyTree (SrcChild, DstChild, Depth + 1);
            DstChild->SetInfo (DstChild, &gEfiFileInfoGuid,
                (UINTN)Fi->Size, Fi);      /* dir timestamps/attrs */
            SrcChild->Close (SrcChild);
            DstChild->Close (DstChild);
        } else {
            UINT8      *Chunk;
            UINTN       ChunkSz = 64 * 1024;
            UINT64      Copied = 0;
            { UINTN q; for (q = 0; Fi->FileName[q] && q < sizeof (gLastName) - 1; q++) gLastName[q] = (CHAR8)Fi->FileName[q]; gLastName[q] = 0; }

            if (EFI_ERROR (SrcDir->Open (SrcDir, &SrcChild, Fi->FileName,
                    EFI_FILE_MODE_READ, 0))) { gSrcFileFail++; continue; }
            St = DstDir->Open (DstDir, &DstChild, Fi->FileName,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
            if (EFI_ERROR (St)) {
                ProbePrint (L"    copy CREATE FAIL '%s': %r\r\n", Fi->FileName, St);
                SrcChild->Close (SrcChild);
                continue;
            }

            { VOID *cp = NULL; gBS->AllocatePool (EfiBootServicesData, ChunkSz, &cp); Chunk = (UINT8 *)cp; }
            if (Chunk != NULL) {
                for (;;) {
                    UINTN Got = ChunkSz;
                    UINTN Put;
                    if (EFI_ERROR (SrcChild->Read (SrcChild, &Got, Chunk)) || Got == 0) break;
                    Put = Got;
                    if (EFI_ERROR (DstChild->Write (DstChild, &Put, Chunk)) || Put != Got) {
                        ProbePrint (L"    copy WRITE FAIL '%s' at %ld\r\n", Fi->FileName, Copied);
                        break;
                    }
                    Copied += Got;
                }
                gBS->FreePool (Chunk);
            }
            gCopyFiles++;
            gCopyBytes += Copied;
            if (gCopyFiles % 200 == 0) DumpProgress ();
            /* Propagate source timestamps + DOS attributes ONLY. Must not pass
             * the source's FileSize through: SetInfo treats a changed FileSize
             * as a resize, and the source EFI_FILE_INFO (here from QEMU vvfat)
             * can report a bogus size. Read the destination's own info and
             * overlay just the fields we mean to change, leaving FileSize as
             * the bytes we actually wrote. */
            {
                UINT8 DBuf[512]; UINTN DSz = sizeof (DBuf);
                EFI_FILE_INFO *Di = (EFI_FILE_INFO *)DBuf;
                if (!EFI_ERROR (DstChild->GetInfo (DstChild, &gEfiFileInfoGuid, &DSz, DBuf))) {
                    Di->CreateTime       = Fi->CreateTime;
                    Di->ModificationTime = Fi->ModificationTime;
                    Di->LastAccessTime   = Fi->LastAccessTime;
                    Di->Attribute        = Fi->Attribute;
                    DstChild->SetInfo (DstChild, &gEfiFileInfoGuid, (UINTN)Di->Size, Di);
                }
            }
            SrcChild->Close (SrcChild);
            DstChild->Close (DstChild);
        }
    }
    gBS->FreePool (InfoBuf);
    return RetSt;
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
UefiMain (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    );

EFI_STATUS EFIAPI
ProcessModuleEntryPointList (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    return UefiMain (ImageHandle, SystemTable);
}

BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugClearMemoryEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (IN CONST UINTN ErrorLevel) { (VOID)ErrorLevel; return FALSE; }

STATIC CHAR16 *gTestPaths[] = {
    L"\\hello.txt",
    L"\\subdir\\nested.txt",
    L"\\subdir/nested.txt",
    L"\\big.txt",
    L"\\compressed.txt",
    L"\\link_abs.txt",
    L"\\link_rel.txt",
    L"\\cs_dir\\Foo.txt",
    L"\\cs_dir\\foo.txt",
    L"\\YETANO~1.TXT",
    NULL
};

/* True if any mounted volume is labelled BIGDST or NTFSEFITEST (our NTFS driver
 * is already active). */
static BOOLEAN
NtfsAlreadyMounted (EFI_GUID *SfspGuid)
{
    EFI_HANDLE *H = NULL; UINTN HC = 0, i;
    BOOLEAN found = FALSE;
    EFI_GUID FsiGuid = EFI_FILE_SYSTEM_INFO_ID;
    if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, SfspGuid, NULL, &HC, &H))) return FALSE;
    for (i = 0; i < HC && !found; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sf; EFI_FILE_PROTOCOL *r;
        UINT8 ib[512]; UINTN is = sizeof (ib);
        if (EFI_ERROR (gBS->OpenProtocol (H[i], SfspGuid, (VOID **)&sf, gImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL))) continue;
        if (EFI_ERROR (sf->OpenVolume (sf, &r))) continue;
        if (!EFI_ERROR (r->GetInfo (r, &FsiGuid, &is, ib))) {
            CHAR16 *L = ((EFI_FILE_SYSTEM_INFO *)ib)->VolumeLabel;
            if ((L[0]==L'B'&&L[1]==L'I'&&L[2]==L'G'&&L[3]==L'D') ||
                (L[0]==L'N'&&L[1]==L'T'&&L[2]==L'F'&&L[3]==L'S')) found = TRUE;
        }
        r->Close (r);
    }
    if (H) gBS->FreePool (H);
    return found;
}

/* Load ntfs.efi from the ESP this probe booted off, start it (installs the
 * driver binding), and connect all controllers so it mounts the NTFS volumes.
 * Lets the probe run as a self-contained BOOTX64.EFI on Hyper-V (no UEFI shell /
 * startup.nsh). No-op-ish if the driver is already active. */
static VOID
SelfLoadDriver (EFI_HANDLE ImageHandle, EFI_LOADED_IMAGE_PROTOCOL *li, EFI_GUID *SfspGuid)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *esp; EFI_FILE_PROTOCOL *root, *drv;
    EFI_STATUS St;
    VOID *buf = NULL; UINTN sz;
    EFI_HANDLE drvHandle = NULL;
    EFI_HANDLE *H = NULL; UINTN HC = 0, i;
    UINT8 ib[512]; UINTN is;

    if (EFI_ERROR (gBS->HandleProtocol (li->DeviceHandle, SfspGuid, (VOID **)&esp))) {
        ProbePrint (L"  self-load: no ESP SFSP\r\n"); return;
    }
    if (EFI_ERROR (esp->OpenVolume (esp, &root))) { ProbePrint (L"  self-load: OpenVolume fail\r\n"); return; }
    if (EFI_ERROR (root->Open (root, &drv, L"\\ntfs.efi", EFI_FILE_MODE_READ, 0))) {
        ProbePrint (L"  self-load: no \\ntfs.efi\r\n"); root->Close (root); return;
    }
    /* size via GetInfo */
    is = sizeof (ib);
    { EFI_GUID fig = EFI_FILE_INFO_ID; drv->GetInfo (drv, &fig, &is, ib); }
    sz = (UINTN)((EFI_FILE_INFO *)ib)->FileSize;
    if (EFI_ERROR (gBS->AllocatePool (EfiBootServicesData, sz, &buf))) { drv->Close (drv); root->Close (root); return; }
    { UINTN rd = sz; drv->Read (drv, &rd, buf); sz = rd; }
    drv->Close (drv); root->Close (root);

    St = gBS->LoadImage (FALSE, ImageHandle, NULL, buf, sz, &drvHandle);
    if (EFI_ERROR (St)) { ProbePrint (L"  self-load: LoadImage %r\r\n", St); gBS->FreePool (buf); return; }
    St = gBS->StartImage (drvHandle, NULL, NULL);
    ProbePrint (L"  self-load: StartImage %r\r\n", St);
    gBS->FreePool (buf);

    /* connect every controller so the driver binds the NTFS disks */
    if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, SfspGuid, NULL, &HC, &H))) { if (H) gBS->FreePool (H); }
    {
        EFI_GUID DiskIoGuid = { 0xCE345171,0xBA0B,0x11d2,{0x8e,0x4F,0x00,0xa0,0xc9,0x69,0x72,0x3b} };
        H = NULL; HC = 0;
        if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &DiskIoGuid, NULL, &HC, &H))) {
            for (i = 0; i < HC; i++) gBS->ConnectController (H[i], NULL, NULL, TRUE);
            if (H) gBS->FreePool (H);
        }
    }
    (VOID)is;
}

EFI_STATUS EFIAPI
UefiMain (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    EFI_STATUS                        Status;
    EFI_HANDLE                        *Handles = NULL;
    UINTN                              HandleCount = 0;
    UINTN                              h;
    EFI_LOADED_IMAGE_PROTOCOL         *LoadedImage;
    EFI_GUID                          LoadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID                          SfspGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    gImageHandle = ImageHandle;
    gST          = SystemTable;
    gBS          = SystemTable->BootServices;

    ProbePrint (L"==NTFS-PROBE-START==\r\n");

    Status = gBS->OpenProtocol (ImageHandle, &LoadedImageGuid,
                    (VOID **)&LoadedImage, ImageHandle, NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (!EFI_ERROR (Status) && !NtfsAlreadyMounted (&SfspGuid)) {
        ProbePrint (L"  no NTFS mounted - self-loading ntfs.efi\r\n");
        SelfLoadDriver (ImageHandle, LoadedImage, &SfspGuid);
    }
    if (EFI_ERROR (Status)) {
        ProbePrint (L"OpenProtocol(LoadedImage) failed: %r\r\n", Status);
        return Status;
    }

    Status = gBS->LocateHandleBuffer (ByProtocol, &SfspGuid, NULL, &HandleCount, &Handles);
    if (EFI_ERROR (Status)) {
        ProbePrint (L"LocateHandleBuffer failed: %r\r\n", Status);
        return Status;
    }

    for (h = 0; h < HandleCount; h++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfsp;
        EFI_FILE_PROTOCOL               *Root;
        UINTN                            p;

        if (Handles[h] == LoadedImage->DeviceHandle) continue; /* skip our own ESP */

        Status = gBS->OpenProtocol (Handles[h], &SfspGuid,
                        (VOID **)&Sfsp, ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR (Status)) continue;

        Status = Sfsp->OpenVolume (Sfsp, &Root);
        ProbePrint (L"--- handle[%d] = %p, OpenVolume: %r ---\r\n", (UINT32)h, Handles[h], Status);
        if (EFI_ERROR (Status)) continue;

        for (p = 0; gTestPaths[p] != NULL; p++) {
            EFI_FILE_PROTOCOL *F;
            EFI_STATUS OpenStatus = Root->Open (Root, &F, gTestPaths[p], EFI_FILE_MODE_READ, 0);
            if (EFI_ERROR (OpenStatus)) {
                ProbePrint (L"  OPEN FAIL  %s -> %r\r\n", gTestPaths[p], OpenStatus);
            } else {
                CHAR8  ReadBuf[128];
                UINTN  ReadSize = sizeof (ReadBuf) - 1;
                EFI_STATUS RdStatus = F->Read (F, &ReadSize, ReadBuf);
                CHAR16 Preview[64];
                UINTN  i;
                UINTN  n = ReadSize < 48 ? ReadSize : 48;
                for (i = 0; i < n; i++) {
                    CHAR8 c = ReadBuf[i];
                    Preview[i] = (c >= 0x20 && c < 0x7f) ? (CHAR16)c : L'.';
                }
                Preview[n] = L'\0';
                ProbePrint (L"  OPEN OK    %s  Read:%r bytes=%d head='%s'\r\n",
                    gTestPaths[p], RdStatus, (UINT32)ReadSize, Preview);

                /* Full-file readback + simple rolling checksum, to verify
                 * (potentially LZNT1-decompressed) content byte-for-byte
                 * against a reference checksum computed on the host. */
                {
                    UINT32 Checksum = 0;
                    UINT64 Total    = 0;
                    CHAR8  Chunk[4096];
                    F->SetPosition (F, 0);
                    for (;;) {
                        UINTN ChunkSize = sizeof (Chunk);
                        EFI_STATUS ReadStatus = F->Read (F, &ChunkSize, Chunk);
                        UINTN ci;
                        if (EFI_ERROR (ReadStatus) || ChunkSize == 0) break;
                        for (ci = 0; ci < ChunkSize; ci++) {
                            Checksum = (Checksum * 131) + (UINT8)Chunk[ci];
                        }
                        Total += ChunkSize;
                    }
                    ProbePrint (L"    fullread: bytes=%ld checksum=%08x\r\n", Total, Checksum);
                }

                F->Close (F);
            }
        }

        /* Protocol-contract smoke tests used by file-manager style apps:
         * relative "."/".." navigation and shell-style slash separators. */
        {
            EFI_FILE_PROTOCOL *SubDir;
            EFI_STATUS St = Root->Open (Root, &SubDir, L"\\subdir", EFI_FILE_MODE_READ, 0);
            if (EFI_ERROR (St)) {
                ProbePrint (L"  NAV FAIL   open \\subdir -> %r\r\n", St);
            } else {
                EFI_FILE_PROTOCOL *ParentDir;
                EFI_FILE_PROTOCOL *Nested;

                St = SubDir->Open (SubDir, &ParentDir, L"..", EFI_FILE_MODE_READ, 0);
                if (EFI_ERROR (St)) {
                    ProbePrint (L"  NAV FAIL   subdir\\.. -> %r\r\n", St);
                } else {
                    ProbePrint (L"  NAV OK     subdir\\.. resolved\r\n");
                    ParentDir->Close (ParentDir);
                }

                St = SubDir->Open (SubDir, &Nested, L"./nested.txt", EFI_FILE_MODE_READ, 0);
                if (EFI_ERROR (St)) {
                    ProbePrint (L"  NAV FAIL   ./nested.txt -> %r\r\n", St);
                } else {
                    ProbePrint (L"  NAV OK     ./nested.txt resolved\r\n");
                    Nested->Close (Nested);
                }

                SubDir->Close (SubDir);
            }
        }

        /* Directory listing: verify system metadata files ($MFT etc.) are
         * hidden from enumeration, and dump free-space/volume info. */
        {
            EFI_FILE_PROTOCOL *ListRoot;
            EFI_STATUS ListStatus = Root->Open (Root, &ListRoot, L"\\", EFI_FILE_MODE_READ, 0);
            if (!EFI_ERROR (ListStatus)) {
                UINT8 InfoBuf[512];
                ProbePrint (L"  --- root listing ---\r\n");
                for (;;) {
                    UINTN InfoSize = sizeof (InfoBuf);
                    EFI_STATUS RdStatus = ListRoot->Read (ListRoot, &InfoSize, InfoBuf);
                    EFI_FILE_INFO *Info;
                    if (EFI_ERROR (RdStatus) || InfoSize == 0) break;
                    Info = (EFI_FILE_INFO *)InfoBuf;
                    ProbePrint (L"    [%s]%s\r\n", Info->FileName,
                        (Info->Attribute & EFI_FILE_DIRECTORY) ? L" <DIR>" : L"");
                }
                ListRoot->Close (ListRoot);
            }

            {
                UINT8 FsInfoBuf[512];
                UINTN FsInfoSize = sizeof (FsInfoBuf);
                EFI_GUID FsInfoGuid = EFI_FILE_SYSTEM_INFO_ID;
                EFI_STATUS FsStatus = Root->GetInfo (Root, &FsInfoGuid, &FsInfoSize, FsInfoBuf);
                if (!EFI_ERROR (FsStatus)) {
                    EFI_FILE_SYSTEM_INFO *Fsi = (EFI_FILE_SYSTEM_INFO *)FsInfoBuf;
                    ProbePrint (L"  FileSystemInfo: VolumeSize=%ld FreeSpace=%ld BlockSize=%d ReadOnly=%d Label='%s'\r\n",
                        Fsi->VolumeSize, Fsi->FreeSpace, Fsi->BlockSize,
                        (UINT32)Fsi->ReadOnly, Fsi->VolumeLabel);
                } else {
                    ProbePrint (L"  FileSystemInfo: GetInfo failed %r\r\n", FsStatus);
                }
            }
        }
    }

    /*
     * BIG TEST MODE: if a volume labelled "BIGSRC" (FAT source, read by OVMF)
     * and one labelled "BIGDST" (our NTFS target) are both present, copy the
     * entire source tree onto the target through ntfs.efi and stop - this is
     * the large real-file test. No synthetic battery runs in this mode.
     */
    {
        EFI_HANDLE *BH = NULL; UINTN BHC = 0, bi;
        EFI_FILE_PROTOCOL *BigSrc = NULL, *BigDst = NULL;
        EFI_HANDLE BigDstHandle = NULL;
        EFI_GUID FsiGuid = EFI_FILE_SYSTEM_INFO_ID;

        if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &SfspGuid, NULL, &BHC, &BH))) {
            for (bi = 0; bi < BHC; bi++) {
                EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sf; EFI_FILE_PROTOCOL *r;
                UINT8 ib[512]; UINTN is = sizeof (ib);
                if (EFI_ERROR (gBS->OpenProtocol (BH[bi], &SfspGuid, (VOID **)&sf,
                        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL))) continue;
                if (EFI_ERROR (sf->OpenVolume (sf, &r))) continue;
                if (EFI_ERROR (r->GetInfo (r, &FsiGuid, &is, ib))) { r->Close (r); continue; }
                {
                    CHAR16 *L = ((EFI_FILE_SYSTEM_INFO *)ib)->VolumeLabel;
                    if      (L[0]==L'B'&&L[1]==L'I'&&L[2]==L'G'&&L[3]==L'S'&&L[4]==L'R'&&L[5]==L'C'&&L[6]==0 && !BigSrc) BigSrc = r;
                    else if (L[0]==L'B'&&L[1]==L'I'&&L[2]==L'G'&&L[3]==L'D'&&L[4]==L'S'&&L[5]==L'T'&&L[6]==0 && !BigDst) { BigDst = r; BigDstHandle = BH[bi]; }
                    else r->Close (r);
                }
            }
        }

        if (BigSrc != NULL && BigDst != NULL) {
            ProbePrint (L"== BIG TEST: BIGSRC -> BIGDST (through ntfs.efi) ==\r\n");
            gCopyFiles = 0; gCopyDirs = 0; gCopyBytes = 0;
            gBigDst = BigDst;
            CopyTree (BigSrc, BigDst, 0);
            ProbePrint (L"  big-copy-done: files=%d dirs=%d bytes=%ld\r\n",
                (UINT32)gCopyFiles, (UINT32)gCopyDirs, gCopyBytes);
            ProbePrint (L"  big-copy-fails: srcFile=%d srcDir=%d dstDir=%d\r\n",
                (UINT32)gSrcFileFail, (UINT32)gSrcDirFail, (UINT32)gDstDirFail);
            /* persist a machine-readable result onto BIGDST (through ntfs.efi):
             * on Hyper-V there is no serial to scrape, so verify reads this. */
            {
                EFI_FILE_PROTOCOL *RF;
                if (!EFI_ERROR (BigDst->Open (BigDst, &RF, L"_RESULT.txt",
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
                    CHAR8 line[160]; UINTN n;
                    AsciiSPrint (line, sizeof (line),
                        "files=%u dirs=%u bytes=%lu srcFail=%u dstDirFail=%u\n",
                        (UINT32)gCopyFiles, (UINT32)gCopyDirs, gCopyBytes,
                        (UINT32)(gSrcFileFail + gSrcDirFail), (UINT32)gDstDirFail);
                    for (n = 0; line[n]; n++) {}
                    RF->Write (RF, &n, line);
                    RF->Close (RF);
                }
            }
            BigSrc->Close (BigSrc);
            BigDst->Flush (BigDst);
            BigDst->Close (BigDst);
            {
                EFI_STATUS DcSt = gBS->DisconnectController (BigDstHandle, NULL, NULL);
                ProbePrint (L"  big-clean-unmount: %r\r\n", DcSt);
            }
            ProbePrint (L"==NTFS-PROBE-END==\r\n");
            /* Power off so a Hyper-V harness knows the run finished (no serial
             * there). Harmless under QEMU too (it has -no-reboot). */
            SystemTable->RuntimeServices->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
            return EFI_SUCCESS;   /* big test done - skip synthetic battery */
        }
        if (BH != NULL) gBS->FreePool (BH);
    }

    /*
     * Write-support smoke test - phase 1: in-place overwrite of existing
     * bytes only. Per the "reads on real Hyper-V, writes only on
     * synthetic" rule, this only runs when \write_test.txt actually
     * exists (test-qemu.ps1's synthetic prep script creates it; the
     * production w11 deployment never has this file, so the block below
     * is inert there without needing a build-time flag).
     */
    {
        EFI_STATUS         WStatus;
        EFI_HANDLE         *WHandles = NULL;
        UINTN               WHandleCount = 0;

        WStatus = gBS->LocateHandleBuffer (ByProtocol, &SfspGuid, NULL, &WHandleCount, &WHandles);
        if (!EFI_ERROR (WStatus)) {
            UINTN wh;
            for (wh = 0; wh < WHandleCount; wh++) {
                EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *WSfsp;
                EFI_FILE_PROTOCOL               *WRoot;
                EFI_FILE_PROTOCOL               *WFile;

                if (WHandles[wh] == LoadedImage->DeviceHandle) continue;
                if (EFI_ERROR (gBS->OpenProtocol (WHandles[wh], &SfspGuid, (VOID **)&WSfsp,
                        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL))) continue;
                if (EFI_ERROR (WSfsp->OpenVolume (WSfsp, &WRoot))) continue;

                /* --- real recursive copy: ESP \src (host-staged System32
                 * files) -> NTFS \copied, all writes through ntfs.efi --- */
                {
                    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *EspSfsp;
                    EFI_FILE_PROTOCOL               *EspRoot, *SrcDir, *DstDir;
                    EFI_STATUS                       CpSt;

                    CpSt = gBS->OpenProtocol (LoadedImage->DeviceHandle, &SfspGuid,
                               (VOID **)&EspSfsp, ImageHandle, NULL,
                               EFI_OPEN_PROTOCOL_GET_PROTOCOL);
                    if (!EFI_ERROR (CpSt) && !EFI_ERROR (EspSfsp->OpenVolume (EspSfsp, &EspRoot))) {
                        if (!EFI_ERROR (EspRoot->Open (EspRoot, &SrcDir, L"\\src",
                                EFI_FILE_MODE_READ, 0))) {
                            CpSt = WRoot->Open (WRoot, &DstDir, L"copied",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                EFI_FILE_DIRECTORY);
                            ProbePrint (L"  --- copy test: \\src -> \\copied (%r) ---\r\n", CpSt);
                            if (!EFI_ERROR (CpSt)) {
                                gCopyFiles = 0; gCopyDirs = 0; gCopyBytes = 0;
                                CopyTree (SrcDir, DstDir, 0);
                                ProbePrint (L"    copy-done: files=%d dirs=%d bytes=%ld\r\n",
                                    (UINT32)gCopyFiles, (UINT32)gCopyDirs, gCopyBytes);
                                DstDir->Close (DstDir);
                            }
                            SrcDir->Close (SrcDir);
                        } else {
                            ProbePrint (L"  --- copy test: no \\src on ESP (skipped) ---\r\n");
                        }
                    }
                }

                if (!EFI_ERROR (WRoot->Open (WRoot, &WFile, L"\\write_test.txt",
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
                    CHAR8      Before[64], After[64], Patch[16];
                    UINTN      Size;
                    EFI_STATUS RdStatus, WrStatus;

                    ProbePrint (L"  --- write test: \\write_test.txt on handle[%d] ---\r\n", (UINT32)wh);

                    Size = sizeof (Before) - 1;
                    RdStatus = WFile->Read (WFile, &Size, Before);
                    Before[Size] = 0;
                    ProbePrint (L"    before: %r bytes=%d\r\n", RdStatus, (UINT32)Size);

                    CopyMem (Patch, "NTFS_WRITE_OK!!", 15);
                    WFile->SetPosition (WFile, 0);
                    Size = 15;
                    WrStatus = WFile->Write (WFile, &Size, Patch);
                    ProbePrint (L"    write:  %r bytes=%d\r\n", WrStatus, (UINT32)Size);

                    WFile->SetPosition (WFile, 0);
                    Size = sizeof (After) - 1;
                    RdStatus = WFile->Read (WFile, &Size, After);
                    After[Size] = 0;
                    {
                        CHAR16 AfterW[64];
                        UINTN  ci;
                        for (ci = 0; ci < Size && ci < 63; ci++) {
                            CHAR8 c = After[ci];
                            AfterW[ci] = (c >= 0x20 && c < 0x7f) ? (CHAR16)c : L'.';
                        }
                        AfterW[ci] = L'\0';
                        ProbePrint (L"    after:  %r bytes=%d content='%s'\r\n", RdStatus, (UINT32)Size, AfterW);
                    }

                    WFile->Close (WFile);
                }

                /* negative test: writing through a READ-only handle must
                 * be rejected (EFI_ACCESS_DENIED), not silently accepted */
                if (!EFI_ERROR (WRoot->Open (WRoot, &WFile, L"\\write_test.txt",
                        EFI_FILE_MODE_READ, 0))) {
                    CHAR8      Dummy[4] = "XXXX";
                    UINTN      Size = 4;
                    EFI_STATUS WrStatus = WFile->Write (WFile, &Size, Dummy);
                    ProbePrint (L"    readonly-write-reject: %r (expect Access Denied)\r\n", WrStatus);
                    WFile->Close (WFile);
                }

                /* growth test 1: resident file, append past current EOF
                 * (in-record grow, no cluster allocation) */
                if (!EFI_ERROR (WRoot->Open (WRoot, &WFile, L"\\grow_resident.txt",
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
                    UINT64     Pos;
                    CHAR8      Patch[24];
                    UINTN      Size;
                    EFI_STATUS St;

                    ProbePrint (L"  --- grow test: \\grow_resident.txt ---\r\n");
                    WFile->GetPosition (WFile, &Pos);
                    ProbePrint (L"    (unused start pos=%ld)\r\n", Pos);

                    WFile->SetPosition (WFile, 0xFFFFFFFFFFFFFFFFULL);  /* seek to EOF */
                    WFile->GetPosition (WFile, &Pos);
                    ProbePrint (L"    eof pos=%ld\r\n", Pos);

                    CopyMem (Patch, "_APPENDED_GROWTH_TAIL!!", 23);
                    Size = 23;
                    St = WFile->Write (WFile, &Size, Patch);
                    ProbePrint (L"    append-write: %r bytes=%d\r\n", St, (UINT32)Size);

                    {
                        CHAR8  Full[128];
                        CHAR16 FullW[128];
                        UINTN  ci;
                        WFile->SetPosition (WFile, 0);
                        Size = sizeof (Full) - 1;
                        St = WFile->Read (WFile, &Size, Full);
                        for (ci = 0; ci < Size && ci < 127; ci++) {
                            CHAR8 c = Full[ci];
                            FullW[ci] = (c >= 0x20 && c < 0x7f) ? (CHAR16)c : L'.';
                        }
                        FullW[ci] = L'\0';
                        ProbePrint (L"    readback: %r bytes=%d content='%s'\r\n", St, (UINT32)Size, FullW);
                    }
                    WFile->Close (WFile);
                }

                /* growth test 2: non-resident file, append past current
                 * AllocatedSize (real $Bitmap cluster allocation +
                 * mapping-pairs extend) */
                if (!EFI_ERROR (WRoot->Open (WRoot, &WFile, L"\\grow_nonres.bin",
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
                    CHAR8      Patch[9000];
                    UINTN      Size;
                    EFI_STATUS St;
                    UINTN      i;

                    ProbePrint (L"  --- grow test: \\grow_nonres.bin ---\r\n");
                    for (i = 0; i < sizeof (Patch); i++) Patch[i] = (CHAR8)('A' + (i % 26));

                    WFile->SetPosition (WFile, 0xFFFFFFFFFFFFFFFFULL);
                    Size = sizeof (Patch);
                    St = WFile->Write (WFile, &Size, Patch);
                    ProbePrint (L"    append-write: %r bytes=%d (expected %d)\r\n", St, (UINT32)Size, (UINT32)sizeof (Patch));

                    {
                        UINT8 InfoBuf[512];
                        UINTN InfoSize = sizeof (InfoBuf);
                        EFI_GUID FileInfoGuid = EFI_FILE_INFO_ID;
                        if (!EFI_ERROR (WFile->GetInfo (WFile, &FileInfoGuid, &InfoSize, InfoBuf))) {
                            EFI_FILE_INFO *Fi = (EFI_FILE_INFO *)InfoBuf;
                            ProbePrint (L"    post-grow FileSize=%ld PhysicalSize=%ld\r\n", Fi->FileSize, Fi->PhysicalSize);
                        }
                    }

                    {
                        UINT32 Checksum = 0;
                        UINT64 Total    = 0;
                        CHAR8  Chunk[4096];
                        WFile->SetPosition (WFile, 0);
                        for (;;) {
                            UINTN ChunkSize = sizeof (Chunk);
                            EFI_STATUS RdStatus = WFile->Read (WFile, &ChunkSize, Chunk);
                            UINTN ci;
                            if (EFI_ERROR (RdStatus) || ChunkSize == 0) break;
                            for (ci = 0; ci < ChunkSize; ci++) Checksum = (Checksum * 131) + (UINT8)Chunk[ci];
                            Total += ChunkSize;
                        }
                        ProbePrint (L"    fullread-after-grow: bytes=%ld checksum=%08x\r\n", Total, Checksum);
                    }
                    WFile->Close (WFile);
                }

                /* create test: brand-new file inside \cs_dir (now overflowed,
                 * still root-only $INDEX_ROOT - the volume root itself
                 * has long since overflowed into $INDEX_ALLOCATION from
                 * earlier test files, which this first cautious round of
                 * create doesn't support yet). CREATE is single-component
                 * only, so open the parent directory first, same as a
                 * real bootloader would. Write content, close, reopen
                 * fresh to prove it's really durable, not an in-memory
                 * handle echo. */
                {
                    EFI_FILE_PROTOCOL *CsDir;
                    EFI_STATUS DirStatus = WRoot->Open (WRoot, &CsDir, L"\\cs_dir",
                        EFI_FILE_MODE_READ, 0);
                    ProbePrint (L"  --- create test: \\cs_dir\\created_by_probe17.txt ---\r\n");
                    ProbePrint (L"    open parent \\cs_dir: %r\r\n", DirStatus);
                    if (!EFI_ERROR (DirStatus)) {
                        EFI_STATUS CrStatus = CsDir->Open (CsDir, &WFile, L"created_by_probe17.txt",
                            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                        ProbePrint (L"    create-open: %r\r\n", CrStatus);
                        if (!EFI_ERROR (CrStatus)) {
                            CHAR8      Content[] = "HELLO_FROM_A_BRAND_NEW_FILE!!";
                            UINTN      Size = sizeof (Content) - 1;
                            EFI_STATUS St = WFile->Write (WFile, &Size, Content);
                            ProbePrint (L"    write: %r bytes=%d\r\n", St, (UINT32)Size);
                            WFile->Close (WFile);
                        }

                        /* reopen fresh (no CREATE flag - must already exist) */
                        CrStatus = CsDir->Open (CsDir, &WFile, L"created_by_probe17.txt",
                            EFI_FILE_MODE_READ, 0);
                        ProbePrint (L"    reopen: %r\r\n", CrStatus);
                        if (!EFI_ERROR (CrStatus)) {
                            CHAR8  Buf[64];
                            CHAR16 BufW[64];
                            UINTN  Size = sizeof (Buf) - 1;
                            UINTN  ci;
                            EFI_STATUS St = WFile->Read (WFile, &Size, Buf);
                            for (ci = 0; ci < Size && ci < 63; ci++) {
                                CHAR8 c = Buf[ci];
                                BufW[ci] = (c >= 0x20 && c < 0x7f) ? (CHAR16)c : L'.';
                            }
                            BufW[ci] = L'\0';
                            ProbePrint (L"    readback: %r bytes=%d content='%s'\r\n", St, (UINT32)Size, BufW);
                            WFile->Close (WFile);
                        }

                        /* create-if-exists must NOT fail and must NOT duplicate */
                        CrStatus = CsDir->Open (CsDir, &WFile, L"created_by_probe17.txt",
                            EFI_FILE_MODE_READ | EFI_FILE_MODE_CREATE, 0);
                        ProbePrint (L"    create-on-existing: %r (expect Success, same file)\r\n", CrStatus);
                        if (!EFI_ERROR (CrStatus)) WFile->Close (WFile);

                        {
                            EFI_FILE_PROTOCOL *NewDir;
                            EFI_STATUS MkStatus = CsDir->Open (CsDir, &NewDir, L"mkdir_probe5",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                EFI_FILE_DIRECTORY);
                            ProbePrint (L"    mkdir-open: %r\r\n", MkStatus);
                            if (!EFI_ERROR (MkStatus)) {
                                EFI_FILE_PROTOCOL *Nested;
                                EFI_STATUS NestedStatus = NewDir->Open (NewDir, &Nested, L"nested.txt",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                                ProbePrint (L"    nested-create: %r\r\n", NestedStatus);
                                if (!EFI_ERROR (NestedStatus)) {
                                    CHAR8      Content[] = "HELLO_FROM_NESTED_FILE";
                                    UINTN      Size = sizeof (Content) - 1;
                                    EFI_STATUS St = Nested->Write (Nested, &Size, Content);
                                    ProbePrint (L"    nested-write: %r bytes=%d\r\n", St, (UINT32)Size);
                                    Nested->Close (Nested);
                                }

                                NestedStatus = NewDir->Open (NewDir, &Nested, L"nested.txt",
                                    EFI_FILE_MODE_READ, 0);
                                ProbePrint (L"    nested-reopen: %r\r\n", NestedStatus);
                                if (!EFI_ERROR (NestedStatus)) {
                                    CHAR8  Buf[64];
                                    CHAR16 BufW[64];
                                    UINTN  Size = sizeof (Buf) - 1;
                                    UINTN  ci;
                                    EFI_STATUS St = Nested->Read (Nested, &Size, Buf);
                                    for (ci = 0; ci < Size && ci < 63; ci++) {
                                        CHAR8 c = Buf[ci];
                                        BufW[ci] = (c >= 0x20 && c < 0x7f) ? (CHAR16)c : L'.';
                                    }
                                    BufW[ci] = L'\0';
                                    ProbePrint (L"    nested-readback: %r bytes=%d content='%s'\r\n",
                                        St, (UINT32)Size, BufW);
                                    Nested->Close (Nested);
                                }

                                NewDir->Close (NewDir);
                            }
                        }

                        {
                            EFI_STATUS LargeStatus = CsDir->Open (CsDir, &WFile, L"large_probe5.bin",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                            ProbePrint (L"    large-create: %r\r\n", LargeStatus);
                            if (!EFI_ERROR (LargeStatus)) {
                                CHAR8      Chunk[4096];
                                UINTN      Block;
                                UINT32     WriteChecksum = 0;
                                UINT32     ReadChecksum = 0;
                                UINT64     Total = 0;
                                EFI_STATUS St = EFI_SUCCESS;

                                for (Block = 0; Block < 512 && !EFI_ERROR (St); Block++) {
                                    UINTN i;
                                    UINTN Size = sizeof (Chunk);
                                    for (i = 0; i < sizeof (Chunk); i++) {
                                        Chunk[i] = (CHAR8)((Block * 37 + i * 13 + (i >> 3)) & 0xff);
                                        WriteChecksum = (WriteChecksum * 131) + (UINT8)Chunk[i];
                                    }
                                    St = WFile->Write (WFile, &Size, Chunk);
                                    if (EFI_ERROR (St) || Size != sizeof (Chunk)) break;
                                }
                                ProbePrint (L"    large-write: %r blocks=%d checksum=%08x\r\n",
                                    St, (UINT32)Block, WriteChecksum);
                                WFile->Close (WFile);

                                LargeStatus = CsDir->Open (CsDir, &WFile, L"large_probe5.bin",
                                    EFI_FILE_MODE_READ, 0);
                                ProbePrint (L"    large-reopen: %r\r\n", LargeStatus);
                                if (!EFI_ERROR (LargeStatus)) {
                                    for (;;) {
                                        UINTN Size = sizeof (Chunk);
                                        UINTN i;
                                        St = WFile->Read (WFile, &Size, Chunk);
                                        if (EFI_ERROR (St) || Size == 0) break;
                                        for (i = 0; i < Size; i++) {
                                            ReadChecksum = (ReadChecksum * 131) + (UINT8)Chunk[i];
                                        }
                                        Total += Size;
                                    }
                                    ProbePrint (L"    large-readback: %r bytes=%ld checksum=%08x\r\n",
                                        St, Total, ReadChecksum);
                                    WFile->Close (WFile);
                                }
                            }
                        }

                        {
                            EFI_FILE_PROTOCOL *FanDir;
                            EFI_STATUS FanStatus = CsDir->Open (CsDir, &FanDir, L"fanout_probe5",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                EFI_FILE_DIRECTORY);
                            ProbePrint (L"    fanout-mkdir: %r\r\n", FanStatus);
                            if (!EFI_ERROR (FanStatus)) {
                                UINTN i;
                                for (i = 0; i < 18; i++) {
                                    EFI_FILE_PROTOCOL *Leaf;
                                    CHAR16 Name[96];
                                    EFI_STATUS LeafStatus;
                                    UnicodeSPrint (Name, sizeof (Name),
                                        L"entry_%02d_long_name_to_force_index_allocation_%02d.bin",
                                        (UINT32)i, (UINT32)i);
                                    LeafStatus = FanDir->Open (FanDir, &Leaf, Name,
                                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                                    ProbePrint (L"    fanout-create[%d]: %r\r\n", (UINT32)i, LeafStatus);
                                    if (!EFI_ERROR (LeafStatus)) {
                                        CHAR8 Byte = (CHAR8)(0x30 + (i % 10));
                                        UINTN Size = 1;
                                        EFI_STATUS St = Leaf->Write (Leaf, &Size, &Byte);
                                        ProbePrint (L"    fanout-write[%d]: %r bytes=%d\r\n",
                                            (UINT32)i, St, (UINT32)Size);
                                        Leaf->Close (Leaf);
                                    }
                                }

                                {
                                    EFI_FILE_PROTOCOL *Leaf;
                                    EFI_STATUS LeafStatus = FanDir->Open (FanDir, &Leaf,
                                        L"entry_17_long_name_to_force_index_allocation_17.bin",
                                        EFI_FILE_MODE_READ, 0);
                                    ProbePrint (L"    fanout-last-reopen: %r\r\n", LeafStatus);
                                    if (!EFI_ERROR (LeafStatus)) {
                                        CHAR8 Byte = 0;
                                        UINTN Size = 1;
                                        EFI_STATUS St = Leaf->Read (Leaf, &Size, &Byte);
                                        ProbePrint (L"    fanout-last-read: %r bytes=%d value=%02x\r\n",
                                            St, (UINT32)Size, (UINT32)(UINT8)Byte);
                                        Leaf->Close (Leaf);
                                    }
                                }
                                FanDir->Close (FanDir);
                            }
                        }

                        /* --- delete test --- */
                        {
                            EFI_FILE_PROTOCOL *DF;
                            EFI_STATUS St;

                            /* (a) create small file, delete it, prove it's gone */
                            St = CsDir->Open (CsDir, &DF, L"del_probe1.txt",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                            ProbePrint (L"    del-create: %r\r\n", St);
                            if (!EFI_ERROR (St)) {
                                CHAR8 Content[] = "DELETE_ME";
                                UINTN Size = sizeof (Content) - 1;
                                DF->Write (DF, &Size, Content);
                                DF->Close (DF);
                            }
                            St = CsDir->Open (CsDir, &DF, L"del_probe1.txt",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                            if (!EFI_ERROR (St)) {
                                St = DF->Delete (DF);   /* frees DF */
                                ProbePrint (L"    del-delete: %r (expect Success)\r\n", St);
                            } else {
                                ProbePrint (L"    del-reopen-for-delete: %r\r\n", St);
                            }
                            St = CsDir->Open (CsDir, &DF, L"del_probe1.txt", EFI_FILE_MODE_READ, 0);
                            ProbePrint (L"    del-reopen-after: %r (expect Not Found)\r\n", St);
                            if (!EFI_ERROR (St)) DF->Close (DF);

                            /* (b) empty directory: create then delete */
                            St = CsDir->Open (CsDir, &DF, L"deldir_probe1",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                EFI_FILE_DIRECTORY);
                            ProbePrint (L"    del-mkdir: %r\r\n", St);
                            if (!EFI_ERROR (St)) DF->Close (DF);
                            St = CsDir->Open (CsDir, &DF, L"deldir_probe1",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, EFI_FILE_DIRECTORY);
                            if (!EFI_ERROR (St)) {
                                St = DF->Delete (DF);
                                ProbePrint (L"    del-rmdir-empty: %r (expect Success)\r\n", St);
                            }
                            St = CsDir->Open (CsDir, &DF, L"deldir_probe1", EFI_FILE_MODE_READ, 0);
                            ProbePrint (L"    del-rmdir-reopen: %r (expect Not Found)\r\n", St);
                            if (!EFI_ERROR (St)) DF->Close (DF);

                            /* (c) non-empty directory must be refused */
                            St = CsDir->Open (CsDir, &DF, L"deldir_probe2",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                EFI_FILE_DIRECTORY);
                            if (!EFI_ERROR (St)) {
                                EFI_FILE_PROTOCOL *Inner;
                                EFI_STATUS Si = DF->Open (DF, &Inner, L"keep.txt",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                                if (!EFI_ERROR (Si)) Inner->Close (Inner);
                                DF->Close (DF);
                            }
                            St = CsDir->Open (CsDir, &DF, L"deldir_probe2",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, EFI_FILE_DIRECTORY);
                            if (!EFI_ERROR (St)) {
                                St = DF->Delete (DF);   /* frees DF even on failure */
                                ProbePrint (L"    del-rmdir-nonempty: %r (expect Warning Delete Failure)\r\n", St);
                            }

                            /* (d) delete from split directory: should succeed if leaf, or return EFI_UNSUPPORTED if separator, but never crash */
                            {
                                EFI_FILE_PROTOCOL *SplitDir;
                                St = WRoot->Open (WRoot, &SplitDir, L"\\copied\\many_split", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                                if (!EFI_ERROR (St)) {
                                    EFI_FILE_PROTOCOL *DelFile;
                                    St = SplitDir->Open (SplitDir, &DelFile, L"synth_entry_20_reasonably_long_filename.txt",
                                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                                    if (!EFI_ERROR (St)) {
                                        St = DelFile->Delete (DelFile); /* frees DelFile */
                                        ProbePrint (L"    del-split-file-20: %r (Success or Unsupported)\r\n", St);
                                    } else {
                                        ProbePrint (L"    open-split-file-20: %r\r\n", St);
                                    }

                                    /* try another one that is likely a leaf */
                                    St = SplitDir->Open (SplitDir, &DelFile, L"synth_entry_00_reasonably_long_filename.txt",
                                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                                    if (!EFI_ERROR (St)) {
                                        St = DelFile->Delete (DelFile); /* frees DelFile */
                                        ProbePrint (L"    del-split-file-00: %r (Success or Unsupported)\r\n", St);
                                    } else {
                                        ProbePrint (L"    open-split-file-00: %r\r\n", St);
                                    }
                                    SplitDir->Close (SplitDir);
                                } else {
                                    ProbePrint (L"    open-many-split-dir: %r\r\n", St);
                                }
                            }

                            /* (e) rename test: create -> SetInfo(new name) -> verify */
                            {
                                EFI_FILE_PROTOCOL *RF;
                                St = CsDir->Open (CsDir, &RF, L"rename_src.txt",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                                if (!EFI_ERROR (St)) {
                                    CHAR8 Body[] = "RENAME_PAYLOAD_1234567890";
                                    UINTN Sz = sizeof (Body) - 1;
                                    RF->Write (RF, &Sz, Body);
                                    RF->Close (RF);
                                }
                                St = CsDir->Open (CsDir, &RF, L"rename_src.txt",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                                if (!EFI_ERROR (St)) {
                                    UINT8 IBuf[512];
                                    UINTN ISize = sizeof (IBuf);
                                    EFI_FILE_INFO *Fi = (EFI_FILE_INFO *)IBuf;
                                    St = RF->GetInfo (RF, &gEfiFileInfoGuid, &ISize, IBuf);
                                    if (!EFI_ERROR (St)) {
                                        CHAR16 *NewNm = L"rename_dst.txt";
                                        UINTN   Nl = 0; while (NewNm[Nl]) Nl++;
                                        CopyMem (Fi->FileName, NewNm, (Nl + 1) * sizeof (CHAR16));
                                        Fi->Size = SIZE_OF_EFI_FILE_INFO + (Nl + 1) * sizeof (CHAR16);
                                        St = RF->SetInfo (RF, &gEfiFileInfoGuid, (UINTN)Fi->Size, Fi);
                                        ProbePrint (L"    rename-setinfo: %r\r\n", St);
                                    }
                                    RF->Close (RF);
                                }
                                /* old name gone? */
                                St = CsDir->Open (CsDir, &RF, L"rename_src.txt", EFI_FILE_MODE_READ, 0);
                                ProbePrint (L"    rename-old-gone: %r (expect Not Found)\r\n", St);
                                if (!EFI_ERROR (St)) RF->Close (RF);
                                /* new name present + content intact? */
                                St = CsDir->Open (CsDir, &RF, L"rename_dst.txt", EFI_FILE_MODE_READ, 0);
                                if (!EFI_ERROR (St)) {
                                    CHAR8 Rb[64]; CHAR16 Rw[64]; UINTN Rs = sizeof (Rb) - 1; UINTN ci;
                                    EFI_STATUS Rr = RF->Read (RF, &Rs, Rb);
                                    for (ci = 0; ci < Rs && ci < 63; ci++) Rw[ci] = (Rb[ci] >= 0x20 && Rb[ci] < 0x7f) ? (CHAR16)Rb[ci] : L'.';
                                    Rw[ci] = L'\0';
                                    ProbePrint (L"    rename-new-read: %r bytes=%d '%s'\r\n", Rr, (UINT32)Rs, Rw);
                                    RF->Close (RF);
                                } else {
                                    ProbePrint (L"    rename-new-open: %r (expect Success)\r\n", St);
                                }
                            }

                            /* (f) cross-directory move test:
                             *   - file: cs_dir\move_src.txt -> \move_dst.txt (to root)
                             *   - dir : cs_dir\movedir\ (with a child) -> \movedir
                             *   - reject moving movedir into itself (cycle) */
                            {
                                EFI_FILE_PROTOCOL *MF, *MD, *MC;
                                /* file source in cs_dir */
                                St = CsDir->Open (CsDir, &MF, L"move_src.txt",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                                if (!EFI_ERROR (St)) {
                                    CHAR8 B[] = "MOVE_PAYLOAD_XDIR_42"; UINTN Sz = sizeof (B) - 1;
                                    MF->Write (MF, &Sz, B);
                                    /* move to volume root via absolute path */
                                    UINT8 IBuf[512]; UINTN ISize = sizeof (IBuf);
                                    EFI_FILE_INFO *Fi = (EFI_FILE_INFO *)IBuf;
                                    if (!EFI_ERROR (MF->GetInfo (MF, &gEfiFileInfoGuid, &ISize, IBuf))) {
                                        CHAR16 *Nm = L"\\move_dst.txt"; UINTN Nl = 0; while (Nm[Nl]) Nl++;
                                        CopyMem (Fi->FileName, Nm, (Nl + 1) * sizeof (CHAR16));
                                        Fi->Size = SIZE_OF_EFI_FILE_INFO + (Nl + 1) * sizeof (CHAR16);
                                        St = MF->SetInfo (MF, &gEfiFileInfoGuid, (UINTN)Fi->Size, Fi);
                                        ProbePrint (L"    move-file-setinfo: %r\r\n", St);
                                    }
                                    MF->Close (MF);
                                }
                                St = CsDir->Open (CsDir, &MF, L"move_src.txt", EFI_FILE_MODE_READ, 0);
                                ProbePrint (L"    move-file-old-gone: %r (expect Not Found)\r\n", St);
                                if (!EFI_ERROR (St)) MF->Close (MF);
                                St = WRoot->Open (WRoot, &MF, L"move_dst.txt", EFI_FILE_MODE_READ, 0);
                                ProbePrint (L"    move-file-new-open: %r (expect Success)\r\n", St);
                                if (!EFI_ERROR (St)) MF->Close (MF);

                                /* directory with a child, moved to root */
                                St = CsDir->Open (CsDir, &MD, L"movedir",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                    EFI_FILE_DIRECTORY);
                                if (!EFI_ERROR (St)) {
                                    St = MD->Open (MD, &MC, L"inside.txt",
                                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                                    if (!EFI_ERROR (St)) {
                                        CHAR8 B[] = "CHILD_STAYS"; UINTN Sz = sizeof (B) - 1;
                                        MC->Write (MC, &Sz, B); MC->Close (MC);
                                    }
                                    /* illegal: move movedir INTO itself -> expect Access Denied */
                                    {
                                        UINT8 IBuf[512]; UINTN ISize = sizeof (IBuf);
                                        EFI_FILE_INFO *Fi = (EFI_FILE_INFO *)IBuf;
                                        if (!EFI_ERROR (MD->GetInfo (MD, &gEfiFileInfoGuid, &ISize, IBuf))) {
                                            CHAR16 *Nm = L"\\cs_dir\\movedir\\loop"; UINTN Nl = 0; while (Nm[Nl]) Nl++;
                                            CopyMem (Fi->FileName, Nm, (Nl + 1) * sizeof (CHAR16));
                                            Fi->Size = SIZE_OF_EFI_FILE_INFO + (Nl + 1) * sizeof (CHAR16);
                                            St = MD->SetInfo (MD, &gEfiFileInfoGuid, (UINTN)Fi->Size, Fi);
                                            ProbePrint (L"    move-dir-cycle-reject: %r (expect Access Denied)\r\n", St);
                                        }
                                        /* legal: move movedir to root */
                                        if (!EFI_ERROR (MD->GetInfo (MD, &gEfiFileInfoGuid, &ISize, IBuf))) {
                                            CHAR16 *Nm = L"\\movedir"; UINTN Nl = 0; while (Nm[Nl]) Nl++;
                                            CopyMem (Fi->FileName, Nm, (Nl + 1) * sizeof (CHAR16));
                                            Fi->Size = SIZE_OF_EFI_FILE_INFO + (Nl + 1) * sizeof (CHAR16);
                                            St = MD->SetInfo (MD, &gEfiFileInfoGuid, (UINTN)Fi->Size, Fi);
                                            ProbePrint (L"    move-dir-setinfo: %r\r\n", St);
                                        }
                                    }
                                    MD->Close (MD);
                                    /* child still reachable under the new location? */
                                    St = WRoot->Open (WRoot, &MD, L"movedir", EFI_FILE_MODE_READ, 0);
                                    if (!EFI_ERROR (St)) {
                                        St = MD->Open (MD, &MC, L"inside.txt", EFI_FILE_MODE_READ, 0);
                                        ProbePrint (L"    move-dir-child-reachable: %r (expect Success)\r\n", St);
                                        if (!EFI_ERROR (St)) MC->Close (MC);
                                        MD->Close (MD);
                                    } else {
                                        ProbePrint (L"    move-dir-new-open: %r (expect Success)\r\n", St);
                                    }
                                }
                            }

                            /* (g) truncate test via SetInfo(smaller FileSize):
                             *   - resident: 40 bytes -> 10 bytes
                             *   - non-resident: ~200 KB -> 100000 bytes
                             * verify new size + surviving-prefix content, and
                             * that a GROW request is refused. */
                            {
                                EFI_FILE_PROTOCOL *TF;
                                UINT8 IBuf[512]; UINTN ISize; EFI_FILE_INFO *Fi = (EFI_FILE_INFO *)IBuf;

                                /* resident shrink */
                                St = CsDir->Open (CsDir, &TF, L"trunc_r.txt",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                                if (!EFI_ERROR (St)) {
                                    CHAR8 B[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!!!!"; UINTN Sz = 40;
                                    TF->Write (TF, &Sz, B);
                                    ISize = sizeof (IBuf);
                                    if (!EFI_ERROR (TF->GetInfo (TF, &gEfiFileInfoGuid, &ISize, IBuf))) {
                                        Fi->FileSize = 10;
                                        St = TF->SetInfo (TF, &gEfiFileInfoGuid, (UINTN)ISize, Fi);
                                        ProbePrint (L"    trunc-res-setinfo: %r\r\n", St);
                                    }
                                    TF->Close (TF);
                                    St = CsDir->Open (CsDir, &TF, L"trunc_r.txt", EFI_FILE_MODE_READ, 0);
                                    if (!EFI_ERROR (St)) {
                                        CHAR8 Rb[64]; CHAR16 Rw[64]; UINTN Rs = sizeof (Rb) - 1, ci;
                                        TF->Read (TF, &Rs, Rb);
                                        for (ci = 0; ci < Rs && ci < 63; ci++) Rw[ci] = (Rb[ci] >= 0x20 && Rb[ci] < 0x7f) ? (CHAR16)Rb[ci] : L'.';
                                        Rw[ci] = L'\0';
                                        ProbePrint (L"    trunc-res-read: bytes=%d '%s' (expect 10 '0123456789')\r\n", (UINT32)Rs, Rw);
                                        TF->Close (TF);
                                    }
                                }

                                /* non-resident shrink */
                                St = CsDir->Open (CsDir, &TF, L"trunc_nr.bin",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                                if (!EFI_ERROR (St)) {
                                    static CHAR8 Blk[4096]; UINTN i, Sz;
                                    for (i = 0; i < sizeof (Blk); i++) Blk[i] = (CHAR8)('A' + (i & 15));
                                    for (i = 0; i < 50; i++) { Sz = sizeof (Blk); TF->Write (TF, &Sz, Blk); }  /* ~200 KB */
                                    ISize = sizeof (IBuf);
                                    if (!EFI_ERROR (TF->GetInfo (TF, &gEfiFileInfoGuid, &ISize, IBuf))) {
                                        ProbePrint (L"    trunc-nr-before: size=%ld\r\n", (UINT64)Fi->FileSize);
                                        Fi->FileSize = 100000;
                                        St = TF->SetInfo (TF, &gEfiFileInfoGuid, (UINTN)ISize, Fi);
                                        ProbePrint (L"    trunc-nr-setinfo: %r\r\n", St);
                                        /* grow back up to 300000: zero-fill new range */
                                        ISize = sizeof (IBuf); TF->GetInfo (TF, &gEfiFileInfoGuid, &ISize, IBuf);
                                        Fi->FileSize = 300000;
                                        St = TF->SetInfo (TF, &gEfiFileInfoGuid, (UINTN)ISize, Fi);
                                        ProbePrint (L"    trunc-nr-grow: %r (expect Success)\r\n", St);
                                    }
                                    TF->Close (TF);
                                    St = CsDir->Open (CsDir, &TF, L"trunc_nr.bin", EFI_FILE_MODE_READ, 0);
                                    if (!EFI_ERROR (St)) {
                                        CHAR8 Tail[8]; UINTN Ts = 8;
                                        ISize = sizeof (IBuf);
                                        TF->GetInfo (TF, &gEfiFileInfoGuid, &ISize, IBuf);
                                        ProbePrint (L"    trunc-nr-after: size=%ld (expect 300000)\r\n", (UINT64)Fi->FileSize);
                                        TF->SetPosition (TF, 250000);   /* inside the grown (zero) range */
                                        TF->Read (TF, &Ts, Tail);
                                        ProbePrint (L"    trunc-nr-grow-zero: b0=%d b7=%d (expect 0 0)\r\n", (UINT32)Tail[0], (UINT32)Tail[7]);
                                        TF->Close (TF);
                                    }
                                }

                                /* non-resident shrink is guarded off (frees wrong
                                 * ranges - see NtfsShrinkNonResidentRuns); SetInfo
                                 * with a smaller FileSize on a multi-run file must be
                                 * REFUSED (Unsupported) and leave the file intact. */
                                St = CsDir->Open (CsDir, &TF, L"bigtrunc.bin",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                                if (!EFI_ERROR (St)) {
                                    static CHAR8 Blk[65536]; UINTN i, Sz;
                                    for (i = 0; i < sizeof (Blk); i++) Blk[i] = (CHAR8)('a' + (i & 7));
                                    for (i = 0; i < 24; i++) { Sz = sizeof (Blk); TF->Write (TF, &Sz, Blk); }  /* 1.5 MB */
                                    ISize = sizeof (IBuf);
                                    if (!EFI_ERROR (TF->GetInfo (TF, &gEfiFileInfoGuid, &ISize, IBuf))) {
                                        Fi->FileSize = 600000;
                                        St = TF->SetInfo (TF, &gEfiFileInfoGuid, (UINTN)Fi->Size, Fi);
                                        ProbePrint (L"    bigtrunc-nr-shrink: %r\r\n", St);
                                    }
                                    TF->Close (TF);
                                }

                                /* grow a small resident file past a cluster:
                                 * forces resident->non-resident conversion + zero-fill,
                                 * original prefix must survive */
                                St = CsDir->Open (CsDir, &TF, L"grow_r.txt",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                                if (!EFI_ERROR (St)) {
                                    CHAR8 B[] = "PREFIX10!!"; UINTN Sz = 10;
                                    TF->Write (TF, &Sz, B);
                                    ISize = sizeof (IBuf);
                                    if (!EFI_ERROR (TF->GetInfo (TF, &gEfiFileInfoGuid, &ISize, IBuf))) {
                                        Fi->FileSize = 5000;
                                        St = TF->SetInfo (TF, &gEfiFileInfoGuid, (UINTN)ISize, Fi);
                                        ProbePrint (L"    grow-res-setinfo: %r (expect Success)\r\n", St);
                                    }
                                    TF->Close (TF);
                                    St = CsDir->Open (CsDir, &TF, L"grow_r.txt", EFI_FILE_MODE_READ, 0);
                                    if (!EFI_ERROR (St)) {
                                        CHAR8 Rb[16]; CHAR16 Rw[16]; UINTN Rs = 10, ci;
                                        ISize = sizeof (IBuf); TF->GetInfo (TF, &gEfiFileInfoGuid, &ISize, IBuf);
                                        TF->Read (TF, &Rs, Rb);
                                        for (ci = 0; ci < Rs && ci < 15; ci++) Rw[ci] = (Rb[ci] >= 0x20 && Rb[ci] < 0x7f) ? (CHAR16)Rb[ci] : L'.';
                                        Rw[ci] = L'\0';
                                        ProbePrint (L"    grow-res-after: size=%ld prefix='%s' (expect 5000 'PREFIX10!!')\r\n", (UINT64)Fi->FileSize, Rw);
                                        TF->Close (TF);
                                    }
                                }
                            }
                        }

                        CsDir->Close (CsDir);
                    }
                }

                /* ROOT-DIR STRESS: create many directories in the volume ROOT
                 * with long names, reproducing the big-test MKDIR failures on a
                 * small synthetic volume where the driver DebugLog stays usable.
                 * Report the first failure's name + status. */
                {
                    UINTN rd, mkOk = 0, mkFail = 0;
                    EFI_STATUS St;
                    for (rd = 0; rd < 40; rd++) {
                        EFI_FILE_PROTOCOL *RDir;
                        CHAR16 nm[80];
                        UINTN k = 0; CONST CHAR16 *pfx = L"stress_directory_with_a_long_name_";
                        while (pfx[k]) { nm[k] = pfx[k]; k++; }
                        nm[k++] = (CHAR16)(L'0' + (rd / 10));
                        nm[k++] = (CHAR16)(L'0' + (rd % 10));
                        nm[k] = 0;
                        St = WRoot->Open (WRoot, &RDir, nm,
                            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                            EFI_FILE_DIRECTORY);
                        if (EFI_ERROR (St)) {
                            mkFail++;
                            if (mkFail <= 3) ProbePrint (L"    rootstress MKDIR #%d FAIL: %r\r\n", (UINT32)rd, St);
                        } else { mkOk++; RDir->Close (RDir); }
                    }
                    ProbePrint (L"    rootstress: ok=%d fail=%d\r\n", (UINT32)mkOk, (UINT32)mkFail);
                }

                /* Clean unmount: flush and clear the $Volume dirty flag via
                 * the driver's real unmount path, so Windows mounts the
                 * result WITHOUT a "there's a problem with this drive - scan
                 * it" prompt. SFSP is installed on the disk controller
                 * handle, so disconnecting that handle drives
                 * NtfsEfiBindingStop -> NtfsEfiUnmountVolume (clears dirty). */
                WRoot->Flush (WRoot);
                WRoot->Close (WRoot);
                {
                    EFI_STATUS DcSt = gBS->DisconnectController (WHandles[wh], NULL, NULL);
                    ProbePrint (L"    clean-unmount (disconnect): %r\r\n", DcSt);
                }
            }
            gBS->FreePool (WHandles);
        }
    }

    ProbePrint (L"==NTFS-PROBE-END==\r\n");
    if (Handles != NULL) gBS->FreePool (Handles);

    return EFI_SUCCESS;
}
