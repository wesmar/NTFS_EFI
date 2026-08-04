/**
 * ntfs_symlink.c - NTFS symbolic link resolution via $REPARSE_POINT.
 *
 * The on-disk format ported here (NTFS_SYMLINK_REPARSE_BUFFER, in ntfs.h)
 * is not in the NT source snapshot available for reference (ms/ predates
 * IO_REPARSE_TAG_SYMLINK - it only has IO_REPARSE_TAG_MOUNT_POINT, no
 * Flags field yet). It is instead taken from Microsoft's public [MS-FSCC]
 * "Symbolic Link Reparse Data Buffer" documentation, stable and unchanged
 * since Vista, and matches what every other independent implementation
 * (ntfs-3g, WinBtrfs, Linux ntfs3, maharmstone/ntfs-efi) uses.
 */

#include "ntfs.h"

/*
 * Single static buffer: this driver is strictly single-threaded (no boot
 * services concurrency), and the caller (NtfsEfiLookupPath) consumes the
 * result immediately before any subsequent call, so there is no aliasing
 * hazard in practice.
 */
static WCHAR gSymlinkTargetBuf[NTFS_MAX_PATH_CHARS];

BOOLEAN
NtfsEfiTryResolveSymlink (
    IN  PNTFS_EFI_VCB Vcb,
    IN  ULONGLONG     MFTIndex,
    OUT CONST WCHAR  **Target,
    OUT BOOLEAN       *IsRelative
    )
{
    PFILE_RECORD_HEADER          Rec;
    PNTFS_ATTR_CTX                RpCtx;
    UINT64                        RpLen;
    PUCHAR                        RpBuf;
    PNTFS_SYMLINK_REPARSE_BUFFER  Sym;
    CONST WCHAR                  *NamePtr;
    UINTN                         NameChars;
    UINTN                         SrcIdx, DstIdx;

    Rec = AllocatePool (Vcb->BytesPerFileRecord);
    if (Rec == NULL) return FALSE;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, MFTIndex, Rec))) {
        FreePool (Rec);
        return FALSE;
    }

    RpCtx = NtfsEfiFindAttribute (Vcb, Rec, AttributeReparsePoint, NULL, 0, NULL);
    FreePool (Rec);
    if (RpCtx == NULL) return FALSE;

    RpLen = NtfsEfiAttrDataLength (RpCtx);
    if (RpLen < FIELD_OFFSET (NTFS_SYMLINK_REPARSE_BUFFER, PathBuffer) || RpLen > 8192) {
        NtfsEfiFreeAttrCtx (RpCtx);
        return FALSE;
    }

    RpBuf = AllocatePool ((UINTN)RpLen);
    if (RpBuf == NULL) {
        NtfsEfiFreeAttrCtx (RpCtx);
        return FALSE;
    }
    NtfsEfiReadAttr (Vcb, RpCtx, 0, (PCHAR)RpBuf, (ULONG)RpLen);
    NtfsEfiFreeAttrCtx (RpCtx);

    PUCHAR PathBufferStart;

    Sym = (PNTFS_SYMLINK_REPARSE_BUFFER)RpBuf;
    if (Sym->ReparseTag != IO_REPARSE_TAG_SYMLINK && Sym->ReparseTag != IO_REPARSE_TAG_MOUNT_POINT) {
        Print (L"[ntfs] Symlink: MFT=%ld has reparse point but tag=%08x (not supported)\n",
            MFTIndex, Sym->ReparseTag);
        FreePool (RpBuf);
        return FALSE;
    }

    if (Sym->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        PathBufferStart = (PUCHAR)Sym->PathBuffer;
        *IsRelative = ((Sym->Flags & SYMLINK_FLAG_RELATIVE) != 0);
    } else {
        /* Mount Point (Junction): starts at offset 16 (where Flags would be), always absolute */
        PathBufferStart = (PUCHAR)&Sym->Flags;
        *IsRelative = FALSE;
    }

    NameChars = Sym->SubstituteNameLength / sizeof (WCHAR);
    NamePtr   = (CONST WCHAR *)(PathBufferStart + Sym->SubstituteNameOffset);

    if ((PUCHAR)(NamePtr + NameChars) > RpBuf + RpLen) {
        Print (L"[ntfs] Symlink: MFT=%ld SubstituteName out of bounds\n", MFTIndex);
        FreePool (RpBuf);
        return FALSE;
    }

    /*
     * Strip the NT object-manager device prefix ("\??\") and a drive
     * letter ("X:") if present - this driver mounts and serves exactly
     * one volume, so any drive letter in an absolute target is assumed to
     * refer to that same volume (there is nowhere else it could resolve
     * to from inside a bootloader-style single-volume driver anyway).
     */
    SrcIdx = 0;
    if (NameChars >= 4 && NamePtr[0] == L'\\' && NamePtr[1] == L'?' &&
        NamePtr[2] == L'?' && NamePtr[3] == L'\\') {
        SrcIdx = 4;
    }
    if (NameChars - SrcIdx >= 2 && NamePtr[SrcIdx + 1] == L':') {
        SrcIdx += 2;
    }

    DstIdx = 0;
    if (SrcIdx >= NameChars || NamePtr[SrcIdx] != L'\\') {
        gSymlinkTargetBuf[DstIdx++] = L'\\';
    }
    for (; SrcIdx < NameChars && DstIdx < NTFS_MAX_PATH_CHARS - 1; SrcIdx++, DstIdx++) {
        gSymlinkTargetBuf[DstIdx] = NamePtr[SrcIdx];
    }
    gSymlinkTargetBuf[DstIdx] = L'\0';

    Print (L"[ntfs] Symlink: MFT=%ld -> '%s' (relative=%d, junction=%d)\n",
        MFTIndex, gSymlinkTargetBuf, *IsRelative, Sym->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT);

    *Target = gSymlinkTargetBuf;

    FreePool (RpBuf);
    return TRUE;
}
