// Search.h - recursive file search across a directory tree.
#pragma once

#include <Uefi.h>
#include "FileSystem.h"

// Bounds. A pre-boot tool has no swap and no way to say "sorry, out of
// memory" halfway through a walk, so the walk is capped up front and the
// caller is told when a cap was reached rather than being handed a silently
// short answer.
#define SEARCH_MAX_HITS   512
#define SEARCH_MAX_DEPTH  24

typedef struct {
  CHAR16 Dir[MAX_PATH_LEN];   // directory the hit lives in
  CHAR16 Name[256];
  UINT64 Size;
  BOOLEAN IsDirectory;
} SEARCH_HIT;

typedef struct {
  SEARCH_HIT* Hits;
  UINTN HitCount;
  UINTN DirsVisited;
  BOOLEAN HitLimit;      // stopped at SEARCH_MAX_HITS
  BOOLEAN DepthLimit;    // at least one branch was deeper than SEARCH_MAX_DEPTH
  BOOLEAN Aborted;       // the user pressed Esc
} SEARCH_RESULT;

// Walks Root and collects every entry whose name matches Mask. Directories are
// matched too, so "*log*" finds a log directory as well as the files in it.
// Result->Hits is allocated here and released by SearchFree. No UI at all:
// this is the entry point the self-test drives.
EFI_STATUS SearchCollect(
  IN  CONST CHAR16* Root,
  IN  CONST CHAR16* Mask,
  OUT SEARCH_RESULT* Result
);

VOID SearchFree(IN OUT SEARCH_RESULT* Result);

// Asks for a mask, searches under Root, shows the hits and lets one be picked.
// Returns TRUE when the user chose a hit; OutDir then holds the directory to
// jump to and OutName the entry to put the cursor on.
BOOLEAN SearchRunInteractive(
  IN  CONST CHAR16* Root,
  OUT CHAR16* OutDir,
  OUT CHAR16* OutName
);
