/*
 * Console.c - the Ctrl+O command line.
 *
 * The panels are a good way to move files and a poor way to say "load that
 * driver with these arguments". This gives EC the other half: a prompt, in the
 * firmware's own text mode rather than in the GOP screen EC normally paints,
 * so output from anything EC did not print itself - a driver's banner, a shell
 * command - lands where it would have landed anyway.
 *
 * The built-in commands are thin: everything they do already exists in
 * FileSystem.c and is reachable from the panels too. What they add is reach in
 * one line, on a machine where there is no shell to fall back to.
 */
#include "Console.h"
#include "Checksum.h"
#include "FileSystem.h"
#include "Gui.h"
#include "PanelOps.h"
#include "UiConsole.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/Shell.h>

#define CONSOLE_TYPE_BYTES 4096

static EFI_GUID gConsoleShellProtocolGuid = EFI_SHELL_PROTOCOL_GUID;

// ---------------------------------------------------------------- text work

UINTN ConsoleSplitArgs(
  IN OUT CHAR16* Line,
  OUT    CHAR16** Args,
  IN     UINTN MaxArgs
) {
  UINTN count = 0;
  CHAR16* at;

  if (Line == NULL || Args == NULL || MaxArgs == 0) return 0;

  at = Line;
  while (*at != L'\0' && count < MaxArgs) {
    while (*at == L' ' || *at == L'\t') at++;
    if (*at == L'\0') break;

    if (*at == L'"') {
      at++;
      Args[count++] = at;
      while (*at != L'\0' && *at != L'"') at++;
    } else {
      Args[count++] = at;
      while (*at != L'\0' && *at != L' ' && *at != L'\t') at++;
    }
    if (*at != L'\0') {
      *at = L'\0';
      at++;
    }
  }
  return count;
}

BOOLEAN ConsoleResolvePath(
  IN  CONST CHAR16* WorkingDir,
  IN  CONST CHAR16* Typed,
  OUT CHAR16* Out,
  IN  UINTN OutChars
) {
  UINTN i;

  if (WorkingDir == NULL || Typed == NULL || Out == NULL || OutChars == 0) return FALSE;
  Out[0] = L'\0';
  if (Typed[0] == L'\0') return FALSE;

  for (i = 0; Typed[i] != L'\0'; i++) {
    if (Typed[i] == L':') {
      if (StrLen(Typed) + 1 > OutChars) return FALSE;
      StrCpyS(Out, OutChars, Typed);
      return TRUE;
    }
  }

  if (Typed[0] == L'\\') {
    // Root-relative: keep the working directory's volume, drop its path.
    CHAR16 volume[MAX_PATH_LEN];
    UINTN colon = 0;

    while (WorkingDir[colon] != L'\0' && WorkingDir[colon] != L':') colon++;
    if (WorkingDir[colon] != L':') return FALSE;
    if (colon + 2 > ARRAY_SIZE(volume)) return FALSE;
    CopyMem(volume, WorkingDir, (colon + 1) * sizeof(CHAR16));
    volume[colon + 1] = L'\0';
    if (StrLen(volume) + StrLen(Typed) + 1 > OutChars) return FALSE;
    StrCpyS(Out, OutChars, volume);
    StrCatS(Out, OutChars, Typed);
    return TRUE;
  }

  if (StrLen(WorkingDir) + StrLen(Typed) + 2 > OutChars) return FALSE;
  FsCombinePath(Out, WorkingDir, Typed);
  return TRUE;
}

// ------------------------------------------------------------ line editing

