/**
 * ntfs_wof.c - read-only support for WOF file-provider compression.
 *
 * Windows CompactOS stores the unnamed $DATA as a sparse placeholder and
 * keeps the real bytes in the named :WofCompressedData stream. The stream
 * begins with a chunk-offset table followed by independently compressed
 * chunks, either XPRESS-Huffman ([MS-XCA]) or LZX in the flavour WIM uses.
 * Both decoders live here; the reparse point says which one a file needs.
 */

#include "ntfs.h"

#define IO_REPARSE_TAG_WOF                    0x80000017UL
#define WOF_PROVIDER_FILE                     2UL
#define FILE_PROVIDER_COMPRESSION_XPRESS4K    0UL
#define FILE_PROVIDER_COMPRESSION_LZX         1UL
#define FILE_PROVIDER_COMPRESSION_XPRESS8K    2UL
#define FILE_PROVIDER_COMPRESSION_XPRESS16K   3UL
#define XPRESS_SYMBOLS                        512U
#define XPRESS_MAX_CODE_BITS                  15U
#define XPRESS_TABLE_SIZE                     (1U << XPRESS_MAX_CODE_BITS)

#pragma pack(push, 1)
typedef struct {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    ULONG  WofVersion;
    ULONG  Provider;
    ULONG  FileVersion;
    ULONG  Algorithm;
} NTFS_WOF_REPARSE_DATA;
#pragma pack(pop)

static USHORT NtfsWofReadU16 (IN CONST UCHAR *p)
{
    return (USHORT)((USHORT)p[0] | ((USHORT)p[1] << 8));
}

static ULONG NtfsWofReadU32 (IN CONST UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) |
           ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static UINT64 NtfsWofReadU64 (IN CONST UCHAR *p)
{
    return (UINT64)NtfsWofReadU32 (p) |
           ((UINT64)NtfsWofReadU32 (p + 4) << 32);
}

/* Decode one XPRESS-Huffman block as specified by [MS-XCA] section 2.2. */
static EFI_STATUS
NtfsWofXpressDecompress (
    IN  CONST UCHAR *Input,
    IN  ULONG        InputSize,
    OUT UCHAR       *Output,
    IN  ULONG        OutputSize
    )
{
    UCHAR  Lengths[XPRESS_SYMBOLS];
    PUSHORT Table;
    ULONG  Symbol, BitLength, Entry, Repeat;
    ULONG  Position;
    UINT32 NextBits;
    INT32  ExtraBits;
    ULONG  OutPos = 0;

    if (OutputSize == 0) return EFI_SUCCESS;
    if (Input == NULL || Output == NULL || InputSize < 260) return EFI_COMPROMISED_DATA;

    for (Symbol = 0; Symbol < XPRESS_SYMBOLS / 2; Symbol++) {
        Lengths[Symbol * 2]     = Input[Symbol] & 0x0F;
        Lengths[Symbol * 2 + 1] = Input[Symbol] >> 4;
    }

    Table = AllocatePool (XPRESS_TABLE_SIZE * sizeof (USHORT));
    if (Table == NULL) return EFI_OUT_OF_RESOURCES;

    Entry = 0;
    for (BitLength = 1; BitLength <= XPRESS_MAX_CODE_BITS; BitLength++) {
        for (Symbol = 0; Symbol < XPRESS_SYMBOLS; Symbol++) {
            if (Lengths[Symbol] != BitLength) continue;
            Repeat = 1U << (XPRESS_MAX_CODE_BITS - BitLength);
            if (Entry > XPRESS_TABLE_SIZE - Repeat) goto Corrupt;
            while (Repeat-- != 0) Table[Entry++] = (USHORT)Symbol;
        }
    }
    if (Entry != XPRESS_TABLE_SIZE) goto Corrupt;

    Position = 260;
    NextBits = ((UINT32)NtfsWofReadU16 (Input + 256) << 16) |
                (UINT32)NtfsWofReadU16 (Input + 258);
    ExtraBits = 16;

    while (OutPos < OutputSize) {
        ULONG CodeLength, MatchLength, OffsetBits, MatchOffset;

        Symbol = Table[NextBits >> (32 - XPRESS_MAX_CODE_BITS)];
        CodeLength = Lengths[Symbol];
        if (CodeLength == 0) goto Corrupt;

        NextBits <<= CodeLength;
        ExtraBits -= (INT32)CodeLength;
        if (ExtraBits < 0) {
            if (Position > InputSize - 2) goto Corrupt;
            NextBits |= (UINT32)NtfsWofReadU16 (Input + Position) << (ULONG)(-ExtraBits);
            Position += 2;
            ExtraBits += 16;
        }

        if (Symbol < 256) {
            Output[OutPos++] = (UCHAR)Symbol;
            continue;
        }

        Symbol -= 256;
        MatchLength = Symbol & 0x0F;
        OffsetBits  = Symbol >> 4;

        if (MatchLength == 15) {
            if (Position >= InputSize) goto Corrupt;
            MatchLength = Input[Position++];
            if (MatchLength == 255) {
                if (Position > InputSize - 2) goto Corrupt;
                MatchLength = NtfsWofReadU16 (Input + Position);
                Position += 2;
                if (MatchLength < 15) goto Corrupt;
                MatchLength -= 15;
            }
            MatchLength += 15;
        }
        MatchLength += 3;

        MatchOffset = 1U << OffsetBits;
        if (OffsetBits != 0) {
            MatchOffset += NextBits >> (32 - OffsetBits);
            NextBits <<= OffsetBits;
            ExtraBits -= (INT32)OffsetBits;
            if (ExtraBits < 0) {
                if (Position > InputSize - 2) goto Corrupt;
                NextBits |= (UINT32)NtfsWofReadU16 (Input + Position) << (ULONG)(-ExtraBits);
                Position += 2;
                ExtraBits += 16;
            }
        }

        if (MatchOffset > OutPos || MatchLength > OutputSize - OutPos) goto Corrupt;
        while (MatchLength-- != 0) {
            Output[OutPos] = Output[OutPos - MatchOffset];
            OutPos++;
        }
    }

    FreePool (Table);
    return EFI_SUCCESS;

Corrupt:
    FreePool (Table);
    return EFI_COMPROMISED_DATA;
}

