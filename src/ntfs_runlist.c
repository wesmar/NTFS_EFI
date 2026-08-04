/**
 * ntfs_runlist.c - data-run (mapping pairs) decoding for non-resident
 * attributes. Replaces LARGE_MCB + FsRtl* entirely: runs are decoded once
 * into a flat NTFS_RUN_ENTRY array up front.
 */

#include "ntfs.h"

/*
 * OutOffset==-1 used to double as "this is a sparse run", but a real
 * fragment can legitimately start exactly 1 cluster before the previous
 * one's LCN (delta == -1) - that collided with the sparse sentinel and
 * got silently treated as a hole (zeros on read, write-refused). Sparse
 * is now its own explicit flag, decided the only correct way: OffBytes
 * (the run's own on-disk offset-field-size nibble) == 0.
 */
static PUCHAR
NtfsDecodeRunEntry (
    IN  PUCHAR     DataRun,
    OUT LONGLONG  *OutOffset,    /* delta LCN; meaningless if *OutIsSparse   */
    OUT UINT64    *OutLength,    /* run length in clusters                  */
    OUT BOOLEAN   *OutIsSparse
    )
{
    UCHAR LenBytes  = *DataRun & 0x0F;
    UCHAR OffBytes  = (*DataRun >> 4) & 0x0F;
    UINTN i;

    *OutLength = 0;
    *OutOffset = 0;
    *OutIsSparse = (OffBytes == 0);
    DataRun++;

    for (i = 0; i < LenBytes; i++) {
        *OutLength |= ((UINT64)*DataRun) << (i * 8);
        DataRun++;
    }
    if (!*OutIsSparse) {
        for (i = 0; i < OffBytes - 1; i++) {
            *OutOffset |= ((UINT64)*DataRun) << (i * 8);
            DataRun++;
        }
        /* sign-extend the most-significant byte */
        *OutOffset = (LONGLONG)((INT64)(CCHAR)(*DataRun) << (INT32)(i * 8)) + *OutOffset;
        DataRun++;
    }
    return DataRun;
}

/* Decode all data runs from an attribute record into a flat NTFS_RUN_ENTRY array. */
EFI_STATUS
NtfsBuildRunList (
    IN  PNTFS_ATTR_RECORD  AttrRecord,
    OUT NTFS_RUN_ENTRY    *Runs,
    IN  ULONG              MaxRuns,
    OUT ULONG             *RunCount
    )
{
    PUCHAR   DataRun;
    LONGLONG DeltaLCN;
    UINT64   RunLen;
    BOOLEAN  IsSparse;
    INT64    CurrentLCN = 0;
    UINT64   CurrentVBN = AttrRecord->NonResident.LowestVCN;
    ULONG    Count      = 0;

    DataRun = (PUCHAR)AttrRecord + AttrRecord->NonResident.MappingPairsOffset;

    {
        /* clamp the debug dump to the attribute's own bounds - the
         * mapping-pairs stream is often shorter than 10 bytes and sits
         * near the end of a heap allocation sized to AttrRecord->Length */
        ULONG DumpLen = AttrRecord->Length - AttrRecord->NonResident.MappingPairsOffset;
        if (DumpLen > 10) DumpLen = 10;
        Print (L"[ntfs] BuildRunList: MappingPairsOffset=%d LowestVCN=%ld HighestVCN=%ld CompressionUnit=%d bytes:",
            AttrRecord->NonResident.MappingPairsOffset, AttrRecord->NonResident.LowestVCN,
            AttrRecord->NonResident.HighestVCN, AttrRecord->NonResident.CompressionUnit);
        {
            ULONG di;
            for (di = 0; di < DumpLen; di++) Print (L" %02x", DataRun[di]);
            Print (L"\n");
        }
    }

    while (*DataRun != 0) {
        if (Count >= MaxRuns) {
            return EFI_BUFFER_TOO_SMALL;
        }
        DataRun = NtfsDecodeRunEntry (DataRun, &DeltaLCN, &RunLen, &IsSparse);

        if (IsSparse) {
            Runs[Count].LBN = -1LL; /* sparse */
        } else {
            CurrentLCN += DeltaLCN;
            Runs[Count].LBN = CurrentLCN;
        }
        Runs[Count].VBN = CurrentVBN;
        Runs[Count].Len = RunLen;
        CurrentVBN += RunLen;
        Count++;
    }
    *RunCount = Count;
    return EFI_SUCCESS;
}

/* Byte length of an attribute's mapping-pairs stream, INCLUDING the
 * terminating 0x00, starting at AttrRecord->NonResident.MappingPairsOffset. */
UINTN
NtfsMappingPairsSize (
    IN PNTFS_ATTR_RECORD AttrRecord
    )
{
    PUCHAR p = (PUCHAR)AttrRecord + AttrRecord->NonResident.MappingPairsOffset;
    PUCHAR start = p;

    while (*p != 0) {
        UCHAR LenBytes = *p & 0x0F;
        UCHAR OffBytes = (*p >> 4) & 0x0F;
        p += 1 + LenBytes + OffBytes;
    }
    return (UINTN)(p - start) + 1;   /* +1 for the terminator itself */
}

/*
 * Encode one data-run entry (a single contiguous, non-sparse extent) in
 * mapping-pairs format, matching NtfsDecodeRunEntry()'s expectations
 * exactly: length is unsigned little-endian in the minimum number of
 * bytes, LCN delta is signed (two's complement) little-endian in the
 * minimum number of bytes that preserves its sign under sign-extension.
 * Returns the number of bytes written (does NOT include a terminator).
 */
UINTN
NtfsEncodeRunEntry (
    OUT PUCHAR  Dest,
    IN  UINT64  Length,
    IN  INT64   LcnDelta
    )
{
    UCHAR  LenBytes, OffBytes;
    UINT64 L = Length;
    UINTN  i;

    /* minimum bytes to hold an unsigned value; add a leading zero byte if the
     * top bit of the highest byte is set. NTFS's run-length is unsigned, but
     * Windows encodes/expects it the same way as the signed LCN offset (never
     * a set high bit in the last byte) - e.g. 147 (0x93) is written as 93 00,
     * not a bare 93. A bare high-bit length makes chkdsk read $DATA as corrupt. */
    LenBytes = 0;
    do { LenBytes++; L >>= 8; } while (L != 0);
    if ((Length >> ((LenBytes - 1) * 8)) & 0x80) LenBytes++;

    /* minimum bytes to hold a signed value under sign-extension */
    {
        INT64 D = LcnDelta;
        OffBytes = 1;
        for (;;) {
            INT64 Min = -(((INT64)1) << (OffBytes * 8 - 1));
            INT64 Max = (((INT64)1) << (OffBytes * 8 - 1)) - 1;
            if (D >= Min && D <= Max) break;
            OffBytes++;
        }
    }

    Dest[0] = (UCHAR)(LenBytes | (OffBytes << 4));
    for (i = 0; i < LenBytes; i++) {
        Dest[1 + i] = (UCHAR)((Length >> (i * 8)) & 0xFF);
    }
    for (i = 0; i < OffBytes; i++) {
        Dest[1 + LenBytes + i] = (UCHAR)(((UINT64)LcnDelta >> (i * 8)) & 0xFF);
    }
    return 1 + LenBytes + OffBytes;
}
