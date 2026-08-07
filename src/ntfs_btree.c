/**
 * ntfs_btree.c - directory lookup (B+tree index traversal).
 *
 * Two traversal strategies share the same on-disk data:
 *  - NtfsEfiScanIndexBlock/NtfsEfiBrowseSubNode: exhaustive, visits every
 *    entry and every sub-node in order. Needed for full directory
 *    enumeration (DirSearch == TRUE, i.e. EFI_FILE_PROTOCOL.Read() on a
 *    directory handle) since listing "everything" is inherently O(n).
 *  - NtfsEfiSearchIndexBlock/NtfsEfiSearchSubNode: directed single-branch
 *    B+tree descent for exact-match Open() lookups (DirSearch == FALSE).
 *    NTFS index entries within a node are collation-sorted, so comparing
 *    against the search key picks exactly one child subtree to descend
 *    into, like any other B+tree - O(log n) instead of O(n). This matters:
 *    a large real-world directory (\Windows\System32, thousands of
 *    entries) made the exhaustive walk impractically slow. Confirmed
 *    against the independent EFI-native reference driver
 *    maharmstone/ntfs-efi, which uses the same cmp==0/cmp<0/cmp>0 shape.
 */

#include "ntfs.h"

/*
 * Case-insensitive wide-string comparison of exactly Len characters, using
 * the real on-disk $UpCase table (Upcase[c] = uppercase form of c) instead
 * of an ASCII-only fold - this is what makes accented / non-Latin names
 * (e.g. polskie znaki: A, C, E, L, N, O, S, Z, Z) compare correctly.
 * Returns 0 if equal, <0 / >0 otherwise (same contract as StrnCmp but ignores case).
 */
static INTN
NtfsEfiWcsniCmp (IN CONST WCHAR *A, IN CONST WCHAR *B, IN UINTN Len, IN CONST USHORT *Upcase)
{
    UINTN i;
    for (i = 0; i < Len; i++) {
        WCHAR Ca = (WCHAR)Upcase[(USHORT)A[i]];
        WCHAR Cb = (WCHAR)Upcase[(USHORT)B[i]];
        if (Ca != Cb) return (INTN)Ca - (INTN)Cb;
    }
    return 0;
}

/*
 * Match a component name against an index entry.
 * DirSearch = FALSE -> exact match (used during open-path lookup).
 * DirSearch = TRUE  -> wildcard match not implemented; treated as exact.
 */
static BOOLEAN
NtfsEfiMatchEntry (
    IN CONST WCHAR              *Name,
    IN UINTN                     NameLen,
    IN PINDEX_ENTRY_ATTRIBUTE    Entry,
    IN BOOLEAN                   CaseSensitive,
    IN CONST USHORT              *Upcase
    )
{
    UINTN EntryLen = Entry->FileName.NameLength;
    if (EntryLen != NameLen) return FALSE;
    if (CaseSensitive)
        return (CompareMem (Name, Entry->FileName.Name, NameLen * sizeof (WCHAR)) == 0);
    return (NtfsEfiWcsniCmp (Name, Entry->FileName.Name, NameLen, Upcase) == 0);
}

/* Scan a flat array of index entries (from INDEX_ROOT or INDEX_BUFFER).
 * Returns MFT index on success, MAX_UINT64 on miss.
 * *StartEntry / *CurrentEntry implement the sequential directory enumeration
 * position: during DirSearch each matching non-DOS entry increments *CurrentEntry.
 */
static ULONGLONG
NtfsEfiScanIndexBlock (
    IN     PINDEX_ENTRY_ATTRIBUTE  First,
    IN     PINDEX_ENTRY_ATTRIBUTE  Last,
    IN     CONST WCHAR            *Name,
    IN     UINTN                   NameLen,
    IN     BOOLEAN                 DirSearch,
    IN     BOOLEAN                 CaseSensitive,
    IN     CONST USHORT           *Upcase,
    IN OUT ULONG                  *StartEntry,
    IN OUT ULONG                  *CurrentEntry
    );

/* Forward declaration for mutual recursion with sub-node traversal */
static ULONGLONG
NtfsEfiBrowseSubNode (
    IN     PNTFS_EFI_VCB      Vcb,
    IN     PNTFS_ATTR_CTX     IndexAllocCtx,
    IN     PUCHAR              Bitmap,
    IN     ULONG               BitmapBits,
    IN     ULONG               ClustPerBlock,
    IN     ULONGLONG           VCN,
    IN     CONST WCHAR        *Name,
    IN     UINTN               NameLen,
    IN     BOOLEAN             DirSearch,
    IN     BOOLEAN             CaseSensitive,
    IN OUT ULONG              *StartEntry,
    IN OUT ULONG              *CurrentEntry,
    IN     ULONG               Depth
    );