/*
 * LZX, the second codec WOF uses (FILE_PROVIDER_COMPRESSION_LZX). CompactOS
 * picks it with `compact /c /exe:LZX`, and it packs noticeably harder than
 * XPRESS - a real shell32.dll goes 7.6 MB to 3.3 MB - at the cost of a much
 * bigger decoder.
 *
 * The stream is LZX as WIM uses it, not as CAB uses it, and the differences
 * are exactly the places where a decoder written from the CAB description
 * falls apart:
 *
 *   - No E8 header. A CAB stream opens with a bit saying whether x86 call
 *     translation is on, and a 32-bit file size if it is. Here the first bits
 *     of a chunk are already a block header.
 *   - Block size is a flag, not a number: one bit set means the block covers
 *     the whole 32 KiB chunk, and only when it is clear does a 16-bit size
 *     follow.
 *   - Call translation is applied unconditionally on the way out, with the
 *     fixed size 12000000 that WIM uses.
 *
 * Each 32 KiB chunk is an independent stream: the window, the three repeated
 * offsets and every code length start over. Matches therefore never reach
 * behind the start of the chunk, which is what makes decoding into the
 * caller's chunk buffer safe.
 */

#define LZX_SLOTS            30U     /* 32 KiB window */
#define LZX_MAIN_SYMS        (256U + LZX_SLOTS * 8U)
#define LZX_LEN_SYMS         249U
#define LZX_ALIGNED_SYMS     8U
#define LZX_PRETREE_SYMS     20U
#define LZX_MAX_CODE_BITS    16U
#define LZX_MIN_MATCH        2U
#define LZX_CHUNK            32768U
#define LZX_E8_FILE_SIZE     12000000

static CONST ULONG NtfsLzxSlotBase[LZX_SLOTS] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192,
    256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192,
    12288, 16384, 24576
};

static CONST UCHAR NtfsLzxSlotBits[LZX_SLOTS] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/*
 * Canonical Huffman without a flat lookup table. A 16-bit main code would need
 * a 128 KB table per tree, three of them per chunk; this keeps the counts, the
 * first code of every length and the symbols sorted by (length, symbol), which
 * is under 1 KB and decodes in at most 16 steps.
 */
