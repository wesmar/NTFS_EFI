/**
 * ntfs_lznt1.c - LZNT1 decompression, ported from the real NT source
 * (base\ntos\rtl\lznt1.c: LZNT1DecompressChunk / RtlDecompressBufferLZNT1).
 * NTFS.SYS itself does not contain this codec - it calls into ntoskrnl's
 * RtlDecompressBuffer, so this is a from-source (not from-reverse-
 * engineering) port of the actual on-disk format decoder.
 *
 * On-disk shape for a compressed non-resident attribute:
 *  - Data is split into "compression units" of (1 << CompressionUnit)
 *    clusters (almost always 16 clusters = 64 KiB with 4 KiB clusters).
 *  - If a unit didn't compress at all, it is stored as one full-size real
 *    run - read it back verbatim, no LZNT1 involved.
 *  - If it did compress, the run list holds a short real run (the
 *    compressed bytes) followed by a sparse/hole run padding out to the
 *    full unit size. The compressed bytes are themselves a sequence of
 *    LZNT1 "chunks", each up to 4096 bytes uncompressed, each prefixed by
 *    a 2-byte chunk header (12-bit size-1 field is size-3, 3-bit
 *    signature always 3, 1-bit compressed flag).
 *  - Within one chunk, matches are encoded as 2-byte copy tokens whose
 *    length/displacement bit split shrinks as the output position grows
 *    (4/12 bits near the start of the chunk, down to 12/4 bits near the
 *    end) - the FORMAT4xx family below.
 */

#include "ntfs.h"

#define LZNT1_MAX_UNCOMPRESSED_CHUNK_SIZE 4096

#pragma pack(push, 1)
typedef union {
    struct { USHORT SizeMinus3 : 12; USHORT Signature : 3; USHORT IsCompressed : 1; } Chunk;
    USHORT Short;
} LZNT1_CHUNK_HEADER;

typedef union {
    struct { USHORT Length : 12; USHORT Displacement :  4; } F412;
    struct { USHORT Length : 11; USHORT Displacement :  5; } F511;
    struct { USHORT Length : 10; USHORT Displacement :  6; } F610;
    struct { USHORT Length :  9; USHORT Displacement :  7; } F79;
    struct { USHORT Length :  8; USHORT Displacement :  8; } F88;
    struct { USHORT Length :  7; USHORT Displacement :  9; } F97;
    struct { USHORT Length :  6; USHORT Displacement : 10; } F106;
    struct { USHORT Length :  5; USHORT Displacement : 11; } F115;
    struct { USHORT Length :  4; USHORT Displacement : 12; } F124;
    UCHAR  Bytes[2];
} LZNT1_COPY_TOKEN;
#pragma pack(pop)

/* index 0..8 = FORMAT412..FORMAT124; selects which bit split above is active */
static CONST ULONG NtfsLznt1MaxDisplacement[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };

static ULONG
NtfsLznt1GetLength (IN ULONG Format, IN LZNT1_COPY_TOKEN Ct)
{
    switch (Format) {
        case 0: return Ct.F412.Length  + 3;
        case 1: return Ct.F511.Length  + 3;
        case 2: return Ct.F610.Length  + 3;
        case 3: return Ct.F79.Length   + 3;
        case 4: return Ct.F88.Length   + 3;
        case 5: return Ct.F97.Length   + 3;
        case 6: return Ct.F106.Length  + 3;
        case 7: return Ct.F115.Length  + 3;
        default: return Ct.F124.Length + 3;
    }
}

static ULONG
NtfsLznt1GetDisplacement (IN ULONG Format, IN LZNT1_COPY_TOKEN Ct)
{
    switch (Format) {
        case 0: return Ct.F412.Displacement  + 1;
        case 1: return Ct.F511.Displacement  + 1;
        case 2: return Ct.F610.Displacement  + 1;
        case 3: return Ct.F79.Displacement   + 1;
        case 4: return Ct.F88.Displacement   + 1;
        case 5: return Ct.F97.Displacement   + 1;
        case 6: return Ct.F106.Displacement  + 1;
        case 7: return Ct.F115.Displacement  + 1;
        default: return Ct.F124.Displacement + 1;
    }
}

