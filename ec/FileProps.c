// FileProps.c - view and edit the DOS attributes and modification time of a file.
//
// The driver has been able to write both for a while; until now nothing in the
// manager asked it to. The case that matters in rescue work is clearing
// ReadOnly on a system file before overwriting it, which is why the attribute
// toggles are single keys and the dialog writes only what was actually changed.
#include "FileProps.h"

#include "Colors.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include "FileSystem.h"
#include "Gui.h"
#include "UiConsole.h"

static BOOLEAN FilePropsDigits(IN CONST CHAR16* Text, IN UINTN Count, OUT UINT32* Value)
{
  UINT32 v = 0;
  UINTN i;

  for (i = 0; i < Count; i++) {
    if (Text[i] < L'0' || Text[i] > L'9') return FALSE;
    v = v * 10 + (UINT32)(Text[i] - L'0');
  }
  *Value = v;
  return TRUE;
}

BOOLEAN FilePropsParseTime(IN CONST CHAR16* Text, OUT EFI_TIME* Time)
{
  UINT32 year, month, day, hour = 0, minute = 0, second = 0;

  if (Text == NULL || Time == NULL) return FALSE;
  if (StrLen(Text) < 10) return FALSE;
  if (!FilePropsDigits(Text, 4, &year)) return FALSE;
  if (Text[4] != L'-' || !FilePropsDigits(Text + 5, 2, &month)) return FALSE;
  if (Text[7] != L'-' || !FilePropsDigits(Text + 8, 2, &day)) return FALSE;

  if (StrLen(Text) >= 16) {
    if (Text[10] != L' ' && Text[10] != L'T') return FALSE;
    if (!FilePropsDigits(Text + 11, 2, &hour)) return FALSE;
    if (Text[13] != L':' || !FilePropsDigits(Text + 14, 2, &minute)) return FALSE;
    if (StrLen(Text) >= 19) {
      if (Text[16] != L':' || !FilePropsDigits(Text + 17, 2, &second)) return FALSE;
    }
  }

  if (year < 1980 || year > 9999) return FALSE;
  if (month < 1 || month > 12 || day < 1 || day > 31) return FALSE;
  if (hour > 23 || minute > 59 || second > 59) return FALSE;

  ZeroMem(Time, sizeof(*Time));
  Time->Year   = (UINT16)year;
  Time->Month  = (UINT8)month;
  Time->Day    = (UINT8)day;
  Time->Hour   = (UINT8)hour;
  Time->Minute = (UINT8)minute;
  Time->Second = (UINT8)second;
  Time->Daylight = 0;
  Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
  return TRUE;
}

static VOID FilePropsFormatTime(IN CONST EFI_TIME* Time, OUT CHAR16* Text, IN UINTN Bytes)
{
  UnicodeSPrint(Text, Bytes, L"%04d-%02d-%02d %02d:%02d:%02d",
                Time->Year, Time->Month, Time->Day,
                Time->Hour, Time->Minute, Time->Second);
}

static VOID FilePropsAttrLine(IN UINT64 Attr, OUT CHAR16* Text, IN UINTN Bytes)
{
  UnicodeSPrint(Text, Bytes, L"[%c] R  ReadOnly     [%c] H  Hidden     [%c] S  System     [%c] A  Archive",
                (Attr & EFI_FILE_READ_ONLY) ? L'x' : L' ',
                (Attr & EFI_FILE_HIDDEN)    ? L'x' : L' ',
                (Attr & EFI_FILE_SYSTEM)    ? L'x' : L' ',
                (Attr & EFI_FILE_ARCHIVE)   ? L'x' : L' ');
}