typedef struct {
    USHORT Count[LZX_MAX_CODE_BITS + 1];
    ULONG  First[LZX_MAX_CODE_BITS + 1];
    USHORT Index[LZX_MAX_CODE_BITS + 1];
    USHORT Sorted[LZX_MAIN_SYMS];
} NTFS_LZX_TREE;

typedef struct {
    CONST UCHAR *Data;
    ULONG        Size;
    ULONG        Pos;        /* byte offset of the next 16-bit word */
    ULONG        BitBuf;
    ULONG        BitCount;
} NTFS_LZX_BITS;

typedef struct {
    NTFS_LZX_TREE Main;
    NTFS_LZX_TREE Length;
    NTFS_LZX_TREE Aligned;
    NTFS_LZX_TREE Pretree;
    UCHAR         MainLen[LZX_MAIN_SYMS];
    UCHAR         LenLen[LZX_LEN_SYMS];
    UCHAR         AlignedLen[LZX_ALIGNED_SYMS];
    UCHAR         PreLen[LZX_PRETREE_SYMS];
} NTFS_LZX_STATE;

static VOID
NtfsLzxBitsInit (
    OUT NTFS_LZX_BITS *B,
    IN  CONST UCHAR   *Data,
    IN  ULONG          Size
    )
{
    B->Data     = Data;
    B->Size     = Size;
    B->Pos      = 0;
    B->BitBuf   = 0;
    B->BitCount = 0;
}

/* LZX feeds its bit stream 16 little-endian bits at a time, consumed from the
 * top. Reading past the end yields zeroes; every caller bounds its output, so
 * a truncated stream ends as a decode failure rather than a run-on. */
static ULONG
NtfsLzxRead (
    IN OUT NTFS_LZX_BITS *B,
    IN     ULONG          Bits
    )
{
    ULONG Value;

    if (Bits == 0) return 0;
    while (B->BitCount < Bits) {
        ULONG Word = 0;
        if (B->Pos + 2 <= B->Size) {
            Word = (ULONG)B->Data[B->Pos] | ((ULONG)B->Data[B->Pos + 1] << 8);
        } else if (B->Pos < B->Size) {
            Word = (ULONG)B->Data[B->Pos];
        }
        B->Pos     += 2;
        B->BitBuf   = (B->BitBuf << 16) | Word;
        B->BitCount += 16;
    }
    Value       = (B->BitBuf >> (B->BitCount - Bits)) & ((1UL << Bits) - 1);
    B->BitCount -= Bits;
    B->BitBuf   &= (B->BitCount == 32) ? 0xFFFFFFFFUL : ((1UL << B->BitCount) - 1);
    return Value;
}

/* Drop to the next 16-bit boundary. Whole words already pulled into the buffer
 * are handed back to the byte position, so an uncompressed block reads its
 * bytes from where the stream really stands. */
static VOID
NtfsLzxAlign (
    IN OUT NTFS_LZX_BITS *B
    )
{
    while (B->BitCount >= 16) {
        B->Pos      -= 2;
        B->BitCount -= 16;
    }
    B->BitCount = 0;
    B->BitBuf   = 0;
}

static BOOLEAN
NtfsLzxBuildTree (
    IN  CONST UCHAR   *Lengths,
    IN  ULONG          Count,
    OUT NTFS_LZX_TREE *Tree
    )
{
    ULONG Bits, Sym, Code, Total, Next[LZX_MAX_CODE_BITS + 1];

    ZeroMem (Tree->Count, sizeof (Tree->Count));
    for (Sym = 0; Sym < Count; Sym++) {
        if (Lengths[Sym] > LZX_MAX_CODE_BITS) return FALSE;
        Tree->Count[Lengths[Sym]]++;
    }
    Tree->Count[0] = 0;

    Code  = 0;
    Total = 0;
    for (Bits = 1; Bits <= LZX_MAX_CODE_BITS; Bits++) {
        Tree->First[Bits] = Code;
        Tree->Index[Bits] = (USHORT)Total;
        Next[Bits]        = Total;
        Total            += Tree->Count[Bits];
        Code              = (Code + Tree->Count[Bits]) << 1;
        /* an over-subscribed set of lengths is corruption, not a short read */
        if (Code > (1UL << (Bits + 1))) return FALSE;
    }
    if (Total > Count) return FALSE;

    for (Sym = 0; Sym < Count; Sym++) {
        if (Lengths[Sym] != 0) {
            Tree->Sorted[Next[Lengths[Sym]]++] = (USHORT)Sym;
        }
    }
    return TRUE;
}