/*
 * Decompress exactly one LZNT1 chunk (the payload after its 2-byte chunk
 * header). Direct port of LZNT1DecompressChunk from base\ntos\rtl\lznt1.c.
 */
static EFI_STATUS
NtfsEfiLznt1DecompressChunk (
    OUT PUCHAR UncompressedBuffer,
    IN  PUCHAR EndOfUncompressedBufferPlus1,
    IN  PUCHAR CompressedBuffer,
    IN  PUCHAR EndOfCompressedBufferPlus1,
    OUT ULONG *FinalUncompressedChunkSize
    )
{
    PUCHAR OutputPointer = UncompressedBuffer;
    PUCHAR InputPointer  = CompressedBuffer;
    UCHAR  FlagByte;
    ULONG  FlagBit;
    ULONG  Format = 0;

    if (InputPointer >= EndOfCompressedBufferPlus1) {
        *FinalUncompressedChunkSize = 0;
        return EFI_SUCCESS;
    }

    FlagByte = *(InputPointer++);
    FlagBit  = 0;

    while (OutputPointer < EndOfUncompressedBufferPlus1 && InputPointer < EndOfCompressedBufferPlus1) {

        while (UncompressedBuffer + NtfsLznt1MaxDisplacement[Format] < OutputPointer) Format += 1;

        if (!(FlagByte & (1 << FlagBit))) {
            /* literal byte */
            *(OutputPointer++) = *(InputPointer++);
        } else {
            LZNT1_COPY_TOKEN CopyToken;
            ULONG Displacement;
            ULONG Length;

            if (InputPointer + 1 >= EndOfCompressedBufferPlus1) {
                *FinalUncompressedChunkSize = (ULONG)(OutputPointer - UncompressedBuffer);
                return EFI_COMPROMISED_DATA;
            }

            CopyToken.Bytes[0] = *(InputPointer++);
            CopyToken.Bytes[1] = *(InputPointer++);

            Displacement = NtfsLznt1GetDisplacement (Format, CopyToken);
            Length       = NtfsLznt1GetLength (Format, CopyToken);

            if (Displacement > (ULONG)(OutputPointer - UncompressedBuffer)) {
                *FinalUncompressedChunkSize = (ULONG)(OutputPointer - UncompressedBuffer);
                return EFI_COMPROMISED_DATA;
            }

            if ((OutputPointer + Length) >= EndOfUncompressedBufferPlus1) {
                Length = (ULONG)(EndOfUncompressedBufferPlus1 - OutputPointer);
            }

            /* byte-at-a-time: overlapping copy semantics are required (LZ77) */
            while (Length > 0) {
                *OutputPointer = *(OutputPointer - Displacement);
                OutputPointer++;
                Length--;
            }
        }

        FlagBit = (FlagBit + 1) % 8;
        if (FlagBit == 0) {
            if (InputPointer >= EndOfCompressedBufferPlus1) break;
            FlagByte = *(InputPointer++);
        }
    }

    *FinalUncompressedChunkSize = (ULONG)(OutputPointer - UncompressedBuffer);
    return EFI_SUCCESS;
}

/*
 * Decompress a whole compression unit's worth of on-disk compressed bytes
 * (a sequence of chunk-header-prefixed chunks) into UncompressedBuffer.
 * Direct port of RtlDecompressBufferLZNT1.
 */
