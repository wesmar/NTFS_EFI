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

// The longest text a content search will look for, in bytes. The dialog that
// asks for it is narrower than this, so the cap can only be reached by a caller
// driving SearchCollect directly.
#define SEARCH_MAX_NEEDLE 128

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
  UINTN FilesScanned;    // files whose contents were read, 0 for a name search
  BOOLEAN HitLimit;      // stopped at SEARCH_MAX_HITS
  BOOLEAN DepthLimit;    // at least one branch was deeper than SEARCH_MAX_DEPTH
  BOOLEAN Aborted;       // the user pressed Esc
  BOOLEAN ReadErrors;    // at least one candidate file could not be read
} SEARCH_RESULT;

/*
 * Walks Root and collects every entry whose name matches Mask. Directories are
 * matched too, so "*log*" finds a log directory as well as the files in it.
 *
 * Containing narrows that further: when it is a non-empty string, a file is a
 * hit only if its contents hold that text, matched case-insensitively the same
 * way the viewer's find does. Directories never match a content search, since
 * a directory has no contents of its own to look in. NULL or an empty string
 * searches by name alone.
 *
 * Content search reads the whole of every candidate file in chunks, so the mask
 * is what keeps it affordable: "*.ini" with a needle reads a few kilobytes per
 * file, "*" with a needle reads the volume.
 *
 * Returns EFI_INVALID_PARAMETER for a needle that is not plain ASCII, rather
 * than truncating it into a search for something else.
 *
 * Result->Hits is allocated here and released by SearchFree. No UI at all:
 * this is the entry point the self-test drives.
 */
EFI_STATUS SearchCollect(
  IN  CONST CHAR16* Root,
  IN  CONST CHAR16* Mask,
  IN  CONST CHAR16* Containing,
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
