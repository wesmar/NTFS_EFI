// Sync.c - recursive compare/update kept separate from the interactive UI.
#include "Sync.h"
#include "Checksum.h"
#include "FileSystem.h"

#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>

#define SYNC_MAX_DEPTH 64

// What the walk carries from level to level: where to report, how far it has
// got, and whether the user has stopped it. Counters are the ones the progress
// box shows, not the ones the summary reports - an entry is counted when it is
// looked at, whatever the comparison then decides about it.
typedef struct {
  EC_SYNC_PROGRESS Report;
  UINTN Files;
  UINTN Directories;
  BOOLEAN Aborted;
} SYNC_WALK;

// Count one entry and give the caller its say. FALSE means stop: every loop in
// this file breaks on it and every level passes EFI_ABORTED up.
static BOOLEAN SyncTick(
  IN OUT SYNC_WALK* Walk,
  IN CONST CHAR16* Path,
  IN BOOLEAN IsDirectory
) {
  if (Walk->Aborted) return FALSE;
  if (IsDirectory) Walk->Directories++;
  else Walk->Files++;
  if (Walk->Report == NULL) return TRUE;
  if (!Walk->Report(Path, Walk->Files, Walk->Directories)) {
    Walk->Aborted = TRUE;
    return FALSE;
  }
  return TRUE;
}

static CONST FS_FILE_ITEM* SyncFindItem(
  IN CONST FS_FILE_ITEM* Items,
  IN UINTN Count,
  IN CONST CHAR16* Name
) {
  if (Items == NULL || Name == NULL) return NULL;
  for (UINTN i = 0; i < Count; i++) {
    if (StrCmp(Items[i].Name, Name) == 0) return &Items[i];
  }
  return NULL;
}

static BOOLEAN SyncUsable(IN CONST FS_FILE_ITEM* Item)
{
  return Item != NULL && StrCmp(Item->Name, L".") != 0 && StrCmp(Item->Name, L"..") != 0;
}

static EFI_STATUS SyncFilesEqual(
  IN CONST CHAR16* Left,
  IN CONST CHAR16* Right,
  OUT BOOLEAN* Equal
) {
  EC_FILE_CHECKSUM leftChecksum;
  EC_FILE_CHECKSUM rightChecksum;
  EFI_STATUS status;

  if (Equal == NULL) return EFI_INVALID_PARAMETER;
  *Equal = FALSE;
  status = ChecksumFile(Left, &leftChecksum);
  if (EFI_ERROR(status)) return status;
  status = ChecksumFile(Right, &rightChecksum);
  if (EFI_ERROR(status)) return status;
  *Equal = ChecksumEqual(&leftChecksum, &rightChecksum);
  return EFI_SUCCESS;
}

static EFI_STATUS SyncCompareInternal(
  IN CONST CHAR16* LeftRoot,
  IN CONST CHAR16* RightRoot,
  IN UINTN Depth,
  IN OUT SYNC_WALK* Walk,
  IN OUT EC_SYNC_SUMMARY* Summary
) {
  FS_FILE_ITEM* left = NULL;
  FS_FILE_ITEM* right = NULL;
  UINTN leftCount = 0;
  UINTN rightCount = 0;
  EFI_STATUS status;

  if (Depth > SYNC_MAX_DEPTH) return EFI_BAD_BUFFER_SIZE;
  status = FsListDirectory(LeftRoot, &left, &leftCount);
  if (EFI_ERROR(status)) return status;
  status = FsListDirectory(RightRoot, &right, &rightCount);
  if (EFI_ERROR(status)) {
    if (left != NULL) FreePool(left);
    return status;
  }

  for (UINTN i = 0; i < leftCount; i++) {
    CONST FS_FILE_ITEM* other;
    CHAR16 leftPath[MAX_PATH_LEN];
    CHAR16 rightPath[MAX_PATH_LEN];
    if (!SyncUsable(&left[i])) continue;

    FsCombinePath(leftPath, LeftRoot, left[i].Name);
    FsCombinePath(rightPath, RightRoot, left[i].Name);
    if (!SyncTick(Walk, leftPath, left[i].IsDirectory)) {
      status = EFI_ABORTED;
      break;
    }

    other = SyncFindItem(right, rightCount, left[i].Name);
    if (other == NULL) {
      Summary->LeftOnly++;
      continue;
    }
    if (left[i].IsDirectory != other->IsDirectory) {
      Summary->Different++;
      continue;
    }
    if (left[i].IsDirectory) {
      Summary->CommonDirectories++;
      status = SyncCompareInternal(leftPath, rightPath, Depth + 1, Walk, Summary);
      if (EFI_ERROR(status)) {
        if (status != EFI_ABORTED) Summary->Errors++;
        break;
      }
    } else if (left[i].Size != other->Size) {
      Summary->Different++;
    } else {
      BOOLEAN equal = FALSE;
      status = SyncFilesEqual(leftPath, rightPath, &equal);
      if (EFI_ERROR(status)) {
        Summary->Errors++;
        break;
      }
      if (equal) Summary->EqualFiles++;
      else Summary->Different++;
    }
  }

  if (!EFI_ERROR(status)) {
    for (UINTN i = 0; i < rightCount; i++) {
      if (SyncUsable(&right[i]) && SyncFindItem(left, leftCount, right[i].Name) == NULL) {
        Summary->RightOnly++;
      }
    }
  }

  if (left != NULL) FreePool(left);
  if (right != NULL) FreePool(right);
  return status;
}