/*
 * Directed (single-branch) B+tree lookup, used for exact-match Open()
 * lookups (DirSearch == FALSE).
 */
static ULONGLONG
NtfsEfiSearchSubNode (
    IN PNTFS_EFI_VCB      Vcb,
    IN PNTFS_ATTR_CTX     IndexAllocCtx,
    IN PUCHAR             Bitmap,
    IN ULONG              BitmapBits,
    IN ULONG              ClustPerBlock,
    IN ULONGLONG          VCN,
    IN CONST WCHAR       *Name,
    IN UINTN              NameLen,
    IN BOOLEAN            CaseSensitive,
    IN ULONG              Depth
    );

/*
 * Depth cap for every recursive INDX descent below.
 *
 * A B+tree node's child pointers are on-disk VCNs with nothing stopping a
 * corrupt (or deliberately crafted) index from pointing a node at itself or at
 * an ancestor. Each recursion level allocates a BytesPerIndexRecord pool buffer
 * and a stack frame, so an index cycle otherwise recurses until the DXE stack
 * or the pool is exhausted - a hang/crash reachable by merely LISTING a
 * directory. The write-side walkers in ntfs_delete.c already cap at 32; this
 * gives the read side the same guard. A real NTFS directory index is a handful
 * of levels deep even with millions of entries.
 */
#define NTFS_MAX_INDEX_DEPTH 32

/* Case-aware ordering compare of two (possibly different-length) names,
 * matching NTFS's "shorter-is-less-when-prefix-equal" collation rule. */
static INTN
NtfsEfiCompareNames (
    IN CONST WCHAR *A, IN UINTN ALen,
    IN CONST WCHAR *B, IN UINTN BLen,
    IN BOOLEAN      CaseSensitive,
    IN CONST USHORT *Upcase
    )
{
    UINTN i;
    UINTN MinLen = (ALen < BLen) ? ALen : BLen;

    for (i = 0; i < MinLen; i++) {
        WCHAR Ca = A[i];
        WCHAR Cb = B[i];
        if (!CaseSensitive) {
            Ca = (WCHAR)Upcase[(USHORT)Ca];
            Cb = (WCHAR)Upcase[(USHORT)Cb];
        }
        if (Ca != Cb) return (INTN)Ca - (INTN)Cb;
    }
    if (ALen != BLen) return (ALen < BLen) ? -1 : 1;
    return 0;
}

