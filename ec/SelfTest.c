// SelfTest.c - scripted, screenless run of the file operations, for the harness.
//
// EC is a full-screen interactive program, which normally means it can only be
// tested by hand. This module gives the parts that touch a volume - the tree
// search and the metadata writes - a path that a script can drive: boot the
// image in a VM with a flag file present, let it work on a fixture the host
// laid down, and read a result file afterwards. No screen, no keyboard, no
// timing assumptions.
//
// Everything here is inside #if EC_SELFTEST, so the release build carries none
// of it.
#include "SelfTest.h"

#if EC_SELFTEST

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#include "FileProps.h"
#include "FileSystem.h"
#include "Search.h"
#include "Viewer.h"
#include "Panel.h"
#include "PanelOps.h"
#include "Checksum.h"
#include "Sync.h"
#include "Config.h"

#define ST_FLAG_FILE    L"\\_ECTEST.on"
#define ST_RESULT_FILE  L"\\_ECTEST_RESULT.txt"
#define ST_FIXTURE_DIR  L"\\_ECTEST"

static CHAR8  gStLog[8192];
static UINTN  gStLogLen = 0;
static UINTN  gStPassed = 0;
static UINTN  gStFailed = 0;

static VOID StLog(IN CONST CHAR8* Fmt, ...)
{
  VA_LIST marker;
  UINTN room = sizeof(gStLog) - gStLogLen;

  if (room < 2) return;
  VA_START(marker, Fmt);
  gStLogLen += AsciiVSPrint(gStLog + gStLogLen, room, Fmt, marker);
  VA_END(marker);
}

static VOID StCheck(IN BOOLEAN Ok, IN CONST CHAR8* What)
{
  if (Ok) gStPassed++; else gStFailed++;
  StLog("%a  %a\n", Ok ? "PASS" : "FAIL", What);
}

// The volume EC booted from, which is where the flag and the result live.
static EFI_FILE_PROTOCOL* StOpenBootRoot(IN EFI_HANDLE ImageHandle)
{
  EFI_LOADED_IMAGE_PROTOCOL* image = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfs = NULL;
  EFI_FILE_PROTOCOL* root = NULL;
  EFI_GUID imageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_GUID sfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

  if (EFI_ERROR(gBS->HandleProtocol(ImageHandle, &imageGuid, (VOID**)&image)) || image == NULL) {
    return NULL;
  }
  if (EFI_ERROR(gBS->HandleProtocol(image->DeviceHandle, &sfsGuid, (VOID**)&sfs)) || sfs == NULL) {
    return NULL;
  }
  if (EFI_ERROR(sfs->OpenVolume(sfs, &root))) {
    return NULL;
  }
  return root;
}

static BOOLEAN StFlagPresent(IN EFI_FILE_PROTOCOL* Root)
{
  EFI_FILE_PROTOCOL* f = NULL;

  if (Root == NULL) return FALSE;
  if (EFI_ERROR(Root->Open(Root, &f, ST_FLAG_FILE, EFI_FILE_MODE_READ, 0))) {
    return FALSE;
  }
  f->Close(f);
  return TRUE;
}