/* Returns the symbol, or -1 when no code of any length matches. */
static INT32
NtfsLzxDecodeSym (
    IN OUT NTFS_LZX_BITS      *B,
    IN     CONST NTFS_LZX_TREE *Tree
    )
{
    ULONG Bits, Code = 0;

    for (Bits = 1; Bits <= LZX_MAX_CODE_BITS; Bits++) {
        Code = (Code << 1) | NtfsLzxRead (B, 1);
        if (Tree->Count[Bits] != 0) {
            ULONG Offset = Code - Tree->First[Bits];
            if (Offset < Tree->Count[Bits]) {
                return (INT32)Tree->Sorted[Tree->Index[Bits] + Offset];
            }
        }
    }
    return -1;
}

/*
 * Code lengths are stored as a difference from the lengths the previous block
 * left behind, run-length coded, and themselves Huffman-coded with a 20-symbol
 * pretree whose lengths are four raw bits each.
 */
static BOOLEAN
NtfsLzxReadLengths (
    IN OUT NTFS_LZX_BITS  *B,
    IN OUT NTFS_LZX_STATE *S,
    IN OUT UCHAR          *Lengths,
    IN     ULONG           First,
    IN     ULONG           Count
    )
{
    ULONG i, End = First + Count;

    for (i = 0; i < LZX_PRETREE_SYMS; i++) {
        S->PreLen[i] = (UCHAR)NtfsLzxRead (B, 4);
    }
    if (!NtfsLzxBuildTree (S->PreLen, LZX_PRETREE_SYMS, &S->Pretree)) return FALSE;

    i = First;
    while (i < End) {
        INT32 Sym = NtfsLzxDecodeSym (B, &S->Pretree);
        ULONG Run;

        if (Sym < 0) return FALSE;
        if (Sym == 17) {
            Run = NtfsLzxRead (B, 4) + 4;
            while (Run-- != 0 && i < End) Lengths[i++] = 0;
        } else if (Sym == 18) {
            Run = NtfsLzxRead (B, 5) + 20;
            while (Run-- != 0 && i < End) Lengths[i++] = 0;
        } else if (Sym == 19) {
            INT32 Delta;
            UCHAR Value;

            Run   = NtfsLzxRead (B, 1) + 4;
            Delta = NtfsLzxDecodeSym (B, &S->Pretree);
            if (Delta < 0 || Delta > 16) return FALSE;
            Value = (UCHAR)(((ULONG)Lengths[i] + 17 - (ULONG)Delta) % 17);
            while (Run-- != 0 && i < End) Lengths[i++] = Value;
        } else {
            Lengths[i] = (UCHAR)(((ULONG)Lengths[i] + 17 - (ULONG)Sym) % 17);
            i++;
        }
    }
    return TRUE;
}

/*
 * Undo the x86 call translation the compressor applied. Every 0xE8 byte
 * followed by a displacement that fell inside the fixed 12000000-byte span was
 * rewritten from relative to absolute; this puts it back. The last ten bytes
 * are left alone, which is where the compressor stopped as well.
 */
static VOID
NtfsLzxUndoE8 (
    IN OUT UCHAR *Data,
    IN     ULONG  Size
    )
{
    ULONG i = 0;

    if (Size <= 10) return;
    while (i < Size - 10) {
        if (Data[i] == 0xE8) {
            INT32 Value = (INT32)((ULONG)Data[i + 1] | ((ULONG)Data[i + 2] << 8) |
                                  ((ULONG)Data[i + 3] << 16) | ((ULONG)Data[i + 4] << 24));
            if (Value >= -(INT32)i && Value < LZX_E8_FILE_SIZE) {
                INT32 Fixed = (Value >= 0) ? (Value - (INT32)i)
                                           : (Value + LZX_E8_FILE_SIZE);
                Data[i + 1] = (UCHAR)((ULONG)Fixed & 0xFF);
                Data[i + 2] = (UCHAR)(((ULONG)Fixed >> 8) & 0xFF);
                Data[i + 3] = (UCHAR)(((ULONG)Fixed >> 16) & 0xFF);
                Data[i + 4] = (UCHAR)(((ULONG)Fixed >> 24) & 0xFF);
            }
            i += 5;
        } else {
            i++;
        }
    }
}