// One line from the keyboard, echoed as it is typed. Returns FALSE when the
// user left the prompt with Esc or Ctrl+O rather than submitting a line.
static BOOLEAN ConsoleReadLine(
  OUT CHAR16* Line,
  IN  UINTN LineChars,
  IN  CHAR16 History[CONSOLE_HISTORY][CONSOLE_LINE_CHARS],
  IN  UINTN HistoryCount
) {
  UINTN length = 0;
  UINTN recall = HistoryCount;   /* one past the newest: nothing recalled yet */
  EFI_INPUT_KEY key;
  UINTN index;

  Line[0] = L'\0';
  for (;;) {
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
    if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) continue;

    if (key.UnicodeChar == 15) return FALSE;                 /* Ctrl+O again */
    if (key.ScanCode == SCAN_ESC) {
      if (length == 0) return FALSE;
      while (length > 0) {                                   /* rub the line out */
        Print(L"\b \b");
        length--;
      }
      Line[0] = L'\0';
      continue;
    }

    if (key.ScanCode == SCAN_UP || key.ScanCode == SCAN_DOWN) {
      UINTN wanted = recall;

      if (HistoryCount == 0) continue;
      if (key.ScanCode == SCAN_UP) {
        if (wanted == 0) continue;
        wanted--;
      } else {
        if (wanted + 1 >= HistoryCount) {
          wanted = HistoryCount;                             /* back to an empty line */
        } else {
          wanted++;
        }
      }
      while (length > 0) {
        Print(L"\b \b");
        length--;
      }
      recall = wanted;
      if (wanted < HistoryCount) {
        StrCpyS(Line, LineChars, History[wanted]);
        length = StrLen(Line);
        Print(L"%s", Line);
      } else {
        Line[0] = L'\0';
      }
      continue;
    }

    if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Print(L"\r\n");
      Line[length] = L'\0';
      return TRUE;
    }
    if (key.UnicodeChar == CHAR_BACKSPACE) {
      if (length > 0) {
        length--;
        Line[length] = L'\0';
        Print(L"\b \b");
      }
      continue;
    }
    if (key.UnicodeChar >= L' ' && length + 1 < LineChars) {
      Line[length++] = key.UnicodeChar;
      Line[length] = L'\0';
      Print(L"%c", key.UnicodeChar);
    }
  }
}

// ------------------------------------------------------------- the commands

static VOID ConsoleHelp(VOID)
{
  Print(L"\r\n");
  Print(L"  map                     mounted volumes, with free space\r\n");
  Print(L"  cd [path]               change the working directory\r\n");
  Print(L"  dir [mask]              list it\r\n");
  Print(L"  type <file>             first %d bytes as text\r\n", CONSOLE_TYPE_BYTES);
  Print(L"  copy <src> <dst>        recursive copy\r\n");
  Print(L"  move <src> <dst>        rename or move\r\n");
  Print(L"  del <path>              delete, recursive\r\n");
  Print(L"  md <path>               create a directory\r\n");
  Print(L"  sha256 <file>           SHA-256 and CRC32\r\n");
  Print(L"  load <driver.efi>       start an image as an EFI driver\r\n");
  Print(L"  run <app.efi> [args]    start an application, args as LoadOptions\r\n");
  Print(L"  cls                     clear the screen\r\n");
  Print(L"  reset / shutdown        restart or power off the machine\r\n");
  Print(L"  exit                    back to the panels (so does Ctrl+O)\r\n");
  Print(L"\r\n");
  Print(L"  Anything else is passed to the UEFI Shell if the firmware has one.\r\n");
  Print(L"  Paths: fs0:\\dir\\file absolute, \\dir from this volume's root,\r\n");
  Print(L"  anything else relative to the working directory. Quote spaces.\r\n\r\n");
}

static VOID ConsoleMap(VOID)
{
  UINTN i;

  Print(L"\r\n");
  for (i = 0; i < gVolumeCount; i++) {
    UINT64 total = 0;
    UINT64 free = 0;
    CHAR16 label[32] = { 0 };
    CHAR16 freeText[32];
    CHAR16 totalText[32];

    if (EFI_ERROR(FsGetVolumeInfo(&gVolumes[i], &total, &free, label, ARRAY_SIZE(label)))) {
      Print(L"  %-8s  (information unavailable)\r\n", gVolumes[i].Name);
      continue;
    }
    FormatFileSize(free, freeText, ARRAY_SIZE(freeText));
    FormatFileSize(total, totalText, ARRAY_SIZE(totalText));
    Print(L"  %-8s  %-16s  %s free of %s\r\n",
          gVolumes[i].Name, label[0] != L'\0' ? label : L"<no label>", freeText, totalText);
  }
  Print(L"\r\n");
}

