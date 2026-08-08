// Sync.h - recursive directory comparison and one-way update.
#pragma once

#include <Uefi.h>

typedef struct {
  UINTN LeftOnly;
  UINTN RightOnly;
  UINTN Different;
  UINTN EqualFiles;
  UINTN CommonDirectories;
  UINTN Errors;
} EC_SYNC_SUMMARY;

typedef struct {
  UINTN CopiedFiles;
  UINTN CopiedTrees;
  UINTN ReplacedEntries;
  UINTN SkippedEqual;
  UINTN Errors;
} EC_SYNC_RESULT;

// Compares file contents with SHA-256. A directory missing on one side counts
// as one tree entry; its descendants are deliberately not double-counted.
EFI_STATUS SyncCompareTrees(
  IN CONST CHAR16* LeftRoot,
  IN CONST CHAR16* RightRoot,
  OUT EC_SYNC_SUMMARY* Summary
);

// Copies missing and content-different entries Source -> Destination. Entries
// existing only at Destination are retained: this is an update, not a mirror.
EFI_STATUS SyncUpdateTree(
  IN CONST CHAR16* SourceRoot,
  IN CONST CHAR16* DestinationRoot,
  OUT EC_SYNC_RESULT* Result
);

