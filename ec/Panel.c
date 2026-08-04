// Panel.c — Panel state and directory navigation logic.
#include "Panel.h"
#include "Gui.h"
#include "PanelOps.h"
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "UiConsole.h"

VOID PanelInit(PANEL* Panel, IN CONST CHAR16* DefaultPath)
{
  if (Panel == NULL) return;
  ZeroMem(Panel, sizeof(PANEL));
  Panel->SortMode = PANEL_SORT_NAME;
  Panel->SortDescending = FALSE;
  StrCpyS(Panel->FilterMask, 128, L"*");
  if (DefaultPath != NULL) {
    StrCpyS(Panel->Path, MAX_PATH_LEN, DefaultPath);
  }
}

static BOOLEAN PanelFilterEnabled(IN CONST PANEL* Panel)
{
  return Panel != NULL &&
         Panel->FilterMask[0] != L'\0' &&
         !(Panel->FilterMask[0] == L'*' && Panel->FilterMask[1] == L'\0');
}

static VOID PanelApplyFilter(PANEL* Panel)
{
  UINTN Write = 0;
  if (Panel == NULL || Panel->Files == NULL || !PanelFilterEnabled(Panel)) return;

  for (UINTN Read = 0; Read < Panel->FileCount; Read++) {
    BOOLEAN Keep = FALSE;
    if (StrCmp(Panel->Files[Read].Name, L"..") == 0) {
      Keep = TRUE;
    } else if (PanelOpsMatchMask(Panel->Files[Read].Name, Panel->FilterMask)) {
      Keep = TRUE;
    }

    if (Keep) {
      if (Write != Read) {
        CopyMem(&Panel->Files[Write], &Panel->Files[Read], sizeof(FS_FILE_ITEM));
      }
      Write++;
    }
  }

  Panel->FileCount = Write;
}

static INTN CompareTime(IN CONST EFI_TIME* A, IN CONST EFI_TIME* B)
{
  if (A->Year != B->Year) return (A->Year < B->Year) ? -1 : 1;
  if (A->Month != B->Month) return (A->Month < B->Month) ? -1 : 1;
  if (A->Day != B->Day) return (A->Day < B->Day) ? -1 : 1;
  if (A->Hour != B->Hour) return (A->Hour < B->Hour) ? -1 : 1;
  if (A->Minute != B->Minute) return (A->Minute < B->Minute) ? -1 : 1;
  if (A->Second != B->Second) return (A->Second < B->Second) ? -1 : 1;
  return 0;
}

static CONST CHAR16* FileExtension(IN CONST CHAR16* Name)
{
  CONST CHAR16* Dot = NULL;
  if (Name == NULL) return L"";
  for (UINTN i = 0; Name[i] != L'\0'; i++) {
    if (Name[i] == L'.') Dot = &Name[i + 1];
  }
  return (Dot != NULL) ? Dot : L"";
}

static CHAR16 UpCaseChar(CHAR16 Ch)
{
  if (Ch >= L'a' && Ch <= L'z') return (CHAR16)(Ch - (L'a' - L'A'));
  return Ch;
}

static INTN CompareTextInsensitive(IN CONST CHAR16* A, IN CONST CHAR16* B)
{
  UINTN i = 0;
  if (A == NULL && B == NULL) return 0;
  if (A == NULL) return -1;
  if (B == NULL) return 1;

  while (A[i] != L'\0' && B[i] != L'\0') {
    CHAR16 Ca = UpCaseChar(A[i]);
    CHAR16 Cb = UpCaseChar(B[i]);
    if (Ca != Cb) return (Ca < Cb) ? -1 : 1;
    i++;
  }
  if (A[i] == B[i]) return 0;
  return (A[i] == L'\0') ? -1 : 1;
}