static VOID ConsoleDir(IN CONST CHAR16* Dir, IN CONST CHAR16* Mask)
{
  FS_FILE_ITEM* items = NULL;
  UINTN count = 0;
  UINTN files = 0;
  UINTN directories = 0;
  UINT64 bytes = 0;
  UINTN i;

  if (EFI_ERROR(FsListDirectory(Dir, &items, &count)) || items == NULL) {
    Print(L"cannot list %s\r\n", Dir);
    return;
  }

  Print(L"\r\n  %s\r\n\r\n", Dir);
  for (i = 0; i < count; i++) {
    CHAR16 sizeText[32];

    if (!PanelOpsMatchMask(items[i].Name, Mask)) continue;
    if (items[i].IsDirectory) {
      directories++;
      Print(L"  %-40s  %10s\r\n", items[i].Name, L"<DIR>");
    } else {
      files++;
      bytes += items[i].Size;
      FormatFileSize(items[i].Size, sizeText, ARRAY_SIZE(sizeText));
      Print(L"  %-40s  %10s\r\n", items[i].Name, sizeText);
    }
  }
  {
    CHAR16 totalText[32];
    FormatFileSize(bytes, totalText, ARRAY_SIZE(totalText));
    Print(L"\r\n  %d files, %s, %d directories\r\n\r\n",
          (UINT32)files, totalText, (UINT32)directories);
  }
  FreePool(items);
}

static VOID ConsoleType(IN CONST CHAR16* Path)
{
  UINT8 buffer[CONSOLE_TYPE_BYTES];
  UINTN read = 0;
  UINT64 total = 0;
  UINTN i;

  if (EFI_ERROR(FsReadFilePrefix(Path, buffer, sizeof(buffer), &read, &total))) {
    Print(L"cannot read %s\r\n", Path);
    return;
  }

  Print(L"\r\n");
  for (i = 0; i < read; i++) {
    UINT8 ch = buffer[i];
    if (ch == '\n') Print(L"\r\n");
    else if (ch == '\t') Print(L"  ");
    else if (ch >= 32 && ch < 127) Print(L"%c", (CHAR16)ch);
    else if (ch != '\r') Print(L".");
  }
  Print(L"\r\n");
  if (total > read) {
    Print(L"\r\n  ... %ld bytes total, first %d shown\r\n", total, (UINT32)read);
  }
  Print(L"\r\n");
}

static VOID ConsoleSha256(IN CONST CHAR16* Path)
{
  EC_FILE_CHECKSUM sum;
  CHAR16 text[65];
  EFI_STATUS status = ChecksumFile(Path, &sum);

  if (EFI_ERROR(status)) {
    Print(L"cannot read %s : %r\r\n", Path, status);
    return;
  }
  ChecksumSha256ToText(sum.Sha256, text);
  Print(L"  CRC32   %08x\r\n", sum.Crc32);
  Print(L"  SHA-256 %s\r\n", text);
}

// The command line as typed, for the shell that may or may not be there.
static VOID ConsoleTryShell(IN EFI_HANDLE ImageHandle, IN CONST CHAR16* Line)
{
  EFI_SHELL_PROTOCOL* shell = NULL;
  EFI_STATUS commandStatus = EFI_SUCCESS;
  EFI_STATUS status;
  CHAR16* copy;
  UINTN bytes;

  status = gBS->LocateProtocol(&gConsoleShellProtocolGuid, NULL, (VOID**)&shell);
  if (EFI_ERROR(status) || shell == NULL || shell->Execute == NULL) {
    Print(L"unknown command, and this firmware has no UEFI Shell to ask\r\n");
    return;
  }

  // Execute takes a writable command line.
  bytes = (StrLen(Line) + 1) * sizeof(CHAR16);
  copy = AllocateCopyPool(bytes, Line);
  if (copy == NULL) {
    Print(L"out of memory\r\n");
    return;
  }
  status = shell->Execute(&ImageHandle, copy, NULL, &commandStatus);
  FreePool(copy);
  if (EFI_ERROR(status)) {
    Print(L"the shell refused the command: %r\r\n", status);
  } else if (EFI_ERROR(commandStatus)) {
    Print(L"the command returned %r\r\n", commandStatus);
  }
}