static EFI_STATUS
NtfsWofLzxDecompress (
    IN  CONST UCHAR *Input,
    IN  ULONG        InputSize,
    OUT UCHAR       *Output,
    IN  ULONG        OutputSize
    )
{
    NTFS_LZX_BITS   Bits;
    NTFS_LZX_STATE *S;
    ULONG           OutPos = 0;
    ULONG           R0 = 1, R1 = 1, R2 = 1;
    EFI_STATUS      Status = EFI_COMPROMISED_DATA;

    if (OutputSize == 0) return EFI_SUCCESS;
    if (Input == NULL || Output == NULL || OutputSize > LZX_CHUNK) return EFI_COMPROMISED_DATA;

    S = AllocateZeroPool (sizeof (NTFS_LZX_STATE));
    if (S == NULL) return EFI_OUT_OF_RESOURCES;
    NtfsLzxBitsInit (&Bits, Input, InputSize);

    while (OutPos < OutputSize) {
        ULONG BlockType = NtfsLzxRead (&Bits, 3);
        ULONG BlockSize;
        ULONG Produced = 0;

        /* one bit for "this block is a whole 32 KiB chunk", else a 16-bit size */
        BlockSize = NtfsLzxRead (&Bits, 1) ? LZX_CHUNK : NtfsLzxRead (&Bits, 16);
        if (BlockSize == 0 || BlockSize > OutputSize - OutPos) {
            BlockSize = OutputSize - OutPos;
        }

        if (BlockType == 3) {
            /* stored: 16-bit alignment, then the three offsets, then raw bytes */
            NtfsLzxAlign (&Bits);
            if (Bits.Pos + 12 + BlockSize > Bits.Size) goto Done;
            R0 = (ULONG)Input[Bits.Pos] | ((ULONG)Input[Bits.Pos + 1] << 8) |
                 ((ULONG)Input[Bits.Pos + 2] << 16) | ((ULONG)Input[Bits.Pos + 3] << 24);
            R1 = (ULONG)Input[Bits.Pos + 4] | ((ULONG)Input[Bits.Pos + 5] << 8) |
                 ((ULONG)Input[Bits.Pos + 6] << 16) | ((ULONG)Input[Bits.Pos + 7] << 24);
            R2 = (ULONG)Input[Bits.Pos + 8] | ((ULONG)Input[Bits.Pos + 9] << 8) |
                 ((ULONG)Input[Bits.Pos + 10] << 16) | ((ULONG)Input[Bits.Pos + 11] << 24);
            Bits.Pos += 12;
            CopyMem (Output + OutPos, Input + Bits.Pos, BlockSize);
            Bits.Pos += BlockSize + (BlockSize & 1);
            OutPos   += BlockSize;
            continue;
        }
        if (BlockType != 1 && BlockType != 2) goto Done;

        if (BlockType == 2) {
            ULONG i;
            for (i = 0; i < LZX_ALIGNED_SYMS; i++) {
                S->AlignedLen[i] = (UCHAR)NtfsLzxRead (&Bits, 3);
            }
            if (!NtfsLzxBuildTree (S->AlignedLen, LZX_ALIGNED_SYMS, &S->Aligned)) goto Done;
        }
        if (!NtfsLzxReadLengths (&Bits, S, S->MainLen, 0, 256)) goto Done;
        if (!NtfsLzxReadLengths (&Bits, S, S->MainLen, 256, LZX_MAIN_SYMS - 256)) goto Done;
        if (!NtfsLzxReadLengths (&Bits, S, S->LenLen, 0, LZX_LEN_SYMS)) goto Done;
        if (!NtfsLzxBuildTree (S->MainLen, LZX_MAIN_SYMS, &S->Main)) goto Done;
        if (!NtfsLzxBuildTree (S->LenLen, LZX_LEN_SYMS, &S->Length)) goto Done;

        while (Produced < BlockSize && OutPos < OutputSize) {
            INT32 Sym = NtfsLzxDecodeSym (&Bits, &S->Main);
            ULONG Slot, LenHeader, MatchLen, Offset, i;

            if (Sym < 0) goto Done;
            if (Sym < 256) {
                Output[OutPos++] = (UCHAR)Sym;
                Produced++;
                continue;
            }

            Sym      -= 256;
            LenHeader = (ULONG)Sym & 7;
            Slot      = (ULONG)Sym >> 3;
            if (Slot >= LZX_SLOTS) goto Done;

            if (LenHeader == 7) {
                INT32 Extra = NtfsLzxDecodeSym (&Bits, &S->Length);
                if (Extra < 0) goto Done;
                MatchLen = (ULONG)Extra + 7 + LZX_MIN_MATCH;
            } else {
                MatchLen = LenHeader + LZX_MIN_MATCH;
            }

            if (Slot <= 2) {
                if (Slot == 0) {
                    Offset = R0;
                } else if (Slot == 1) {
                    Offset = R1; R1 = R0; R0 = Offset;
                } else {
                    Offset = R2; R2 = R0; R0 = Offset;
                }
            } else {
                ULONG Footer = NtfsLzxSlotBits[Slot];
                ULONG Formatted;

                if (BlockType == 2 && Footer >= 3) {
                    INT32 Aligned;
                    Formatted = NtfsLzxSlotBase[Slot] + (NtfsLzxRead (&Bits, Footer - 3) << 3);
                    Aligned   = NtfsLzxDecodeSym (&Bits, &S->Aligned);
                    if (Aligned < 0) goto Done;
                    Formatted += (ULONG)Aligned;
                } else {
                    Formatted = NtfsLzxSlotBase[Slot] + NtfsLzxRead (&Bits, Footer);
                }
                if (Formatted < 2) goto Done;
                Offset = Formatted - 2;
                R2 = R1; R1 = R0; R0 = Offset;
            }

            /* a chunk is its own window: a match can only reach back into what
             * this chunk has already produced */
            if (Offset == 0 || Offset > OutPos) goto Done;
            if (MatchLen > OutputSize - OutPos) MatchLen = OutputSize - OutPos;
            for (i = 0; i < MatchLen; i++) {
                Output[OutPos] = Output[OutPos - Offset];
                OutPos++;
            }
            Produced += MatchLen;
        }
    }

    NtfsLzxUndoE8 (Output, OutputSize);
    Status = EFI_SUCCESS;

Done:
    FreePool (S);
    return Status;
}