static EFI_STATUS
NtfsEfiLznt1DecompressBuffer (
    OUT PUCHAR UncompressedBuffer,
    IN  ULONG  UncompressedBufferSize,
    IN  PUCHAR CompressedBuffer,
    IN  ULONG  CompressedBufferSize,
    OUT ULONG *FinalUncompressedSize
    )
{
    PUCHAR CompressedChunk    = CompressedBuffer;
    PUCHAR UncompressedChunk  = UncompressedBuffer;
    PUCHAR EndOfUncompressed  = UncompressedBuffer + UncompressedBufferSize;
    PUCHAR EndOfCompressed    = CompressedBuffer + CompressedBufferSize;
    LZNT1_CHUNK_HEADER Header;
    LONG   SavedChunkSize;

    if (CompressedChunk > EndOfCompressed - 2) {
        *FinalUncompressedSize = 0;
        return EFI_SUCCESS;
    }
    CopyMem (&Header, CompressedChunk, sizeof (Header));

    for (;;) {
        ULONG CompressedChunkSize = (ULONG)Header.Chunk.SizeMinus3 + 3;
        ULONG UncompressedChunkSize;

        if (CompressedChunk + CompressedChunkSize > EndOfCompressed) {
            *FinalUncompressedSize = (ULONG)(CompressedChunk - CompressedBuffer);
            return EFI_COMPROMISED_DATA;
        }

        if (Header.Chunk.IsCompressed) {
            EFI_STATUS Status = NtfsEfiLznt1DecompressChunk (
                    UncompressedChunk, EndOfUncompressed,
                    CompressedChunk + sizeof (Header), CompressedChunk + CompressedChunkSize,
                    &UncompressedChunkSize);
            if (EFI_ERROR (Status)) {
                *FinalUncompressedSize = (ULONG)((UncompressedChunk - UncompressedBuffer) + UncompressedChunkSize);
                return Status;
            }
        } else {
            UncompressedChunkSize = LZNT1_MAX_UNCOMPRESSED_CHUNK_SIZE;
            if (UncompressedChunk + UncompressedChunkSize > EndOfUncompressed) {
                UncompressedChunkSize = (ULONG)(EndOfUncompressed - UncompressedChunk);
            }
            if (CompressedChunk + sizeof (Header) + UncompressedChunkSize > EndOfCompressed) {
                *FinalUncompressedSize = (ULONG)(CompressedChunk - CompressedBuffer);
                return EFI_COMPROMISED_DATA;
            }
            CopyMem (UncompressedChunk, CompressedChunk + sizeof (Header), UncompressedChunkSize);
        }

        CompressedChunk   += CompressedChunkSize;
        UncompressedChunk += UncompressedChunkSize;

        if (UncompressedChunk == EndOfUncompressed) break;
        if (CompressedChunk > EndOfCompressed - 2) break;

        SavedChunkSize = LZNT1_MAX_UNCOMPRESSED_CHUNK_SIZE;
        CopyMem (&Header, CompressedChunk, sizeof (Header));
        if (Header.Short == 0) break; /* end-of-buffer sentinel */

        /* previous chunk was short (trailing zero region) - zero-fill the gap */
        if ((LONG)UncompressedChunkSize < SavedChunkSize) {
            LONG   ZeroLen = SavedChunkSize - (LONG)UncompressedChunkSize;
            PUCHAR ZeroEnd = UncompressedChunk + ZeroLen;
            if (ZeroEnd >= EndOfUncompressed) break;
            ZeroMem (UncompressedChunk, (UINTN)ZeroLen);
            UncompressedChunk = ZeroEnd;
        }
    }

    *FinalUncompressedSize = (ULONG)(UncompressedChunk - UncompressedBuffer);
    return EFI_SUCCESS;
}

/*
 * Read Length bytes at Offset from a compressed non-resident attribute.
 * Unlike NtfsEfiReadAttr(), this cannot walk the run list 1:1 against the
 * logical offset - compression units (1 << CompressionUnit clusters,
 * almost always 16 = 64 KiB) must be located, read whole, and decompressed
 * before the requested slice can be extracted.
 */
