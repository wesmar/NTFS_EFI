// Main.c — EC.efi main application entry point and event loop.
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PrintLib.h>

#include "UiConsole.h"
#include "Colors.h"
#include "FileSystem.h"
#include "Panel.h"
#include "Gui.h"
#include "Viewer.h"
#include "Editor.h"
#include "PanelOps.h"
#include "Config.h"
#include "Navigation.h"
#include "Search.h"
#include "FileProps.h"
#include "SelfTest.h"
#include "Checksum.h"
#include "Sync.h"
#include "UefiTools.h"
#include <Protocol/SimpleTextInEx.h>

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

static BOOLEAN SaveConfigOrReport(VOID)
{
  EFI_STATUS Status = ConfigSave();
  if (EFI_ERROR(Status)) {
    CHAR16 Message[160];
    UnicodeSPrint(Message, sizeof(Message), L"Could not save EC.ini: %r", Status);
    GuiDrawMsgBox(L"Settings error", Message);
    return FALSE;
  }
  return TRUE;
}

static VOID RecountHotDirectories(VOID)
{
  gEcConfig.HotDirCount = 0;
  for (UINTN Index = 0; Index < EC_MAX_HOTDIRS; Index++) {
    if (gEcConfig.HotDirs[Index][0] != L'\0') {
      gEcConfig.HotDirCount = Index + 1;
    }
  }
}

static VOID ShowHotDirectorySettings(VOID)
{
  CHAR16 LineStorage[EC_MAX_HOTDIRS + 1][192];
  CONST CHAR16* Lines[EC_MAX_HOTDIRS + 1];
  UINTN Chosen = 0;

  for (;;) {
    for (UINTN Index = 0; Index < EC_MAX_HOTDIRS; Index++) {
      CONST CHAR16* Value = gEcConfig.HotDirs[Index][0] != L'\0'
                          ? gEcConfig.HotDirs[Index] : L"<empty>";
      UnicodeSPrint(LineStorage[Index], sizeof(LineStorage[Index]),
                    L"HotDir%d: %s", (UINT32)(Index + 1), Value);
      Lines[Index] = LineStorage[Index];
    }
    StrCpyS(LineStorage[EC_MAX_HOTDIRS], 192, L"Back");
    Lines[EC_MAX_HOTDIRS] = LineStorage[EC_MAX_HOTDIRS];

    if (!GuiDrawListPicker(L"Hot directories - changes save immediately",
                           Lines, EC_MAX_HOTDIRS + 1, &Chosen) ||
        Chosen == EC_MAX_HOTDIRS) {
      return;
    }

    {
      CHAR16 Value[MAX_PATH_LEN];
      StrCpyS(Value, MAX_PATH_LEN, gEcConfig.HotDirs[Chosen]);
      if (GuiDrawInputBox(L"Hot directory", L"Path (empty removes entry):",
                          Value, MAX_PATH_LEN)) {
        StrCpyS(gEcConfig.HotDirs[Chosen], MAX_PATH_LEN, Value);
        RecountHotDirectories();
        SaveConfigOrReport();
      }
    }
  }
}

