// Sync.c - recursive compare/update kept separate from the interactive UI.
#include "Sync.h"
#include "Checksum.h"
#include "FileSystem.h"

#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>

#define SYNC_MAX_DEPTH 64

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
    other = SyncFindItem(right, rightCount, left[i].Name);
    if (other == NULL) {
      Summary->LeftOnly++;
      continue;
    }
    if (left[i].IsDirectory != other->IsDirectory) {
      Summary->Different++;
      continue;
    }
    FsCombinePath(leftPath, LeftRoot, left[i].Name);
    FsCombinePath(rightPath, RightRoot, left[i].Name);
    if (left[i].IsDirectory) {
      Summary->CommonDirectories++;
      status = SyncCompareInternal(leftPath, rightPath, Depth + 1, Summary);
      if (EFI_ERROR(status)) {
        Summary->Errors++;
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
  IN CONST CHAR16* LeftRoot,
  IN CONST CHAR16* RightRoot,
  OUT EC_SYNC_SUMMARY* Summary
) {
  BOOLEAN leftDirectory = FALSE;
  BOOLEAN rightDirectory = FALSE;
  if (LeftRoot == NULL || RightRoot == NULL || Summary == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(Summary, sizeof(*Summary));
  if (!FsFileExists(LeftRoot, &leftDirectory) || !leftDirectory ||
      !FsFileExists(RightRoot, &rightDirectory) || !rightDirectory) {
    return EFI_NOT_FOUND;
  }
  return SyncCompareInternal(LeftRoot, RightRoot, 0, Summary);
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
      status = SyncUpdateInternal(sourcePath, destinationPath, Depth + 1, Result);
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
  IN CONST CHAR16* SourceRoot,
  IN CONST CHAR16* DestinationRoot,
  OUT EC_SYNC_RESULT* Result
) {
  BOOLEAN sourceDirectory = FALSE;
  BOOLEAN destinationDirectory = FALSE;
  EFI_STATUS status;

  if (SourceRoot == NULL || DestinationRoot == NULL || Result == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(Result, sizeof(*Result));
  if (!FsFileExists(SourceRoot, &sourceDirectory) || !sourceDirectory ||
      !FsFileExists(DestinationRoot, &destinationDirectory) || !destinationDirectory) {
    return EFI_NOT_FOUND;
  }
  status = SyncUpdateInternal(SourceRoot, DestinationRoot, 0, Result);
  if (!EFI_ERROR(status)) status = FsFlushVolumeForPath(DestinationRoot);
  return status;
}

