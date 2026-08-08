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

// Esc between directories cancels. Reading the key here and nowhere else keeps
// the walk itself free of UI concerns.
static BOOLEAN SearchAbortRequested(VOID)
{
  EFI_INPUT_KEY key;

  if (gST == NULL || gST->ConIn == NULL) return FALSE;
  if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) return FALSE;
  return (BOOLEAN)(key.ScanCode == SCAN_ESC || key.UnicodeChar == 27);
}

static VOID SearchWalk(
  IN CONST CHAR16* Dir,
  IN CONST CHAR16* Mask,
  IN UINTN Depth,
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

    if (PanelOpsMatchMask(items[i].Name, Mask)) {
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
  }

  for (i = 0; i < count && !Result->Aborted && !Result->HitLimit; i++) {
    if (!items[i].IsDirectory || !PanelOpsIsUsableItem(&items[i])) continue;
    {
      CHAR16 child[MAX_PATH_LEN];
      FsCombinePath(child, Dir, items[i].Name);
      SearchWalk(child, Mask, Depth + 1, Result);
    }
  }

  FreePool(items);
}

EFI_STATUS SearchCollect(
  IN  CONST CHAR16* Root,
  IN  CONST CHAR16* Mask,
  OUT SEARCH_RESULT* Result
) {
  if (Root == NULL || Mask == NULL || Result == NULL || Mask[0] == L'\0') {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem(Result, sizeof(*Result));
  Result->Hits = AllocateZeroPool(SEARCH_MAX_HITS * sizeof(SEARCH_HIT));
  if (Result->Hits == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  SearchWalk(Root, Mask, 0, Result);
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
  SEARCH_RESULT result;
  CHAR16* lineStore = NULL;
  CHAR16** lines = NULL;
  UINTN chosen = 0;
  BOOLEAN picked = FALSE;
  UINTN i;

  if (Root == NULL || Root[0] == L'\0' || OutDir == NULL || OutName == NULL) {
    return FALSE;
  }

  StrCpyS(mask, ARRAY_SIZE(mask), L"*.*");
  if (!GuiDrawInputBox(L"Find file", L"Name or mask (e.g. *.sys):", mask, ARRAY_SIZE(mask))) {
    return FALSE;
  }
  if (mask[0] == L'\0') return FALSE;

  GuiDrawSearchProgress(Root, mask, 0);
  if (EFI_ERROR(SearchCollect(Root, mask, &result))) {
    GuiDrawMsgBox(L"Find file", L"Not enough memory for the search.");
    return FALSE;
  }

  if (result.HitCount == 0) {
    GuiDrawMsgBox(L"Find file", result.Aborted ? L"Search cancelled." : L"Nothing matched.");
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
    UnicodeSPrint(title, sizeof(title), L"Found %d in %d dirs%s",
                  (UINT32)result.HitCount, (UINT32)result.DirsVisited, note);
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