static INTN CompareItems(IN CONST PANEL* Panel, IN CONST FS_FILE_ITEM* A, IN CONST FS_FILE_ITEM* B)
{
  INTN Result = 0;

  if (StrCmp(A->Name, L"..") == 0) return -1;
  if (StrCmp(B->Name, L"..") == 0) return 1;

  if (A->IsDirectory != B->IsDirectory) {
    return A->IsDirectory ? -1 : 1;
  }

  switch (Panel->SortMode) {
    case PANEL_SORT_EXTENSION:
      Result = CompareTextInsensitive(FileExtension(A->Name), FileExtension(B->Name));
      if (Result == 0) Result = CompareTextInsensitive(A->Name, B->Name);
      break;

    case PANEL_SORT_MODIFIED:
      Result = CompareTime(&A->ModificationTime, &B->ModificationTime);
      if (Result == 0) Result = CompareTextInsensitive(A->Name, B->Name);
      break;

    case PANEL_SORT_SIZE:
      if (A->Size != B->Size) Result = (A->Size < B->Size) ? -1 : 1;
      if (Result == 0) Result = CompareTextInsensitive(A->Name, B->Name);
      break;

    case PANEL_SORT_NAME:
    default:
      Result = CompareTextInsensitive(A->Name, B->Name);
      break;
  }

  if (Panel->SortDescending && Result != 0 && StrCmp(A->Name, L"..") != 0 && StrCmp(B->Name, L"..") != 0) {
    Result = -Result;
  }
  return Result;
}

static VOID PanelSortItems(PANEL* Panel)
{
  UINTN Gap;
  if (Panel == NULL || Panel->Files == NULL || Panel->FileCount < 2) return;

  for (Gap = Panel->FileCount / 2; Gap > 0; Gap /= 2) {
    for (UINTN i = Gap; i < Panel->FileCount; i++) {
      FS_FILE_ITEM Temp;
      UINTN j = i;
      CopyMem(&Temp, &Panel->Files[i], sizeof(FS_FILE_ITEM));

      while (j >= Gap && CompareItems(Panel, &Panel->Files[j - Gap], &Temp) > 0) {
        CopyMem(&Panel->Files[j], &Panel->Files[j - Gap], sizeof(FS_FILE_ITEM));
        j -= Gap;
      }

      CopyMem(&Panel->Files[j], &Temp, sizeof(FS_FILE_ITEM));
    }
  }
}

VOID PanelFree(PANEL* Panel)
{
  if (Panel == NULL) return;
  if (Panel->Files != NULL) {
    FreePool(Panel->Files);
    Panel->Files = NULL;
  }
  Panel->FileCount = 0;
}

EFI_STATUS PanelRefreshKeep(PANEL* Panel, IN CONST CHAR16* PreferredName, IN INTN FallbackIndex)
{
  if (Panel == NULL) return EFI_INVALID_PARAMETER;

  FS_FILE_ITEM* oldFiles = Panel->Files;
  UINTN oldFileCount = Panel->FileCount;
  INTN oldSelected = Panel->SelectedIndex;
  CHAR16 oldSelectedName[256] = { 0 };

  // Save selection name so we can try to restore it after refresh
  if (oldSelected >= 0 && (UINTN)oldSelected < oldFileCount && oldFiles != NULL) {
    StrCpyS(oldSelectedName, 256, oldFiles[oldSelected].Name);
  }
  if (PreferredName != NULL && PreferredName[0] != L'\0') {
    StrCpyS(oldSelectedName, 256, PreferredName);
  }
  if (FallbackIndex < 0) {
    FallbackIndex = oldSelected;
  }

  Panel->Files = NULL;
  Panel->FileCount = 0;

  EFI_STATUS status = FsListDirectory(Panel->Path, &Panel->Files, &Panel->FileCount);
  if (oldFiles != NULL) {
    FreePool(oldFiles);
  }

  if (EFI_ERROR(status)) {
    Panel->SelectedIndex = -1;
    Panel->TopIndex = 0;
    return status;
  }

  if (Panel->FileCount > 0) {
    BOOLEAN restored = FALSE;
    PanelApplyFilter(Panel);
    PanelSortItems(Panel);
    Panel->SelectedIndex = (Panel->FileCount > 0) ? 0 : -1;
    if (oldSelectedName[0] != L'\0' && Panel->SelectedIndex >= 0) {
      for (UINTN i = 0; i < Panel->FileCount; i++) {
        if (StrCmp(Panel->Files[i].Name, oldSelectedName) == 0) {
          Panel->SelectedIndex = (INTN)i;
          restored = TRUE;
          break;
        }
      }
    }
    if (!restored && Panel->SelectedIndex >= 0) {
      if (FallbackIndex >= (INTN)Panel->FileCount) {
        FallbackIndex = (INTN)Panel->FileCount - 1;
      }
      if (FallbackIndex < 0) {
        FallbackIndex = 0;
      }
      Panel->SelectedIndex = FallbackIndex;
    }
  } else {
    Panel->SelectedIndex = -1;
  }

  // Adjust scroll TopIndex to keep selection visible
  if (Panel->SelectedIndex >= 0) {
    if ((UINTN)Panel->SelectedIndex < Panel->TopIndex) {
      Panel->TopIndex = (UINTN)Panel->SelectedIndex;
    }
  } else {
    Panel->TopIndex = 0;
  }

  return EFI_SUCCESS;
}