static EFI_STATUS
NtfsWofGetAlgorithm (
    IN  PNTFS_EFI_VCB       Vcb,
    IN  PFILE_RECORD_HEADER Record,
    OUT ULONG              *Algorithm
    )
{
    PNTFS_ATTR_CTX RpCtx;
    NTFS_WOF_REPARSE_DATA Rp;

    RpCtx = NtfsEfiFindAttribute (Vcb, Record, AttributeReparsePoint, NULL, 0, NULL);
    if (RpCtx == NULL) {
        Print (L"[wof] no $REPARSE_POINT attribute\n");
        return EFI_UNSUPPORTED;
    }
    Print (L"[wof] reparse length=%ld\n", NtfsEfiAttrDataLength (RpCtx));
    if (NtfsEfiAttrDataLength (RpCtx) < sizeof (Rp) ||
        NtfsEfiReadAttr (Vcb, RpCtx, 0, (PCHAR)&Rp, sizeof (Rp)) != sizeof (Rp)) {
        NtfsEfiFreeAttrCtx (RpCtx);
        return EFI_VOLUME_CORRUPTED;
    }
    NtfsEfiFreeAttrCtx (RpCtx);

    Print (L"[wof] tag=%08x ver=%d provider=%d filever=%d algorithm=%d\n",
        Rp.ReparseTag, Rp.WofVersion, Rp.Provider, Rp.FileVersion, Rp.Algorithm);

    if (Rp.ReparseTag != IO_REPARSE_TAG_WOF || Rp.WofVersion != 1 ||
        Rp.Provider != WOF_PROVIDER_FILE || Rp.FileVersion != 1) return EFI_UNSUPPORTED;
    if (Rp.Algorithm != FILE_PROVIDER_COMPRESSION_XPRESS4K &&
        Rp.Algorithm != FILE_PROVIDER_COMPRESSION_XPRESS8K &&
        Rp.Algorithm != FILE_PROVIDER_COMPRESSION_XPRESS16K &&
        Rp.Algorithm != FILE_PROVIDER_COMPRESSION_LZX) return EFI_UNSUPPORTED;
    *Algorithm = Rp.Algorithm;
    return EFI_SUCCESS;
}