EFI_STATUS SyncCompareTrees(
  IN  CONST CHAR16* LeftRoot,
  IN  CONST CHAR16* RightRoot,
  IN  EC_SYNC_PROGRESS Progress,
  OUT EC_SYNC_SUMMARY* Summary
) {
  SYNC_WALK walk;
  BOOLEAN leftDirectory = FALSE;
  BOOLEAN rightDirectory = FALSE;

  if (LeftRoot == NULL || RightRoot == NULL || Summary == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(Summary, sizeof(*Summary));
  ZeroMem(&walk, sizeof(walk));
  walk.Report = Progress;
  if (!FsFileExists(LeftRoot, &leftDirectory) || !leftDirectory ||
      !FsFileExists(RightRoot, &rightDirectory) || !rightDirectory) {
    return EFI_NOT_FOUND;
  }
  return SyncCompareInternal(LeftRoot, RightRoot, 0, &walk, Summary);
}

static EFI_STATUS SyncCopyEntry(
  IN CONST CHAR16* Source,
  IN CONST CHAR16* Destination,
  IN BOOLEAN IsTree,
  IN OUT EC_SYNC_RESULT* Result
) {
  EFI_STATUS status = FsCopyRecursive(Source, Destination, NULL);
  if (EFI_ERROR(status)) {
    Result->Errors++;
  } else if (IsTree) {
    Result->CopiedTrees++;
  } else {
    Result->CopiedFiles++;
  }
  return status;
}

static EFI_STATUS SyncUpdateInternal(
  IN CONST CHAR16* SourceRoot,
  IN CONST CHAR16* DestinationRoot,
  IN UINTN Depth,
  IN OUT SYNC_WALK* Walk,
  IN OUT EC_SYNC_RESULT* Result
) {
  FS_FILE_ITEM* source = NULL;
  FS_FILE_ITEM* destination = NULL;
  UINTN sourceCount = 0;
  UINTN destinationCount = 0;
  EFI_STATUS status;

  if (Depth > SYNC_MAX_DEPTH) return EFI_BAD_BUFFER_SIZE;
  status = FsListDirectory(SourceRoot, &source, &sourceCount);
  if (EFI_ERROR(status)) return status;
  status = FsListDirectory(DestinationRoot, &destination, &destinationCount);
  if (EFI_ERROR(status)) {
    FreePool(source);
    return status;
  }

  for (UINTN i = 0; i < sourceCount; i++) {
    CONST FS_FILE_ITEM* other;
    CHAR16 sourcePath[MAX_PATH_LEN];
    CHAR16 destinationPath[MAX_PATH_LEN];
    BOOLEAN equal = FALSE;

    if (!SyncUsable(&source[i])) continue;
    FsCombinePath(sourcePath, SourceRoot, source[i].Name);
    FsCombinePath(destinationPath, DestinationRoot, source[i].Name);
    if (!SyncTick(Walk, sourcePath, source[i].IsDirectory)) {
      status = EFI_ABORTED;
      break;
    }

    other = SyncFindItem(destination, destinationCount, source[i].Name);

    if (other == NULL) {
      status = SyncCopyEntry(sourcePath, destinationPath, source[i].IsDirectory, Result);
      if (EFI_ERROR(status)) break;
      continue;
    }

    if (source[i].IsDirectory != other->IsDirectory) {
      status = FsDeleteRecursive(destinationPath);
      if (EFI_ERROR(status)) { Result->Errors++; break; }
      Result->ReplacedEntries++;
      status = SyncCopyEntry(sourcePath, destinationPath, source[i].IsDirectory, Result);
      if (EFI_ERROR(status)) break;
      continue;
    }

    if (source[i].IsDirectory) {
      status = SyncUpdateInternal(sourcePath, destinationPath, Depth + 1, Walk, Result);
      if (EFI_ERROR(status)) break;
      continue;
    }

    if (source[i].Size == other->Size) {
      status = SyncFilesEqual(sourcePath, destinationPath, &equal);
      if (EFI_ERROR(status)) { Result->Errors++; break; }
    }
    if (equal) {
      Result->SkippedEqual++;
      continue;
    }

    status = FsDeleteRecursive(destinationPath);
    if (EFI_ERROR(status)) { Result->Errors++; break; }
    Result->ReplacedEntries++;
    status = SyncCopyEntry(sourcePath, destinationPath, FALSE, Result);
    if (EFI_ERROR(status)) break;
  }

  if (source != NULL) FreePool(source);
  if (destination != NULL) FreePool(destination);
  return status;
}

EFI_STATUS SyncUpdateTree(
  IN  CONST CHAR16* SourceRoot,
  IN  CONST CHAR16* DestinationRoot,
  IN  EC_SYNC_PROGRESS Progress,
  OUT EC_SYNC_RESULT* Result
) {
  SYNC_WALK walk;
  BOOLEAN sourceDirectory = FALSE;
  BOOLEAN destinationDirectory = FALSE;
  EFI_STATUS status;

  if (SourceRoot == NULL || DestinationRoot == NULL || Result == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(Result, sizeof(*Result));
  ZeroMem(&walk, sizeof(walk));
  walk.Report = Progress;
  if (!FsFileExists(SourceRoot, &sourceDirectory) || !sourceDirectory ||
      !FsFileExists(DestinationRoot, &destinationDirectory) || !destinationDirectory) {
    return EFI_NOT_FOUND;
  }
  status = SyncUpdateInternal(SourceRoot, DestinationRoot, 0, &walk, Result);
  // An interrupted update has still written whatever it managed to write, so
  // the destination is flushed either way; only a hard error skips that.
  if (!EFI_ERROR(status) || status == EFI_ABORTED) {
    EFI_STATUS flush = FsFlushVolumeForPath(DestinationRoot);
    if (!EFI_ERROR(status) && EFI_ERROR(flush)) status = flush;
  }
  return status;
}