static VOID ShowSettingsMenu(IN PANEL* LeftPanel, IN PANEL* RightPanel)
{
  enum { SETTINGS_LINE_COUNT = 13 };
  CHAR16 LineStorage[SETTINGS_LINE_COUNT][192];
  CONST CHAR16* Lines[SETTINGS_LINE_COUNT];
  UINTN Chosen = 0;

  for (;;) {
    CONST CHAR16* DefaultLeft = gEcConfig.DefaultLeft[0] != L'\0'
                              ? gEcConfig.DefaultLeft : L"<automatic>";
    CONST CHAR16* DefaultRight = gEcConfig.DefaultRight[0] != L'\0'
                               ? gEcConfig.DefaultRight : L"<automatic>";
    CONST CHAR16* NtfsPath = gEcConfig.NtfsDriverSetting[0] != L'\0'
                           ? gEcConfig.NtfsDriverSetting : L"<next to EC>";

    UnicodeSPrint(LineStorage[0], sizeof(LineStorage[0]), L"Confirm delete: %s",
                  gEcConfig.ConfirmDelete ? L"On" : L"Off");
    UnicodeSPrint(LineStorage[1], sizeof(LineStorage[1]), L"Confirm overwrite: %s",
                  gEcConfig.ConfirmOverwrite ? L"On" : L"Off");
    UnicodeSPrint(LineStorage[2], sizeof(LineStorage[2]), L"Success messages: %s",
                  gEcConfig.ShowSuccessMessages ? L"On" : L"Off");
    UnicodeSPrint(LineStorage[3], sizeof(LineStorage[3]), L"Operation summary: %s",
                  gEcConfig.ShowOperationSummary ? L"On" : L"Off");
    UnicodeSPrint(LineStorage[4], sizeof(LineStorage[4]), L"Startup left: %s", DefaultLeft);
    UnicodeSPrint(LineStorage[5], sizeof(LineStorage[5]), L"Startup right: %s", DefaultRight);
    UnicodeSPrint(LineStorage[6], sizeof(LineStorage[6]), L"Left filter: %s", gEcConfig.FilterLeft);
    UnicodeSPrint(LineStorage[7], sizeof(LineStorage[7]), L"Right filter: %s", gEcConfig.FilterRight);
    UnicodeSPrint(LineStorage[8], sizeof(LineStorage[8]), L"NTFS driver (next start): %s", NtfsPath);
    StrCpyS(LineStorage[9], 192, L"Hot directories...");
    StrCpyS(LineStorage[10], 192, L"Use current panel paths at startup");
    UnicodeSPrint(LineStorage[11], sizeof(LineStorage[11]), L"Verify after copy: %s",
                  gEcConfig.VerifyAfterCopy ? L"On" : L"Off");
    StrCpyS(LineStorage[12], 192, L"Back");
    for (UINTN Index = 0; Index < SETTINGS_LINE_COUNT; Index++) Lines[Index] = LineStorage[Index];

    if (!GuiDrawListPicker(L"EC Settings - changes save immediately",
                           Lines, SETTINGS_LINE_COUNT, &Chosen) || Chosen == 12) {
      return;
    }

    switch (Chosen) {
      case 0:
        gEcConfig.ConfirmDelete = !gEcConfig.ConfirmDelete;
        SaveConfigOrReport();
        break;
      case 1:
        gEcConfig.ConfirmOverwrite = !gEcConfig.ConfirmOverwrite;
        SaveConfigOrReport();
        break;
      case 2:
        gEcConfig.ShowSuccessMessages = !gEcConfig.ShowSuccessMessages;
        SaveConfigOrReport();
        break;
      case 3:
        gEcConfig.ShowOperationSummary = !gEcConfig.ShowOperationSummary;
        SaveConfigOrReport();
        break;
      case 4:
      case 5: {
        CHAR16 Value[MAX_PATH_LEN];
        CHAR16* Target = (Chosen == 4) ? gEcConfig.DefaultLeft : gEcConfig.DefaultRight;
        StrCpyS(Value, MAX_PATH_LEN, Target);
        if (GuiDrawInputBox(L"Startup path", L"Path (empty uses automatic):", Value, MAX_PATH_LEN)) {
          StrCpyS(Target, MAX_PATH_LEN, Value);
          SaveConfigOrReport();
        }
        break;
      }
      case 6:
      case 7: {
        CHAR16 Value[128];
        CHAR16* Target = (Chosen == 6) ? gEcConfig.FilterLeft : gEcConfig.FilterRight;
        PANEL* TargetPanel = (Chosen == 6) ? LeftPanel : RightPanel;
        StrCpyS(Value, 128, Target);
        if (GuiDrawInputBox(L"Panel filter", L"Mask; empty means *:", Value, 128)) {
          if (Value[0] == L'\0') StrCpyS(Value, 128, L"*");
          StrCpyS(Target, 128, Value);
          PanelSetFilter(TargetPanel, Value);
          SaveConfigOrReport();
        }
        break;
      }
      case 8: {
        CHAR16 Value[MAX_PATH_LEN];
        StrCpyS(Value, MAX_PATH_LEN, gEcConfig.NtfsDriverSetting);
        if (GuiDrawInputBox(L"NTFS driver", L"Path (empty means next to EC):", Value, MAX_PATH_LEN)) {
          StrCpyS(gEcConfig.NtfsDriverSetting, MAX_PATH_LEN, Value);
          SaveConfigOrReport();
        }
        break;
      }
      case 9:
        ShowHotDirectorySettings();
        break;
      case 10:
        StrCpyS(gEcConfig.DefaultLeft, MAX_PATH_LEN, LeftPanel->Path);
        StrCpyS(gEcConfig.DefaultRight, MAX_PATH_LEN, RightPanel->Path);
        SaveConfigOrReport();
        break;
      case 11:
        gEcConfig.VerifyAfterCopy = !gEcConfig.VerifyAfterCopy;
        SaveConfigOrReport();
        break;
    }
  }
}

