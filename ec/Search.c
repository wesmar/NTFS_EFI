// Search.c - recursive file search across a directory tree.
//
// The walk is deliberately depth-first with an explicit depth cap and an
// explicit hit cap, and it polls the keyboard between directories: a search
// started by accident at the root of a 7 GB volume has to be interruptible,
// and it must not be able to exhaust the pool on a directory loop that a
// damaged volume can present.
#include "Search.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "Gui.h"
#include "PanelOps.h"
#include "UiConsole.h"
#include "Viewer.h"

// One read at a time when scanning contents. The buffer keeps room for the
// tail of the previous chunk in front of it, so a match lying across a chunk
// boundary is still found.
#define SEARCH_CHUNK 65536

// What the walk needs to know when it is looking inside files as well as at
// their names. Kept beside the walk rather than inside SEARCH_RESULT: the
// caller has no use for a scratch buffer.
typedef struct {
  CONST UINT8* Needle;
  UINTN NeedleLen;
  UINT8* Buffer;         // SEARCH_CHUNK + NeedleLen - 1 bytes
  UINTN BufferSize;
  CONST CHAR16* Root;    // for the progress box only
} SEARCH_CONTENT;

// Esc between directories cancels. Reading the key here and nowhere else keeps
// the walk itself free of UI concerns.
static BOOLEAN SearchAbortRequested(VOID)
{
  EFI_INPUT_KEY key;

  if (gST == NULL || gST->ConIn == NULL) return FALSE;
  if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) return FALSE;
  return (BOOLEAN)(key.ScanCode == SCAN_ESC || key.UnicodeChar == 27);
}

/*
 * Whether the file at Path holds the needle. The file is read in chunks and
 * each chunk is searched together with the last NeedleLen-1 bytes of the one
 * before it, which is what stops a match that straddles a chunk boundary from
 * being missed. Nothing is allocated per file; the buffer belongs to the walk.
 */
static BOOLEAN SearchFileContains(
  IN CONST CHAR16* Path,
  IN OUT SEARCH_CONTENT* Content,
  OUT BOOLEAN* ReadFailed
) {
  EFI_FILE_PROTOCOL* file = NULL;
  UINTN carry = 0;
  BOOLEAN found = FALSE;

  *ReadFailed = FALSE;
  if (EFI_ERROR(FsOpenFileForRead(Path, &file)) || file == NULL) {
    *ReadFailed = TRUE;
    return FALSE;
  }

  for (;;) {
    UINTN readSize = Content->BufferSize - carry;
    UINT64 hit = 0;
    UINTN filled;

    if (EFI_ERROR(file->Read(file, &readSize, Content->Buffer + carry))) {
      *ReadFailed = TRUE;
      break;
    }
    if (readSize == 0) break;

    filled = carry + readSize;
    if (ViewerFindBytes(Content->Buffer, filled, Content->Needle, Content->NeedleLen, 0, &hit)) {
      found = TRUE;
      break;
    }

    // Carry the tail forward, so the next chunk is searched with it in front.
    carry = Content->NeedleLen - 1;
    if (carry > filled) carry = filled;
    if (carry > 0) {
      CopyMem(Content->Buffer, Content->Buffer + filled - carry, carry);
    }
  }

  file->Close(file);
  return found;
}