EFI_STATUS PanelRefresh(PANEL* Panel)
{
  return PanelRefreshKeep(Panel, NULL, -1);
}

VOID PanelSetFilter(PANEL* Panel, IN CONST CHAR16* Mask)
{
  if (Panel == NULL) return;
  if (Mask == NULL || Mask[0] == L'\0') {
    StrCpyS(Panel->FilterMask, 128, L"*");
  } else {
    StrCpyS(Panel->FilterMask, 128, Mask);
  }
  Panel->TopIndex = 0;
  Panel->SelectedIndex = 0;
  PanelRefresh(Panel);
}

VOID PanelSetSortMode(PANEL* Panel, PANEL_SORT_MODE SortMode)
{
  CHAR16 selectedName[256] = { 0 };
  if (Panel == NULL) return;

  if (Panel->SelectedIndex >= 0 && Panel->Files != NULL && (UINTN)Panel->SelectedIndex < Panel->FileCount) {
    StrCpyS(selectedName, 256, Panel->Files[Panel->SelectedIndex].Name);
  }

  if (Panel->SortMode == SortMode) {
    Panel->SortDescending = !Panel->SortDescending;
  } else {
    Panel->SortMode = SortMode;
    Panel->SortDescending = (SortMode == PANEL_SORT_MODIFIED || SortMode == PANEL_SORT_SIZE);
  }

  PanelSortItems(Panel);

  if (selectedName[0] != L'\0') {
    for (UINTN i = 0; i < Panel->FileCount; i++) {
      if (StrCmp(Panel->Files[i].Name, selectedName) == 0) {
        Panel->SelectedIndex = (INTN)i;
        break;
      }
    }
  }
}

VOID PanelNavigateUp(PANEL* Panel)
{
  if (Panel == NULL || Panel->FileCount == 0 || Panel->SelectedIndex <= 0) {
    return;
  }
  Panel->SelectedIndex--;
  if ((UINTN)Panel->SelectedIndex < Panel->TopIndex) {
    Panel->TopIndex = (UINTN)Panel->SelectedIndex;
  }
}

VOID PanelNavigateDown(PANEL* Panel, UINTN PageSize)
{
  if (Panel == NULL || Panel->FileCount == 0 || (UINTN)Panel->SelectedIndex + 1 >= Panel->FileCount) {
    return;
  }
  Panel->SelectedIndex++;
  if ((UINTN)Panel->SelectedIndex >= Panel->TopIndex + PageSize) {
    Panel->TopIndex = (UINTN)Panel->SelectedIndex - PageSize + 1;
  }
}

VOID PanelPageUp(PANEL* Panel, UINTN PageSize)
{
  if (Panel == NULL || Panel->FileCount == 0 || Panel->SelectedIndex <= 0) {
    return;
  }
  if (Panel->SelectedIndex >= (INTN)PageSize) {
    Panel->SelectedIndex -= PageSize;
  } else {
    Panel->SelectedIndex = 0;
  }

  if ((UINTN)Panel->SelectedIndex < Panel->TopIndex) {
    Panel->TopIndex = (UINTN)Panel->SelectedIndex;
  }
}