// Symbol-based selection shortcuts are not equally easy to enter on every
// firmware keyboard layout, so expose the same operations from F9 as well.
// Returns TRUE when an operation was performed and the program menu should
// close, letting the user see its result immediately.
static BOOLEAN ShowSelectionMenu(IN OUT PANEL* ActivePanel)
{
  STATIC CONST CHAR16* Lines[] = {
    L"Select all  [Ctrl+A]",
    L"Select by mask...  [+]",
    L"Unselect by mask...  [-]",
    L"Invert selection  [*]",
    L"Clear selection  [Ctrl+U]",
    L"Back"
  };
  UINTN Chosen = 0;

  for (;;) {
    if (!GuiDrawListPicker(L"Selection tools", Lines, ARRAY_SIZE(Lines), &Chosen)) return FALSE;
    switch (Chosen) {
      case 0:
        PanelOpsSelectByMask(ActivePanel, L"*", TRUE);
        return TRUE;
      case 1: {
        CHAR16 Mask[128] = L"*";
        if (GuiDrawInputBox(L"Select", L"Select mask:", Mask, ARRAY_SIZE(Mask))) {
          PanelOpsSelectByMask(ActivePanel, Mask, TRUE);
          return TRUE;
        }
        break;
      }
      case 2: {
        CHAR16 Mask[128] = L"*";
        if (GuiDrawInputBox(L"Unselect", L"Unselect mask:", Mask, ARRAY_SIZE(Mask))) {
          PanelOpsSelectByMask(ActivePanel, Mask, FALSE);
          return TRUE;
        }
        break;
      }
      case 3:
        PanelOpsInvertSelection(ActivePanel);
        return TRUE;
      case 4:
        PanelOpsClearSelection(ActivePanel);
        return TRUE;
      default:
        return FALSE;
    }
  }
}

static VOID ShowCurrentFileChecksum(IN PANEL* ActivePanel)
{
  CHAR16 Path[MAX_PATH_LEN];
  CHAR16 Sha256[65];
  CHAR16 Half[33];
  CHAR16 Message[256];
  EC_FILE_CHECKSUM Result;
  EFI_STATUS Status;

  if (!GetCurrentItemPath(ActivePanel, Path)) {
    GuiDrawMsgBox(L"Checksum", L"Select a file first.");
    return;
  }
  if (ActivePanel->Files[ActivePanel->SelectedIndex].IsDirectory) {
    GuiDrawMsgBox(L"Checksum", L"Checksums are available for files only.");
    return;
  }

  GuiDrawSearchProgress(Path, L"SHA-256 + CRC32", 0);
  Status = ChecksumFile(Path, &Result);
  if (EFI_ERROR(Status)) {
    UnicodeSPrint(Message, sizeof(Message), L"Could not read the file: %r", Status);
    GuiDrawMsgBox(L"Checksum error", Message);
    return;
  }

  ChecksumSha256ToText(Result.Sha256, Sha256);
  // 64 hex digits do not fit one dialog line at every resolution, so the digest
  // is split in the middle by hand rather than by a precision specifier.
  StrnCpyS(Half, ARRAY_SIZE(Half), Sha256, 32);
  UnicodeSPrint(Message, sizeof(Message),
                L"CRC32: %08x\nSHA-256: %s\n         %s",
                Result.Crc32, Half, &Sha256[32]);
  GuiDrawMsgBox(L"File checksum", Message);
}