// TRUE while the prompt should keep running.
static BOOLEAN ConsoleDispatch(
  IN     EFI_HANDLE ImageHandle,
  IN     CONST CHAR16* WholeLine,
  IN     CHAR16** Args,
  IN     UINTN ArgCount,
  IN OUT CHAR16* WorkingDir,
  IN     UINTN WorkingDirChars
) {
  CHAR16 first[MAX_PATH_LEN];
  CHAR16 second[MAX_PATH_LEN];
  EFI_STATUS status;

  if (ArgCount == 0) return TRUE;

  if (StrCmp(Args[0], L"exit") == 0 || StrCmp(Args[0], L"quit") == 0) return FALSE;

  if (StrCmp(Args[0], L"help") == 0 || StrCmp(Args[0], L"?") == 0) {
    ConsoleHelp();
  } else if (StrCmp(Args[0], L"cls") == 0) {
    gST->ConOut->ClearScreen(gST->ConOut);
  } else if (StrCmp(Args[0], L"map") == 0) {
    ConsoleMap();
  } else if (StrCmp(Args[0], L"pwd") == 0) {
    Print(L"  %s\r\n", WorkingDir);
  } else if (StrCmp(Args[0], L"cd") == 0) {
    BOOLEAN isDirectory = FALSE;
    if (ArgCount < 2) {
      Print(L"  %s\r\n", WorkingDir);
    } else if (!ConsoleResolvePath(WorkingDir, Args[1], first, ARRAY_SIZE(first))) {
      Print(L"path too long\r\n");
    } else if (!FsFileExists(first, &isDirectory) || !isDirectory) {
      Print(L"no such directory: %s\r\n", first);
    } else {
      StrCpyS(WorkingDir, WorkingDirChars, first);
    }
  } else if (StrCmp(Args[0], L"dir") == 0 || StrCmp(Args[0], L"ls") == 0) {
    ConsoleDir(WorkingDir, ArgCount > 1 ? Args[1] : L"*");
  } else if (StrCmp(Args[0], L"type") == 0 || StrCmp(Args[0], L"cat") == 0) {
    if (ArgCount < 2) Print(L"type <file>\r\n");
    else if (!ConsoleResolvePath(WorkingDir, Args[1], first, ARRAY_SIZE(first))) Print(L"path too long\r\n");
    else ConsoleType(first);
  } else if (StrCmp(Args[0], L"sha256") == 0) {
    if (ArgCount < 2) Print(L"sha256 <file>\r\n");
    else if (!ConsoleResolvePath(WorkingDir, Args[1], first, ARRAY_SIZE(first))) Print(L"path too long\r\n");
    else ConsoleSha256(first);
  } else if (StrCmp(Args[0], L"copy") == 0 || StrCmp(Args[0], L"cp") == 0) {
    if (ArgCount < 3) {
      Print(L"copy <source> <destination>\r\n");
    } else if (!ConsoleResolvePath(WorkingDir, Args[1], first, ARRAY_SIZE(first)) ||
               !ConsoleResolvePath(WorkingDir, Args[2], second, ARRAY_SIZE(second))) {
      Print(L"path too long\r\n");
    } else {
      status = FsCopyRecursive(first, second, NULL);
      if (EFI_ERROR(status)) Print(L"copy failed: %r\r\n", status);
      else {
        FsFlushVolumeForPath(second);
        Print(L"  copied\r\n");
      }
    }
  } else if (StrCmp(Args[0], L"move") == 0 || StrCmp(Args[0], L"ren") == 0) {
    if (ArgCount < 3) {
      Print(L"move <source> <destination>\r\n");
    } else if (!ConsoleResolvePath(WorkingDir, Args[1], first, ARRAY_SIZE(first)) ||
               !ConsoleResolvePath(WorkingDir, Args[2], second, ARRAY_SIZE(second))) {
      Print(L"path too long\r\n");
    } else {
      status = FsRenameOrMove(first, second);
      if (EFI_ERROR(status)) Print(L"move failed: %r\r\n", status);
      else Print(L"  moved\r\n");
    }
  } else if (StrCmp(Args[0], L"del") == 0 || StrCmp(Args[0], L"rm") == 0) {
    if (ArgCount < 2) {
      Print(L"del <path>\r\n");
    } else if (!ConsoleResolvePath(WorkingDir, Args[1], first, ARRAY_SIZE(first))) {
      Print(L"path too long\r\n");
    } else {
      status = FsDeleteRecursive(first);
      if (EFI_ERROR(status)) Print(L"delete failed: %r\r\n", status);
      else {
        FsFlushVolumeForPath(first);
        Print(L"  deleted\r\n");
      }
    }
  } else if (StrCmp(Args[0], L"md") == 0 || StrCmp(Args[0], L"mkdir") == 0) {
    if (ArgCount < 2) {
      Print(L"md <path>\r\n");
    } else if (!ConsoleResolvePath(WorkingDir, Args[1], first, ARRAY_SIZE(first))) {
      Print(L"path too long\r\n");
    } else {
      status = FsCreateDir(first);
      if (EFI_ERROR(status)) Print(L"mkdir failed: %r\r\n", status);
      else FsFlushVolumeForPath(first);
    }
  } else if (StrCmp(Args[0], L"load") == 0) {
    if (ArgCount < 2) {
      Print(L"load <driver.efi>\r\n");
    } else if (!ConsoleResolvePath(WorkingDir, Args[1], first, ARRAY_SIZE(first))) {
      Print(L"path too long\r\n");
    } else {
      status = FsStartEfiDriver(ImageHandle, first);
      Print(L"  driver start: %r\r\n", status);
    }
  } else if (StrCmp(Args[0], L"run") == 0) {
    if (ArgCount < 2) {
      Print(L"run <app.efi> [arguments]\r\n");
    } else if (!ConsoleResolvePath(WorkingDir, Args[1], first, ARRAY_SIZE(first))) {
      Print(L"path too long\r\n");
    } else {
      CHAR16 arguments[CONSOLE_LINE_CHARS];
      UINTN i;

      arguments[0] = L'\0';
      for (i = 2; i < ArgCount; i++) {
        if (arguments[0] != L'\0') StrCatS(arguments, ARRAY_SIZE(arguments), L" ");
        StrCatS(arguments, ARRAY_SIZE(arguments), Args[i]);
      }
      status = FsStartEfiAppWithArgs(ImageHandle, first, arguments);
      Print(L"\r\n  application returned: %r\r\n", status);
    }
  } else if (StrCmp(Args[0], L"reset") == 0) {
    gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
  } else if (StrCmp(Args[0], L"shutdown") == 0) {
    gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  } else {
    ConsoleTryShell(ImageHandle, WholeLine);
  }

  return TRUE;
}

