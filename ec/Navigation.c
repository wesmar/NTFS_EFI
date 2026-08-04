// Navigation.c - path history and hotlist helpers.
#include "Navigation.h"
#include "Config.h"
#include "Gui.h"
#include "UiConsole.h"

#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>

static BOOLEAN SamePath(IN CONST CHAR16* A, IN CONST CHAR16* B)
{
  if (A == NULL || B == NULL) return FALSE;
  return StrCmp(A, B) == 0;
}

VOID NavHistoryInit(OUT NAV_HISTORY* History, IN CONST CHAR16* InitialPath)
{
  if (History == NULL) return;
  ZeroMem(History, sizeof(NAV_HISTORY));
  if (InitialPath != NULL) {
    StrCpyS(History->Entries[0], MAX_PATH_LEN, InitialPath);
    History->Count = 1;
    History->Current = 0;
  }
}

VOID NavHistoryPush(IN OUT NAV_HISTORY* History, IN CONST CHAR16* Path)
{
  if (History == NULL || Path == NULL) return;
  if (History->Count > 0 && SamePath(History->Entries[History->Current], Path)) return;

  if (History->Current + 1 < History->Count) {
    History->Count = History->Current + 1;
  }

  if (History->Count < NAV_HISTORY_MAX) {
    History->Current = History->Count++;
  } else {
    for (UINTN i = 1; i < NAV_HISTORY_MAX; i++) {
      StrCpyS(History->Entries[i - 1], MAX_PATH_LEN, History->Entries[i]);
    }
    History->Current = NAV_HISTORY_MAX - 1;
  }
  StrCpyS(History->Entries[History->Current], MAX_PATH_LEN, Path);
}

BOOLEAN NavHistoryBack(IN OUT NAV_HISTORY* History, OUT CHAR16* Path)
{
  if (History == NULL || Path == NULL || History->Count == 0 || History->Current == 0) return FALSE;
  History->Current--;
  StrCpyS(Path, MAX_PATH_LEN, History->Entries[History->Current]);
  return TRUE;
}

BOOLEAN NavHistoryForward(IN OUT NAV_HISTORY* History, OUT CHAR16* Path)
{
  if (History == NULL || Path == NULL || History->Count == 0 || History->Current + 1 >= History->Count) return FALSE;
  History->Current++;
  StrCpyS(Path, MAX_PATH_LEN, History->Entries[History->Current]);
  return TRUE;
}

BOOLEAN NavApplyPath(IN OUT PANEL* Panel, IN OUT NAV_HISTORY* History, IN CONST CHAR16* Path)
{
  EFI_STATUS Status;
  if (Panel == NULL || Path == NULL) return FALSE;
  StrCpyS(Panel->Path, MAX_PATH_LEN, Path);
  Status = PanelRefreshKeep(Panel, NULL, Panel->SelectedIndex);
  if (EFI_ERROR(Status)) return FALSE;
  NavHistoryPush(History, Panel->Path);
  return TRUE;
}

BOOLEAN NavShowHotlist(IN OUT PANEL* Panel, IN OUT NAV_HISTORY* History)
{
  UINTN width, height;
  UINTN cellW, cellH;
  UINTN boxW;
  UINTN boxH;
  UINTN boxX;
  UINTN boxY;

  if (gEcConfig.HotDirCount == 0) {
    GuiDrawMsgBox(L"Hotlist", L"No HotDir entries configured in EC.ini.");
    return FALSE;
  }

  UiGfxGetDimensions(&width, &height);
  UiGfxGetCellSize(&cellW, &cellH);
  boxW = width > 900 ? 760 : width - 80;
  boxH = cellH * (gEcConfig.HotDirCount + 5);
  boxX = (width - boxW) / 2;
  boxY = (height - boxH) / 2;

  UiGfxFillRectRgb(boxX, boxY, boxW, boxH, 0, 0, 170);
  DrawBorder(boxX, boxY, boxW, boxH, 255, 255, 85, 3);
  UiGfxDrawAsciiAt(boxX + 20, boxY + 15, "Directory Hotlist", 255, 255, 85);

  for (UINTN i = 0; i < gEcConfig.HotDirCount; i++) {
    CHAR16 line[MAX_PATH_LEN + 8];
    UnicodeSPrint(line, sizeof(line), L"%d  %s", (UINT32)(i + 1), gEcConfig.HotDirs[i]);
    UiGfxDrawUnicodeAt(boxX + 25, boxY + 15 + cellH * (i + 2), line, 255, 255, 255);
  }
  UiGfxDrawAsciiAt(boxX + 25, boxY + boxH - cellH - 15, "1-9 select, ESC cancel", 85, 255, 255);
  UiGfxFlush();

  for (;;) {
    EFI_INPUT_KEY key;
    UINTN index;
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
    if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) continue;
    if (key.ScanCode == SCAN_ESC || key.UnicodeChar == 27) return FALSE;
    if (key.UnicodeChar >= L'1' && key.UnicodeChar <= L'9') {
      UINTN selected = (UINTN)(key.UnicodeChar - L'1');
      if (selected < gEcConfig.HotDirCount) {
        return NavApplyPath(Panel, History, gEcConfig.HotDirs[selected]);
      }
    }
  }
}