static VOID ShowRecursiveSync(IN OUT PANEL* LeftPanel, IN OUT PANEL* RightPanel)
{
  EC_SYNC_SUMMARY Summary;
  EC_SYNC_RESULT Result;
  EFI_STATUS Status;
  CHAR16 Message[320];
  STATIC CONST CHAR16* Actions[] = {
    L"Update RIGHT from LEFT (keep right-only entries)",
    L"Update LEFT from RIGHT (keep left-only entries)",
    L"Close"
  };
  UINTN Chosen = 0;
  CONST CHAR16* Source;
  CONST CHAR16* Destination;

  if (LeftPanel->Path[0] == L'\0' || RightPanel->Path[0] == L'\0') {
    GuiDrawMsgBox(L"Recursive compare", L"Choose a directory in both panels first.");
    return;
  }
  if (StrCmp(LeftPanel->Path, RightPanel->Path) == 0) {
    GuiDrawMsgBox(L"Recursive compare", L"Both panels show the same directory.");
    return;
  }

  GuiDrawSearchProgress(LeftPanel->Path, L"recursive SHA-256 compare", 0);
  Status = SyncCompareTrees(LeftPanel->Path, RightPanel->Path, &Summary);
  if (EFI_ERROR(Status)) {
    UnicodeSPrint(Message, sizeof(Message), L"Comparison failed: %r", Status);
    GuiDrawMsgBox(L"Recursive compare", Message);
    return;
  }

  UnicodeSPrint(Message, sizeof(Message),
                L"Only left: %d   Only right: %d\nDifferent: %d   Equal files: %d\nCommon directories: %d",
                (UINT32)Summary.LeftOnly, (UINT32)Summary.RightOnly,
                (UINT32)Summary.Different, (UINT32)Summary.EqualFiles,
                (UINT32)Summary.CommonDirectories);
  GuiDrawMsgBox(L"Recursive comparison", Message);

  if (Summary.LeftOnly == 0 && Summary.RightOnly == 0 && Summary.Different == 0) return;
  if (!GuiDrawListPicker(L"One-way directory update", Actions, ARRAY_SIZE(Actions), &Chosen) || Chosen == 2) {
    return;
  }

  Source = Chosen == 0 ? LeftPanel->Path : RightPanel->Path;
  Destination = Chosen == 0 ? RightPanel->Path : LeftPanel->Path;
  UnicodeSPrint(Message, sizeof(Message),
                L"Copy missing/different entries from %s to %s? Destination-only entries will be kept.",
                Source, Destination);
  if (GuiDrawConfirmDialog(L"Confirm directory update", Message, FALSE) != 1) return;

  GuiDrawSearchProgress(Source, L"updating destination", 0);
  Status = SyncUpdateTree(Source, Destination, &Result);
  PanelRefresh(LeftPanel);
  PanelRefresh(RightPanel);
  UnicodeSPrint(Message, sizeof(Message),
                L"Copied files: %d   copied trees: %d\nReplaced: %d   equal skipped: %d\nErrors: %d   status: %r",
                (UINT32)Result.CopiedFiles, (UINT32)Result.CopiedTrees,
                (UINT32)Result.ReplacedEntries, (UINT32)Result.SkippedEqual,
                (UINT32)Result.Errors, Status);
  GuiDrawMsgBox(EFI_ERROR(Status) ? L"Directory update incomplete" : L"Directory update complete", Message);
}