EFI_STATUS
NtfsEfiReadWofAttr (
    IN  PNTFS_EFI_VCB       Vcb,
    IN  PFILE_RECORD_HEADER Record,
    IN  UINT64              FileSize,
    IN  UINT64              Offset,
    OUT PCHAR               Buffer,
    IN  ULONG               Length,
    OUT ULONG              *BytesRead
    )
{
    static CONST WCHAR WofStreamName[] = L"WofCompressedData";
    PNTFS_ATTR_CTX WofCtx;
    EFI_STATUS Status;
    ULONG Algorithm, ChunkSize, EntrySize;
    UINT64 ChunkCount, TableSize, StreamSize;
    PUCHAR Compressed = NULL;
    PUCHAR Decompressed = NULL;
    ULONG Done = 0;

    if (BytesRead == NULL) return EFI_INVALID_PARAMETER;
    *BytesRead = 0;
    if (Length == 0 || Offset >= FileSize) return EFI_SUCCESS;

    Status = NtfsWofGetAlgorithm (Vcb, Record, &Algorithm);
    if (EFI_ERROR (Status)) return Status;
    ChunkSize = (Algorithm == FILE_PROVIDER_COMPRESSION_XPRESS4K)  ? 4096U :
                (Algorithm == FILE_PROVIDER_COMPRESSION_XPRESS8K)  ? 8192U :
                (Algorithm == FILE_PROVIDER_COMPRESSION_XPRESS16K) ? 16384U : LZX_CHUNK;
    ChunkCount = (FileSize + ChunkSize - 1) / ChunkSize;
    EntrySize  = (FileSize > 0xFFFFFFFFULL) ? 8U : 4U;
    TableSize  = (ChunkCount > 0 ? ChunkCount - 1 : 0) * EntrySize;

    WofCtx = NtfsEfiFindAttribute (Vcb, Record, AttributeData,
                                    WofStreamName,
                                    (USHORT)(sizeof (WofStreamName) / sizeof (WCHAR) - 1), NULL);
    if (WofCtx == NULL) {
        Print (L"[wof] no :WofCompressedData stream\n");
        return EFI_UNSUPPORTED;
    }
    StreamSize = NtfsEfiAttrDataLength (WofCtx);
    Print (L"[wof] file=%ld algorithm=%d chunk=%d chunks=%ld table=%ld stream=%ld\n",
        FileSize, Algorithm, ChunkSize, ChunkCount, TableSize, StreamSize);
    if (TableSize > StreamSize) { Status = EFI_VOLUME_CORRUPTED; goto Done; }

    /* the chunk read is widened to whole sectors, so the buffer carries one
     * sector of slack at each end */
    Compressed = AllocatePool (ChunkSize + 2 * Vcb->BytesPerSector);
    Decompressed = AllocatePool (ChunkSize);
    if (Compressed == NULL || Decompressed == NULL) {
        Status = EFI_OUT_OF_RESOURCES; goto Done;
    }

    while (Done < Length && Offset + Done < FileSize) {
        UINT64 Current = Offset + Done;
        UINT64 Chunk = Current / ChunkSize;
        ULONG Within = (ULONG)(Current % ChunkSize);
        ULONG PlainSize = (ULONG)min ((UINT64)ChunkSize, FileSize - Chunk * ChunkSize);
        UINT64 StartRel = 0;
        UINT64 EndRel = StreamSize - TableSize;
        UINT64 Start, End;
        ULONG StoredSize, Take;
        UCHAR EntryBuf[8];

        if (Chunk > 0) {
            UINT64 EntryOffset = (Chunk - 1) * EntrySize;
            if (NtfsEfiReadAttr (Vcb, WofCtx, EntryOffset, (PCHAR)EntryBuf, EntrySize) != EntrySize) {
                Status = EFI_DEVICE_ERROR; goto Done;
            }
            StartRel = (EntrySize == 4) ? NtfsWofReadU32 (EntryBuf) : NtfsWofReadU64 (EntryBuf);
        }
        if (Chunk + 1 < ChunkCount) {
            UINT64 EntryOffset = Chunk * EntrySize;
            if (NtfsEfiReadAttr (Vcb, WofCtx, EntryOffset, (PCHAR)EntryBuf, EntrySize) != EntrySize) {
                Status = EFI_DEVICE_ERROR; goto Done;
            }
            EndRel = (EntrySize == 4) ? NtfsWofReadU32 (EntryBuf) : NtfsWofReadU64 (EntryBuf);
        }
        if (StartRel > EndRel || EndRel > StreamSize - TableSize) {
            Status = EFI_VOLUME_CORRUPTED; goto Done;
        }

        Start = TableSize + StartRel;
        End = TableSize + EndRel;
        if (End - Start > ChunkSize) { Status = EFI_VOLUME_CORRUPTED; goto Done; }
        StoredSize = (ULONG)(End - Start);
        Print (L"[wof] chunk=%ld plain=%d stored=%d start=%ld end=%ld\n",
            Chunk, PlainSize, StoredSize, Start, End);
        /*
         * Read the chunk on sector boundaries. A chunk begins wherever the
         * previous one ended, so its offset inside the stream is arbitrary,
         * and an unaligned span that crosses sectors is a request some
         * firmware DiskIo implementations refuse outright - Hyper-V returns
         * an error rather than a short read, and the chunk then looks like a
         * corrupt one. Widening to whole sectors makes every request the kind
         * the firmware is happiest with, and costs one CopyMem.
         */
        {
            UINT64 SecMask   = Vcb->BytesPerSector - 1;
            UINT64 AlignStart = Start & ~SecMask;
            UINT64 AlignEnd   = (End + SecMask) & ~SecMask;
            ULONG  Skew       = (ULONG)(Start - AlignStart);
            ULONG  Span       = (ULONG)(AlignEnd - AlignStart);
            ULONG  GotBytes;

            if (StoredSize == 0) { Status = EFI_VOLUME_CORRUPTED; goto Done; }
            if (AlignEnd > StreamSize) {
                AlignEnd = StreamSize;
                Span     = (ULONG)(AlignEnd - AlignStart);
            }
            GotBytes = NtfsEfiReadAttr (Vcb, WofCtx, AlignStart, (PCHAR)Compressed, Span);
            if (GotBytes < Skew + StoredSize) { Status = EFI_DEVICE_ERROR; goto Done; }
            if (Skew != 0) {
                CopyMem (Compressed, Compressed + Skew, StoredSize);
            }
        }

        if (StoredSize == PlainSize) CopyMem (Decompressed, Compressed, PlainSize);
        else if (Algorithm == FILE_PROVIDER_COMPRESSION_LZX) {
            Status = NtfsWofLzxDecompress (Compressed, StoredSize, Decompressed, PlainSize);
            if (EFI_ERROR (Status)) {
                Print (L"[wof] LZX decode failed: %r\n", Status);
                goto Done;
            }
        } else {
            Status = NtfsWofXpressDecompress (Compressed, StoredSize, Decompressed, PlainSize);
            if (EFI_ERROR (Status)) {
                Print (L"[wof] XPRESS decode failed: %r\n", Status);
                goto Done;
            }
        }

        Take = (ULONG)min ((UINT64)(PlainSize - Within), (UINT64)(Length - Done));
        CopyMem (Buffer + Done, Decompressed + Within, Take);
        Done += Take;
    }
    Status = EFI_SUCCESS;

Done:
    if (Compressed) FreePool (Compressed);
    if (Decompressed) FreePool (Decompressed);
    NtfsEfiFreeAttrCtx (WofCtx);
    *BytesRead = Done;
    return Status;
}