static ULONGLONG
NtfsEfiSearchIndexBlock (
    IN PINDEX_ENTRY_ATTRIBUTE First,
    IN PINDEX_ENTRY_ATTRIBUTE Last,
    IN CONST WCHAR           *Name,
    IN UINTN                  NameLen,
    IN BOOLEAN                CaseSensitive,
    IN PNTFS_EFI_VCB          Vcb,            /* NULL if this node has no subnodes to descend into */
    IN PNTFS_ATTR_CTX         IndexAllocCtx,
    IN PUCHAR                 Bitmap,
    IN ULONG                  BitmapBits,
    IN ULONG                  ClustPerBlock,
    IN ULONG                  Depth
    )
{
    PINDEX_ENTRY_ATTRIBUTE Entry = First;

    while ((PUCHAR)Entry < (PUCHAR)Last) {
        /*
         * A dummy/terminal END entry legitimately omits the FileName
         * payload and can be shorter than sizeof(INDEX_ENTRY_ATTRIBUTE) -
         * only reject truly-zero-length entries (which would spin the
         * loop forever), and check END/NODE before anything else.
         */
        if (Entry->Length == 0) break;
        if ((PUCHAR)Entry + Entry->Length > (PUCHAR)Last) break;

        if (Entry->Flags & NTFS_INDEX_ENTRY_END) {
            Print (L"[ntfs] Search: hit END, NODE=%d IndexAllocCtx=%p\n",
                (Entry->Flags & NTFS_INDEX_ENTRY_NODE) != 0, IndexAllocCtx);
            if ((Entry->Flags & NTFS_INDEX_ENTRY_NODE) && IndexAllocCtx != NULL &&
                Entry->Length >= sizeof (ULONGLONG)) {
                ULONGLONG SubVCN = *(PULONGLONG)
                    ((PUCHAR)Entry + Entry->Length - sizeof (ULONGLONG));
                return NtfsEfiSearchSubNode (Vcb, IndexAllocCtx, Bitmap, BitmapBits,
                            ClustPerBlock, SubVCN, Name, NameLen, CaseSensitive, Depth);
            }
            return (ULONGLONG)-1LL;
        }

        {
            /*
             * Unlike enumeration (NtfsEfiScanIndexBlock, which must skip
             * DOS-only 8.3 alias entries or every long-named file with a
             * generated short name would be listed twice), an exact-match
             * Open() lookup should also accept a match against a DOS-only
             * alias: opening "PROGRA~1" must resolve to "Program Files"
             * just like real Windows does, regardless of which name form
             * the caller happened to use.
             */
            BOOLEAN IsRealEntry =
                (Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE;
            INTN Cmp;
            /* the key is compared below: it must lie inside this entry */
            if ((UINT64)FIELD_OFFSET (INDEX_ENTRY_ATTRIBUTE, FileName.Name) +
                    (UINT64)Entry->FileName.NameLength * sizeof (WCHAR) > (UINT64)Entry->Length) {
                break;
            }
            Cmp = NtfsEfiCompareNames (Name, NameLen,
                            Entry->FileName.Name, Entry->FileName.NameLength,
                            CaseSensitive, Vcb->UpcaseTable);
            /* {
                CHAR16 EBuf[40];
                UINTN  ECopy = (Entry->FileName.NameLength < 39) ? Entry->FileName.NameLength : 39;
                CopyMem (EBuf, Entry->FileName.Name, ECopy * sizeof (CHAR16));
                EBuf[ECopy] = L'\0';
                Print (L"[ntfs] Search: vs '%s' (mft=%ld,type=%d) Cmp=%d Real=%d Flags=%d Len=%d\n",
                    EBuf, Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK,
                    Entry->FileName.NameType, Cmp, IsRealEntry, Entry->Flags, Entry->Length);
            } */

            if (Cmp == 0 && IsRealEntry) {
                return (Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
            }
            if (Cmp < 0) {
                /* Target sorts before this entry: it can only be in the
                 * subtree of keys smaller than this entry, if any. */
                if ((Entry->Flags & NTFS_INDEX_ENTRY_NODE) && IndexAllocCtx != NULL &&
                    Entry->Length >= sizeof (ULONGLONG)) {
                    ULONGLONG SubVCN = *(PULONGLONG)
                        ((PUCHAR)Entry + Entry->Length - sizeof (ULONGLONG));
                    return NtfsEfiSearchSubNode (Vcb, IndexAllocCtx, Bitmap, BitmapBits,
                                ClustPerBlock, SubVCN, Name, NameLen, CaseSensitive, Depth);
                }
                return (ULONGLONG)-1LL;
            }
            /* Cmp > 0 (or Cmp == 0 but filtered, e.g. a DOS-only alias):
             * target is not in this entry's smaller-keys subtree either -
             * move on to the next entry in this same node. */
        }
        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
    }
    return (ULONGLONG)-1LL;
}

static ULONGLONG
NtfsEfiSearchSubNode (
    IN PNTFS_EFI_VCB      Vcb,
    IN PNTFS_ATTR_CTX     IndexAllocCtx,
    IN PUCHAR             Bitmap,
    IN ULONG              BitmapBits,
    IN ULONG              ClustPerBlock,
    IN ULONGLONG          VCN,
    IN CONST WCHAR       *Name,
    IN UINTN              NameLen,
    IN BOOLEAN            CaseSensitive,
    IN ULONG              Depth
    )
{
    ULONG                  NodeNumber = (ULONG)(VCN / ClustPerBlock);
    PUCHAR                 IndexBuf;
    PINDEX_BUFFER          Block;
    PINDEX_ENTRY_ATTRIBUTE First, Last;
    ULONGLONG              Result;

    if (Depth >= NTFS_MAX_INDEX_DEPTH) return (ULONGLONG)-1LL;

    Print (L"[ntfs] SearchSubNode: VCN=%ld ClustPerBlock=%d NodeNumber=%d Bitmap=%p BitmapBits=%d\n",
        VCN, ClustPerBlock, NodeNumber, Bitmap, BitmapBits);

    if (Bitmap != NULL && BitmapBits > 0 && NodeNumber < BitmapBits) {
        if (!((Bitmap[NodeNumber / 8] >> (NodeNumber % 8)) & 1)) {
            Print (L"[ntfs] SearchSubNode: bitmap says node %d NOT in use, bailing\n", NodeNumber);
            return (ULONGLONG)-1LL;
        }
    }

    IndexBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (IndexBuf == NULL) return (ULONGLONG)-1LL;

    {
        ULONG RdBytes = NtfsEfiReadAttr (Vcb, IndexAllocCtx,
                              VCN * Vcb->BytesPerCluster,
                              (PCHAR)IndexBuf,
                              Vcb->BytesPerIndexRecord);
        if (RdBytes != Vcb->BytesPerIndexRecord) {
            Print (L"[ntfs] SearchSubNode: ReadAttr short read %d != %d\n", RdBytes, Vcb->BytesPerIndexRecord);
            FreePool (IndexBuf); return (ULONGLONG)-1LL;
        }
    }
    Block = (PINDEX_BUFFER)IndexBuf;
    if (Block->Ntfs.Type != NRH_INDX_TYPE) {
        Print (L"[ntfs] SearchSubNode: bad signature %08x\n", Block->Ntfs.Type);
        FreePool (IndexBuf); return (ULONGLONG)-1LL;
    }
    if (EFI_ERROR (NtfsEfiFixupRecord (Vcb, &((PFILE_RECORD_HEADER)Block)->Ntfs)) ||
        !NtfsEfiIndexBlockOk (Vcb, Block)) {
        Print (L"[ntfs] SearchSubNode: bad fixup/index header\n");
        FreePool (IndexBuf); return (ULONGLONG)-1LL;
    }

    First = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
    Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries);

    Result = NtfsEfiSearchIndexBlock (First, Last, Name, NameLen, CaseSensitive,
                Vcb, IndexAllocCtx, Bitmap, BitmapBits, ClustPerBlock, Depth + 1);

    FreePool (IndexBuf);
    return Result;
}

ULONGLONG
NtfsEfiFindInDirectory (
    IN     PNTFS_EFI_VCB      Vcb,
    IN     ULONGLONG           DirMFTIndex,
    IN     CONST WCHAR        *Name,        /* component only, no backslashes */
    IN     UINTN               NameLen,
    IN     BOOLEAN             DirSearch,
    IN     BOOLEAN             CaseSensitive,
    IN OUT ULONG              *StartEntry   /* for sequential dir enumeration  */
    )
{
    PFILE_RECORD_HEADER DirRecord;
    PNTFS_ATTR_CTX      IndexRootCtx;
    PNTFS_ATTR_CTX      IndexAllocCtx;
    PNTFS_ATTR_CTX      BitmapCtx;
    PUCHAR              IndexBuf;
    PUCHAR              BitmapBuf;
    PINDEX_ROOT_ATTRIBUTE IndexRoot;
    PINDEX_ENTRY_ATTRIBUTE First, Last;
    ULONG               CurrentEntry = 0;
    ULONG               ClustPerBlock;
    ULONGLONG           Result = (ULONGLONG)-1LL;

    DirRecord = AllocatePool (Vcb->BytesPerFileRecord);
    if (DirRecord == NULL) return (ULONGLONG)-1LL;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, DirMFTIndex, DirRecord))) {
        FreePool (DirRecord); return (ULONGLONG)-1LL;
    }

    IndexRootCtx = NtfsEfiFindAttribute (Vcb, DirRecord,
                        AttributeIndexRoot, L"$I30", 4, NULL);
    if (IndexRootCtx == NULL) { FreePool (DirRecord); return (ULONGLONG)-1LL; }

    IndexBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (IndexBuf == NULL) {
        NtfsEfiFreeAttrCtx (IndexRootCtx); FreePool (DirRecord);
        return (ULONGLONG)-1LL;
    }
    {
        ULONG RootBytes = NtfsEfiReadAttr (Vcb, IndexRootCtx, 0, (PCHAR)IndexBuf,
                                          Vcb->BytesPerIndexRecord);
        NtfsEfiFreeAttrCtx (IndexRootCtx);

        IndexRoot = (PINDEX_ROOT_ATTRIBUTE)IndexBuf;
        /* validate against what was actually read, not the buffer size */
        if (RootBytes <= (ULONG)FIELD_OFFSET (INDEX_ROOT_ATTRIBUTE, Header) ||
            !NtfsEfiIndexHeaderOk (&IndexRoot->Header,
                (UINT64)RootBytes - FIELD_OFFSET (INDEX_ROOT_ATTRIBUTE, Header))) {
            FreePool (IndexBuf); FreePool (DirRecord);
            return (ULONGLONG)-1LL;
        }
    }
    /*
     * BOTH offsets are relative to the INDEX_HEADER, not to the start of the
     * $INDEX_ROOT value. Last used to be computed from IndexBuf, i.e. 16 bytes
     * (the AttributeType/CollationRule/SizeOfEntry/ClustersPerIndexRecord
     * prefix) short of where the entries really end - every other index walk in
     * the driver uses &Header. With a 16-byte END entry the walk then stopped
     * one entry early: for a small-but-large-index directory that meant never
     * descending through the END entry's sub-node pointer, so a name that sorts
     * after everything in the root read back as EFI_NOT_FOUND.
     */
    First     = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header
                    + IndexRoot->Header.FirstEntryOffset);
    Last      = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IndexRoot->Header
                    + IndexRoot->Header.TotalSizeOfEntries);

    Print (L"[ntfs] FindInDirectory: DirMFT=%ld IndexBuf=%p HeaderAt=%p FirstEntryOffset=%d TotalSizeOfEntries=%d AllocatedSize=%d First=%p Last=%p\n",
        DirMFTIndex, IndexBuf, &IndexRoot->Header,
        IndexRoot->Header.FirstEntryOffset, IndexRoot->Header.TotalSizeOfEntries,
        IndexRoot->Header.AllocatedSize, First, Last);

    /* Try to find in $INDEX_ROOT before touching $INDEX_ALLOCATION */
    IndexAllocCtx = NtfsEfiFindAttribute (Vcb, DirRecord,
                        AttributeIndexAllocation, L"$I30", 4, NULL);
    BitmapBuf = NULL;
    BitmapCtx = NULL;
    ClustPerBlock = Vcb->BytesPerIndexRecord / Vcb->BytesPerCluster;

    Print (L"[ntfs] FindInDirectory: IndexAllocCtx=%p\n", IndexAllocCtx);

    if (IndexAllocCtx != NULL) {
        UINT64 BitmapLen;
        BitmapCtx = NtfsEfiFindAttribute (Vcb, DirRecord,
                        AttributeBitmap, L"$I30", 4, NULL);
        Print (L"[ntfs] FindInDirectory: BitmapCtx=%p\n", BitmapCtx);
        if (BitmapCtx != NULL) {
            BitmapLen = NtfsEfiAttrDataLength (BitmapCtx);
            Print (L"[ntfs] FindInDirectory: BitmapLen=%ld\n", BitmapLen);
            BitmapBuf = AllocateZeroPool ((UINTN)BitmapLen + sizeof (ULONG));
            if (BitmapBuf != NULL)
                NtfsEfiReadAttr (Vcb, BitmapCtx, 0, (PCHAR)BitmapBuf, (ULONG)BitmapLen);
            NtfsEfiFreeAttrCtx (BitmapCtx);
        }
    }

    if (DirSearch) {
        /* Full enumeration needs to visit every entry - can't shortcut. */
        Result = NtfsEfiScanIndexBlock (First, Last, Name, NameLen,
                                         DirSearch, CaseSensitive, Vcb->UpcaseTable,
                                         StartEntry, &CurrentEntry);

        if (Result == (ULONGLONG)-1LL && IndexAllocCtx != NULL) {
            PINDEX_ENTRY_ATTRIBUTE Entry = First;
            while ((PUCHAR)Entry < (PUCHAR)Last) {
                if (Entry->Length == 0) break;
                if ((Entry->Flags & NTFS_INDEX_ENTRY_NODE) &&
                    Entry->Length >= sizeof (ULONGLONG))
                {
                    ULONGLONG SubVCN = *(PULONGLONG)
                        ((PUCHAR)Entry + Entry->Length - sizeof (ULONGLONG));
                    ULONG BitmapBits = BitmapBuf ? (ULONG)(NtfsEfiAttrDataLength (IndexAllocCtx)
                                        / (ClustPerBlock * Vcb->BytesPerCluster)) : 0;
                    Result = NtfsEfiBrowseSubNode (Vcb, IndexAllocCtx,
                                BitmapBuf, BitmapBits, ClustPerBlock,
                                SubVCN, Name, NameLen,
                                DirSearch, CaseSensitive,
                                StartEntry, &CurrentEntry, 0);
                    if (Result != (ULONGLONG)-1LL) break;
                }
                if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;
                Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
            }
        }
    } else {
        /* Exact-match Open() lookup: directed single-branch B+tree descent. */
        ULONG BitmapBits = (BitmapBuf && IndexAllocCtx) ?
            (ULONG)(NtfsEfiAttrDataLength (IndexAllocCtx) / (ClustPerBlock * Vcb->BytesPerCluster)) : 0;
        Result = NtfsEfiSearchIndexBlock (First, Last, Name, NameLen, CaseSensitive,
                    Vcb, IndexAllocCtx, BitmapBuf, BitmapBits, ClustPerBlock, 0);
    }

    if (BitmapBuf)     FreePool (BitmapBuf);
    if (IndexAllocCtx) NtfsEfiFreeAttrCtx (IndexAllocCtx);
    FreePool (IndexBuf);
    FreePool (DirRecord);
    return Result;
}