ULONG
NtfsEfiReadCompressedAttr (
    IN  PNTFS_EFI_VCB  Vcb,
    IN  PNTFS_ATTR_CTX Ctx,
    IN  UINT64         Offset,
    OUT PCHAR          Buffer,
    IN  ULONG          Length
    )
{
    ULONG   CompUnitClusters = 1U << Ctx->pRecord->NonResident.CompressionUnit;
    UINT64  CompUnitBytes    = (UINT64)CompUnitClusters * Vcb->BytesPerCluster;
    ULONG   AlreadyRead      = 0;
    PUCHAR  UnitBuf;
    PUCHAR  RawBuf;

    UnitBuf = AllocatePool ((UINTN)CompUnitBytes);
    RawBuf  = AllocatePool ((UINTN)CompUnitBytes);
    if (UnitBuf == NULL || RawBuf == NULL) {
        if (UnitBuf) FreePool (UnitBuf);
        if (RawBuf)  FreePool (RawBuf);
        return 0;
    }

    Print (L"[ntfs] ReadCompressedAttr: CompUnitClusters=%d CompUnitBytes=%ld RunCount=%d Offset=%ld Length=%d\n",
        CompUnitClusters, CompUnitBytes, Ctx->RunCount, Offset, Length);
    {
        ULONG dbg;
        for (dbg = 0; dbg < Ctx->RunCount && dbg < 8; dbg++) {
            Print (L"[ntfs]   run[%d]: VBN=%ld LBN=%ld Len=%ld\n",
                dbg, Ctx->Runs[dbg].VBN, Ctx->Runs[dbg].LBN, Ctx->Runs[dbg].Len);
        }
    }

    while (AlreadyRead < Length) {
        UINT64 CurOffset   = Offset + AlreadyRead;
        UINT64 UnitIndex   = CurOffset / CompUnitBytes;
        UINT64 UnitStartVBN = UnitIndex * CompUnitClusters;
        UINT64 UnitInner   = CurOffset % CompUnitBytes;
        ULONG  Take;

        UINT64 RealClusters = 0;
        INT64  FirstLBN     = -1;
        ULONG  i;
        BOOLEAN SawSparse = FALSE;
        ULONG  ValidBytes;

        /* Walk the run list for the [UnitStartVBN, UnitStartVBN+CompUnitClusters)
         * window: real runs before any sparse hole hold the compressed bytes. */
        for (i = 0; i < Ctx->RunCount; i++) {
            UINT64 RunVBN = Ctx->Runs[i].VBN;
            UINT64 RunLen = Ctx->Runs[i].Len;
            if (RunVBN + RunLen <= UnitStartVBN) continue;
            if (RunVBN >= UnitStartVBN + CompUnitClusters) break;

            if (Ctx->Runs[i].LBN == -1LL) {
                SawSparse = TRUE;
                continue;
            }
            if (SawSparse) {
                /* real run after a sparse hole within the same unit -
                 * not the simple layout this decoder understands. */
                break;
            }
            if (FirstLBN == -1) FirstLBN = Ctx->Runs[i].LBN;
            RealClusters += RunLen;
        }

        ZeroMem (UnitBuf, (UINTN)CompUnitBytes);

        if (FirstLBN == -1) {
            /* fully sparse compression unit: all zero */
            ValidBytes = (ULONG)CompUnitBytes;
        } else if (!SawSparse && RealClusters >= CompUnitClusters) {
            /* stored raw (didn't compress) - read the whole unit verbatim */
            EFI_STATUS Status = NtfsEfiReadDisk (Vcb,
                    (UINT64)FirstLBN * Vcb->BytesPerCluster,
                    (UINTN)CompUnitBytes, UnitBuf);
            if (EFI_ERROR (Status)) break;
            ValidBytes = (ULONG)CompUnitBytes;
        } else {
            /* compressed: read only the real (non-sparse) prefix, then
             * LZNT1-decode it into the full unit buffer */
            UINTN RawBytes = (UINTN)(RealClusters * Vcb->BytesPerCluster);
            EFI_STATUS Status;

            if (RawBytes == 0 || RawBytes > (UINTN)CompUnitBytes) break;
            Status = NtfsEfiReadDisk (Vcb, (UINT64)FirstLBN * Vcb->BytesPerCluster,
                    RawBytes, RawBuf);
            if (EFI_ERROR (Status)) break;

            Status = NtfsEfiLznt1DecompressBuffer (UnitBuf, (ULONG)CompUnitBytes,
                    RawBuf, (ULONG)RawBytes, &ValidBytes);
            if (EFI_ERROR (Status) && ValidBytes == 0) break;
        }

        /*
         * UnitBuf was zeroed above, so bytes past ValidBytes (a short
         * final chunk run, or a fully-sparse unit) already read as zero -
         * no separate branch needed for that case.
         */
        (VOID)ValidBytes;
        Take = (ULONG)min ((UINT64)(CompUnitBytes - UnitInner), (UINT64)(Length - AlreadyRead));

        CopyMem (Buffer + AlreadyRead, UnitBuf + UnitInner, Take);
        AlreadyRead += Take;
    }

    FreePool (UnitBuf);
    FreePool (RawBuf);
    return AlreadyRead;
}