static VOID SearchWalk(
  IN CONST CHAR16* Dir,
  IN CONST CHAR16* Mask,
  IN UINTN Depth,
  IN OUT SEARCH_CONTENT* Content,
  IN OUT SEARCH_RESULT* Result
) {
  FS_FILE_ITEM* items = NULL;
  UINTN count = 0;
  UINTN i;

  if (Result->Aborted || Result->HitLimit) return;
  if (Depth >= SEARCH_MAX_DEPTH) {
    Result->DepthLimit = TRUE;
    return;
  }
  if (EFI_ERROR(FsListDirectory(Dir, &items, &count)) || items == NULL) {
    return;
  }

  Result->DirsVisited++;
  if (SearchAbortRequested()) {
    Result->Aborted = TRUE;
    FreePool(items);
    return;
  }

  for (i = 0; i < count; i++) {
    if (!PanelOpsIsUsableItem(&items[i])) continue;
    if (!PanelOpsMatchMask(items[i].Name, Mask)) continue;

    // Reading a file takes long enough that Esc has to be polled per file, not
    // only per directory, and long enough that the box has to say where it is.
    if (Content != NULL) {
      CHAR16 candidate[MAX_PATH_LEN];
      BOOLEAN readFailed = FALSE;

      if (items[i].IsDirectory) continue;
      FsCombinePath(candidate, Dir, items[i].Name);
      Result->FilesScanned++;
      if ((Result->FilesScanned & 0x07) == 1) {
        GuiDrawTreeProgress(L"Searching file contents", candidate,
                            Result->FilesScanned, Result->DirsVisited);
      }
      if (SearchAbortRequested()) {
        Result->Aborted = TRUE;
        break;
      }
      if (!SearchFileContains(candidate, Content, &readFailed)) {
        if (readFailed) Result->ReadErrors = TRUE;
        continue;
      }
    }

    if (Result->HitCount >= SEARCH_MAX_HITS) {
      Result->HitLimit = TRUE;
      break;
    }
    {
      SEARCH_HIT* hit = &Result->Hits[Result->HitCount++];
      StrCpyS(hit->Dir, MAX_PATH_LEN, Dir);
      StrCpyS(hit->Name, ARRAY_SIZE(hit->Name), items[i].Name);
      hit->Size = items[i].Size;
      hit->IsDirectory = items[i].IsDirectory;
    }
  }

  for (i = 0; i < count && !Result->Aborted && !Result->HitLimit; i++) {
    if (!items[i].IsDirectory || !PanelOpsIsUsableItem(&items[i])) continue;
    {
      CHAR16 child[MAX_PATH_LEN];
      FsCombinePath(child, Dir, items[i].Name);
      SearchWalk(child, Mask, Depth + 1, Content, Result);
    }
  }

  FreePool(items);
}

