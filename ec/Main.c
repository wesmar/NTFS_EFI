// Main.c — EC.efi main application entry point and event loop.
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PrintLib.h>

#include "UiConsole.h"
#include "FileSystem.h"
#include "Panel.h"
#include "Gui.h"
#include "Viewer.h"
#include "Editor.h"
#include "PanelOps.h"
#include "Config.h"
#include "Navigation.h"
#include <Protocol/SimpleTextInEx.h>

#define COLOR_BLUE_R 0
#define COLOR_BLUE_G 0
#define COLOR_BLUE_B 170

#define COLOR_YELLOW_R 255
#define COLOR_YELLOW_G 255
#define COLOR_YELLOW_B 85

#define COLOR_WHITE_R 255
#define COLOR_WHITE_G 255
#define COLOR_WHITE_B 255

#define COLOR_CYAN_R 85
#define COLOR_CYAN_G 255
#define COLOR_CYAN_B 255

#define COLOR_BLACK_R 0
#define COLOR_BLACK_G 0
#define COLOR_BLACK_B 0

EFI_GUID gEfiSimpleTextInputExProtocolGuid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;

// Stubs for MSVC build compatibility with UefiApplicationEntryPoint.lib
extern CONST UINT32 _gUefiDriverRevision = 0;
CHAR8* gEfiCallerBaseName = "EC";
EFI_STATUS EFIAPI UefiUnload(IN EFI_HANDLE ImageHandle) { (VOID)ImageHandle; return EFI_SUCCESS; }

EFI_STATUS EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
);

VOID EFIAPI
ProcessLibraryConstructorList (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
)
{
  (VOID)ImageHandle;
  (VOID)SystemTable;
}

VOID EFIAPI
ProcessLibraryDestructorList (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
)
{
  (VOID)ImageHandle;
  (VOID)SystemTable;
}

EFI_STATUS EFIAPI
ProcessModuleEntryPointList (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
)
{
  return UefiMain (ImageHandle, SystemTable);
}

// Global copy paths for progress callback
static CHAR16 gCopySrc[MAX_PATH_LEN] = { 0 };
static CHAR16 gCopyDst[MAX_PATH_LEN] = { 0 };
static NAV_HISTORY gLeftHistory;
static NAV_HISTORY gRightHistory;

static VOID CopyCallback(UINT64 Copied, UINT64 Total)
{
  GuiDrawCopyProgress(gCopySrc, gCopyDst, Copied, Total);
}

static BOOLEAN IsPathDirectory(IN CONST CHAR16* Path)
{
  BOOLEAN isDir = FALSE;
  return FsFileExists(Path, &isDir) && isDir;
}

static BOOLEAN HasVolumePrefix(IN CONST CHAR16* Path)
{
  if (Path == NULL) return FALSE;
  for (UINTN i = 0; Path[i] != L'\0'; i++) {
    if (Path[i] == L':') {
      return TRUE;
    }
    if (Path[i] == L'\\') {
      return FALSE;
    }
  }
  return FALSE;
}

static VOID BuildPathForSingleTarget(
  OUT CHAR16* TargetPath,
  IN CONST CHAR16* TypedTarget,
  IN CONST CHAR16* SourceName,
  IN CONST CHAR16* RelativeBase
) {
  if (TargetPath == NULL || TypedTarget == NULL || SourceName == NULL) return;

  if (!HasVolumePrefix(TypedTarget) && RelativeBase != NULL && RelativeBase[0] != L'\0') {
    FsCombinePath(TargetPath, RelativeBase, TypedTarget);
  } else {
    StrCpyS(TargetPath, MAX_PATH_LEN, TypedTarget);
    if (IsPathDirectory(TypedTarget)) {
      FsCombinePath(TargetPath, TypedTarget, SourceName);
    }
  }
}

static VOID BuildPathForGroupTarget(
  OUT CHAR16* TargetPath,
  IN CONST CHAR16* TargetDir,
  IN CONST CHAR16* SourceName
) {
  if (TargetPath == NULL || TargetDir == NULL || SourceName == NULL) return;
  FsCombinePath(TargetPath, TargetDir, SourceName);
}

static BOOLEAN GetCurrentItemPath(IN PANEL* Panel, OUT CHAR16* Path)
{
  if (Panel == NULL || Path == NULL || Panel->SelectedIndex < 0 || Panel->Files == NULL) {
    return FALSE;
  }
  FS_FILE_ITEM* selected = &Panel->Files[Panel->SelectedIndex];
  if (!PanelOpsIsUsableItem(selected)) {
    return FALSE;
  }
  FsCombinePath(Path, Panel->Path, selected->Name);
  return TRUE;
}