static VOID RunCurrentEfiWithArguments(IN PANEL* ActivePanel)
{
  CHAR16 Path[MAX_PATH_LEN];
  CHAR16 Arguments[256] = { 0 };
  CHAR16 Message[256];
  EFI_STATUS Status;

  if (!GetCurrentItemPath(ActivePanel, Path) ||
      ActivePanel->Files[ActivePanel->SelectedIndex].IsDirectory) {
    GuiDrawMsgBox(L"Run EFI", L"Select an EFI application first.");
    return;
  }
  if (!GuiDrawInputBox(L"Run EFI application", L"Arguments (empty is allowed):",
                       Arguments, ARRAY_SIZE(Arguments))) return;
  Status = FsStartEfiAppWithArgs(gImageHandle, Path, Arguments);
  if (EFI_ERROR(Status)) {
    UnicodeSPrint(Message, sizeof(Message), L"Application returned: %r", Status);
    GuiDrawMsgBox(L"Run EFI", Message);
  }
}

static VOID ShowUefiTools(
  IN OUT PANEL* LeftPanel,
  IN OUT PANEL* RightPanel,
  IN PANEL* ActivePanel
)
{
  STATIC CONST CHAR16* Lines[] = {
    L"Current volume details",
    L"Rescan devices and filesystems",
    L"Load selected EFI image as driver...",
    L"BootOrder / BootNext entries (read-only)",
    L"Back"
  };
  UINTN Chosen = 0;

  for (;;) {
    if (!GuiDrawListPicker(L"UEFI tools", Lines, ARRAY_SIZE(Lines), &Chosen) || Chosen == 4) return;
    switch (Chosen) {
      case 0: {
        FS_VOLUME* Volume = FsFindVolumeForPath(ActivePanel->Path);
        UINT64 Total = 0;
        UINT64 Free = 0;
        UINT32 BlockSize = 0;
        BOOLEAN ReadOnly = FALSE;
        CHAR16 Label[64] = { 0 };
        CHAR16 Message[320];
        EFI_STATUS Status;
        if (Volume == NULL) {
          GuiDrawMsgBox(L"Volume details", L"Choose a mounted volume first.");
          break;
        }
        Status = FsGetVolumeDetails(Volume, &Total, &Free, &BlockSize, &ReadOnly,
                                    Label, ARRAY_SIZE(Label));
        if (EFI_ERROR(Status)) {
          UnicodeSPrint(Message, sizeof(Message), L"Could not read volume information: %r", Status);
        } else {
          UnicodeSPrint(Message, sizeof(Message),
                        L"Volume: %s   Label: %s\nTotal: %ld bytes   Free: %ld bytes\nBlock: %d bytes   Read-only: %s",
                        Volume->Name, Label[0] != L'\0' ? Label : L"<none>",
                        Total, Free, BlockSize, ReadOnly ? L"Yes" : L"No");
        }
        GuiDrawMsgBox(L"Volume details", Message);
        break;
      }
      case 1:
        FsRescanDevices();
        PanelRefresh(LeftPanel);
        PanelRefresh(RightPanel);
        GuiDrawMsgBox(L"UEFI tools", L"Controllers reconnected and filesystems rescanned.");
        break;
      case 2: {
        CHAR16 Path[MAX_PATH_LEN];
        CHAR16 Prompt[256];
        EFI_STATUS Status;
        if (!GetCurrentItemPath(ActivePanel, Path) ||
            ActivePanel->Files[ActivePanel->SelectedIndex].IsDirectory) {
          GuiDrawMsgBox(L"Load EFI driver", L"Select an EFI driver image first.");
          break;
        }
        UnicodeSPrint(Prompt, sizeof(Prompt), L"Start %s as an EFI driver?", Path);
        if (GuiDrawConfirmDialog(L"Load EFI driver", Prompt, FALSE) != 1) break;
        Status = FsStartEfiDriver(gImageHandle, Path);
        PanelRefresh(LeftPanel);
        PanelRefresh(RightPanel);
        UnicodeSPrint(Prompt, sizeof(Prompt), L"Driver start result: %r", Status);
        GuiDrawMsgBox(L"Load EFI driver", Prompt);
        break;
      }
      case 3:
        UefiToolsShowBootEntries();
        break;
    }
  }
}