/* ---- O(n) full directory collection (in-order B+tree walk) ---------------- */

static VOID NtfsCollectSub (PNTFS_EFI_VCB Vcb, PNTFS_ATTR_CTX Alloc,
                            ULONG ClustPerBlock, ULONGLONG VCN,
                            ULONGLONG *Out, ULONG Max, ULONG *Count, ULONG Depth);

static VOID
NtfsCollectBlock (
    IN PNTFS_EFI_VCB          Vcb,
    IN PNTFS_ATTR_CTX         Alloc,
    IN ULONG                  ClustPerBlock,
    IN PINDEX_ENTRY_ATTRIBUTE First,
    IN PINDEX_ENTRY_ATTRIBUTE Last,
    OUT ULONGLONG            *Out,
    IN ULONG                  Max,
    IN OUT ULONG             *Count,
    IN ULONG                  Depth
    )
{
    PINDEX_ENTRY_ATTRIBUTE E = First;
    while ((PUCHAR)E < (PUCHAR)Last && *Count < Max) {
        if (E->Length == 0) break;
        if ((E->Flags & NTFS_INDEX_ENTRY_NODE) && Alloc != NULL &&
            E->Length >= sizeof (ULONGLONG)) {
            ULONGLONG Sub = *(PULONGLONG)((PUCHAR)E + E->Length - sizeof (ULONGLONG));
            NtfsCollectSub (Vcb, Alloc, ClustPerBlock, Sub, Out, Max, Count, Depth);  /* in-order: subtree first */
        }
        if (E->Flags & NTFS_INDEX_ENTRY_END) break;
        if ((E->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE
            && E->FileName.NameType != NTFS_FILE_NAME_DOS
            && *Count < Max) {
            Out[(*Count)++] = E->Data.Directory.IndexedFile & NTFS_MFT_MASK;
        }
        E = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)E + E->Length);
    }
}