static BOOLEAN IsQuickFindChar(CHAR16 Ch)
{
  return (Ch >= L'0' && Ch <= L'9') ||
         (Ch >= L'A' && Ch <= L'Z') ||
         (Ch >= L'a' && Ch <= L'z') ||
         Ch == L'_' || Ch == L'-' || Ch == L'.';
}

static VOID QuickFindReset(OUT CHAR16* Buffer, OUT UINTN* Length)
{
  if (Buffer != NULL) Buffer[0] = L'\0';
  if (Length != NULL) *Length = 0;
}

static BOOLEAN QuickFindAppend(
  IN OUT PANEL* Panel,
  IN CHAR16 Ch,
  IN OUT CHAR16* Buffer,
  IN OUT UINTN* Length
) {
  if (Panel == NULL || Buffer == NULL || Length == NULL) return FALSE;
  if (*Length + 1 >= 128) {
    QuickFindReset(Buffer, Length);
  }
  Buffer[*Length] = Ch;
  (*Length)++;
  Buffer[*Length] = L'\0';

  if (PanelOpsFindPrefixNext(Panel, Buffer, FALSE)) {
    return TRUE;
  }

  if (*Length > 1) {
    Buffer[0] = Ch;
    Buffer[1] = L'\0';
    *Length = 1;
    return PanelOpsFindPrefixNext(Panel, Buffer, TRUE);
  }

  return FALSE;
}