// Returns TRUE only when the user chooses to quit EC.
static BOOLEAN ShowProgramMenu(
  IN OUT PANEL* LeftPanel,
  IN OUT PANEL* RightPanel,
  IN BOOLEAN LeftActive,
  IN OUT BOOLEAN* QuickView
)
{
  STATIC CONST CHAR16* Lines[] = {
    L"Refresh both panels",
    L"Change active drive...  [F2]",
    L"Find file in active tree...  [Alt+F7]",
    L"Compare panel directories  [=]",
    L"Recursive compare / update...",
    L"Checksum selected file...",
    L"Toggle Quick View  [Ctrl+Q]",
    L"Run selected EFI with arguments...",
    L"UEFI tools...",
    L"Selection tools...",
    L"Set active panel filter...  [Ctrl+F12]",
    L"Directory hotlist...  [Alt+F10]",
    L"Settings...",
    L"Help  [F1]",
    L"Quit EC  [F10]",
    L"Close menu"
  };
  UINTN Chosen = 0;
  PANEL* ActivePanel = LeftActive ? LeftPanel : RightPanel;
  NAV_HISTORY* ActiveHistory = LeftActive ? &gLeftHistory : &gRightHistory;

  for (;;) {
    if (!GuiDrawListPicker(L"EC Menu", Lines, ARRAY_SIZE(Lines), &Chosen)) return FALSE;
    switch (Chosen) {
      case 0:
        FsInit();
        PanelRefresh(LeftPanel);
        PanelRefresh(RightPanel);
        return FALSE;
      case 1: {
        CHAR16 Before[MAX_PATH_LEN];
        StrCpyS(Before, ARRAY_SIZE(Before), ActivePanel->Path);
        ShowDriveMenu(LeftPanel, RightPanel, LeftActive, LeftActive);
        if (StrCmp(Before, ActivePanel->Path) != 0) {
          NavHistoryPush(ActiveHistory, ActivePanel->Path);
        }
        return FALSE;
      }
      case 2: {
        CHAR16 HitDir[MAX_PATH_LEN];
        CHAR16 HitName[256];
        if (ActivePanel->Path[0] == L'\0') {
          GuiDrawMsgBox(L"Find file", L"Choose a drive before searching.");
          break;
        }
        if (SearchRunInteractive(ActivePanel->Path, HitDir, HitName)) {
          StrCpyS(ActivePanel->Path, MAX_PATH_LEN, HitDir);
          NavHistoryPush(ActiveHistory, ActivePanel->Path);
          PanelRefreshKeep(ActivePanel, HitName, 0);
        }
        return FALSE;
      }
      case 3: {
        UINTN Marked = PanelOpsCompareSelect(LeftPanel, RightPanel);
        if (Marked == 0) {
          GuiDrawMsgBox(L"Compare", L"The two directories agree.");
        }
        return FALSE;
      }
      case 4:
        ShowRecursiveSync(LeftPanel, RightPanel);
        return FALSE;
      case 5:
        ShowCurrentFileChecksum(ActivePanel);
        break;
      case 6:
        if (QuickView != NULL) {
          *QuickView = !*QuickView;
          if (!*QuickView) GuiQuickViewReset();
        }
        return FALSE;
      case 7:
        RunCurrentEfiWithArguments(ActivePanel);
        return FALSE;
      case 8:
        ShowUefiTools(LeftPanel, RightPanel, ActivePanel);
        break;
      case 9:
        if (ShowSelectionMenu(ActivePanel)) return FALSE;
        break;
      case 10: {
        CHAR16 Mask[128];
        StrCpyS(Mask, ARRAY_SIZE(Mask), ActivePanel->FilterMask);
        if (GuiDrawInputBox(L"Filter", L"Show mask (* clears):", Mask, ARRAY_SIZE(Mask))) {
          PanelSetFilter(ActivePanel, Mask);
          return FALSE;
        }
        break;
      }
      case 11:
        NavShowHotlist(ActivePanel, ActiveHistory);
        return FALSE;
      case 12:
        ShowSettingsMenu(LeftPanel, RightPanel);
        break;
      case 13:
        GuiDrawHelp();
        break;
      case 14:
        return TRUE;
      default:
        return FALSE;
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

  // Scripted run for the harness. Compiled out of the release build entirely,
  // and even in a self-test build it only fires when the boot volume carries
  // the flag file, so the same binary is still a usable file manager.
  if (EcSelfTestMaybeRun(ImageHandle)) {
    UiConsoleShutdown();
    gST->ConOut->SetMode(gST->ConOut, originalTextMode);
    return EFI_SUCCESS;
  }

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
  BOOLEAN quickView = FALSE;
  CHAR16 lastSearch[MAX_PATH_LEN] = { 0 };
  CHAR16 quickSearch[128] = { 0 };
  UINTN quickSearchLen = 0;

  while (!quit) {
    PANEL* activePanel = leftActive ? &leftPanel : &rightPanel;
    PANEL* inactivePanel = leftActive ? &rightPanel : &leftPanel;
    NAV_HISTORY* activeHistory = leftActive ? &gLeftHistory : &gRightHistory;

    UINTN pH = GuiPanelHeight();
    UINTN pageSize = GuiGetPageSize(pH);

    // 1. Draw panels and menu
    GuiDrawPanels(&leftPanel, &rightPanel, leftActive);
    if (quickView) GuiDrawQuickView(activePanel, (BOOLEAN)!leftActive);
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
      } else if (key.ScanCode == SCAN_F7) {
        // Find a file anywhere under the active panel's directory. A hit taken
        // from the list moves the panel to the directory holding it, with the
        // cursor already on the entry - the point is to get there, not just to
        // learn that it exists.
        CHAR16 hitDir[MAX_PATH_LEN];
        CHAR16 hitName[256];
        if (activePanel->Path[0] != L'\0' &&
            SearchRunInteractive(activePanel->Path, hitDir, hitName)) {
          StrCpyS(activePanel->Path, MAX_PATH_LEN, hitDir);
          NavHistoryPush(activeHistory, activePanel->Path);
          PanelRefreshKeep(activePanel, hitName, 0);
        }
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
          case SCAN_F2: {
            // Attributes and modification time of the entry under the cursor.
            QuickFindReset(quickSearch, &quickSearchLen);
            if (activePanel->SelectedIndex >= 0 &&
                (UINTN)activePanel->SelectedIndex < activePanel->FileCount) {
              FS_FILE_ITEM* item = &activePanel->Files[activePanel->SelectedIndex];
              if (PanelOpsIsUsableItem(item)) {
                CHAR16 target[MAX_PATH_LEN];
                FsCombinePath(target, activePanel->Path, item->Name);
                if (FilePropsEdit(target)) {
                  PanelRefreshKeep(activePanel, item->Name, activePanel->SelectedIndex);
                }
              }
            }
            break;
          }
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
          QuickFindReset(quickSearch, &quickSearchLen);
          if (ShowProgramMenu(&leftPanel, &rightPanel, leftActive, &quickView)) quit = TRUE;
          break;
      }
    } else {
      // Handle Unicode Characters
      if (ctrlPressed && (key.UnicodeChar == L'q' || key.UnicodeChar == L'Q' || key.UnicodeChar == 17)) {
        QuickFindReset(quickSearch, &quickSearchLen);
        quickView = !quickView;
        if (!quickView) GuiQuickViewReset();
        continue;
      }
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

        case L'=': {
          // Compare the two panels and light up whatever differs. It joins the
          // family of selection keys - '+' by mask, '-' by mask, '*' inverts -
          // and needs no modifier, which matters on consoles that swallow Alt.
          UINTN marked;
          QuickFindReset(quickSearch, &quickSearchLen);
          marked = PanelOpsCompareSelect(&leftPanel, &rightPanel);
          if (marked == 0) {
            GuiDrawMsgBox(L"Compare", L"The two directories agree.");
          }
          break;
        }

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
  GuiQuickViewReset();

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