BOOLEAN FilePropsEdit(IN CONST CHAR16* Path)
{
  UINT64 attrOriginal = 0, attr = 0;
  EFI_TIME created, modified, accessed, modifiedOriginal;
  CHAR16 timeText[32];
  CHAR16 line[MAX_PATH_LEN];
  BOOLEAN timeChanged = FALSE;
  BOOLEAN wrote = FALSE;
  UINTN width, height, cellW, cellH;
  UINTN boxW, boxH, boxX, boxY;

  if (Path == NULL || Path[0] == L'\0') return FALSE;

  if (EFI_ERROR(FsGetFileMeta(Path, &attrOriginal, &created, &modified, &accessed))) {
    GuiDrawMsgBox(L"Properties", L"Cannot read this entry's metadata.");
    return FALSE;
  }
  attr = attrOriginal;
  modifiedOriginal = modified;

  UiGfxGetDimensions(&width, &height);
  UiGfxGetCellSize(&cellW, &cellH);
  boxW = GuiDialogWidth(900);
  boxH = cellH * 12;
  boxX = (width - boxW) / 2;
  boxY = (height - boxH) / 2;

  for (;;) {
    UINTN textX = boxX + 25;
    UINTN maxChars = GuiCharsForWidth(boxW - 50, cellW);
    UINTN y = boxY + 15;

    UiGfxFillRectRgb(boxX, boxY, boxW, boxH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
    DrawBorder(boxX, boxY, boxW, boxH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B, 3);
    UiGfxDrawAsciiAt(boxX + 20, y, "Properties", COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

    y += cellH * 2;
    GuiDrawUnicodeClippedAt(textX, y, Path, maxChars, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

    y += cellH * 2;
    FilePropsAttrLine(attr, line, sizeof(line));
    GuiDrawUnicodeClippedAt(textX, y, line, maxChars, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

    y += cellH * 2;
    FilePropsFormatTime(&modified, timeText, sizeof(timeText));
    UnicodeSPrint(line, sizeof(line), L"Modified  %s%s", timeText, timeChanged ? L"  (edited)" : L"");
    GuiDrawUnicodeClippedAt(textX, y, line, maxChars, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

    y += cellH;
    FilePropsFormatTime(&created, timeText, sizeof(timeText));
    UnicodeSPrint(line, sizeof(line), L"Created   %s", timeText);
    GuiDrawUnicodeClippedAt(textX, y, line, maxChars, COLOR_GRAY_R, COLOR_GRAY_G, COLOR_GRAY_B);

    UiGfxDrawAsciiAt(textX, boxY + boxH - cellH - 15,
                     "R H S A toggle, D edits the date, Enter writes, Esc cancels",
                     COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    UiGfxFlush();

    {
      UINTN index;
      EFI_INPUT_KEY key;
      CHAR16 ch;

      gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
      if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) continue;

      if (key.ScanCode == SCAN_ESC) return FALSE;

      ch = key.UnicodeChar;
      if (ch >= L'a' && ch <= L'z') ch = (CHAR16)(ch - (L'a' - L'A'));

      if (ch == L'R') { attr ^= EFI_FILE_READ_ONLY; continue; }
      if (ch == L'H') { attr ^= EFI_FILE_HIDDEN;    continue; }
      if (ch == L'S') { attr ^= EFI_FILE_SYSTEM;    continue; }
      if (ch == L'A') { attr ^= EFI_FILE_ARCHIVE;   continue; }

      if (ch == L'D') {
        CHAR16 edit[32];
        FilePropsFormatTime(&modified, edit, sizeof(edit));
        if (GuiDrawInputBox(L"Modification time", L"YYYY-MM-DD HH:MM:SS", edit, ARRAY_SIZE(edit))) {
          EFI_TIME parsed;
          if (FilePropsParseTime(edit, &parsed)) {
            modified = parsed;
            timeChanged = TRUE;
          } else {
            GuiDrawMsgBox(L"Modification time", L"Use YYYY-MM-DD HH:MM:SS.");
          }
        }
        continue;
      }

      if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) break;
    }
  }

  if (attr != attrOriginal || timeChanged) {
    EFI_STATUS status = FsSetFileMeta(Path,
                                      (attr != attrOriginal) ? &attr : NULL,
                                      timeChanged ? &modified : NULL);
    if (EFI_ERROR(status)) {
      UnicodeSPrint(line, sizeof(line), L"The volume refused the change: %r", status);
      GuiDrawMsgBox(L"Properties", line);
    } else {
      wrote = TRUE;
    }
  }

  (VOID)modifiedOriginal;
  (VOID)accessed;
  return wrote;
}