static VOID ShowDriveMenu(
  IN PANEL* LeftPanel,
  IN PANEL* RightPanel,
  IN BOOLEAN LeftActive,
  IN BOOLEAN IsLeftPanelMenu
) {
  PANEL* targetPanel = IsLeftPanelMenu ? LeftPanel : RightPanel;
  
  UINTN width, height;
  UiGfxGetDimensions(&width, &height);
  UINTN cellW, cellH;
  UiGfxGetCellSize(&cellW, &cellH);

  if (gVolumeCount == 0) return;

  UINTN boxW = 350;
  UINTN boxH = (gVolumeCount + 2) * cellH + 30;
  UINTN boxX = (width - boxW) / 2;
  UINTN boxY = (height - boxH) / 2;

  UINTN selectedIdx = 0;
  for (UINTN v = 0; v < gVolumeCount; v++) {
    if (StrLen(targetPanel->Path) >= StrLen(gVolumes[v].Name) &&
        StrnCmp(targetPanel->Path, gVolumes[v].Name, StrLen(gVolumes[v].Name)) == 0) {
      selectedIdx = v;
      break;
    }
  }

  BOOLEAN done = FALSE;
  while (!done) {
    // 1. Draw panels background
    GuiDrawPanels(LeftPanel, RightPanel, LeftActive);
    GuiDrawBottomMenu();

    // 2. Draw Drive Menu box
    UiGfxFillRectRgb(boxX, boxY, boxW, boxH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
    DrawBorder(boxX, boxY, boxW, boxH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B, 3);

    // Title
    UiGfxDrawAsciiAt(boxX + 20, boxY + 15, "Change Drive", COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

    // List volumes
    for (UINTN v = 0; v < gVolumeCount; v++) {
      CHAR16 volStr[64];
      UnicodeSPrint(volStr, sizeof(volStr), L"   %s   ", gVolumes[v].Name);

      UINTN yPos = boxY + 40 + v * cellH;
      if (v == selectedIdx) {
        // Highlight active selection
        UiGfxFillRectRgb(boxX + 15, yPos, boxW - 30, cellH, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        UiGfxDrawUnicodeAt(boxX + 20, yPos, volStr, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B);
      } else {
        UiGfxDrawUnicodeAt(boxX + 20, yPos, volStr, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
      }
    }

    UiGfxFlush();

    // 3. Read key
    EFI_INPUT_KEY key;
    UINTN eventIndex;
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &eventIndex);
    EFI_STATUS status = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
    if (EFI_ERROR(status)) continue;

    if (key.ScanCode != 0) {
      switch (key.ScanCode) {
        case SCAN_UP:
          if (selectedIdx > 0) selectedIdx--;
          break;
        case SCAN_DOWN:
          if (selectedIdx + 1 < gVolumeCount) selectedIdx++;
          break;
        case SCAN_ESC:
          done = TRUE;
          break;
      }
    } else {
      if (key.UnicodeChar == 13) { // Enter
        // Set new path
        StrCpyS(targetPanel->Path, MAX_PATH_LEN, gVolumes[selectedIdx].Name);
        // Ensure trailing slash
        UINTN len = StrLen(targetPanel->Path);
        if (len > 0 && targetPanel->Path[len - 1] != L'\\') {
          StrCatS(targetPanel->Path, MAX_PATH_LEN, L"\\");
        }
        targetPanel->TopIndex = 0;
        targetPanel->SelectedIndex = 0;
        PanelRefresh(targetPanel);
        done = TRUE;
      } else if (key.UnicodeChar == 27) { // ESC
        done = TRUE;
      }
    }
  }
}

EFI_STATUS EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
)
{
  // Initialize standard tables
  gImageHandle = ImageHandle;
  gST          = SystemTable;
  gBS          = SystemTable->BootServices;
  gRT          = SystemTable->RuntimeServices;

  // Save original text mode
  INT32 originalTextMode = SystemTable->ConOut->Mode->Mode;

  // Initialize Graphic GOP console
  if (!UiConsoleInit(SystemTable)) {
    Print(L"Error: GOP Graphic Console initialization failed.\n");
    return EFI_UNSUPPORTED;
  }

  // Initial volume scan
  FsInit();
  ConfigLoad(ImageHandle);

  // Try to load ntfs.efi at startup silently
  FsLoadNtfsDriver(ImageHandle);

  // Re-scan volumes in case NTFS driver added new ones
  FsInit();

  // Set up panels
  PANEL leftPanel;
  PANEL rightPanel;
  
  // Default path is fs0:\ if available, otherwise empty string (drive selection)
  CHAR16* defaultPath = L"";
  if (gVolumeCount > 0) {
    defaultPath = gVolumes[0].Name;
  }

  PanelInit(&leftPanel, gEcConfig.DefaultLeft[0] != L'\0' ? gEcConfig.DefaultLeft : defaultPath);
  PanelInit(&rightPanel, gEcConfig.DefaultRight[0] != L'\0' ? gEcConfig.DefaultRight : defaultPath);
  PanelSetFilter(&leftPanel, gEcConfig.FilterLeft);
  PanelSetFilter(&rightPanel, gEcConfig.FilterRight);

  PanelRefresh(&leftPanel);
  PanelRefresh(&rightPanel);

  NavHistoryInit(&gLeftHistory, leftPanel.Path);
  NavHistoryInit(&gRightHistory, rightPanel.Path);

  BOOLEAN leftActive = TRUE;
  BOOLEAN quit = FALSE;
  CHAR16 lastSearch[MAX_PATH_LEN] = { 0 };
  CHAR16 quickSearch[128] = { 0 };
  UINTN quickSearchLen = 0;

  while (!quit) {
    PANEL* activePanel = leftActive ? &leftPanel : &rightPanel;
    PANEL* inactivePanel = leftActive ? &rightPanel : &leftPanel;
    NAV_HISTORY* activeHistory = leftActive ? &gLeftHistory : &gRightHistory;

    UINTN width, height;
    UiGfxGetDimensions(&width, &height);
    UINTN pH = height - 80;
    UINTN pageSize = GuiGetPageSize(pH);

    // 1. Draw panels and menu
    GuiDrawPanels(&leftPanel, &rightPanel, leftActive);
    GuiDrawBottomMenu();

    // 2. Flush backbuffer to screen
    UiGfxFlush();

    // 3. Read key with optional Alt detection
    EFI_INPUT_KEY key;
    BOOLEAN altPressed = FALSE;
    BOOLEAN ctrlPressed = FALSE;
    EFI_EVENT waitEvent = gST->ConIn->WaitForKey;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL* inputEx = NULL;
    EFI_STATUS exStatus = gBS->HandleProtocol(gST->ConsoleInHandle, &gEfiSimpleTextInputExProtocolGuid, (VOID**)&inputEx);
    if (!EFI_ERROR(exStatus) && inputEx != NULL) {
      waitEvent = inputEx->WaitForKeyEx;
    }

    UINTN index;
    gBS->WaitForEvent(1, &waitEvent, &index);

    EFI_STATUS status;
    if (inputEx != NULL) {
      EFI_KEY_DATA keyData;
      status = inputEx->ReadKeyStrokeEx(inputEx, &keyData);
      if (!EFI_ERROR(status)) {
        UINT32 shiftState;
        key = keyData.Key;
        shiftState = keyData.KeyState.KeyShiftState;
        if ((shiftState & EFI_SHIFT_STATE_VALID) != 0) {
          if ((shiftState & (EFI_LEFT_ALT_PRESSED | EFI_RIGHT_ALT_PRESSED)) != 0) {
            altPressed = TRUE;
          }
          if ((shiftState & (EFI_LEFT_CONTROL_PRESSED | EFI_RIGHT_CONTROL_PRESSED)) != 0) {
            ctrlPressed = TRUE;
          }
        }
      }
    } else {
      status = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
    }
    if (EFI_ERROR(status)) continue;

    // Handle Alt+F1 / Alt+F2 Drive Menu directly
    if (altPressed && key.ScanCode != 0) {
      QuickFindReset(quickSearch, &quickSearchLen);
      if (key.ScanCode == SCAN_F1) {
        CHAR16 before[MAX_PATH_LEN];
        StrCpyS(before, MAX_PATH_LEN, leftPanel.Path);
        ShowDriveMenu(&leftPanel, &rightPanel, leftActive, TRUE);
        if (StrCmp(before, leftPanel.Path) != 0) NavHistoryPush(&gLeftHistory, leftPanel.Path);
        continue;
      } else if (key.ScanCode == SCAN_F2) {
        CHAR16 before[MAX_PATH_LEN];
        StrCpyS(before, MAX_PATH_LEN, rightPanel.Path);
        ShowDriveMenu(&leftPanel, &rightPanel, leftActive, FALSE);
        if (StrCmp(before, rightPanel.Path) != 0) NavHistoryPush(&gRightHistory, rightPanel.Path);
        continue;
      } else if (key.ScanCode == SCAN_F10) {
        NavShowHotlist(activePanel, activeHistory);
        continue;
      } else if (key.ScanCode == SCAN_LEFT) {
        CHAR16 path[MAX_PATH_LEN];
        if (NavHistoryBack(activeHistory, path)) {
          StrCpyS(activePanel->Path, MAX_PATH_LEN, path);
          PanelRefreshKeep(activePanel, NULL, activePanel->SelectedIndex);
        }
        continue;
      } else if (key.ScanCode == SCAN_RIGHT) {
        CHAR16 path[MAX_PATH_LEN];
        if (NavHistoryForward(activeHistory, path)) {
          StrCpyS(activePanel->Path, MAX_PATH_LEN, path);
          PanelRefreshKeep(activePanel, NULL, activePanel->SelectedIndex);
        }
        continue;
      }
    }

    // Handle Scan Codes
    if (key.ScanCode != 0) {
      if (ctrlPressed) {
        BOOLEAN sortHandled = TRUE;
        switch (key.ScanCode) {
          case SCAN_F3:
            QuickFindReset(quickSearch, &quickSearchLen);
            PanelSetSortMode(activePanel, PANEL_SORT_NAME);
            break;
          case SCAN_F4:
            QuickFindReset(quickSearch, &quickSearchLen);
            PanelSetSortMode(activePanel, PANEL_SORT_EXTENSION);
            break;
          case SCAN_F5:
            QuickFindReset(quickSearch, &quickSearchLen);
            PanelSetSortMode(activePanel, PANEL_SORT_MODIFIED);
            break;
          case SCAN_F6:
            QuickFindReset(quickSearch, &quickSearchLen);
            PanelSetSortMode(activePanel, PANEL_SORT_SIZE);
            break;
          case SCAN_F12: {
            QuickFindReset(quickSearch, &quickSearchLen);
            CHAR16 mask[128];
            StrCpyS(mask, 128, activePanel->FilterMask);
            if (GuiDrawInputBox(L"Filter", L"Show mask (* clears):", mask, 128)) {
              PanelSetFilter(activePanel, mask);
            }
            break;
          }
          default:
            sortHandled = FALSE;
            break;
        }
        if (sortHandled) {
          continue;
        }
      }

      switch (key.ScanCode) {
        case SCAN_UP:
          QuickFindReset(quickSearch, &quickSearchLen);
          PanelNavigateUp(activePanel);
          break;

        case SCAN_DOWN:
          QuickFindReset(quickSearch, &quickSearchLen);
          PanelNavigateDown(activePanel, pageSize);
          break;

        case SCAN_PAGE_UP:
          QuickFindReset(quickSearch, &quickSearchLen);
          PanelPageUp(activePanel, pageSize);
          break;

        case SCAN_PAGE_DOWN:
          QuickFindReset(quickSearch, &quickSearchLen);
          PanelPageDown(activePanel, pageSize);
          break;

        case SCAN_HOME:
          QuickFindReset(quickSearch, &quickSearchLen);
          if (activePanel->FileCount > 0) {
            activePanel->SelectedIndex = 0;
            activePanel->TopIndex = 0;
          }
          break;

        case SCAN_END:
          QuickFindReset(quickSearch, &quickSearchLen);
          if (activePanel->FileCount > 0) {
            activePanel->SelectedIndex = (INTN)(activePanel->FileCount - 1);
            if (activePanel->FileCount > pageSize) {
              activePanel->TopIndex = activePanel->FileCount - pageSize;
            }
          }
          break;

        case SCAN_F10:
          QuickFindReset(quickSearch, &quickSearchLen);
          quit = TRUE;
          break;

        case SCAN_F1:
          QuickFindReset(quickSearch, &quickSearchLen);
          GuiDrawHelp();
          break;

        case SCAN_F2: {
          QuickFindReset(quickSearch, &quickSearchLen);
          // Open Drive Selection Menu for the active panel
          CHAR16 before[MAX_PATH_LEN];
          StrCpyS(before, MAX_PATH_LEN, activePanel->Path);
          ShowDriveMenu(&leftPanel, &rightPanel, leftActive, leftActive);
          if (StrCmp(before, activePanel->Path) != 0) NavHistoryPush(activeHistory, activePanel->Path);
          break;
        }

        case SCAN_F3: {
          // View selected item
          if (activePanel->SelectedIndex < 0 || activePanel->Files == NULL) break;
          FS_FILE_ITEM* selected = &activePanel->Files[activePanel->SelectedIndex];
          
          if (StrCmp(selected->Name, L"..") == 0) {
            break;
          }
          if (selected->IsDirectory) {
            GuiDrawMsgBox(L"Info", L"Viewing directories is not supported (files only).");
            break;
          }

          CHAR16 viewPath[MAX_PATH_LEN] = { 0 };
          FsCombinePath(viewPath, activePanel->Path, selected->Name);
          ViewerShow(ImageHandle, viewPath);
          
          // Re-refresh panels when returning
          PanelRefresh(&leftPanel);
          PanelRefresh(&rightPanel);
          break;
        }

        case SCAN_F4: {
          // Edit selected item in HxD Hex Editor
          if (activePanel->SelectedIndex < 0 || activePanel->Files == NULL) break;
          FS_FILE_ITEM* selected = &activePanel->Files[activePanel->SelectedIndex];
          
          if (StrCmp(selected->Name, L"..") == 0) {
            break;
          }
          if (selected->IsDirectory) {
            GuiDrawMsgBox(L"Info", L"Editing directories is not supported (files only).");
            break;
          }

          CHAR16 editPath[MAX_PATH_LEN] = { 0 };
          FsCombinePath(editPath, activePanel->Path, selected->Name);
          EditorShow(ImageHandle, editPath);
          
          // Re-refresh panels when returning
          PanelRefresh(&leftPanel);
          PanelRefresh(&rightPanel);
          break;
        }

        case SCAN_F5: {
          // Copy selected item(s) (recursive)
          if (activePanel->SelectedIndex < 0 || activePanel->Files == NULL) break;

          if (inactivePanel->Path[0] == L'\0') {
            GuiDrawMsgBox(L"Error", L"Cannot copy to virtual drives list!");
            break;
          }

          UINTN markedCount = PanelOpsCountSelected(activePanel);
          CHAR16 targetInput[MAX_PATH_LEN] = { 0 };
          StrCpyS(targetInput, MAX_PATH_LEN, inactivePanel->Path);

          if (markedCount > 0) {
            if (!GuiDrawInputBox(L"Copy", L"Copy selected items to:", targetInput, MAX_PATH_LEN)) {
              break;
            }
            if (!IsPathDirectory(targetInput)) {
              GuiDrawMsgBox(L"Error", L"Group copy target must be an existing directory.");
              break;
            }

            BOOLEAN anyCopied = FALSE;
            for (UINTN idx = 0; idx < activePanel->FileCount; idx++) {
              if (activePanel->Files[idx].Selected && PanelOpsIsUsableItem(&activePanel->Files[idx])) {
                CHAR16 srcPath[MAX_PATH_LEN] = { 0 };
                FsCombinePath(srcPath, activePanel->Path, activePanel->Files[idx].Name);

                CHAR16 targetPath[MAX_PATH_LEN] = { 0 };
                BuildPathForGroupTarget(targetPath, targetInput, activePanel->Files[idx].Name);

                StrCpyS(gCopySrc, MAX_PATH_LEN, srcPath);
                StrCpyS(gCopyDst, MAX_PATH_LEN, targetPath);

                EFI_STATUS copyStatus = FsCopyRecursive(srcPath, targetPath, CopyCallback);
                if (copyStatus == EFI_ABORTED) {
                  break;
                }
                if (!EFI_ERROR(copyStatus)) {
                  activePanel->Files[idx].Selected = FALSE;
                  anyCopied = TRUE;
                }
              }
            }
            FsFlushVolumeForPath(targetInput);
            if (anyCopied) {
              PanelRefreshKeep(&leftPanel, NULL, leftPanel.SelectedIndex);
              PanelRefreshKeep(&rightPanel, NULL, rightPanel.SelectedIndex);
            }
          } else {
            // Single file copy
            FS_FILE_ITEM* selected = &activePanel->Files[activePanel->SelectedIndex];
            if (StrCmp(selected->Name, L"..") == 0) {
              GuiDrawMsgBox(L"Error", L"Cannot copy parent directory reference '..'");
              break;
            }

            CHAR16 srcPath[MAX_PATH_LEN] = { 0 };
            FsCombinePath(srcPath, activePanel->Path, selected->Name);

            FsCombinePath(targetInput, inactivePanel->Path, selected->Name);
            if (!GuiDrawInputBox(L"Copy", L"Copy item to:", targetInput, MAX_PATH_LEN)) {
              break;
            }

            CHAR16 targetPath[MAX_PATH_LEN] = { 0 };
            BuildPathForSingleTarget(targetPath, targetInput, selected->Name, inactivePanel->Path);
            StrCpyS(gCopySrc, MAX_PATH_LEN, srcPath);
            StrCpyS(gCopyDst, MAX_PATH_LEN, targetPath);

            EFI_STATUS copyStatus = FsCopyRecursive(srcPath, targetPath, CopyCallback);
            FsFlushVolumeForPath(targetPath);
            if (!EFI_ERROR(copyStatus) && gEcConfig.ShowSuccessMessages) {
              GuiDrawMsgBox(L"Success", L"Item copied successfully!");
            }
            PanelRefreshKeep(&leftPanel, NULL, leftPanel.SelectedIndex);
            PanelRefreshKeep(&rightPanel, NULL, rightPanel.SelectedIndex);
          }
          break;
        }

        case SCAN_F6: {
          // Rename or move selected item(s)
          if (activePanel->SelectedIndex < 0 || activePanel->Files == NULL) break;

          UINTN markedCount = PanelOpsCountSelected(activePanel);
          if (markedCount > 0) {
            if (inactivePanel->Path[0] == L'\0') {
              GuiDrawMsgBox(L"Error", L"Cannot move to virtual drives list!");
              break;
            }

            CHAR16 targetDir[MAX_PATH_LEN] = { 0 };
            StrCpyS(targetDir, MAX_PATH_LEN, inactivePanel->Path);
            if (!GuiDrawInputBox(L"Move", L"Move selected items to:", targetDir, MAX_PATH_LEN)) {
              break;
            }
            if (!IsPathDirectory(targetDir)) {
              GuiDrawMsgBox(L"Error", L"Group move target must be an existing directory.");
              break;
            }

            BOOLEAN anyMoved = FALSE;
            for (UINTN idx = 0; idx < activePanel->FileCount; idx++) {
              if (activePanel->Files[idx].Selected && PanelOpsIsUsableItem(&activePanel->Files[idx])) {
                CHAR16 srcPath[MAX_PATH_LEN] = { 0 };
                FsCombinePath(srcPath, activePanel->Path, activePanel->Files[idx].Name);
  
                CHAR16 targetPath[MAX_PATH_LEN] = { 0 };
                BuildPathForGroupTarget(targetPath, targetDir, activePanel->Files[idx].Name);
  
                EFI_STATUS moveStatus = FsRenameOrMove(srcPath, targetPath);
                if (EFI_ERROR(moveStatus)) {
                  if (moveStatus == EFI_UNSUPPORTED) {
                    GuiDrawMsgBox(L"Error", L"Moving across different volumes is not supported. Use Copy (F5) and Delete (F8).");
                    break;
                  }
                  GuiDrawMsgBox(L"Error", L"Failed to move selected item.");
                  break;
                }
                activePanel->Files[idx].Selected = FALSE;
                anyMoved = TRUE;
              }
            }
            if (anyMoved) {
              PanelRefreshKeep(&leftPanel, NULL, leftPanel.SelectedIndex);
              PanelRefreshKeep(&rightPanel, NULL, rightPanel.SelectedIndex);
            }
          } else {
            FS_FILE_ITEM* selected = &activePanel->Files[activePanel->SelectedIndex];
            if (!PanelOpsIsUsableItem(selected)) break;

            CHAR16 srcPath[MAX_PATH_LEN] = { 0 };
            FsCombinePath(srcPath, activePanel->Path, selected->Name);

            CHAR16 defaultTarget[MAX_PATH_LEN] = { 0 };
            if (inactivePanel->Path[0] == L'\0' || StrCmp(activePanel->Path, inactivePanel->Path) == 0) {
              FsCombinePath(defaultTarget, activePanel->Path, selected->Name);
            } else {
              FsCombinePath(defaultTarget, inactivePanel->Path, selected->Name);
            }

            if (GuiDrawInputBox(L"Rename/Move", L"New name or destination:", defaultTarget, MAX_PATH_LEN)) {
              CHAR16 targetPath[MAX_PATH_LEN] = { 0 };
              CONST CHAR16* preferredName = defaultTarget;
              for (UINTN namePos = StrLen(defaultTarget); namePos > 0; namePos--) {
                if (defaultTarget[namePos - 1] == L'\\' || defaultTarget[namePos - 1] == L':') {
                  preferredName = &defaultTarget[namePos];
                  break;
                }
              }
              BuildPathForSingleTarget(targetPath, defaultTarget, selected->Name, activePanel->Path);

              EFI_STATUS moveStatus = FsRenameOrMove(srcPath, targetPath);
              if (EFI_ERROR(moveStatus)) {
                if (moveStatus == EFI_UNSUPPORTED) {
                  GuiDrawMsgBox(L"Error", L"Moving across different volumes is not supported. Use Copy (F5) and Delete (F8).");
                } else {
                  GuiDrawMsgBox(L"Error", L"Failed to rename or move item!");
                }
              } else {
                PanelRefreshKeep(&leftPanel, (activePanel == &leftPanel) ? preferredName : NULL, leftPanel.SelectedIndex);
                PanelRefreshKeep(&rightPanel, (activePanel == &rightPanel) ? preferredName : NULL, rightPanel.SelectedIndex);
              }
            }
          }
          break;
        }

        case SCAN_F7: {
          // MkDir
          if (activePanel->Path[0] == L'\0') {
            GuiDrawMsgBox(L"Error", L"Cannot create folder in virtual drives list!");
            break;
          }

          CHAR16 folderName[128] = { 0 };
          if (GuiDrawInputBox(L"Create Folder", L"Enter new folder name:", folderName, 128)) {
            if (folderName[0] != L'\0') {
              CHAR16 newDirPath[MAX_PATH_LEN] = { 0 };
              FsCombinePath(newDirPath, activePanel->Path, folderName);
              EFI_STATUS dirStatus = FsCreateDir(newDirPath);
              if (EFI_ERROR(dirStatus)) {
                GuiDrawMsgBox(L"Error", L"Failed to create directory!");
              } else {
                PanelRefresh(activePanel);
              }
            }
          }
          break;
        }

        case SCAN_F8:
        case SCAN_DELETE: {
          // Delete selected item(s) (recursive)
          if (activePanel->SelectedIndex < 0 || activePanel->Files == NULL) break;

          UINTN markedCount = PanelOpsCountSelected(activePanel);

          if (markedCount > 0) {
            CHAR16 promptMsg[128] = { 0 };
            UnicodeSPrint(promptMsg, sizeof(promptMsg), L"Delete %d selected items?", markedCount);

            UINTN response = gEcConfig.ConfirmDelete ? GuiDrawConfirmDialog(L"Confirm Group Delete", promptMsg, FALSE) : 1;
            if (response == 1) { // Yes
              BOOLEAN anyDeleted = FALSE;
              for (UINTN idx = 0; idx < activePanel->FileCount; idx++) {
                if (activePanel->Files[idx].Selected && PanelOpsIsUsableItem(&activePanel->Files[idx])) {
                  CHAR16 deletePath[MAX_PATH_LEN] = { 0 };
                  FsCombinePath(deletePath, activePanel->Path, activePanel->Files[idx].Name);

                  EFI_STATUS delStatus = FsDeleteRecursive(deletePath);
                  if (delStatus == EFI_ABORTED) {
                    break;
                  }
                  anyDeleted = TRUE;
                }
              }
              if (anyDeleted) {
                PanelRefreshKeep(activePanel, NULL, activePanel->SelectedIndex);
              }
            }
          } else {
            // Single file delete
            FS_FILE_ITEM* selected = &activePanel->Files[activePanel->SelectedIndex];
            if (StrCmp(selected->Name, L"..") == 0) {
              GuiDrawMsgBox(L"Error", L"Cannot delete parent directory reference '..'");
              break;
            }

            CHAR16 promptMsg[128] = { 0 };
            UnicodeSPrint(promptMsg, sizeof(promptMsg), L"Delete \"%s\"?", selected->Name);

            UINTN response = gEcConfig.ConfirmDelete ? GuiDrawConfirmDialog(L"Confirm Delete", promptMsg, FALSE) : 1;
            if (response == 1) { // Yes
              CHAR16 deletePath[MAX_PATH_LEN] = { 0 };
              FsCombinePath(deletePath, activePanel->Path, selected->Name);

              EFI_STATUS delStatus = FsDeleteRecursive(deletePath);
              if (EFI_ERROR(delStatus) && delStatus != EFI_ABORTED) {
                CHAR16 delErr[256];
                UnicodeSPrint(delErr, sizeof(delErr), L"Failed to delete '%s': %r", selected->Name, delStatus);
                GuiDrawMsgBox(L"Error", delErr);
              }
              PanelRefreshKeep(activePanel, NULL, activePanel->SelectedIndex);
            }
          }
          break;
        }

        case SCAN_INSERT: {
          // Toggle selection of current item and move cursor down
          if (activePanel->SelectedIndex >= 0 && activePanel->Files != NULL) {
            FS_FILE_ITEM* selected = &activePanel->Files[activePanel->SelectedIndex];
            if (StrCmp(selected->Name, L"..") != 0) {
              selected->Selected = !selected->Selected;
              PanelNavigateDown(activePanel, pageSize);
            }
          }
          break;
        }

        case SCAN_F9:
          // Refresh panels manually
          FsInit();
          PanelRefresh(&leftPanel);
          PanelRefresh(&rightPanel);
          break;
      }
    } else {
      // Handle Unicode Characters
      if (ctrlPressed && (key.UnicodeChar == L'a' || key.UnicodeChar == L'A' || key.UnicodeChar == 1)) {
        QuickFindReset(quickSearch, &quickSearchLen);
        PanelOpsSelectByMask(activePanel, L"*", TRUE);
        continue;
      }
      if (ctrlPressed && (key.UnicodeChar == L'u' || key.UnicodeChar == L'U' || key.UnicodeChar == 21)) {
        QuickFindReset(quickSearch, &quickSearchLen);
        PanelOpsClearSelection(activePanel);
        continue;
      }

      switch (key.UnicodeChar) {
        case L' ': {
          QuickFindReset(quickSearch, &quickSearchLen);
          // Toggle selection of current item and move cursor down
          if (activePanel->SelectedIndex >= 0 && activePanel->Files != NULL) {
            FS_FILE_ITEM* selected = &activePanel->Files[activePanel->SelectedIndex];
            if (StrCmp(selected->Name, L"..") != 0) {
              selected->Selected = !selected->Selected;
              PanelNavigateDown(activePanel, pageSize);
            }
          }
          break;
        }

        case L'*':
          QuickFindReset(quickSearch, &quickSearchLen);
          PanelOpsInvertSelection(activePanel);
          break;

        case L'+': {
          QuickFindReset(quickSearch, &quickSearchLen);
          CHAR16 mask[128] = L"*";
          if (GuiDrawInputBox(L"Select", L"Select mask:", mask, 128)) {
            PanelOpsSelectByMask(activePanel, mask, TRUE);
          }
          break;
        }

        case L'-': {
          QuickFindReset(quickSearch, &quickSearchLen);
          CHAR16 mask[128] = L"*";
          if (GuiDrawInputBox(L"Unselect", L"Unselect mask:", mask, 128)) {
            PanelOpsSelectByMask(activePanel, mask, FALSE);
          }
          break;
        }

        case L'/': {
          QuickFindReset(quickSearch, &quickSearchLen);
          if (GuiDrawInputBox(L"Find", L"Find file:", lastSearch, MAX_PATH_LEN)) {
            if (!PanelOpsFindNext(activePanel, lastSearch, FALSE)) {
              GuiDrawMsgBox(L"Find", L"No matching item found.");
            }
          }
          break;
        }

        case L'n':
        case L'N':
          if (quickSearchLen == 0 && lastSearch[0] != L'\0') {
            if (!PanelOpsFindNext(activePanel, lastSearch, TRUE)) {
              GuiDrawMsgBox(L"Find", L"No further matching item found.");
            }
          } else {
            QuickFindAppend(activePanel, key.UnicodeChar, quickSearch, &quickSearchLen);
          }
          break;

        case L'\t': // Tab key: Switch active panel
          QuickFindReset(quickSearch, &quickSearchLen);
          leftActive = !leftActive;
          break;

        case CHAR_CARRIAGE_RETURN: // Enter
        {
          QuickFindReset(quickSearch, &quickSearchLen);
          CHAR16 before[MAX_PATH_LEN];
          StrCpyS(before, MAX_PATH_LEN, activePanel->Path);
          PanelEnter(activePanel, ImageHandle);
          if (StrCmp(before, activePanel->Path) != 0) NavHistoryPush(activeHistory, activePanel->Path);
          break;
        }

        case CHAR_BACKSPACE: // Backspace (Go Up)
          QuickFindReset(quickSearch, &quickSearchLen);
          if (activePanel->Path[0] != L'\0') {
            CHAR16 newPath[MAX_PATH_LEN] = { 0 };
            FsCombinePath(newPath, activePanel->Path, L"..");
            NavApplyPath(activePanel, activeHistory, newPath);
          }
          break;

        default:
          if (IsQuickFindChar(key.UnicodeChar)) {
            QuickFindAppend(activePanel, key.UnicodeChar, quickSearch, &quickSearchLen);
          } else {
            QuickFindReset(quickSearch, &quickSearchLen);
          }
          break;
      }
    }
  }

  // Cleanly unmount NTFS volumes before leaving: clears the $Volume dirty
  // flag and flushes any buffered writes to the physical medium, so Windows
  // doesn't offer a chkdsk on the next boot after we've been editing files.
  FsUnmountAllNtfs();

  // Free resources and restore screen
  PanelFree(&leftPanel);
  PanelFree(&rightPanel);
  UiConsoleShutdown();

  // Restore original text mode
  gST->ConOut->SetMode(gST->ConOut, originalTextMode);
  gST->ConOut->ClearScreen(gST->ConOut);
  gST->ConOut->EnableCursor(gST->ConOut, TRUE);

  return EFI_SUCCESS;
}