// ------------------------------------------------------------- the prompt

VOID ConsoleRun(
  IN     EFI_HANDLE ImageHandle,
  IN     INT32 TextMode,
  IN OUT PANEL* ActivePanel
) {
  CHAR16 history[CONSOLE_HISTORY][CONSOLE_LINE_CHARS];
  CHAR16 working[MAX_PATH_LEN];
  CHAR16 line[CONSOLE_LINE_CHARS];
  CHAR16 parsed[CONSOLE_LINE_CHARS];
  CHAR16* args[CONSOLE_MAX_ARGS];
  UINTN historyCount = 0;
  BOOLEAN running = TRUE;

  if (ActivePanel == NULL) return;
  StrCpyS(working, ARRAY_SIZE(working), ActivePanel->Path);

  gST->ConOut->SetMode(gST->ConOut, TextMode);
  gST->ConOut->ClearScreen(gST->ConOut);
  gST->ConOut->EnableCursor(gST->ConOut, TRUE);

  Print(L"EFI Commander console. \"help\" lists the commands, "
        L"\"exit\" or Ctrl+O returns to the panels.\r\n\r\n");

  while (running) {
    UINTN count;

    Print(L"%s> ", working[0] != L'\0' ? working : L"(no volume)");
    if (!ConsoleReadLine(line, ARRAY_SIZE(line), history, historyCount)) break;
    if (line[0] == L'\0') continue;

    // The whole line is kept intact for the shell; the split works on a copy,
    // because splitting writes terminators into what it is given.
    StrCpyS(parsed, ARRAY_SIZE(parsed), line);
    count = ConsoleSplitArgs(parsed, args, CONSOLE_MAX_ARGS);
    running = ConsoleDispatch(ImageHandle, line, args, count, working, ARRAY_SIZE(working));

    if (historyCount < CONSOLE_HISTORY) {
      StrCpyS(history[historyCount++], CONSOLE_LINE_CHARS, line);
    } else {
      UINTN i;
      for (i = 1; i < CONSOLE_HISTORY; i++) {
        StrCpyS(history[i - 1], CONSOLE_LINE_CHARS, history[i]);
      }
      StrCpyS(history[CONSOLE_HISTORY - 1], CONSOLE_LINE_CHARS, line);
    }
  }

  gST->ConOut->EnableCursor(gST->ConOut, FALSE);
  gST->ConOut->ClearScreen(gST->ConOut);

  // Wherever the console finished is where the panel stands.
  if (StrCmp(working, ActivePanel->Path) != 0) {
    StrCpyS(ActivePanel->Path, MAX_PATH_LEN, working);
  }
}