static VOID
NtfsCollectSub (PNTFS_EFI_VCB Vcb, PNTFS_ATTR_CTX Alloc, ULONG ClustPerBlock,
                ULONGLONG VCN, ULONGLONG *Out, ULONG Max, ULONG *Count, ULONG Depth)
{
    PUCHAR IndexBuf;
    PINDEX_BUFFER Block;
    ULONG Read;
    EFI_STATUS FixupStatus = EFI_NOT_READY;
    if (Depth >= NTFS_MAX_INDEX_DEPTH) return;
    IndexBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (IndexBuf == NULL) return;
    Read = NtfsEfiReadAttr (Vcb, Alloc, VCN * Vcb->BytesPerCluster,
            (PCHAR)IndexBuf, Vcb->BytesPerIndexRecord);
    if (Read == Vcb->BytesPerIndexRecord) {
        Block = (PINDEX_BUFFER)IndexBuf;
        if (Block->Ntfs.Type == NRH_INDX_TYPE) {
            FixupStatus = NtfsEfiFixupRecord (Vcb, &((PFILE_RECORD_HEADER)Block)->Ntfs);
        }
        if (Block->Ntfs.Type == NRH_INDX_TYPE && !EFI_ERROR (FixupStatus) &&
            NtfsEfiIndexBlockOk (Vcb, Block)) {
            PINDEX_ENTRY_ATTRIBUTE F = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
            PINDEX_ENTRY_ATTRIBUTE L = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries);
            NtfsCollectBlock (Vcb, Alloc, ClustPerBlock, F, L, Out, Max, Count, Depth + 1);
        }
    }
    FreePool (IndexBuf);
}