static VOID StWriteResult(IN EFI_FILE_PROTOCOL* Root)
{
  EFI_FILE_PROTOCOL* f = NULL;
  UINTN len = gStLogLen;

  if (Root == NULL) return;
  if (EFI_ERROR(Root->Open(Root, &f, ST_RESULT_FILE,
                           EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
    return;
  }
  f->SetPosition(f, 0);
  f->Write(f, &len, gStLog);
  f->Flush(f);
  f->Close(f);
}

// The host lays the fixture down on one NTFS volume; find it by its marker
// directory rather than by drive letter, which the firmware does not have.
static BOOLEAN StFindFixture(OUT CHAR16* Root, IN UINTN RootChars)
{
  UINTN v;

  for (v = 0; v < gVolumeCount; v++) {
    CHAR16 candidate[MAX_PATH_LEN];
    BOOLEAN isDir = FALSE;

    UnicodeSPrint(candidate, sizeof(candidate), L"%s%s", gVolumes[v].Name, ST_FIXTURE_DIR);
    if (FsFileExists(candidate, &isDir) && isDir) {
      StrCpyS(Root, RootChars, candidate);
      return TRUE;
    }
  }
  return FALSE;
}

/*
 * Searching by content rather than by name. The interesting case is the last
 * one: a needle lying across the boundary between two reads is exactly what a
 * chunked scan loses if it forgets to carry the tail of one chunk into the
 * next, and no small fixture file would ever exercise it.
 */
static VOID StRunContentSearchChecks(IN CONST CHAR16* Fixture)
{
  STATIC CONST CHAR8 straddleNeedle[] = "STRADDLE";
  CONST UINTN straddleSize = 70000;
  CONST UINTN straddleAt = 65532;        /* crosses the 65536-byte read */
  SEARCH_RESULT result;
  CHAR16 straddlePath[MAX_PATH_LEN];
  CHAR16 wideNeedle[3];
  CHAR8* big;

  // 1. only the file whose contents hold the text, out of three that match the mask
  if (!EFI_ERROR(SearchCollect(Fixture, L"*.tag", L"middle", &result))) {
    StLog("     content search: hits=%d files read=%d\n",
          (UINT32)result.HitCount, (UINT32)result.FilesScanned);
    StCheck(result.HitCount == 1 && StrCmp(result.Hits[0].Name, L"middle.tag") == 0,
            "content search: only the file holding the text is a hit");
    StCheck(result.FilesScanned == 3, "content search: every file matching the mask was read");
    SearchFree(&result);
  } else {
    StCheck(FALSE, "content search: collect *.tag containing middle");
  }

  // 2. the match is case-insensitive, as it is in the viewer
  if (!EFI_ERROR(SearchCollect(Fixture, L"*.tag", L"MIDDLE", &result))) {
    StCheck(result.HitCount == 1, "content search: case does not matter");
    SearchFree(&result);
  } else {
    StCheck(FALSE, "content search: collect *.tag containing MIDDLE");
  }

  // 3. text that is in none of them
  if (!EFI_ERROR(SearchCollect(Fixture, L"*.tag", L"nowhere-at-all", &result))) {
    StCheck(result.HitCount == 0 && result.FilesScanned == 3,
            "content search: text present in no file returns nothing");
    SearchFree(&result);
  } else {
    StCheck(FALSE, "content search: collect a needle nothing holds");
  }

  // 4. directories have no contents to look in, so they never answer
  if (!EFI_ERROR(SearchCollect(Fixture, L"*", L"decoy", &result))) {
    StCheck(result.HitCount == 1 && !result.Hits[0].IsDirectory &&
            StrCmp(result.Hits[0].Name, L"decoy.txt") == 0,
            "content search: directories are skipped, the file is found");
    SearchFree(&result);
  } else {
    StCheck(FALSE, "content search: collect * containing decoy");
  }

  // 5. a needle that is not plain ASCII is refused, not truncated
  wideNeedle[0] = L'a';
  wideNeedle[1] = (CHAR16)0x0141;
  wideNeedle[2] = L'\0';
  StCheck(SearchCollect(Fixture, L"*.tag", wideNeedle, &result) == EFI_INVALID_PARAMETER,
          "content search: a non-ASCII needle is refused");

  // 6. a match straddling the boundary between two reads
  big = AllocatePool(straddleSize);
  if (big == NULL) {
    StCheck(FALSE, "content search: allocate the straddle fixture");
    return;
  }
  SetMem(big, straddleSize, 'x');
  CopyMem(big + straddleAt, straddleNeedle, sizeof(straddleNeedle) - 1);
  FsCombinePath(straddlePath, Fixture, L"straddle.bin");
  if (EFI_ERROR(FsWriteFileFromBuffer(straddlePath, big, straddleSize))) {
    FreePool(big);
    StCheck(FALSE, "content search: write the straddle fixture");
    return;
  }
  FreePool(big);

  if (!EFI_ERROR(SearchCollect(Fixture, L"straddle.bin", L"STRADDLE", &result))) {
    StCheck(result.HitCount == 1,
            "content search: a match across a read boundary is still found");
    SearchFree(&result);
  } else {
    StCheck(FALSE, "content search: collect straddle.bin");
  }

  if (!EFI_ERROR(SearchCollect(Fixture, L"straddle.bin", L"STRADDLF", &result))) {
    StCheck(result.HitCount == 0,
            "content search: one byte off the boundary text is not a match");
    SearchFree(&result);
  } else {
    StCheck(FALSE, "content search: collect straddle.bin with a near miss");
  }
}

static VOID StRunSearchChecks(IN CONST CHAR16* Fixture)
{
  SEARCH_RESULT result;
  UINTN tagged = 0;
  UINTN i;

  // 1. every *.tag under the fixture, at three different depths
  if (EFI_ERROR(SearchCollect(Fixture, L"*.tag", NULL, &result))) {
    StCheck(FALSE, "search: collect *.tag");
    return;
  }
  for (i = 0; i < result.HitCount; i++) {
    if (!result.Hits[i].IsDirectory) tagged++;
  }
  StLog("     search *.tag: hits=%d dirs=%d aborted=%d hitlimit=%d depthlimit=%d\n",
        (UINT32)result.HitCount, (UINT32)result.DirsVisited,
        result.Aborted, result.HitLimit, result.DepthLimit);
  StCheck(tagged == 3, "search: three .tag files across three depths");
  StCheck(!result.Aborted && !result.HitLimit, "search: completed without hitting a cap");
  SearchFree(&result);

  // 2. a mask that only the deepest file answers
  if (!EFI_ERROR(SearchCollect(Fixture, L"deep*.tag", NULL, &result))) {
    StCheck(result.HitCount == 1, "search: mask matches exactly one file");
    if (result.HitCount == 1) {
      StLog("     deep hit: %s\\%s\n", result.Hits[0].Dir, result.Hits[0].Name);
    }
    SearchFree(&result);
  } else {
    StCheck(FALSE, "search: collect deep*.tag");
  }

  // 3. directories match too, not only files
  if (!EFI_ERROR(SearchCollect(Fixture, L"level*", NULL, &result))) {
    UINTN dirs = 0;
    for (i = 0; i < result.HitCount; i++) {
      if (result.Hits[i].IsDirectory) dirs++;
    }
    StCheck(dirs >= 2, "search: directories are matched as well as files");
    SearchFree(&result);
  } else {
    StCheck(FALSE, "search: collect level*");
  }

  // 4. a mask nothing answers comes back empty, not with everything
  if (!EFI_ERROR(SearchCollect(Fixture, L"*.nothing-matches-this", NULL, &result))) {
    StCheck(result.HitCount == 0, "search: an unmatched mask returns nothing");
    SearchFree(&result);
  } else {
    StCheck(FALSE, "search: collect a non-matching mask");
  }

  StRunContentSearchChecks(Fixture);
}

static VOID StRunMetaChecks(IN CONST CHAR16* Fixture)
{
  CHAR16 target[MAX_PATH_LEN];
  UINT64 attr = 0, readBack = 0;
  EFI_TIME created, modified, accessed;
  EFI_TIME wanted;
  EFI_STATUS status;

  FsCombinePath(target, Fixture, L"attr.bin");

  if (EFI_ERROR(FsGetFileMeta(target, &attr, &created, &modified, &accessed))) {
    StCheck(FALSE, "meta: read the starting attributes");
    return;
  }
  StLog("     attr.bin starts at %04x, modified %04d-%02d-%02d %02d:%02d:%02d\n",
        (UINT32)attr, modified.Year, modified.Month, modified.Day,
        modified.Hour, modified.Minute, modified.Second);

  // set ReadOnly + Hidden
  attr |= (EFI_FILE_READ_ONLY | EFI_FILE_HIDDEN);
  status = FsSetFileMeta(target, &attr, NULL);
  StCheck(!EFI_ERROR(status), "meta: set ReadOnly and Hidden");
  if (!EFI_ERROR(FsGetFileMeta(target, &readBack, NULL, NULL, NULL))) {
    StCheck((readBack & EFI_FILE_READ_ONLY) != 0 && (readBack & EFI_FILE_HIDDEN) != 0,
            "meta: both bits read back set");
  } else {
    StCheck(FALSE, "meta: re-read after setting bits");
  }

  // clear them again - the case that matters before overwriting a system file
  attr &= ~(UINT64)(EFI_FILE_READ_ONLY | EFI_FILE_HIDDEN);
  status = FsSetFileMeta(target, &attr, NULL);
  StCheck(!EFI_ERROR(status), "meta: clear ReadOnly and Hidden");
  if (!EFI_ERROR(FsGetFileMeta(target, &readBack, NULL, NULL, NULL))) {
    StCheck((readBack & EFI_FILE_READ_ONLY) == 0 && (readBack & EFI_FILE_HIDDEN) == 0,
            "meta: both bits read back clear");
  } else {
    StCheck(FALSE, "meta: re-read after clearing bits");
  }

  // a fixed modification time, then read it back field by field
  ZeroMem(&wanted, sizeof(wanted));
  wanted.Year = 2001; wanted.Month = 2; wanted.Day = 3;
  wanted.Hour = 4; wanted.Minute = 5; wanted.Second = 6;
  wanted.TimeZone = EFI_UNSPECIFIED_TIMEZONE;

  status = FsSetFileMeta(target, NULL, &wanted);
  StCheck(!EFI_ERROR(status), "meta: set the modification time");
  if (!EFI_ERROR(FsGetFileMeta(target, NULL, NULL, &modified, NULL))) {
    StCheck(modified.Year == 2001 && modified.Month == 2 && modified.Day == 3 &&
            modified.Hour == 4 && modified.Minute == 5 && modified.Second == 6,
            "meta: the time reads back exactly as written");
  } else {
    StCheck(FALSE, "meta: re-read after setting the time");
  }

  // writing only the time must not have disturbed the attributes
  if (!EFI_ERROR(FsGetFileMeta(target, &readBack, NULL, NULL, NULL))) {
    StCheck((readBack & (EFI_FILE_READ_ONLY | EFI_FILE_HIDDEN)) == 0,
            "meta: a time-only write left the attributes alone");
  }
}

static VOID StRunFindChecks(VOID)
{
  CONST UINT8 haystack[] = "boot: loading NTFS driver\r\nERROR 0x8007 in Setup\r\nready";
  UINT64 size = sizeof(haystack) - 1;
  UINT8 needle[64];
  UINTN len = 0;
  UINT64 at = 0;

  StCheck(ViewerNeedleToBytes(L"ERROR", needle, sizeof(needle), &len) && len == 5,
          "find: an ASCII needle narrows to bytes");
  StCheck(ViewerFindBytes(haystack, size, needle, len, 0, &at) && haystack[at] == 'E',
          "find: locates the needle");

  StCheck(ViewerNeedleToBytes(L"error", needle, sizeof(needle), &len) &&
          ViewerFindBytes(haystack, size, needle, len, 0, &at) && haystack[at] == 'E',
          "find: case does not matter");

  {
    UINT64 first = 0, second = 0;
    ViewerNeedleToBytes(L"o", needle, sizeof(needle), &len);
    StCheck(ViewerFindBytes(haystack, size, needle, len, 0, &first) &&
            ViewerFindBytes(haystack, size, needle, len, first + 1, &second) &&
            second > first,
            "find: the next match starts past the previous one");
  }

  ViewerNeedleToBytes(L"nowhere", needle, sizeof(needle), &len);
  StCheck(!ViewerFindBytes(haystack, size, needle, len, 0, &at),
          "find: a needle that is absent reports absent");

  StCheck(!ViewerNeedleToBytes(L"", needle, sizeof(needle), &len),
          "find: an empty needle is refused");
  {
    // built character by character rather than written as an escape, so that
    // nothing between here and the compiler can quietly turn it back into
    // ASCII - which is exactly what happened the first time this was written
    CHAR16 wide[4];
    wide[0] = L'a';
    wide[1] = 0x0141;   /* the Polish crossed L, well outside ASCII */
    wide[2] = L'b';
    wide[3] = L'\0';
    StCheck(!ViewerNeedleToBytes(wide, needle, sizeof(needle), &len),
            "find: a non-ASCII needle is refused rather than truncated");
  }
}

static UINTN StCountSelected(IN CONST PANEL* Panel)
{
  UINTN n = 0;
  UINTN i;

  for (i = 0; i < Panel->FileCount; i++) {
    if (Panel->Files[i].Selected) n++;
  }
  return n;
}

static BOOLEAN StIsSelected(IN CONST PANEL* Panel, IN CONST CHAR16* Name)
{
  UINTN i;

  for (i = 0; i < Panel->FileCount; i++) {
    if (StrCmp(Panel->Files[i].Name, Name) == 0) return Panel->Files[i].Selected;
  }
  return FALSE;
}

static VOID StRunCompareChecks(IN CONST CHAR16* Fixture)
{
  PANEL left, right;
  CHAR16 pathA[MAX_PATH_LEN];
  CHAR16 pathB[MAX_PATH_LEN];
  UINTN marked;

  FsCombinePath(pathA, Fixture, L"cmp_a");
  FsCombinePath(pathB, Fixture, L"cmp_b");

  PanelInit(&left, pathA);
  PanelInit(&right, pathB);
  if (EFI_ERROR(PanelRefresh(&left)) || EFI_ERROR(PanelRefresh(&right))) {
    StCheck(FALSE, "compare: read both sides");
    PanelFree(&left);
    PanelFree(&right);
    return;
  }

  marked = PanelOpsCompareSelect(&left, &right);
  StLog("     compare: left=%d right=%d marked=%d\n",
        (UINT32)left.FileCount, (UINT32)right.FileCount, (UINT32)marked);

  StCheck(!StIsSelected(&left, L"same.txt") && !StIsSelected(&right, L"same.txt"),
          "compare: an identical pair stays unselected");
  StCheck(StIsSelected(&left, L"diff.txt") && StIsSelected(&right, L"diff.txt"),
          "compare: differing sizes are marked on both sides");
  StCheck(StIsSelected(&left, L"only_a.txt"), "compare: an entry the other side lacks is marked");
  StCheck(StIsSelected(&right, L"only_b.txt"), "compare: and the same the other way round");
  StCheck(StIsSelected(&left, L"subdir"), "compare: a directory missing on the other side is marked");
  StCheck(StCountSelected(&left) == 3 && StCountSelected(&right) == 2,
          "compare: nothing beyond the real differences is marked");

  marked = PanelOpsCompareSelect(&left, &right);
  StCheck(StCountSelected(&left) == 3 && StCountSelected(&right) == 2,
          "compare: a second run gives the same answer");

  PanelFree(&left);
  PanelFree(&right);
}

static BOOLEAN StWriteFixtureFile(
  IN CONST CHAR16* Directory,
  IN CONST CHAR16* Name,
  IN CONST CHAR8* Data,
  IN UINTN Size
) {
  CHAR16 path[MAX_PATH_LEN];
  FsCombinePath(path, Directory, Name);
  return !EFI_ERROR(FsWriteFileFromBuffer(path, (VOID*)Data, Size));
}

static VOID StRunChecksumChecks(IN CONST CHAR16* Fixture)
{
  STATIC CONST CHAR8 abc[] = "abc";
  CHAR16 emptyPath[MAX_PATH_LEN];
  CHAR16 abcPath[MAX_PATH_LEN];
  CHAR16 copyPath[MAX_PATH_LEN];
  CHAR16 digest[65];
  EC_FILE_CHECKSUM emptyChecksum;
  EC_FILE_CHECKSUM abcChecksum;
  EC_FILE_CHECKSUM copyChecksum;
  BOOLEAN oldVerify = gEcConfig.VerifyAfterCopy;
  UINT8 prefix[2];
  UINTN prefixSize = 0;
  UINT64 totalSize = 0;

  FsCombinePath(emptyPath, Fixture, L"hash-empty.bin");
  FsCombinePath(abcPath, Fixture, L"hash-abc.bin");
  FsCombinePath(copyPath, Fixture, L"hash-copy.bin");
  FsWriteFileFromBuffer(emptyPath, (VOID*)abc, 0);
  FsWriteFileFromBuffer(abcPath, (VOID*)abc, sizeof(abc) - 1);
  FsDeleteRecursive(copyPath);

  if (!EFI_ERROR(ChecksumFile(emptyPath, &emptyChecksum))) {
    ChecksumSha256ToText(emptyChecksum.Sha256, digest);
    StCheck(StrCmp(digest, L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
            "checksum: SHA-256 empty-file vector");
    StCheck(emptyChecksum.Crc32 == 0, "checksum: CRC32 empty-file vector");
  } else {
    StCheck(FALSE, "checksum: read empty fixture");
  }

  if (!EFI_ERROR(ChecksumFile(abcPath, &abcChecksum))) {
    ChecksumSha256ToText(abcChecksum.Sha256, digest);
    StCheck(StrCmp(digest, L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
            "checksum: SHA-256 abc vector");
    StCheck(abcChecksum.Crc32 == 0x352441c2, "checksum: CRC32 abc vector");
  } else {
    StCheck(FALSE, "checksum: read abc fixture");
  }

  gEcConfig.VerifyAfterCopy = TRUE;
  StCheck(!EFI_ERROR(FsCopyRecursive(abcPath, copyPath, NULL)),
          "checksum: verified copy succeeds");
  gEcConfig.VerifyAfterCopy = oldVerify;
  if (!EFI_ERROR(ChecksumFile(copyPath, &copyChecksum))) {
    StCheck(ChecksumEqual(&abcChecksum, &copyChecksum),
            "checksum: copied file is byte-identical");
  } else {
    StCheck(FALSE, "checksum: read verified copy");
  }

  StCheck(!EFI_ERROR(FsReadFilePrefix(abcPath, prefix, sizeof(prefix), &prefixSize, &totalSize)) &&
          prefixSize == 2 && totalSize == 3 && prefix[0] == 'a' && prefix[1] == 'b',
          "quick view: bounded prefix read reports full size");
}

// Counts the entries the walk reports and, when StopAt is set, stops it there.
// A walk that cannot be interrupted is the defect this guards against.
static UINTN gStSyncTicks = 0;
static UINTN gStSyncStopAt = 0;

static BOOLEAN StSyncProgress(
  IN CONST CHAR16* CurrentPath,
  IN UINTN FilesSeen,
  IN UINTN DirectoriesSeen
) {
  gStSyncTicks++;
  if (CurrentPath == NULL || CurrentPath[0] == L'\0') return FALSE;
  if (FilesSeen + DirectoriesSeen != gStSyncTicks) return FALSE;
  return (BOOLEAN)(gStSyncStopAt == 0 || gStSyncTicks < gStSyncStopAt);
}

static VOID StRunSyncChecks(IN CONST CHAR16* Fixture)
{
  STATIC CONST CHAR8 same[] = "same";
  STATIC CONST CHAR8 sourceDiff[] = "source-version";
  STATIC CONST CHAR8 targetDiff[] = "old";
  STATIC CONST CHAR8 sourceOnly[] = "source-only";
  STATIC CONST CHAR8 targetOnly[] = "target-only";
  STATIC CONST CHAR8 deepSource[] = "deep-source";
  STATIC CONST CHAR8 deepTarget[] = "deep-target-old";
  STATIC CONST CHAR8 equalSizeSource[] = "AAAA";
  STATIC CONST CHAR8 equalSizeTarget[] = "BBBB";
  CHAR16 source[MAX_PATH_LEN];
  CHAR16 target[MAX_PATH_LEN];
  CHAR16 sourceNested[MAX_PATH_LEN];
  CHAR16 targetNested[MAX_PATH_LEN];
  EC_SYNC_SUMMARY summary;
  EC_SYNC_RESULT result;
  EFI_STATUS status;

  FsCombinePath(source, Fixture, L"sync_source");
  FsCombinePath(target, Fixture, L"sync_target");
  FsDeleteRecursive(source);
  FsDeleteRecursive(target);
  if (EFI_ERROR(FsCreateDir(source)) || EFI_ERROR(FsCreateDir(target))) {
    StCheck(FALSE, "sync: create roots");
    return;
  }
  FsCombinePath(sourceNested, source, L"nested");
  FsCombinePath(targetNested, target, L"nested");
  FsCreateDir(sourceNested);
  FsCreateDir(targetNested);

  StCheck(StWriteFixtureFile(source, L"same.bin", same, sizeof(same) - 1) &&
          StWriteFixtureFile(target, L"same.bin", same, sizeof(same) - 1) &&
          StWriteFixtureFile(source, L"different.bin", sourceDiff, sizeof(sourceDiff) - 1) &&
          StWriteFixtureFile(target, L"different.bin", targetDiff, sizeof(targetDiff) - 1) &&
          StWriteFixtureFile(source, L"only-source.bin", sourceOnly, sizeof(sourceOnly) - 1) &&
          StWriteFixtureFile(target, L"only-target.bin", targetOnly, sizeof(targetOnly) - 1) &&
          StWriteFixtureFile(source, L"same-size-different.bin", equalSizeSource, sizeof(equalSizeSource) - 1) &&
          StWriteFixtureFile(target, L"same-size-different.bin", equalSizeTarget, sizeof(equalSizeTarget) - 1) &&
          StWriteFixtureFile(sourceNested, L"deep.bin", deepSource, sizeof(deepSource) - 1) &&
          StWriteFixtureFile(targetNested, L"deep.bin", deepTarget, sizeof(deepTarget) - 1),
          "sync: create fixture files");

  gStSyncTicks = 0;
  gStSyncStopAt = 0;
  status = SyncCompareTrees(source, target, StSyncProgress, &summary);
  StCheck(!EFI_ERROR(status), "sync: recursive comparison completes");
  StLog("     sync: progress reported %d entries\n", (UINT32)gStSyncTicks);
  StCheck(gStSyncTicks == 6, "sync: progress reports every entry it walks, once each");
  if (!EFI_ERROR(status)) {
    StLog("     sync before: left=%d right=%d different=%d equal=%d dirs=%d\n",
          (UINT32)summary.LeftOnly, (UINT32)summary.RightOnly,
          (UINT32)summary.Different, (UINT32)summary.EqualFiles,
          (UINT32)summary.CommonDirectories);
    StCheck(summary.LeftOnly == 1 && summary.RightOnly == 1 && summary.Different == 3 &&
            summary.EqualFiles == 1 && summary.CommonDirectories == 1,
            "sync: nested, size and same-size content differences counted exactly");
  }

  // Stopping at the second entry must abort the whole walk, not just one level.
  gStSyncTicks = 0;
  gStSyncStopAt = 2;
  status = SyncCompareTrees(source, target, StSyncProgress, &summary);
  StCheck(status == EFI_ABORTED, "sync: refusing to continue aborts the comparison");
  StCheck(gStSyncTicks == 2, "sync: the walk stops at the entry that refused");
  gStSyncStopAt = 0;

  // Sizing a copy up front: one file, then the whole tree above it.
  {
    CHAR16 onlySource[MAX_PATH_LEN];
    CHAR16 missing[MAX_PATH_LEN];
    UINT64 fileBytes = 0;
    UINT64 treeBytes = 0;

    FsCombinePath(onlySource, source, L"only-source.bin");
    FsCombinePath(missing, Fixture, L"_no_such_entry");
    StCheck(!EFI_ERROR(FsGetTreeSize(onlySource, &fileBytes)) && fileBytes == 11,
            "tree size: a single file reports its own length");
    StCheck(!EFI_ERROR(FsGetTreeSize(source, &treeBytes)) && treeBytes == 44,
            "tree size: a directory reports everything below it");
    StCheck(FsGetTreeSize(missing, &treeBytes) == EFI_NOT_FOUND,
            "tree size: a path that does not exist is refused");
  }

  gStSyncTicks = 0;
  status = SyncUpdateTree(source, target, StSyncProgress, &result);
  StCheck(!EFI_ERROR(status) && result.Errors == 0, "sync: one-way update completes");
  status = SyncCompareTrees(source, target, NULL, &summary);
  StCheck(!EFI_ERROR(status) && summary.LeftOnly == 0 && summary.Different == 0 &&
          summary.RightOnly == 1 && summary.EqualFiles == 5,
          "sync: destination matches source while target-only entry remains");
}

static VOID StRunVolumeChecks(IN CONST CHAR16* Fixture)
{
  FS_VOLUME* volume = FsFindVolumeForPath(Fixture);
  UINT64 total = 0;
  UINT64 free = 0;
  UINT32 blockSize = 0;
  BOOLEAN readOnly = TRUE;
  CHAR16 label[64];
  EFI_STATUS status;

  if (volume == NULL) {
    StCheck(FALSE, "volume tools: resolve fixture volume");
    return;
  }
  status = FsGetVolumeDetails(volume, &total, &free, &blockSize, &readOnly,
                              label, ARRAY_SIZE(label));
  StCheck(!EFI_ERROR(status) && total > 0 && free <= total && blockSize > 0 && !readOnly,
          "volume tools: extended filesystem information");
}

static VOID StRunParseChecks(VOID)
{
  EFI_TIME t;

  StCheck(FilePropsParseTime(L"2001-02-03 04:05:06", &t) &&
          t.Year == 2001 && t.Month == 2 && t.Day == 3 &&
          t.Hour == 4 && t.Minute == 5 && t.Second == 6,
          "parse: full date and time");
  StCheck(FilePropsParseTime(L"1999-12-31", &t) && t.Year == 1999 && t.Hour == 0,
          "parse: date alone leaves midnight");
  StCheck(!FilePropsParseTime(L"2001-13-03 04:05:06", &t), "parse: month 13 refused");
  StCheck(!FilePropsParseTime(L"2001-02-03 25:05:06", &t), "parse: hour 25 refused");
  StCheck(!FilePropsParseTime(L"not a date", &t), "parse: rubbish refused");
  StCheck(!FilePropsParseTime(L"1970-01-01", &t), "parse: pre-1980 refused");
}

BOOLEAN EcSelfTestMaybeRun(IN EFI_HANDLE ImageHandle)
{
  EFI_FILE_PROTOCOL* bootRoot = StOpenBootRoot(ImageHandle);
  CHAR16 fixture[MAX_PATH_LEN];

  if (bootRoot == NULL) return FALSE;
  if (!StFlagPresent(bootRoot)) {
    bootRoot->Close(bootRoot);
    return FALSE;
  }

  StLog("==EC-SELFTEST==\n");

  if (!StFindFixture(fixture, ARRAY_SIZE(fixture))) {
    StLog("     no volume carries %s\n", ST_FIXTURE_DIR);
    StCheck(FALSE, "fixture: found on a mounted volume");
  } else {
    StLog("     fixture: %s\n", fixture);
    StRunSearchChecks(fixture);
    StRunMetaChecks(fixture);
    StRunCompareChecks(fixture);
    StRunChecksumChecks(fixture);
    StRunSyncChecks(fixture);
    StRunVolumeChecks(fixture);
  }
  StRunParseChecks();
  StRunFindChecks();

  StLog("==EC-SELFTEST-DONE== passed=%d failed=%d\n",
        (UINT32)gStPassed, (UINT32)gStFailed);

  StWriteResult(bootRoot);
  bootRoot->Close(bootRoot);

  /*
   * Power the machine off rather than returning to the boot manager. A script
   * driving this waits for the VM to stop; an application that merely returns
   * leaves the firmware sitting at its own menu, and the run looks like a hang
   * with no result until the harness times out.
   */
  gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);

  /* firmware that ignores the shutdown request gets the file manager */
  return TRUE;
}

#endif /* EC_SELFTEST */
