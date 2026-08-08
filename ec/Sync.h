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

/*
 * Called as the walk moves through the tree, so the caller can say where it is
 * and let the user stop it. Returning FALSE aborts; the walk then unwinds and
 * the entry point returns EFI_ABORTED with the counters filled in as far as it
 * got. Hashing a large tree takes minutes, and without this the screen stands
 * still for all of them and looks like a hang.
 *
 * NULL means no reporting and no way to cancel, which is what the self-test
 * wants: it has no screen and no keyboard.
 */
typedef BOOLEAN (*EC_SYNC_PROGRESS)(
  IN CONST CHAR16* CurrentPath,
  IN UINTN FilesSeen,
  IN UINTN DirectoriesSeen
);

// Compares file contents with SHA-256. A directory missing on one side counts
// as one tree entry; its descendants are deliberately not double-counted.
EFI_STATUS SyncCompareTrees(
  IN  CONST CHAR16* LeftRoot,
  IN  CONST CHAR16* RightRoot,
  IN  EC_SYNC_PROGRESS Progress,
  OUT EC_SYNC_SUMMARY* Summary
);

// Copies missing and content-different entries Source -> Destination. Entries
// existing only at Destination are retained: this is an update, not a mirror.
EFI_STATUS SyncUpdateTree(
  IN  CONST CHAR16* SourceRoot,
  IN  CONST CHAR16* DestinationRoot,
  IN  EC_SYNC_PROGRESS Progress,
  OUT EC_SYNC_RESULT* Result
);