ULONG
NtfsEfiCollectDir (
    IN  PNTFS_EFI_VCB Vcb,
    IN  ULONGLONG     DirMFTIndex,
    OUT ULONGLONG    *Out,
    IN  ULONG         Max
    )
{
    PFILE_RECORD_HEADER Rec = AllocatePool (Vcb->BytesPerFileRecord);
    PNTFS_ATTR_CTX      RootCtx, AllocCtx;
    PUCHAR              IndexBuf;
    PINDEX_ROOT_ATTRIBUTE IR;
    ULONG               Count = 0, ClustPerBlock;
    if (Rec == NULL) return 0;
    if (EFI_ERROR (NtfsEfiReadFileRecord (Vcb, DirMFTIndex, Rec))) { FreePool (Rec); return 0; }
    RootCtx = NtfsEfiFindAttribute (Vcb, Rec, AttributeIndexRoot, L"$I30", 4, NULL);
    if (RootCtx == NULL) { FreePool (Rec); return 0; }
    IndexBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (IndexBuf == NULL) { NtfsEfiFreeAttrCtx (RootCtx); FreePool (Rec); return 0; }
    {
        ULONG RootBytes = NtfsEfiReadAttr (Vcb, RootCtx, 0, (PCHAR)IndexBuf,
                                          Vcb->BytesPerIndexRecord);
        NtfsEfiFreeAttrCtx (RootCtx);
        IR = (PINDEX_ROOT_ATTRIBUTE)IndexBuf;
        if (RootBytes <= (ULONG)FIELD_OFFSET (INDEX_ROOT_ATTRIBUTE, Header) ||
            !NtfsEfiIndexHeaderOk (&IR->Header,
                (UINT64)RootBytes - FIELD_OFFSET (INDEX_ROOT_ATTRIBUTE, Header))) {
            FreePool (IndexBuf); FreePool (Rec);
            return 0;
        }
    }
    AllocCtx = NtfsEfiFindAttribute (Vcb, Rec, AttributeIndexAllocation, L"$I30", 4, NULL);
    ClustPerBlock = Vcb->BytesPerIndexRecord / Vcb->BytesPerCluster;
    {
        PINDEX_ENTRY_ATTRIBUTE F = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IR->Header + IR->Header.FirstEntryOffset);
        PINDEX_ENTRY_ATTRIBUTE L = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&IR->Header + IR->Header.TotalSizeOfEntries);
        NtfsCollectBlock (Vcb, AllocCtx, ClustPerBlock, F, L, Out, Max, &Count, 0);
    }
    if (AllocCtx) NtfsEfiFreeAttrCtx (AllocCtx);
    FreePool (IndexBuf);
    FreePool (Rec);
    return Count;
}

