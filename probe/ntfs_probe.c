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

/* When gEspRoot is set (main opens the ESP FAT volume the probe booted from),
 * every ProbePrint line is also appended to \_PROBE_TRACE.txt as ASCII - a full
 * boot trace for Hyper-V, where ConOut is video-only with no capturable serial.
 * Each line is open+append+CLOSE'd: firmware FAT only commits a file on Close,
 * and the probe ends in a hard ResetSystem, so a kept-open handle would be lost. */
static EFI_FILE_PROTOCOL *gEspRoot  = NULL;
static UINT64             gTraceOff = 0;

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

    if (gEspRoot != NULL) {
        EFI_FILE_PROTOCOL *TF = NULL;
        if (!EFI_ERROR (gEspRoot->Open (gEspRoot, &TF, L"\\_PROBE_TRACE.txt",
                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
            CHAR8 A[512]; UINTN n = 0;
            while (Buf[n] != 0 && n < sizeof (A) - 1) { A[n] = (CHAR8)Buf[n]; n++; }
            TF->SetPosition (TF, gTraceOff);
            TF->Write (TF, &n, A);
            TF->Close (TF);        /* commit on FAT */
            gTraceOff += n;
        }
    }
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

/* ---- non-destructive fragmentation battery (real Windows volume) ----------
 * Touches ONLY files/dirs it creates under \_EFITEST\ ; existing user data is
 * never written. It checksums \Windows\System32\notepad.exe before AND after
 * the workload to prove the write+delete traffic left real data byte-identical.
 * Exercises the $Bitmap allocator + delete durability under deliberate
 * fragmentation (interleaved multi-file writes, punch holes, refill holes). */
#define FB_NFILES  16
#define FB_FILESZ  (512u * 1024u)   /* 512 KiB each -> multi-cluster runs */
#define FB_CHUNK   (64u * 1024u)

static UINT8 FbByte (UINTN Idx, UINT64 Pos) { return (UINT8)(Idx * 31u + (UINT32)Pos); }

/* Read an existing file fully; return its size + an additive 32-bit checksum. */
static EFI_STATUS
FbChecksum (
    IN  EFI_FILE_PROTOCOL *Root,
    IN  CONST CHAR16      *Path,
    OUT UINT64            *OutSize,
    OUT UINT32            *OutSum
    )
{
    EFI_FILE_PROTOCOL *F = NULL;
    VOID              *Buf;
    UINT32             Sum = 0;
    UINT64             Tot = 0;

    *OutSize = 0; *OutSum = 0;
    if (EFI_ERROR (Root->Open (Root, &F, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0)))
        return EFI_NOT_FOUND;
    if (EFI_ERROR (gBS->AllocatePool (EfiBootServicesData, FB_CHUNK, &Buf))) {
        F->Close (F); return EFI_OUT_OF_RESOURCES;
    }
    for (;;) {
        UINTN n = FB_CHUNK, i;
        if (EFI_ERROR (F->Read (F, &n, Buf)) || n == 0) break;
        for (i = 0; i < n; i++) Sum += ((UINT8 *)Buf)[i];
        Tot += n;
    }
    gBS->FreePool (Buf);
    F->Close (F);
    *OutSize = Tot; *OutSum = Sum;
    return EFI_SUCCESS;
}

/* Verify one file in Dir holds exactly Len bytes of pattern FbByte(Seed,pos). */
static BOOLEAN
FbVerify (
    IN EFI_FILE_PROTOCOL *Dir,
    IN CONST CHAR16      *Name,
    IN UINTN              Seed,
    IN UINT64             Len,
    IN VOID              *Buf
    )
{
    EFI_FILE_PROTOCOL *F = NULL;
    UINT64             Pos = 0;
    BOOLEAN            Ok = TRUE;

    if (EFI_ERROR (Dir->Open (Dir, &F, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0)))
        return FALSE;
    for (;;) {
        UINTN n = FB_CHUNK, k;
        if (EFI_ERROR (F->Read (F, &n, Buf)) || n == 0) break;
        for (k = 0; k < n; k++) {
            if (((UINT8 *)Buf)[k] != FbByte (Seed, Pos + k)) { Ok = FALSE; break; }
        }
        Pos += n;
        if (!Ok) break;
    }
    F->Close (F);
    return (Ok && Pos == Len);
}

/* Find an existing regular file to use as a read-only baseline: scan Root, then
 * one level into the first real subdirectory. Writes "\...\name" into Out.
 * Returns TRUE if one was found. */
static BOOLEAN
FbFindBaseline (
    IN  EFI_FILE_PROTOCOL *Root,
    OUT CHAR16            *Out,
    IN  UINTN              OutChars
    )
{
    UINT8  Ib[512];
    UINTN  Is;
    EFI_FILE_INFO *Fi = (EFI_FILE_INFO *)Ib;

    Root->SetPosition (Root, 0);
    for (;;) {
        Is = sizeof (Ib);
        if (EFI_ERROR (Root->Read (Root, &Is, Ib)) || Is == 0) break;
        if (!(Fi->Attribute & EFI_FILE_DIRECTORY) && Fi->FileSize > 0) {
            UnicodeSPrint (Out, OutChars * sizeof (CHAR16), L"\\%s", Fi->FileName);
            return TRUE;
        }
    }
    /* no regular file at root - descend into the first non-dotted subdir */
    Root->SetPosition (Root, 0);
    for (;;) {
        EFI_FILE_PROTOCOL *Sub = NULL;
        Is = sizeof (Ib);
        if (EFI_ERROR (Root->Read (Root, &Is, Ib)) || Is == 0) break;
        if ((Fi->Attribute & EFI_FILE_DIRECTORY) &&
            Fi->FileName[0] != L'.' &&
            Fi->FileName[0] != L'$' &&
            !EFI_ERROR (Root->Open (Root, &Sub, Fi->FileName, EFI_FILE_MODE_READ, 0))) {
            CHAR16 DirName[128];
            UINTN  dn = 0;
            while (Fi->FileName[dn] && dn < 127) { DirName[dn] = Fi->FileName[dn]; dn++; }
            DirName[dn] = 0;
            Sub->SetPosition (Sub, 0);
            for (;;) {
                UINT8  Jb[512]; UINTN Js = sizeof (Jb);
                EFI_FILE_INFO *Ji = (EFI_FILE_INFO *)Jb;
                if (EFI_ERROR (Sub->Read (Sub, &Js, Jb)) || Js == 0) break;
                if (!(Ji->Attribute & EFI_FILE_DIRECTORY) && Ji->FileSize > 0) {
                    UnicodeSPrint (Out, OutChars * sizeof (CHAR16), L"\\%s\\%s", DirName, Ji->FileName);
                    Sub->Close (Sub);
                    return TRUE;
                }
            }
            Sub->Close (Sub);
        }
    }
    return FALSE;
}

/*
 * Recursively delete Name (file or whole directory tree) under Parent, exactly
 * the way an application does it: enumerate, recurse into subdirectories,
 * delete files, then remove the now-empty directory.
 *
 * The DRIVER deliberately never recurses - EFI_FILE_PROTOCOL.Delete on a
 * non-empty directory is refused, same as the firmware's FAT driver - so
 * "delete a folder with everything in it" is the caller's loop (this is what
 * EC's FsDeleteRecursive does). This proves that pattern works end to end.
 */
/* EFI_WARN_DELETE_FAILURE ("handle closed, file NOT deleted") is a WARNING, so
 * plain EFI_ERROR() is FALSE for it and a refused delete would read as success. */
#define FB_DELETE_FAILED(St)  (EFI_ERROR (St) || (St) == EFI_WARN_DELETE_FAILURE)

static EFI_STATUS
FbDeleteTree (
    IN     EFI_FILE_PROTOCOL *Parent,
    IN     CONST CHAR16      *Name,
    IN     UINTN              Depth,
    IN OUT UINT32            *Files,
    IN OUT UINT32            *Dirs,
    IN OUT UINT32            *Fail
    )
{
    EFI_FILE_PROTOCOL *Self = NULL;
    EFI_STATUS         St;
    UINT8              Ib[512];
    UINTN              Is;
    EFI_FILE_INFO     *Fi = (EFI_FILE_INFO *)Ib;
    BOOLEAN            IsDir;

    if (Depth > 16) { (*Fail)++; return EFI_UNSUPPORTED; }

    St = Parent->Open (Parent, &Self, (CHAR16 *)Name,
             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR (St)) { (*Fail)++; return St; }

    {
        EFI_GUID Fig = EFI_FILE_INFO_ID;
        Is = sizeof (Ib);
        if (EFI_ERROR (Self->GetInfo (Self, &Fig, &Is, Ib))) {
            Self->Close (Self); (*Fail)++; return EFI_DEVICE_ERROR;
        }
        IsDir = (BOOLEAN)((Fi->Attribute & EFI_FILE_DIRECTORY) != 0);
    }

    if (!IsDir) {
        St = Self->Delete (Self);          /* frees Self even on failure */
        if (FB_DELETE_FAILED (St)) { (*Fail)++; return EFI_ACCESS_DENIED; }
        (*Files)++;
        return EFI_SUCCESS;
    }

    /* Directory: drain children first. Re-scan from the start after each
     * removal - deleting an entry mutates the very index we are walking. */
    for (;;) {
        CHAR16  Child[260];
        BOOLEAN Found = FALSE;

        Self->SetPosition (Self, 0);
        for (;;) {
            Is = sizeof (Ib);
            if (EFI_ERROR (Self->Read (Self, &Is, Ib)) || Is == 0) break;
            if (Fi->FileName[0] == L'.' &&
                (Fi->FileName[1] == 0 ||
                 (Fi->FileName[1] == L'.' && Fi->FileName[2] == 0))) continue;
            { UINTN k = 0;
              while (Fi->FileName[k] != 0 && k < 259) { Child[k] = Fi->FileName[k]; k++; }
              Child[k] = 0; }
            Found = TRUE;
            break;
        }
        if (!Found) break;
        if (EFI_ERROR (FbDeleteTree (Self, Child, Depth + 1, Files, Dirs, Fail))) break;
    }

    St = Self->Delete (Self);              /* frees Self even on failure */
    if (FB_DELETE_FAILED (St)) { (*Fail)++; return EFI_ACCESS_DENIED; }
    (*Dirs)++;
    return EFI_SUCCESS;
}

/* Run the whole battery on a mounted NTFS root, appending a human log to Log.
 * Returns the log length. Only \_EFITEST\ and its children are ever created. */
static UINTN
FbRun (
    IN EFI_FILE_PROTOCOL *Root,
    OUT CHAR8            *Log,
    IN UINTN              Cap
    )
{
    UINTN              L = 0;
    EFI_STATUS         St;
    EFI_FILE_PROTOCOL *Dir = NULL;
    EFI_FILE_PROTOCOL *H[FB_NFILES];
    VOID              *Buf = NULL;
    UINT64             RefSz = 0; UINT32 RefSum = 0;
    UINT32             Chunks = FB_FILESZ / FB_CHUNK;
    UINTN              i;
    CHAR16             BasePath[288];
    BOOLEAN            HaveBase;

    for (i = 0; i < FB_NFILES; i++) H[i] = NULL;

    /* Phase A: baseline checksum of a real, existing file (read-only) so we can
     * prove the write/delete workload left real data byte-identical. Auto-found
     * so this works on any NTFS volume (Windows C:, Recovery, a fresh test vol). */
    HaveBase = FbFindBaseline (Root, BasePath, sizeof (BasePath) / sizeof (CHAR16));
    if (!HaveBase) { UnicodeSPrint (BasePath, sizeof (BasePath), L"(none)"); }
    St = HaveBase ? FbChecksum (Root, BasePath, &RefSz, &RefSum) : EFI_NOT_FOUND;
    {
        CHAR8 bp[300]; UINTN bi = 0;
        while (BasePath[bi] && bi < 299) { bp[bi] = (CHAR8)BasePath[bi]; bi++; } bp[bi] = 0;
        L += AsciiSPrint (Log + L, Cap - L, "A baseline '%a': st=%r size=%lu sum=%08x\n", bp, St, RefSz, RefSum);
    }

    if (EFI_ERROR (gBS->AllocatePool (EfiBootServicesData, FB_CHUNK, &Buf))) {
        L += AsciiSPrint (Log + L, Cap - L, "FATAL: pool alloc failed\n");
        return L;
    }

    /* Phase B: mkdir \_EFITEST, create 16 files, INTERLEAVE 64 KiB chunk writes
     * across all open handles so the allocator hands out fragmented runs. */
    St = Root->Open (Root, &Dir, L"\\_EFITEST",
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
    L += AsciiSPrint (Log + L, Cap - L, "B mkdir \\_EFITEST: %r\n", St);
    if (EFI_ERROR (St)) { gBS->FreePool (Buf); return L; }

    {
        UINT32 Opened = 0;
        for (i = 0; i < FB_NFILES; i++) {
            CHAR16 Nm[32];
            UnicodeSPrint (Nm, sizeof (Nm), L"f%02u.bin", (UINT32)i);
            if (!EFI_ERROR (Dir->Open (Dir, &H[i], Nm,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) Opened++;
        }
        L += AsciiSPrint (Log + L, Cap - L, "B opened %u/%u handles\n", Opened, (UINT32)FB_NFILES);
    }
    {
        UINT64 Pos = 0; UINT32 c; EFI_STATUS WErr = EFI_SUCCESS;
        for (c = 0; c < Chunks; c++) {
            for (i = 0; i < FB_NFILES; i++) {
                UINTN n = FB_CHUNK, k;
                if (H[i] == NULL) continue;
                for (k = 0; k < FB_CHUNK; k++) ((UINT8 *)Buf)[k] = FbByte (i, Pos + k);
                if (EFI_ERROR (H[i]->Write (H[i], &n, Buf)) || n != FB_CHUNK) WErr = EFI_DEVICE_ERROR;
            }
            Pos += FB_CHUNK;
        }
        for (i = 0; i < FB_NFILES; i++) if (H[i]) H[i]->Flush (H[i]);
        L += AsciiSPrint (Log + L, Cap - L,
            "B interleaved write %u x %u KiB: %r\n", (UINT32)FB_NFILES, FB_FILESZ / 1024, WErr);
    }
    for (i = 0; i < FB_NFILES; i++) if (H[i]) { H[i]->Close (H[i]); H[i] = NULL; }

    /* Phase C: reopen + verify every fragmented file byte-for-byte. */
    {
        UINT32 ok = 0, fail = 0;
        for (i = 0; i < FB_NFILES; i++) {
            CHAR16 Nm[32];
            UnicodeSPrint (Nm, sizeof (Nm), L"f%02u.bin", (UINT32)i);
            if (FbVerify (Dir, Nm, i, FB_FILESZ, Buf)) ok++; else fail++;
        }
        L += AsciiSPrint (Log + L, Cap - L, "C verify pass1: ok=%u fail=%u\n", ok, fail);
    }

    /* Phase D: delete even files to punch holes, then create 8 new files that
     * must reuse the freed (scattered) clusters -> heavier fragmentation. */
    {
        UINT32 del = 0, dfail = 0;
        for (i = 0; i < FB_NFILES; i += 2) {
            CHAR16 Nm[32]; EFI_FILE_PROTOCOL *F = NULL;
            UnicodeSPrint (Nm, sizeof (Nm), L"f%02u.bin", (UINT32)i);
            if (!EFI_ERROR (Dir->Open (Dir, &F, Nm, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
                if (!EFI_ERROR (F->Delete (F))) del++; else dfail++;   /* Delete frees F */
            } else dfail++;
        }
        L += AsciiSPrint (Log + L, Cap - L, "D punched %u holes (fail=%u)\n", del, dfail);
    }
    {
        EFI_FILE_PROTOCOL *G[8]; UINT32 Op = 0; UINT64 Pos = 0; UINT32 c; EFI_STATUS WErr = EFI_SUCCESS;
        for (i = 0; i < 8; i++) G[i] = NULL;
        for (i = 0; i < 8; i++) {
            CHAR16 Nm[32]; UnicodeSPrint (Nm, sizeof (Nm), L"g%02u.bin", (UINT32)i);
            if (!EFI_ERROR (Dir->Open (Dir, &G[i], Nm,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) Op++;
        }
        for (c = 0; c < Chunks; c++) {
            for (i = 0; i < 8; i++) {
                UINTN n = FB_CHUNK, k;
                if (G[i] == NULL) continue;
                for (k = 0; k < FB_CHUNK; k++) ((UINT8 *)Buf)[k] = FbByte (i + 100, Pos + k);
                if (EFI_ERROR (G[i]->Write (G[i], &n, Buf)) || n != FB_CHUNK) WErr = EFI_DEVICE_ERROR;
            }
            Pos += FB_CHUNK;
        }
        for (i = 0; i < 8; i++) if (G[i]) { G[i]->Flush (G[i]); G[i]->Close (G[i]); }
        L += AsciiSPrint (Log + L, Cap - L, "D refilled %u holes: %r\n", Op, WErr);

        {
            UINT32 ok = 0, fail = 0;
            for (i = 0; i < 8; i++) {
                CHAR16 Nm[32]; UnicodeSPrint (Nm, sizeof (Nm), L"g%02u.bin", (UINT32)i);
                if (FbVerify (Dir, Nm, i + 100, FB_FILESZ, Buf)) ok++; else fail++;
            }
            L += AsciiSPrint (Log + L, Cap - L, "D verify holes: ok=%u fail=%u\n", ok, fail);
        }
    }

    /* Phase E: delete EVERYTHING we made (odd f-files + all g-files), rmdir. */
    {
        UINT32 del = 0, dfail = 0;
        for (i = 1; i < FB_NFILES; i += 2) {
            CHAR16 Nm[32]; EFI_FILE_PROTOCOL *F = NULL;
            UnicodeSPrint (Nm, sizeof (Nm), L"f%02u.bin", (UINT32)i);
            if (!EFI_ERROR (Dir->Open (Dir, &F, Nm, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
                if (!EFI_ERROR (F->Delete (F))) del++; else dfail++;
            } else dfail++;
        }
        for (i = 0; i < 8; i++) {
            CHAR16 Nm[32]; EFI_FILE_PROTOCOL *F = NULL;
            UnicodeSPrint (Nm, sizeof (Nm), L"g%02u.bin", (UINT32)i);
            if (!EFI_ERROR (Dir->Open (Dir, &F, Nm, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
                if (!EFI_ERROR (F->Delete (F))) del++; else dfail++;
            } else dfail++;
        }
        St = Dir->Delete (Dir);   /* rmdir empty dir; frees Dir */
        Dir = NULL;
        L += AsciiSPrint (Log + L, Cap - L,
            "E deleted %u files (fail=%u), rmdir \\_EFITEST: %r\n", del, dfail, St);
    }
    {
        EFI_FILE_PROTOCOL *F = NULL;
        EFI_STATUS g = Root->Open (Root, &F, L"\\_EFITEST", EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR (g)) { F->Close (F);
            L += AsciiSPrint (Log + L, Cap - L, "E \\_EFITEST STILL PRESENT (BAD)\n"); }
        else
            L += AsciiSPrint (Log + L, Cap - L, "E \\_EFITEST gone: %r (good)\n", g);
    }

    /* Phase G: ROOT-directory stress with LONG names. The root of a grown volume
     * is exactly the directory that keeps its $I30 index in an $ATTRIBUTE_LIST
     * extension record, so this drives B+tree leaf splits, root push-down,
     * separator promotion and $BITMAP:$I30 growth INSIDE that extension record -
     * the paths a single mkdir never reaches. Everything created here is removed
     * again; names are prefixed _EFITG_ so nothing else can collide. */
    {
        UINT32 mk = 0, mkfail = 0, seen = 0, del = 0, dfail = 0;
        UINTN  gi;
        EFI_STATUS LastFail = EFI_SUCCESS;

        for (gi = 0; gi < 48; gi++) {
            CHAR16 Nm[128]; EFI_FILE_PROTOCOL *F = NULL;
            UnicodeSPrint (Nm, sizeof (Nm),
                L"\\_EFITG_%02u_this_is_a_deliberately_long_entry_name_to_force_splits.tmp",
                (UINT32)gi);
            St = Root->Open (Root, &F, Nm,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
            if (!EFI_ERROR (St)) {
                UINTN n = 64;
                SetMem (Buf, 64, (UINT8)gi);
                F->Write (F, &n, Buf);
                F->Close (F);
                mk++;
            } else { mkfail++; LastFail = St; }
        }
        L += AsciiSPrint (Log + L, Cap - L,
            "G root-stress create: ok=%u fail=%u last=%r\n", mk, mkfail, LastFail);

        /* every one must be findable again (proves the index stayed consistent) */
        for (gi = 0; gi < 48; gi++) {
            CHAR16 Nm[128]; EFI_FILE_PROTOCOL *F = NULL;
            UnicodeSPrint (Nm, sizeof (Nm),
                L"\\_EFITG_%02u_this_is_a_deliberately_long_entry_name_to_force_splits.tmp",
                (UINT32)gi);
            if (!EFI_ERROR (Root->Open (Root, &F, Nm, EFI_FILE_MODE_READ, 0))) { seen++; F->Close (F); }
        }
        L += AsciiSPrint (Log + L, Cap - L, "G root-stress reopen: found=%u/%u\n", seen, 48u);

        for (gi = 0; gi < 48; gi++) {
            CHAR16 Nm[128]; EFI_FILE_PROTOCOL *F = NULL;
            UnicodeSPrint (Nm, sizeof (Nm),
                L"\\_EFITG_%02u_this_is_a_deliberately_long_entry_name_to_force_splits.tmp",
                (UINT32)gi);
            if (!EFI_ERROR (Root->Open (Root, &F, Nm, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
                St = F->Delete (F);            /* frees F */
                if (!EFI_ERROR (St)) del++; else { dfail++; LastFail = St; }
            } else dfail++;
        }
        L += AsciiSPrint (Log + L, Cap - L,
            "G root-stress delete: ok=%u fail=%u last=%r\n", del, dfail, LastFail);
    }

    /* Phase H: delete a whole NESTED tree in one go, the way a file manager does
     * (EC's FsDeleteRecursive). Builds \_EFITH with files, a subdirectory and a
     * sub-subdirectory, then removes the top entry recursively and checks that
     * nothing is left behind. */
    {
        EFI_FILE_PROTOCOL *D1 = NULL, *D2 = NULL, *D3 = NULL;
        UINT32 nf = 0, nd = 0, nfail = 0, made = 0;
        UINTN  k;

        St = Root->Open (Root, &D1, L"\\_EFITH",
                 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
        if (!EFI_ERROR (St)) {
            St = D1->Open (D1, &D2, L"sub",
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
        }
        if (!EFI_ERROR (St) && D2 != NULL) {
            St = D2->Open (D2, &D3, L"deeper",
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY);
        }
        if (!EFI_ERROR (St) && D3 != NULL) {
            EFI_FILE_PROTOCOL *Lv[3]; UINTN li;
            Lv[0] = D1; Lv[1] = D2; Lv[2] = D3;
            for (li = 0; li < 3; li++) {
                for (k = 0; k < 3; k++) {
                    CHAR16 Nm[64]; EFI_FILE_PROTOCOL *F = NULL; UINTN n = 128;
                    UnicodeSPrint (Nm, sizeof (Nm), L"tree_%u_%u.dat", (UINT32)li, (UINT32)k);
                    if (!EFI_ERROR (Lv[li]->Open (Lv[li], &F, Nm,
                            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
                        SetMem (Buf, 128, (UINT8)(li * 16 + k));
                        F->Write (F, &n, Buf);
                        F->Close (F);
                        made++;
                    }
                }
            }
        }
        if (D3 != NULL) D3->Close (D3);
        if (D2 != NULL) D2->Close (D2);
        if (D1 != NULL) D1->Close (D1);

        L += AsciiSPrint (Log + L, Cap - L,
            "H tree built: st=%r dirs=3 files=%u\n", St, made);

        /* the driver alone must REFUSE to remove the non-empty top directory -
         * that refusal is what makes the recursive loop necessary */
        {
            EFI_FILE_PROTOCOL *T = NULL;
            EFI_STATUS Direct = EFI_NOT_FOUND;
            if (!EFI_ERROR (Root->Open (Root, &T, L"\\_EFITH",
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
                Direct = T->Delete (T);    /* frees T */
            }
            L += AsciiSPrint (Log + L, Cap - L,
                "H direct rmdir of NON-empty tree: %r %a\n", Direct,
                FB_DELETE_FAILED (Direct) ? "(correctly refused)" : "(BAD - should refuse)");
        }

        St = FbDeleteTree (Root, L"_EFITH", 0, &nf, &nd, &nfail);
        L += AsciiSPrint (Log + L, Cap - L,
            "H recursive delete: st=%r files=%u dirs=%u fail=%u\n", St, nf, nd, nfail);
        {
            EFI_FILE_PROTOCOL *T = NULL;
            EFI_STATUS g = Root->Open (Root, &T, L"\\_EFITH", EFI_FILE_MODE_READ, 0);
            if (!EFI_ERROR (g)) { T->Close (T);
                L += AsciiSPrint (Log + L, Cap - L, "H \\_EFITH STILL PRESENT (BAD)\n"); }
            else
                L += AsciiSPrint (Log + L, Cap - L, "H \\_EFITH gone: %r (good)\n", g);
        }
    }

    /* Phase F: re-checksum the same baseline file; must match Phase A exactly. */
    if (HaveBase) {
        UINT64 Sz2 = 0; UINT32 Sum2 = 0;
        St = FbChecksum (Root, BasePath, &Sz2, &Sum2);
        L += AsciiSPrint (Log + L, Cap - L,
            "F recheck baseline: st=%r size=%lu sum=%08x %a\n",
            St, Sz2, Sum2, (Sz2 == RefSz && Sum2 == RefSum) ? "UNCHANGED-OK" : "CHANGED-BAD");
    } else {
        L += AsciiSPrint (Log + L, Cap - L, "F recheck: no baseline file on this volume (skipped)\n");
    }

    gBS->FreePool (Buf);
    return L;
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

/* Delete every FILE in DirPath one at a time, reopening the directory each pass
 * so enumeration reflects the (rebalanced) tree. Stops at the first file whose
 * delete does not return a clean EFI_SUCCESS and records it. A real-volume test
 * - makes no synthetic-layout assumptions. */
static VOID
ProbeDrainDir (
    IN  EFI_FILE_PROTOCOL *WRoot,
    IN  CONST CHAR16      *DirPath,
    OUT UINT32            *Deleted,
    OUT EFI_STATUS        *FailSt,
    OUT CHAR16            *FailName,   /* >= 260 */
    OUT UINT32            *Remaining
    )
{
    VOID  *Ib = NULL;
    UINT32 iter;

    *Deleted = 0; *FailSt = EFI_SUCCESS; FailName[0] = 0; *Remaining = 0;
    if (EFI_ERROR (gBS->AllocatePool (EfiBootServicesData, 2048, &Ib)) || Ib == NULL) {
        *FailSt = EFI_OUT_OF_RESOURCES; return;
    }

    for (iter = 0; iter < 5000; iter++) {
        EFI_FILE_PROTOCOL *Dir = NULL, *Child = NULL;
        EFI_FILE_INFO     *Fi  = (EFI_FILE_INFO *)Ib;
        CHAR16             Name[260];
        BOOLEAN            Found = FALSE;
        UINTN              Isz;

        if (EFI_ERROR (WRoot->Open (WRoot, &Dir, (CHAR16 *)DirPath,
                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) break;
        Dir->SetPosition (Dir, 0);
        for (;;) {
            Isz = 2048;
            if (EFI_ERROR (Dir->Read (Dir, &Isz, Ib)) || Isz == 0) break;
            if (Fi->FileName[0] == L'.' &&
                (Fi->FileName[1] == 0 || (Fi->FileName[1] == L'.' && Fi->FileName[2] == 0))) continue;
            if (Fi->Attribute & EFI_FILE_DIRECTORY) continue;   /* files only */
            { UINTN c = 0; while (Fi->FileName[c] && c < 259) { Name[c] = Fi->FileName[c]; c++; } Name[c] = 0; }
            Found = TRUE; break;
        }
        if (!Found) { Dir->Close (Dir); break; }

        if (!EFI_ERROR (Dir->Open (Dir, &Child, Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
            EFI_STATUS d = Child->Delete (Child);
            if (d != EFI_SUCCESS) {
                UINTN c = 0; while (Name[c] && c < 259) { FailName[c] = Name[c]; c++; } FailName[c] = 0;
                *FailSt = d; Dir->Close (Dir); break;
            }
            (*Deleted)++;
        } else {
            UINTN c = 0; while (Name[c] && c < 259) { FailName[c] = Name[c]; c++; } FailName[c] = 0;
            *FailSt = EFI_NOT_FOUND; Dir->Close (Dir); break;   /* open-by-name failed */
        }
        Dir->Close (Dir);
    }

    {
        EFI_FILE_PROTOCOL *Dir = NULL;
        EFI_FILE_INFO     *Fi  = (EFI_FILE_INFO *)Ib;
        UINTN              Isz;
        if (!EFI_ERROR (WRoot->Open (WRoot, &Dir, (CHAR16 *)DirPath, EFI_FILE_MODE_READ, 0))) {
            Dir->SetPosition (Dir, 0);
            for (;;) {
                Isz = 2048;
                if (EFI_ERROR (Dir->Read (Dir, &Isz, Ib)) || Isz == 0) break;
                if (Fi->FileName[0] == L'.' &&
                    (Fi->FileName[1] == 0 || (Fi->FileName[1] == L'.' && Fi->FileName[2] == 0))) continue;
                if (Fi->Attribute & EFI_FILE_DIRECTORY) continue;
                (*Remaining)++;
            }
            Dir->Close (Dir);
        }
    }
    gBS->FreePool (Ib);
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

    Status = gBS->OpenProtocol (ImageHandle, &LoadedImageGuid,
                    (VOID **)&LoadedImage, ImageHandle, NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    /* keep the ESP FAT root open so ProbePrint can persist a trace, and drop an
     * immediate _ALIVE.txt marker (create+write+close) to prove we booted and
     * can write the boot FAT even if nothing else runs. */
    if (!EFI_ERROR (Status)) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *EspSfs;
        if (!EFI_ERROR (gBS->HandleProtocol (LoadedImage->DeviceHandle, &SfspGuid, (VOID **)&EspSfs)) &&
            !EFI_ERROR (EspSfs->OpenVolume (EspSfs, &gEspRoot))) {
            EFI_FILE_PROTOCOL *AF = NULL;
            if (!EFI_ERROR (gEspRoot->Open (gEspRoot, &AF, L"\\_ALIVE.txt",
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
                UINTN an = 12; AF->Write (AF, &an, "probe alive\n"); AF->Close (AF);
            }
        }
    }

    ProbePrint (L"==NTFS-PROBE-START==\r\n");

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
        ProbePrint (L"  SFS handles after self-load: %r count=%d\r\n", WStatus, (UINT32)WHandleCount);
        if (!EFI_ERROR (WStatus)) {
            UINTN wh;
            for (wh = 0; wh < WHandleCount; wh++) {
                EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *WSfsp;
                EFI_FILE_PROTOCOL               *WRoot;
                EFI_FILE_PROTOCOL               *WFile;

                if (WHandles[wh] == LoadedImage->DeviceHandle) { ProbePrint (L"  vol[%d]: skip (self ESP)\r\n", (UINT32)wh); continue; }
                if (EFI_ERROR (gBS->OpenProtocol (WHandles[wh], &SfspGuid, (VOID **)&WSfsp,
                        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL))) { ProbePrint (L"  vol[%d]: no SFSP\r\n", (UINT32)wh); continue; }
                if (EFI_ERROR (WSfsp->OpenVolume (WSfsp, &WRoot))) { ProbePrint (L"  vol[%d]: OpenVolume fail\r\n", (UINT32)wh); continue; }
                {
                    EFI_FILE_PROTOCOL *Wd = NULL;
                    EFI_STATUS ws = WRoot->Open (WRoot, &Wd, L"\\Windows", EFI_FILE_MODE_READ, 0);
                    ProbePrint (L"  vol[%d]: mounted, \\Windows=%r\r\n", (UINT32)wh, ws);
                    if (Wd) Wd->Close (Wd);
                }

                /* --- REAL Windows volume test: if this NTFS volume is a Windows
                 * install (\Windows present), run the NON-DESTRUCTIVE fragmentation
                 * battery. It only ever creates/deletes \_EFITEST\ and never writes
                 * real user data (it checksums a real file before+after to prove
                 * it stayed byte-identical). Results -> \_EFITEST_RESULT.txt, then
                 * power off. Bypasses the whole synthetic battery. */
                {
                    EFI_FILE_PROTOCOL *WinDir = NULL;
                    BOOLEAN Qualifies = FALSE;
                    if (!EFI_ERROR (WRoot->Open (WRoot, &WinDir, L"\\Windows", EFI_FILE_MODE_READ, 0))) {
                        WinDir->Close (WinDir); Qualifies = TRUE;
                    } else if (!EFI_ERROR (WRoot->Open (WRoot, &WinDir, L"\\Recovery", EFI_FILE_MODE_READ, 0))) {
                        WinDir->Close (WinDir); Qualifies = TRUE;
                    }
                    if (Qualifies) {
                        CHAR8  Log[4096];
                        UINTN  LogLen;

                        ProbePrint (L"==EFITEST-START== (non-destructive frag battery)\r\n");
                        LogLen = FbRun (WRoot, Log, sizeof (Log));

                        {
                            EFI_FILE_PROTOCOL *RF = NULL;
                            if (!EFI_ERROR (WRoot->Open (WRoot, &RF, L"\\_EFITEST_RESULT.txt",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
                                UINTN wl = LogLen; RF->SetPosition (RF, 0);
                                RF->Write (RF, &wl, Log); RF->Flush (RF); RF->Close (RF);
                            }
                        }
                        /* also drop the result on the ESP FAT so the host can read
                         * it WITHOUT ever mounting the Windows child disk (which
                         * shuffles host drive letters). */
                        if (gEspRoot != NULL) {
                            EFI_FILE_PROTOCOL *EF = NULL;
                            if (!EFI_ERROR (gEspRoot->Open (gEspRoot, &EF, L"\\_EFITEST_RESULT.txt",
                                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
                                UINTN wl = LogLen; EF->SetPosition (EF, 0);
                                EF->Write (EF, &wl, Log); EF->Close (EF);
                            }
                        }
                        /* echo the log to serial too (QEMU / debug console) */
                        {
                            UINTN qi; CHAR16 wc[2]; wc[1] = 0;
                            for (qi = 0; qi < LogLen; qi++) { wc[0] = (CHAR16)Log[qi];
                                gST->ConOut->OutputString (gST->ConOut, wc); }
                        }
                        /* clean unmount of the volume we wrote to: drives
                         * BindingStop -> NtfsEfiUnmountVolume, which clears the
                         * $Volume dirty flag + does a final FlushBlocks. Proves
                         * "no chkdsk prompt on next boot" for the written volume. */
                        {
                            EFI_STATUS Dc = gBS->DisconnectController (WHandles[wh], NULL, NULL);
                            ProbePrint (L"  EFITEST clean-unmount (disconnect): %r\r\n", Dc);
                        }
                        ProbePrint (L"==EFITEST-DONE==\r\n");
                        SystemTable->RuntimeServices->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
                        return EFI_SUCCESS;
                    }
                }

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
                    /* exactly 4 bytes, deliberately unterminated - the Write is
                     * expected to be refused before it ever reads the buffer */
                    CHAR8      Dummy[4] = { 'X', 'X', 'X', 'X' };
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

                        /*
                         * Collation beyond a-z. U+0105 and U+0104 are the same
                         * letter to the volume's $UpCase table, so on a normal
                         * case-insensitive NTFS volume "kolacja_<U+0105>.txt" and
                         * "kolacja_<U+0104>.txt" are ONE name: creating the second
                         * must reach the file the first one wrote, and reading it
                         * back must return that content. A driver that folds only
                         * a-z sees two names, makes a second index entry, and this
                         * readback comes back empty.
                         */
                        {
                            /*
                             * Ordering, not equality - this is what the a-z fold
                             * actually got wrong. $UpCase maps Cyrillic small a
                             * (U+0430) to capital A (U+0410), which sorts BEFORE
                             * capital Ya (U+042F). Folding only a-z leaves U+0430
                             * above U+042F, i.e. AFTER it. So: create Ya first,
                             * then small a. A driver that inserts by the a-z rule
                             * puts small a after Ya, breaking the sorted order the
                             * lookup descent relies on - and the very next Open of
                             * that name walks the node, hits Ya, compares less-than
                             * and gives up: EFI_NOT_FOUND on a file that is there.
                             */
                            CHAR16 NameYa[] = { L'c',L'y',L'r',L'_',0x042F,L'.',L't',L'x',L't',0 };
                            CHAR16 NameA[]  = { L'c',L'y',L'r',L'_',0x0430,L'.',L't',L'x',L't',0 };
                            EFI_FILE_PROTOCOL *F1 = NULL, *F2 = NULL;
                            EFI_FILE_PROTOCOL *CsDir2 = NULL;
                            EFI_STATUS S1, S2, S3;

                            /*
                             * A FRESH directory, because only a directory still
                             * small enough to keep its index in a resident
                             * $INDEX_ROOT goes through NtfsInsertIndexEntrySmall -
                             * the one path that folded a-z by hand. Once a
                             * directory grows an $INDEX_ALLOCATION every insert
                             * takes the $UpCase-based path and the bug is out of
                             * reach.
                             */
                            S1 = CsDir->Open (CsDir, &CsDir2, L"collate_dir",
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                EFI_FILE_DIRECTORY);
                            ProbePrint (L"    collate-mkdir: %r\r\n", S1);
                            if (EFI_ERROR (S1)) CsDir2 = CsDir;

                            S1 = CsDir2->Open (CsDir2, &F1, NameYa,
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                            if (!EFI_ERROR (S1)) F1->Close (F1);
                            ProbePrint (L"    collate-create-Ya: %r\r\n", S1);

                            S2 = CsDir2->Open (CsDir2, &F2, NameA,
                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
                            if (!EFI_ERROR (S2)) F2->Close (F2);
                            ProbePrint (L"    collate-create-a: %r\r\n", S2);

                            /* the decisive one: is the just-created name findable? */
                            F2 = NULL;
                            S3 = CsDir2->Open (CsDir2, &F2, NameA, EFI_FILE_MODE_READ, 0);
                            ProbePrint (L"    collate-reopen-a: %r (expect Success)\r\n", S3);
                            if (!EFI_ERROR (S3)) F2->Close (F2);

                            F1 = NULL;
                            S3 = CsDir2->Open (CsDir2, &F1, NameYa, EFI_FILE_MODE_READ, 0);
                            ProbePrint (L"    collate-reopen-Ya: %r (expect Success)\r\n", S3);
                            if (!EFI_ERROR (S3)) F1->Close (F1);
                            if (CsDir2 != CsDir) CsDir2->Close (CsDir2);
                        }

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
                                /* \copied\many_split is the 62-entry fixture the copy test
                                 * above lays down: a real INDX-split directory that actually holds
                                 * these synth_entry_* names. This used to open \aged_dir, whose
                                 * files are named aged_NNN_*.dat, so both opens below could only
                                 * ever report Not Found - a stale test expectation, not a driver
                                 * fault. */
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

                            /* (d2) DRAIN a split directory to zero: the real
                             * EC recursive-delete case. Repeatedly enumerate
                             * the FIRST real child and delete it, reopening the
                             * directory each pass so the enumeration reflects
                             * the (rebalanced) tree - a separator key must be
                             * removable now, not refused, or files leak. */
                            {
                                UINT32     Deleted = 0;
                                EFI_STATUS LastDel = EFI_SUCCESS;
                                UINT32     Remaining = 0;
                                UINT32     Iter;
                                VOID      *Ib = NULL;
                                CHAR16     FailName[260]; FailName[0] = L'\0';

                                if (!EFI_ERROR (gBS->AllocatePool (EfiBootServicesData, 2048, &Ib)) && Ib != NULL) {
                                    for (Iter = 0; Iter < 500; Iter++) {
                                        EFI_FILE_PROTOCOL *Dir = NULL, *Child = NULL;
                                        EFI_FILE_INFO     *Fi = (EFI_FILE_INFO *)Ib;
                                        CHAR16             Name[260];
                                        BOOLEAN            Found = FALSE;
                                        UINTN              Isz;

                                        if (EFI_ERROR (WRoot->Open (WRoot, &Dir, L"\\aged_dir",
                                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) break;
                                        Dir->SetPosition (Dir, 0);
                                        for (;;) {
                                            Isz = 2048;
                                            if (EFI_ERROR (Dir->Read (Dir, &Isz, Ib)) || Isz == 0) break;
                                            if (Fi->FileName[0] == L'.' &&
                                                (Fi->FileName[1] == L'\0' ||
                                                 (Fi->FileName[1] == L'.' && Fi->FileName[2] == L'\0'))) continue;
                                            if (Fi->Attribute & EFI_FILE_DIRECTORY) continue;   /* files only; leave subdirs */
                                            { UINTN c = 0; while (Fi->FileName[c] != L'\0' && c < 259) { Name[c] = Fi->FileName[c]; c++; } Name[c] = L'\0'; }
                                            Found = TRUE;
                                            break;
                                        }
                                        if (!Found) { Dir->Close (Dir); break; }   /* drained */

                                        if (!EFI_ERROR (Dir->Open (Dir, &Child, Name,
                                                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
                                            LastDel = Child->Delete (Child);   /* frees Child */
                                            /* EFI_WARN_DELETE_FAILURE is NOT an EFI_ERROR - stop on
                                             * anything that isn't a clean success and record it. */
                                            if (LastDel != EFI_SUCCESS) {
                                                UINTN c = 0; while (Name[c] != L'\0' && c < 259) { FailName[c] = Name[c]; c++; } FailName[c] = L'\0';
                                                Dir->Close (Dir); break;
                                            }
                                            Deleted++;
                                        } else {
                                            Dir->Close (Dir); break;
                                        }
                                        Dir->Close (Dir);
                                    }

                                    /* count survivors */
                                    {
                                        EFI_FILE_PROTOCOL *Dir = NULL;
                                        EFI_FILE_INFO     *Fi = (EFI_FILE_INFO *)Ib;
                                        UINTN              Isz;
                                        if (!EFI_ERROR (WRoot->Open (WRoot, &Dir, L"\\aged_dir",
                                                EFI_FILE_MODE_READ, 0))) {
                                            Dir->SetPosition (Dir, 0);
                                            for (;;) {
                                                Isz = 2048;
                                                if (EFI_ERROR (Dir->Read (Dir, &Isz, Ib)) || Isz == 0) break;
                                                if (Fi->FileName[0] == L'.' &&
                                                    (Fi->FileName[1] == L'\0' ||
                                                     (Fi->FileName[1] == L'.' && Fi->FileName[2] == L'\0'))) continue;
                                                /* count only what the drain loop above actually deletes: it
                                                 * skips subdirectories on purpose, so counting them as
                                                 * survivors made a fully drained directory still report
                                                 * remaining=<number of subdirs>. */
                                                if (Fi->Attribute & EFI_FILE_DIRECTORY) continue;
                                                Remaining++;
                                            }
                                            Dir->Close (Dir);
                                        }
                                    }
                                    gBS->FreePool (Ib);
                                }

                                ProbePrint (L"    del-drain-split: deleted=%d remaining=%d lastdel=%r (expect remaining=0)\r\n",
                                    Deleted, Remaining, LastDel);
                                {
                                    EFI_FILE_PROTOCOL *RF;
                                    if (!EFI_ERROR (CsDir->Open (CsDir, &RF, L"_SEPTEST.txt",
                                            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
                                        CHAR8  Line[200];
                                        UINTN  Ln = (UINTN)AsciiSPrint (Line, sizeof (Line),
                                            "drain deleted=%d remaining=%d lastdel=%r fail='%s'\n",
                                            Deleted, Remaining, LastDel, FailName);
                                        RF->Write (RF, &Ln, Line);
                                        RF->Close (RF);
                                    }
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

                                /* REPRO of the EC "92MB -> 256KB" bug: read a big
                                 * file and copy it WITHIN THE SAME NTFS volume
                                 * (source read + dest write on one Vcb), like EC
                                 * does fs1:\xiaomi -> fs1:\aaa. First make a 1 MB
                                 * source, then (a) full-read it, (b) copy it. */
                                {
                                    EFI_FILE_PROTOCOL *Src, *Dst;
                                    UINTN i, Sz;
                                    UINTN BlkSz = 256 * 1024;   /* EC copies in 256 KB chunks */
                                    CHAR8 *Blk = NULL;
                                    { VOID *bp=NULL; gBS->AllocatePool (EfiBootServicesData, BlkSz, &bp); Blk=(CHAR8*)bp; }
                                    if (Blk) for (i = 0; i < BlkSz; i++) Blk[i] = (CHAR8)('A' + (i & 31));
                                    if (Blk && !EFI_ERROR (CsDir->Open (CsDir, &Src, L"samefs_src.bin",
                                            EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE, 0))) {
                                        for (i = 0; i < 368; i++) { Sz = BlkSz; Src->Write (Src, &Sz, Blk); } /* ~92 MB like HyperSploit */
                                        Src->Close (Src);
                                    }
                                    /* (a) FULL READ */
                                    if (!EFI_ERROR (CsDir->Open (CsDir, &Src, L"samefs_src.bin", EFI_FILE_MODE_READ, 0))) {
                                        UINT64 tot = 0; UINTN got;
                                        for (;;) { got = BlkSz; if (EFI_ERROR (Src->Read (Src, &got, Blk)) || got == 0) break; tot += got; }
                                        ProbePrint (L"    samefs-READ: bytes=%ld (expect 96468992)\r\n", tot);
                                        Src->Close (Src);
                                    }
                                    /* (b) COPY within same volume */
                                    if (!EFI_ERROR (CsDir->Open (CsDir, &Src, L"samefs_src.bin", EFI_FILE_MODE_READ, 0)) &&
                                        !EFI_ERROR (CsDir->Open (CsDir, &Dst, L"samefs_dst.bin",
                                            EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE, 0))) {
                                        UINT64 cop = 0; UINTN got, put;
                                        for (;;) {
                                            got = BlkSz;
                                            if (EFI_ERROR (Src->Read (Src, &got, Blk)) || got == 0) break;
                                            put = got;
                                            if (EFI_ERROR (Dst->Write (Dst, &put, Blk)) || put != got) { ProbePrint (L"    samefs-COPY write STOP at %ld got=%d put=%d\r\n", cop, (UINT32)got, (UINT32)put); break; }
                                            cop += put;
                                        }
                                        ProbePrint (L"    samefs-COPY: bytes=%ld (expect 96468992)\r\n", cop);
                                        Src->Close (Src); Dst->Close (Dst);
                                        /* persist result to file (Hyper-V has no serial) */
                                        {
                                            EFI_FILE_PROTOCOL *RF;
                                            if (!EFI_ERROR (CsDir->Open (CsDir, &RF, L"_SAMEFS.txt",
                                                    EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE, 0))) {
                                                CHAR8 b[96]; UINTN n;
                                                AsciiSPrint (b, sizeof (b), "copy=%lu (expect 96468992)\n", cop);
                                                for (n=0; b[n]; n++) {}
                                                RF->Write (RF, &n, b); RF->Close (RF);
                                            }
                                        }
                                    }
                                    if (Blk) gBS->FreePool (Blk);
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

    /* power off so a Hyper-V harness (no serial) knows the battery finished */
    SystemTable->RuntimeServices->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}