EFI_STATUS SearchCollect(
  IN  CONST CHAR16* Root,
  IN  CONST CHAR16* Mask,
  IN  CONST CHAR16* Containing,
  OUT SEARCH_RESULT* Result
) {
  UINT8 needle[SEARCH_MAX_NEEDLE];
  SEARCH_CONTENT content;
  UINTN needleLen = 0;

  if (Root == NULL || Mask == NULL || Result == NULL || Mask[0] == L'\0') {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem(&content, sizeof(content));
  if (Containing != NULL && Containing[0] != L'\0') {
    if (!ViewerNeedleToBytes(Containing, needle, sizeof(needle), &needleLen)) {
      return EFI_INVALID_PARAMETER;
    }
    content.Needle = needle;
    content.NeedleLen = needleLen;
    content.Root = Root;
    content.BufferSize = SEARCH_CHUNK + needleLen - 1;
    content.Buffer = AllocatePool(content.BufferSize);
    if (content.Buffer == NULL) return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem(Result, sizeof(*Result));
  Result->Hits = AllocateZeroPool(SEARCH_MAX_HITS * sizeof(SEARCH_HIT));
  if (Result->Hits == NULL) {
    if (content.Buffer != NULL) FreePool(content.Buffer);
    return EFI_OUT_OF_RESOURCES;
  }

  SearchWalk(Root, Mask, 0, needleLen > 0 ? &content : NULL, Result);
  if (content.Buffer != NULL) FreePool(content.Buffer);
  return EFI_SUCCESS;
}

VOID SearchFree(IN OUT SEARCH_RESULT* Result)
{
  if (Result == NULL) return;
  if (Result->Hits != NULL) {
    FreePool(Result->Hits);
    Result->Hits = NULL;
  }
  Result->HitCount = 0;
}

// One display line per hit: the directory it sits in, then the name, so the
// eye finds the path first when several copies of one file turn up.
static VOID SearchFormatLine(
  IN  CONST SEARCH_HIT* Hit,
  OUT CHAR16* Line,
  IN  UINTN LineChars
) {
  CONST CHAR16* tail = Hit->IsDirectory ? L"\\" : L"";
  UnicodeSPrint(Line, LineChars * sizeof(CHAR16), L"%s\\%s%s", Hit->Dir, Hit->Name, tail);
}

BOOLEAN SearchRunInteractive(
  IN  CONST CHAR16* Root,
  OUT CHAR16* OutDir,
  OUT CHAR16* OutName
) {
  CHAR16 mask[128];
  CHAR16 containing[96];
  SEARCH_RESULT result;
  CHAR16* lineStore = NULL;
  CHAR16** lines = NULL;
  UINTN chosen = 0;
  BOOLEAN picked = FALSE;
  EFI_STATUS status;
  UINTN i;

  if (Root == NULL || Root[0] == L'\0' || OutDir == NULL || OutName == NULL) {
    return FALSE;
  }

  StrCpyS(mask, ARRAY_SIZE(mask), L"*.*");
  if (!GuiDrawInputBox(L"Find file", L"Name or mask (e.g. *.sys):", mask, ARRAY_SIZE(mask))) {
    return FALSE;
  }
  if (mask[0] == L'\0') return FALSE;

  // Asked separately so that leaving it empty is the obvious thing to do: a
  // name search is the common case and must not cost an extra decision.
  containing[0] = L'\0';
  if (!GuiDrawInputBox(L"Find file", L"Containing text (empty searches names only):",
                       containing, ARRAY_SIZE(containing))) {
    return FALSE;
  }

  if (containing[0] != L'\0') {
    GuiDrawTreeProgress(L"Searching file contents", Root, 0, 0);
  } else {
    GuiDrawSearchProgress(Root, mask, 0);
  }
  status = SearchCollect(Root, mask, containing, &result);
  if (status == EFI_INVALID_PARAMETER) {
    GuiDrawMsgBox(L"Find file", L"Type plain ASCII text to look for.");
    return FALSE;
  }
  if (EFI_ERROR(status)) {
    GuiDrawMsgBox(L"Find file", L"Not enough memory for the search.");
    return FALSE;
  }

  if (result.HitCount == 0) {
    GuiDrawMsgBox(L"Find file",
                  result.Aborted     ? L"Search cancelled."
                  : result.ReadErrors ? L"Nothing matched. Some files could not be read."
                                      : L"Nothing matched.");
    SearchFree(&result);
    return FALSE;
  }

  lineStore = AllocateZeroPool(result.HitCount * MAX_PATH_LEN * sizeof(CHAR16));
  lines = AllocateZeroPool(result.HitCount * sizeof(CHAR16*));
  if (lineStore == NULL || lines == NULL) {
    if (lineStore) FreePool(lineStore);
    if (lines) FreePool(lines);
    SearchFree(&result);
    GuiDrawMsgBox(L"Find file", L"Not enough memory to show the results.");
    return FALSE;
  }
  for (i = 0; i < result.HitCount; i++) {
    lines[i] = lineStore + i * MAX_PATH_LEN;
    SearchFormatLine(&result.Hits[i], lines[i], MAX_PATH_LEN);
  }

  {
    CHAR16 title[128];
    CONST CHAR16* note = result.HitLimit  ? L" (first 512)"
                       : result.Aborted   ? L" (cancelled)"
                       : result.DepthLimit ? L" (depth capped)"
                                           : L"";
    if (result.FilesScanned > 0) {
      UnicodeSPrint(title, sizeof(title), L"Found %d in %d files read%s",
                    (UINT32)result.HitCount, (UINT32)result.FilesScanned, note);
    } else {
      UnicodeSPrint(title, sizeof(title), L"Found %d in %d dirs%s",
                    (UINT32)result.HitCount, (UINT32)result.DirsVisited, note);
    }
    picked = GuiDrawListPicker(title, (CONST CHAR16**)lines, result.HitCount, &chosen);
  }

  if (picked && chosen < result.HitCount) {
    StrCpyS(OutDir, MAX_PATH_LEN, result.Hits[chosen].Dir);
    StrCpyS(OutName, 256, result.Hits[chosen].Name);
  } else {
    picked = FALSE;
  }

  FreePool(lines);
  FreePool(lineStore);
  SearchFree(&result);
  return picked;
}
