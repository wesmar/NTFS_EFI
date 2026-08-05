/**
 * ntfs_wof.c - read-only support for WOF file-provider compression.
 *
 * Windows CompactOS stores the unnamed $DATA as a sparse placeholder and
 * keeps the real bytes in the named :WofCompressedData stream. The stream
 * begins with a chunk-offset table followed by independently compressed
 * XPRESS-Huffman chunks. This is a clean implementation of the format
 * documented by Microsoft in [MS-XCA] and FILE_PROVIDER_EXTERNAL_INFO_V1.
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
        Rp.Algorithm != FILE_PROVIDER_COMPRESSION_XPRESS16K) return EFI_UNSUPPORTED;
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
    ChunkSize = (Algorithm == FILE_PROVIDER_COMPRESSION_XPRESS4K) ? 4096U :
                (Algorithm == FILE_PROVIDER_COMPRESSION_XPRESS8K) ? 8192U : 16384U;
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

    Compressed = AllocatePool (ChunkSize);
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
        if (StoredSize == 0 ||
            NtfsEfiReadAttr (Vcb, WofCtx, Start, (PCHAR)Compressed, StoredSize) != StoredSize) {
            Status = EFI_DEVICE_ERROR; goto Done;
        }

        if (StoredSize == PlainSize) CopyMem (Decompressed, Compressed, PlainSize);
        else {
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