VOID PanelPageDown(PANEL* Panel, UINTN PageSize)
{
  if (Panel == NULL || Panel->FileCount == 0 || (UINTN)Panel->SelectedIndex + 1 >= Panel->FileCount) {
    return;
  }
  if ((UINTN)Panel->SelectedIndex + PageSize < Panel->FileCount) {
    Panel->SelectedIndex += PageSize;
  } else {
    Panel->SelectedIndex = (INTN)(Panel->FileCount - 1);
  }
}

VOID PanelEnter(PANEL* Panel, EFI_HANDLE ImageHandle)
{
  if (Panel == NULL || Panel->FileCount == 0 || Panel->SelectedIndex < 0) {
    return;
  }

  FS_FILE_ITEM* selected = &Panel->Files[Panel->SelectedIndex];

  if (selected->IsDirectory) {
    CHAR16 restoreName[256] = { 0 };
    BOOLEAN restoreParentSelection = (StrCmp(selected->Name, L"..") == 0);
    if (restoreParentSelection && Panel->Path[0] != L'\0') {
      UINTN len = StrLen(Panel->Path);
      while (len > 0 && Panel->Path[len - 1] == L'\\') {
        len--;
      }
      for (UINTN i = len; i > 0; i--) {
        if (Panel->Path[i - 1] == L'\\' || Panel->Path[i - 1] == L':') {
          UINTN nameLen = len - i;
          if (nameLen > 0 && nameLen < 256) {
            CopyMem(restoreName, &Panel->Path[i], nameLen * sizeof(CHAR16));
            restoreName[nameLen] = L'\0';
          }
          break;
        }
      }
    }

    CHAR16 oldPath[MAX_PATH_LEN];
    CHAR16 selectedName[256];
    StrCpyS(oldPath, MAX_PATH_LEN, Panel->Path);
    StrCpyS(selectedName, 256, selected->Name);

    // If path is empty, we are at the drives list screen. Hitting enter opens the drive!
    if (Panel->Path[0] == L'\0') {
      StrCpyS(Panel->Path, MAX_PATH_LEN, selected->Name);
      // Append backslash
      StrCatS(Panel->Path, MAX_PATH_LEN, L"\\");
    } else {
      CHAR16 newPath[MAX_PATH_LEN] = { 0 };
      FsCombinePath(newPath, Panel->Path, selected->Name);
      StrCpyS(Panel->Path, MAX_PATH_LEN, newPath);
    }
    Panel->TopIndex = 0;
    Panel->SelectedIndex = 0;

    EFI_STATUS refreshStatus = PanelRefreshKeep(Panel, restoreName, 0);
    if (EFI_ERROR(refreshStatus)) {
      // Revert the path and restore the selection of the item we tried to enter
      StrCpyS(Panel->Path, MAX_PATH_LEN, oldPath);
      PanelRefreshKeep(Panel, selectedName, 0);
      GuiDrawMsgBox(L"Error", L"Failed to enter directory.");
    }
  } else {
    // It's a file. If it ends in .efi, run it!
    UINTN len = StrLen(selected->Name);
    if (len > 4 &&
        (StrCmp(&selected->Name[len - 4], L".efi") == 0 ||
         StrCmp(&selected->Name[len - 4], L".EFI") == 0)) {
      
      CHAR16 appPath[MAX_PATH_LEN] = { 0 };
      FsCombinePath(appPath, Panel->Path, selected->Name);

      // Clean screen, shutdown UI console to restore console mode
      UiConsoleShutdown();

      // Launch application
      gST->ConOut->ClearScreen(gST->ConOut);
      gST->ConOut->EnableCursor(gST->ConOut, TRUE);

      FsStartEfiApp(ImageHandle, appPath);

      // Wait for keypress before returning to EC.efi
      gST->ConOut->OutputString(gST->ConOut, L"\nPress any key to return to EC...");
      UINTN index;
      EFI_INPUT_KEY key;
      gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
      gST->ConIn->ReadKeyStroke(gST->ConIn, &key);

      // Re-initialize GUI
      UiConsoleInit(gST);
      PanelRefresh(Panel);
    }
    else {
      GuiDrawMsgBox(L"File", L"Enter opens directories and EFI applications only. Use F3/F4 for regular files.");
    }
  }
}