static ULONGLONG
NtfsEfiScanIndexBlock (
    IN     PINDEX_ENTRY_ATTRIBUTE  First,
    IN     PINDEX_ENTRY_ATTRIBUTE  Last,
    IN     CONST WCHAR            *Name,
    IN     UINTN                   NameLen,
    IN     BOOLEAN                 DirSearch,
    IN     BOOLEAN                 CaseSensitive,
    IN     CONST USHORT           *Upcase,
    IN OUT ULONG                  *StartEntry,
    IN OUT ULONG                  *CurrentEntry
    )
{
    PINDEX_ENTRY_ATTRIBUTE Entry = First;

    Print (L"[ntfs] ScanIndexBlock: Name='%s' NameLen=%d First=%p Last=%p\n",
        Name ? Name : L"(null)", (UINT32)NameLen, First, Last);

    while ((PUCHAR)Entry < (PUCHAR)Last) {
        if (Entry->Length < sizeof (INDEX_ENTRY_ATTRIBUTE)) break;
        if ((PUCHAR)Entry + Entry->Length > (PUCHAR)Last) break;
        if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;
        /* the name is read below (compare + debug print): it must be inside
         * this entry, NameLength being its own on-disk byte */
        if ((UINT64)FIELD_OFFSET (INDEX_ENTRY_ATTRIBUTE, FileName.Name) +
                (UINT64)Entry->FileName.NameLength * sizeof (WCHAR) > (UINT64)Entry->Length) {
            break;
        }

        {
            UINTN  EntryLen = Entry->FileName.NameLength;
            CHAR16 NameBuf[40];
            UINTN  CopyLen = (EntryLen < 39) ? EntryLen : 39;
            CopyMem (NameBuf, Entry->FileName.Name, CopyLen * sizeof (CHAR16));
            NameBuf[CopyLen] = L'\0';
            Print (L"[ntfs]   entry: mft=%ld type=%d name(%d)='%s'\n",
                Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK,
                Entry->FileName.NameType, (UINT32)EntryLen, NameBuf);
        }

        if ((Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE
            && Entry->FileName.NameType != NTFS_FILE_NAME_DOS)
        {
            if (DirSearch) {
                /* sequential enumeration: return the StartEntry-th match */
                if (*CurrentEntry >= *StartEntry) {
                    /* wildcard '*' - always matches for simple enum */
                    (*StartEntry)++;
                    (*CurrentEntry)++;
                    return (Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
                }
                (*CurrentEntry)++;
            } else {
                /* exact / case-insensitive lookup during Open() */
                if (NtfsEfiMatchEntry (Name, NameLen, Entry, CaseSensitive, Upcase))
                    return (Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK);
            }
        }
        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
    }
    return (ULONGLONG)-1LL;
}

static ULONGLONG
NtfsEfiBrowseSubNode (
    IN     PNTFS_EFI_VCB      Vcb,
    IN     PNTFS_ATTR_CTX     IndexAllocCtx,
    IN     PUCHAR              Bitmap,
    IN     ULONG               BitmapBits,
    IN     ULONG               ClustPerBlock,
    IN     ULONGLONG           VCN,
    IN     CONST WCHAR        *Name,
    IN     UINTN               NameLen,
    IN     BOOLEAN             DirSearch,
    IN     BOOLEAN             CaseSensitive,
    IN OUT ULONG              *StartEntry,
    IN OUT ULONG              *CurrentEntry,
    IN     ULONG               Depth
    )
{
    ULONG               NodeNumber = (ULONG)(VCN / ClustPerBlock);
    PUCHAR              IndexBuf;
    PINDEX_BUFFER       Block;
    PINDEX_ENTRY_ATTRIBUTE First, Last, Entry;
    ULONGLONG           Result;

    if (Depth >= NTFS_MAX_INDEX_DEPTH) return (ULONGLONG)-1LL;

    /* validate against bitmap */
    if (Bitmap != NULL && BitmapBits > 0 && NodeNumber < BitmapBits) {
        if (!((Bitmap[NodeNumber / 8] >> (NodeNumber % 8)) & 1))
            return (ULONGLONG)-1LL;
    }

    IndexBuf = AllocatePool (Vcb->BytesPerIndexRecord);
    if (IndexBuf == NULL) return (ULONGLONG)-1LL;

    if (NtfsEfiReadAttr (Vcb, IndexAllocCtx,
                          VCN * Vcb->BytesPerCluster,
                          (PCHAR)IndexBuf,
                          Vcb->BytesPerIndexRecord)
        != Vcb->BytesPerIndexRecord) {
        FreePool (IndexBuf); return (ULONGLONG)-1LL;
    }
    Block = (PINDEX_BUFFER)IndexBuf;
    if (Block->Ntfs.Type != NRH_INDX_TYPE) {
        FreePool (IndexBuf); return (ULONGLONG)-1LL;
    }
    if (EFI_ERROR (NtfsEfiFixupRecord (Vcb, &((PFILE_RECORD_HEADER)Block)->Ntfs)) ||
        !NtfsEfiIndexBlockOk (Vcb, Block)) {
        FreePool (IndexBuf); return (ULONGLONG)-1LL;
    }

    First = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.FirstEntryOffset);
    Last  = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)&Block->Header + Block->Header.TotalSizeOfEntries);

    Result = NtfsEfiScanIndexBlock (First, Last, Name, NameLen,
                                     DirSearch, CaseSensitive, Vcb->UpcaseTable,
                                     StartEntry, CurrentEntry);

    /* recurse into sub-nodes within this block */
    if (Result == (ULONGLONG)-1LL && (Block->Header.Flags & INDEX_NODE_LARGE)) {
        Entry = First;
        while ((PUCHAR)Entry < (PUCHAR)Last) {
            if (Entry->Length == 0) break;
            /*
             * NODE and END are independent flags: the terminal entry of a
             * node commonly carries NODE too (it still has a subtree to
             * descend into, it just has no filename of its own). Do not
             * exclude END entries from the subnode walk or the last -
             * and on a small/fresh directory, only - branch is skipped.
             */
            if ((Entry->Flags & NTFS_INDEX_ENTRY_NODE) &&
                Entry->Length >= sizeof (ULONGLONG))
            {
                ULONGLONG SubVCN = *(PULONGLONG)
                    ((PUCHAR)Entry + Entry->Length - sizeof (ULONGLONG));
                Result = NtfsEfiBrowseSubNode (Vcb, IndexAllocCtx,
                            Bitmap, BitmapBits, ClustPerBlock,
                            SubVCN, Name, NameLen,
                            DirSearch, CaseSensitive,
                            StartEntry, CurrentEntry, Depth + 1);
                if (Result != (ULONGLONG)-1LL) break;
            }
            if (Entry->Flags & NTFS_INDEX_ENTRY_END) break;
            Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Entry + Entry->Length);
        }
    }
    FreePool (IndexBuf);
    return Result;
}
