// Gui.c — GUI drawing functions for panels, menus, dialogs, and progress bars.
#include "Gui.h"
#include "Colors.h"
#include "UiConsole.h"
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

/*
 * Panel geometry, in text rows. The drawing code and the page-size calculation
 * both need these, and they used to carry their own copies: two functions
 * encoding one layout, where a change to the header would have scrolled the
 * cursor against a list that no longer matched. The row counts live here and
 * both read them.
 */
#define PANEL_HEADER_ROWS      2   /* volume line plus the path line          */
#define PANEL_COLHEADER_ROWS   1   /* the Name / Size / Date strip            */
#define PANEL_FOOTER_ROWS      1   /* the item count along the bottom         */
#define PANEL_LIST_TOP_PAD     4   /* pixels between the strip and row zero   */
#define PANEL_LIST_BOTTOM_PAD  8   /* pixels kept clear above the footer      */

/*
 * How wide a dialog gets: what it asks for, or whatever the screen can spare
 * once a margin is kept clear. Eight dialogs each carried their own pair of
 * numbers for this, with margins of 60, 80 and 100 pixels depending on which
 * one was written first, so a narrow screen showed three different insets side
 * by side. One margin, one rule.
 */
#define GUI_DIALOG_MARGIN 80

UINTN GuiDialogWidth(IN UINTN Preferred)
{
  UINTN width, height;

  UiGfxGetDimensions(&width, &height);
  (VOID)height;

  if (width <= GUI_DIALOG_MARGIN + 120) return width;          /* tiny screen: take it all */
  if (Preferred + GUI_DIALOG_MARGIN > width) return width - GUI_DIALOG_MARGIN;
  return Preferred;
}

/*
 * How tall a dialog gets, in text rows. Same idea as the width, and it was
 * written the same two different ways: one dialog wanted 21 rows and fell back
 * to the screen less 60 pixels, another wanted 17 and fell back to less 80.
 * The row count is the dialog's own business; the margin is not.
 */
UINTN GuiDialogHeight(IN UINTN Rows)
{
  UINTN width, height;
  UINTN cellW, cellH;

  UiGfxGetDimensions(&width, &height);
  UiGfxGetCellSize(&cellW, &cellH);
  (VOID)width;
  (VOID)cellW;

  if (cellH == 0) return height;
  if (height <= GUI_DIALOG_MARGIN + cellH * 4) return height;
  if (Rows * cellH + GUI_DIALOG_MARGIN > height) return height - GUI_DIALOG_MARGIN;
  return Rows * cellH;
}

VOID DrawBorder(UINTN x, UINTN y, UINTN w, UINTN h, UINT8 r, UINT8 g, UINT8 b, UINTN thickness)
{
  UiGfxFillRectRgb(x, y, w, thickness, r, g, b); // Top
  UiGfxFillRectRgb(x, y + h - thickness, w, thickness, r, g, b); // Bottom
  UiGfxFillRectRgb(x, y, thickness, h, r, g, b); // Left
  UiGfxFillRectRgb(x + w - thickness, y, thickness, h, r, g, b); // Right
}

UINTN GuiCharsForWidth(IN UINTN WidthPixels, IN UINTN CellW)
{
  if (CellW == 0) return 0;
  return WidthPixels / CellW;
}

static VOID GuiCopySpan(OUT CHAR16* Dest, IN UINTN DestChars, IN CONST CHAR16* Src, IN UINTN Count)
{
  UINTN i;

  if (Dest == NULL || DestChars == 0) return;
  if (Src == NULL) {
    Dest[0] = L'\0';
    return;
  }

  if (Count + 1 > DestChars) Count = DestChars - 1;
  for (i = 0; i < Count; i++) {
    Dest[i] = Src[i];
  }
  Dest[i] = L'\0';
}

VOID GuiDrawUnicodeClippedAt(
  IN UINTN X,
  IN UINTN Y,
  IN CONST CHAR16* Text,
  IN UINTN MaxChars,
  IN UINT8 R,
  IN UINT8 G,
  IN UINT8 B
) {
  CHAR16 line[160];
  UINTN len;

  if (Text == NULL || MaxChars == 0) return;

  len = StrLen(Text);
  if (len <= MaxChars) {
    UiGfxDrawUnicodeAt(X, Y, Text, R, G, B);
    return;
  }

  if (MaxChars >= sizeof(line) / sizeof(line[0])) {
    MaxChars = sizeof(line) / sizeof(line[0]) - 1;
  }
  GuiCopySpan(line, sizeof(line) / sizeof(line[0]), Text, MaxChars);
  if (MaxChars > 3) {
    line[MaxChars - 3] = L'.';
    line[MaxChars - 2] = L'.';
    line[MaxChars - 1] = L'.';
  }
  UiGfxDrawUnicodeAt(X, Y, line, R, G, B);
}

static VOID GuiDrawAsciiClippedAt(
  IN UINTN X,
  IN UINTN Y,
  IN CONST CHAR8* Text,
  IN UINTN MaxChars,
  IN UINT8 R,
  IN UINT8 G,
  IN UINT8 B
) {
  CHAR8 line[80];
  UINTN len;

  if (Text == NULL || MaxChars == 0) return;

  len = AsciiStrLen(Text);
  if (len <= MaxChars) {
    UiGfxDrawAsciiAt(X, Y, Text, R, G, B);
    return;
  }

  if (MaxChars >= sizeof(line)) {
    MaxChars = sizeof(line) - 1;
  }
  for (UINTN i = 0; i < MaxChars; i++) {
    line[i] = Text[i];
  }
  line[MaxChars] = '\0';
  if (MaxChars > 3) {
    line[MaxChars - 3] = '.';
    line[MaxChars - 2] = '.';
    line[MaxChars - 1] = '.';
  }
  UiGfxDrawAsciiAt(X, Y, line, R, G, B);
}

static VOID GuiDrawUnicodeRightAlignedAt(
  IN UINTN X,
  IN UINTN Y,
  IN UINTN FieldChars,
  IN CONST CHAR16* Text,
  IN UINT8 R,
  IN UINT8 G,
  IN UINT8 B
) {
  UINTN cellW, cellH;
  UINTN len;
  UINTN drawX;

  if (Text == NULL || FieldChars == 0) return;
  UiGfxGetCellSize(&cellW, &cellH);
  len = StrLen(Text);
  if (len > FieldChars) {
    GuiDrawUnicodeClippedAt(X, Y, Text, FieldChars, R, G, B);
    return;
  }
  drawX = X + (FieldChars - len) * cellW;
  UiGfxDrawUnicodeAt(drawX, Y, Text, R, G, B);
}

static UINTN GuiDrawUnicodeWrappedAt(
  IN UINTN X,
  IN UINTN Y,
  IN CONST CHAR16* Text,
  IN UINTN MaxChars,
  IN UINTN MaxLines,
  IN UINTN CellH,
  IN UINT8 R,
  IN UINT8 G,
  IN UINT8 B
) {
  CONST CHAR16* pos = Text;
  UINTN lineNo = 0;

  if (Text == NULL || MaxChars == 0 || MaxLines == 0) return 0;
  if (MaxChars > 150) MaxChars = 150;

  while (*pos != L'\0' && lineNo < MaxLines) {
    CHAR16 line[160];
    UINTN len;
    UINTN take;
    UINTN advance;

    while (*pos == L' ' || *pos == L'\t') pos++;
    if (*pos == L'\0') break;

    len = 0;
    while (pos[len] != L'\0' && pos[len] != L'\r' && pos[len] != L'\n') len++;

    take = len;
    advance = len;
    if (take > MaxChars) {
      UINTN breakAt = 0;
      for (UINTN i = MaxChars; i > 0; i--) {
        if (pos[i] == L' ' || pos[i] == L'\t') {
          breakAt = i;
          break;
        }
      }
      take = breakAt > 0 ? breakAt : MaxChars;
      advance = breakAt > 0 ? breakAt + 1 : MaxChars;
    }

    GuiCopySpan(line, sizeof(line) / sizeof(line[0]), pos, take);
    if (lineNo + 1 == MaxLines && pos[advance] != L'\0') {
      UINTN lineLen = StrLen(line);
      if (lineLen > 3) {
        line[lineLen - 3] = L'.';
        line[lineLen - 2] = L'.';
        line[lineLen - 1] = L'.';
      }
    }

    UiGfxDrawUnicodeAt(X, Y + CellH * lineNo, line, R, G, B);
    lineNo++;

    pos += advance;
    if (*pos == L'\r') pos++;
    if (*pos == L'\n') pos++;
  }

  return lineNo;
}

UINTN GuiGetPageSize(UINTN PanelHeight)
{
  UINTN cellW, cellH;
  UiGfxGetCellSize(&cellW, &cellH);
  if (cellH == 0) return 10;

  UINTN headerH = cellH * PANEL_HEADER_ROWS;
  UINTN columnHeaderH = cellH * PANEL_COLHEADER_ROWS;
  UINTN footerH = cellH * PANEL_FOOTER_ROWS;
  UINTN chrome = headerH + columnHeaderH + footerH + PANEL_LIST_TOP_PAD + PANEL_LIST_BOTTOM_PAD;

  if (PanelHeight <= chrome + cellH) return 1;
  return (PanelHeight - chrome) / cellH;
}

VOID FormatFileSize(UINT64 Size, CHAR16* Buffer, UINTN BufferSize)
{
  if (Size < 1024) {
    UnicodeSPrint(Buffer, BufferSize * sizeof(CHAR16), L"%d B", Size);
  } else if (Size < 1024 * 1024) {
    UnicodeSPrint(Buffer, BufferSize * sizeof(CHAR16), L"%d KB", Size / 1024);
  } else if (Size < 1024 * 1024 * 1024) {
    UnicodeSPrint(Buffer, BufferSize * sizeof(CHAR16), L"%d MB", Size / (1024 * 1024));
  } else {
    UnicodeSPrint(Buffer, BufferSize * sizeof(CHAR16), L"%d GB", Size / (1024 * 1024 * 1024));
  }
}

static VOID FormatFileDate(IN CONST EFI_TIME* Time, OUT CHAR16* Buffer, IN UINTN BufferSize)
{
  if (Time == NULL || Buffer == NULL || BufferSize == 0) return;
  if (Time->Year == 0) {
    StrCpyS(Buffer, BufferSize, L"--.--.-- --:--");
    return;
  }
  UnicodeSPrint(
    Buffer,
    BufferSize * sizeof(CHAR16),
    L"%02d.%02d.%02d %02d:%02d",
    Time->Day,
    Time->Month,
    Time->Year % 100,
    Time->Hour,
    Time->Minute
  );
}

static VOID FormatAttributes(IN UINT64 Attributes, OUT CHAR16* Buffer, IN UINTN BufferSize)
{
  if (Buffer == NULL || BufferSize < 6) return;
  Buffer[0] = (Attributes & EFI_FILE_DIRECTORY) ? L'D' : L'-';
  Buffer[1] = (Attributes & EFI_FILE_READ_ONLY) ? L'R' : L'-';
  Buffer[2] = (Attributes & EFI_FILE_HIDDEN) ? L'H' : L'-';
  Buffer[3] = (Attributes & EFI_FILE_SYSTEM) ? L'S' : L'-';
  Buffer[4] = (Attributes & EFI_FILE_ARCHIVE) ? L'A' : L'-';
  Buffer[5] = L'\0';
}

static CONST CHAR16* SortModeName(IN PANEL_SORT_MODE Mode)
{
  switch (Mode) {
    case PANEL_SORT_EXTENSION: return L"Ext";
    case PANEL_SORT_MODIFIED:  return L"Date";
    case PANEL_SORT_SIZE:      return L"Size";
    case PANEL_SORT_NAME:
    default:                   return L"Name";
  }
}

static CONST CHAR8* SortHeaderMark(IN CONST PANEL* Panel, IN PANEL_SORT_MODE Mode)
{
  if (Panel == NULL || Panel->SortMode != Mode) return "";
  return Panel->SortDescending ? "v" : "^";
}

static BOOLEAN PanelFilterEnabled(IN CONST PANEL* Panel)
{
  return Panel != NULL &&
         Panel->FilterMask[0] != L'\0' &&
         !(Panel->FilterMask[0] == L'*' && Panel->FilterMask[1] == L'\0');
}

static VOID ShortenNameForDisplay(CHAR16* Name, UINTN NameChars, UINTN MaxChars)
{
  UINTN Len;
  INTN Dot = -1;
  UINTN ExtLen = 0;

  if (Name == NULL || NameChars == 0 || MaxChars == 0) return;

  Len = StrLen(Name);
  if (Len <= MaxChars) return;

  if (MaxChars <= 3) {
    Name[MaxChars] = L'\0';
    return;
  }

  for (UINTN i = Len; i > 0; i--) {
    if (Name[i - 1] == L'.') {
      Dot = (INTN)(i - 1);
      break;
    }
  }

  if (Dot >= 0) {
    ExtLen = Len - (UINTN)Dot;
  }

  if (ExtLen > 0 && ExtLen + 2 < MaxChars) {
    UINTN PrefixLen = MaxChars - ExtLen - 1;
    CHAR16 Ext[32] = { 0 };
    if (ExtLen >= 32) ExtLen = 31;
    CopyMem(Ext, &Name[Dot], ExtLen * sizeof(CHAR16));
    Ext[ExtLen] = L'\0';
    Name[PrefixLen] = L'~';
    StrCpyS(&Name[PrefixLen + 1], NameChars - PrefixLen - 1, Ext);
  } else {
    Name[MaxChars] = L'\0';
    Name[MaxChars - 1] = L'~';
  }
}

static VOID GuiDrawOnePanel(PANEL* Panel, UINTN x, UINTN y, UINTN w, UINTN h, BOOLEAN Active)
{
  UINTN cellW, cellH;
  UiGfxGetCellSize(&cellW, &cellH);

  UINTN headerH = cellH * PANEL_HEADER_ROWS;
  UINTN footerH = cellH * PANEL_FOOTER_ROWS;
  UINTN thickness = Active ? 3 : 1;

  UINT8 borderR = Active ? COLOR_YELLOW_R : COLOR_GRAY_R;
  UINT8 borderG = Active ? COLOR_YELLOW_G : COLOR_GRAY_G;
  UINT8 borderB = Active ? COLOR_YELLOW_B : COLOR_GRAY_B;

  // 1. Fill blue background
  UiGfxFillRectRgb(x, y, w, h, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);

  // 2. Draw border
  DrawBorder(x, y, w, h, borderR, borderG, borderB, thickness);

  // 3. Draw header separator
  UiGfxFillRectRgb(x, y + headerH, w, thickness, borderR, borderG, borderB);

  // 4. Draw footer separator
  UiGfxFillRectRgb(x, y + h - footerH - thickness, w, thickness, borderR, borderG, borderB);

  // 5. Draw centered path title
  CHAR16 displayPath[MAX_PATH_LEN] = { 0 };
  if (Panel->Path[0] == L'\0') {
    StrCpyS(displayPath, MAX_PATH_LEN, L"Drive List");
  } else {
    StrCpyS(displayPath, MAX_PATH_LEN, Panel->Path);
  }

  UINTN pathLen = StrLen(displayPath);
  UINTN maxPathChars = (w - 40) / cellW;
  if (pathLen > maxPathChars) {
    // Truncate path for display
    displayPath[maxPathChars] = L'\0';
    if (maxPathChars > 3) {
      displayPath[maxPathChars - 1] = L'.';
      displayPath[maxPathChars - 2] = L'.';
      displayPath[maxPathChars - 3] = L'.';
    }
    pathLen = StrLen(displayPath);
  }

  UINTN titleX = x + (w - pathLen * cellW) / 2;
  UINTN titleY = y + (headerH - cellH) / 2;
  UiGfxDrawUnicodeAt(titleX, titleY, displayPath, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

  // 6. Draw file listing
  UINTN pageSize = GuiGetPageSize(h);

  // Adjust TopIndex dynamically if selection moved out of page viewport
  if (Panel->SelectedIndex >= 0) {
    if ((UINTN)Panel->SelectedIndex < Panel->TopIndex) {
      Panel->TopIndex = (UINTN)Panel->SelectedIndex;
    } else if ((UINTN)Panel->SelectedIndex >= Panel->TopIndex + pageSize) {
      Panel->TopIndex = (UINTN)Panel->SelectedIndex - pageSize + 1;
    }
  }

  UINTN contentX = x + 15;
  UINTN contentRight = (w > thickness + 15) ? x + w - thickness - 10 : x + w;
  UINTN panelChars = (contentRight > contentX && cellW > 0) ? ((contentRight - contentX) / cellW) : 0;
  UINTN gapChars = 1;
  UINTN sizeChars = 10;
  UINTN dateChars = 14;
  UINTN attrChars = 5;
  BOOLEAN showDate = (panelChars >= 50);
  BOOLEAN showAttr = (panelChars >= 66);
  UINTN usedRightChars = sizeChars;
  if (showDate) usedRightChars += gapChars + dateChars;
  if (showAttr) usedRightChars += gapChars + attrChars;

  UINTN sizeCol = (panelChars > usedRightChars) ? panelChars - usedRightChars : 0;
  UINTN dateCol = sizeCol + sizeChars + gapChars;
  UINTN attrCol = dateCol + dateChars + gapChars;
  UINTN sizeX = contentX + sizeCol * cellW;
  UINTN dateX = contentX + dateCol * cellW;
  UINTN attrX = contentX + attrCol * cellW;

  // Draw Column headers inside panel
  UINTN colHeaderY = y + headerH + thickness + 2;
  if (Panel->Path[0] != L'\0') {
    CHAR8 nameHeader[8];
    CHAR8 sizeHeader[8];
    CHAR8 dateHeader[8];
    AsciiSPrint(nameHeader, sizeof(nameHeader), "Name%a", SortHeaderMark(Panel, PANEL_SORT_NAME));
    AsciiSPrint(sizeHeader, sizeof(sizeHeader), "Size%a", SortHeaderMark(Panel, PANEL_SORT_SIZE));
    AsciiSPrint(dateHeader, sizeof(dateHeader), "Date%a", SortHeaderMark(Panel, PANEL_SORT_MODIFIED));
    UiGfxDrawAsciiAt(contentX, colHeaderY, nameHeader, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    UiGfxDrawAsciiAt(sizeX, colHeaderY, sizeHeader, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    if (showDate) UiGfxDrawAsciiAt(dateX, colHeaderY, dateHeader, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    if (showAttr) UiGfxDrawAsciiAt(attrX, colHeaderY, "Attr", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  } else {
    UiGfxDrawAsciiAt(contentX, colHeaderY, "Volume Name", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  }

  // Draw items
  for (UINTN i = 0; i < pageSize; i++) {
    UINTN itemIdx = Panel->TopIndex + i;
    if (itemIdx >= Panel->FileCount) {
      break;
    }

    FS_FILE_ITEM* file = &Panel->Files[itemIdx];
    UINTN itemY = y + headerH + thickness + (i + 1) * cellH;
    BOOLEAN isSelected = (Panel->SelectedIndex >= 0 && (UINTN)Panel->SelectedIndex == itemIdx);

    // Draw Selection Highlight Bar
    if (isSelected) {
      UINT8 bgR = Active ? COLOR_YELLOW_R : COLOR_GRAY_R;
      UINT8 bgG = Active ? COLOR_YELLOW_G : COLOR_GRAY_G;
      UINT8 bgB = Active ? COLOR_YELLOW_B : COLOR_GRAY_B;
      UiGfxFillRectRgb(x + thickness + 2, itemY, w - 2 * thickness - 4, cellH, bgR, bgG, bgB);
    }

    UINT8 textR, textG, textB;
    if (isSelected) {
      textR = COLOR_BLACK_R;
      textG = COLOR_BLACK_G;
      textB = COLOR_BLACK_B;
    } else if (file->Selected) {
      textR = COLOR_YELLOW_R;
      textG = COLOR_YELLOW_G;
      textB = COLOR_YELLOW_B;
    } else {
      textR = file->IsDirectory ? COLOR_CYAN_R : COLOR_WHITE_R;
      textG = file->IsDirectory ? COLOR_CYAN_G : COLOR_WHITE_G;
      textB = file->IsDirectory ? COLOR_CYAN_B : COLOR_WHITE_B;
    }

    // Format Name
    CHAR16 name[256] = { 0 };
    if (file->Selected) {
      StrCpyS(name, 256, L"* ");
      StrCatS(name, 256, file->Name);
    } else {
      StrCpyS(name, 256, file->Name);
    }

    UINTN nameRight = (Panel->Path[0] != L'\0') ? sizeX : contentRight;
    UINTN maxNameChars = (nameRight > contentX + cellW) ? ((nameRight - contentX) / cellW) : 8;
    if (maxNameChars > 0 && Panel->Path[0] != L'\0') {
      maxNameChars--;
    }
    ShortenNameForDisplay(name, 256, maxNameChars);

    // Draw file name
    UiGfxDrawUnicodeAt(contentX, itemY, name, textR, textG, textB);

    // Draw file size / <DIR>
    if (Panel->Path[0] != L'\0') {
      if (file->IsDirectory) {
        if (StrCmp(file->Name, L"..") == 0) {
          GuiDrawAsciiClippedAt(sizeX, itemY, "    UP-DIR", sizeChars, textR, textG, textB);
        } else {
          GuiDrawAsciiClippedAt(sizeX, itemY, "     <DIR>", sizeChars, textR, textG, textB);
        }
      } else {
        CHAR16 sizeStr[32] = { 0 };
        FormatFileSize(file->Size, sizeStr, 32);
        GuiDrawUnicodeRightAlignedAt(sizeX, itemY, sizeChars, sizeStr, textR, textG, textB);
      }

      if (showDate && StrCmp(file->Name, L"..") != 0) {
        CHAR16 dateStr[24] = { 0 };
        FormatFileDate(&file->ModificationTime, dateStr, 24);
        GuiDrawUnicodeClippedAt(dateX, itemY, dateStr, dateChars, textR, textG, textB);
      }
      if (showAttr && StrCmp(file->Name, L"..") != 0) {
        CHAR16 attrStr[8] = { 0 };
        FormatAttributes(file->Attributes, attrStr, 8);
        GuiDrawUnicodeClippedAt(attrX, itemY, attrStr, attrChars, textR, textG, textB);
      }
    }
  }

  // 7. Draw footer info (Status Bar)
  UINTN footerY = y + h - footerH - thickness + (footerH - cellH) / 2 + 2;
  CHAR16 footerStr[256] = { 0 };
  
  // Calculate selection details
  UINTN selectedCount = 0;
  UINT64 selectedSize = 0;
  UINTN visibleCount = 0;
  UINTN visibleDirs = 0;
  UINTN visibleFiles = 0;
  for (UINTN idx = 0; idx < Panel->FileCount; idx++) {
    if (StrCmp(Panel->Files[idx].Name, L"..") != 0) {
      visibleCount++;
      if (Panel->Files[idx].IsDirectory) {
        visibleDirs++;
      } else {
        visibleFiles++;
      }
    }
    if (Panel->Files[idx].Selected) {
      selectedCount++;
      selectedSize += Panel->Files[idx].Size;
    }
  }

  // Retrieve volume details
  CHAR16 volInfoStr[128] = { 0 };
  CHAR16 filterInfoStr[160] = { 0 };
  if (PanelFilterEnabled(Panel)) {
    UnicodeSPrint(filterInfoStr, sizeof(filterInfoStr), L" | Filter: %s", Panel->FilterMask);
  }
  if (Panel->VolumeTotal != 0) {
    CHAR16 freeStr[32] = { 0 };
    CHAR16 totalStr[32] = { 0 };
    FormatFileSize(Panel->VolumeFree, freeStr, 32);
    FormatFileSize(Panel->VolumeTotal, totalStr, 32);
    UnicodeSPrint(volInfoStr, sizeof(volInfoStr), L" | Free: %s/%s (%s)",
                  freeStr, totalStr, Panel->VolumeLabel);
  }

  if (selectedCount > 0) {
    CHAR16 szStr[32] = { 0 };
    FormatFileSize(selectedSize, szStr, 32);
    UnicodeSPrint(footerStr, sizeof(footerStr), L"%d items (%dD/%dF) | Sel %d (%s) | %s %s%s%s", visibleCount, visibleDirs, visibleFiles, selectedCount, szStr, SortModeName(Panel->SortMode), Panel->SortDescending ? L"desc" : L"asc", filterInfoStr, volInfoStr);
  } else if (Panel->FileCount == 0) {
    UnicodeSPrint(footerStr, sizeof(footerStr), L"Empty | %s %s%s%s", SortModeName(Panel->SortMode), Panel->SortDescending ? L"desc" : L"asc", filterInfoStr, volInfoStr);
  } else {
    if (Panel->SelectedIndex >= 0 && Panel->SelectedIndex < (INTN)Panel->FileCount) {
      FS_FILE_ITEM* current = &Panel->Files[Panel->SelectedIndex];
      if (current->IsDirectory) {
        UnicodeSPrint(footerStr, sizeof(footerStr), L"%d items (%dD/%dF) | %s <DIR> | %s %s%s%s", visibleCount, visibleDirs, visibleFiles, current->Name, SortModeName(Panel->SortMode), Panel->SortDescending ? L"desc" : L"asc", filterInfoStr, volInfoStr);
      } else {
        CHAR16 szStr[32] = { 0 };
        FormatFileSize(current->Size, szStr, 32);
        UnicodeSPrint(footerStr, sizeof(footerStr), L"%d items (%dD/%dF) | %s %s | %s %s%s%s", visibleCount, visibleDirs, visibleFiles, current->Name, szStr, SortModeName(Panel->SortMode), Panel->SortDescending ? L"desc" : L"asc", filterInfoStr, volInfoStr);
      }
    } else {
      UnicodeSPrint(footerStr, sizeof(footerStr), L"%d items (%dD/%dF) | %s %s%s%s", visibleCount, visibleDirs, visibleFiles, SortModeName(Panel->SortMode), Panel->SortDescending ? L"desc" : L"asc", filterInfoStr, volInfoStr);
    }
  }

  // Draw status bar text cleanly truncated if it's too wide
  UINTN maxFooterChars = (w - 30) / cellW;
  if (StrLen(footerStr) > maxFooterChars) {
    footerStr[maxFooterChars] = L'\0';
  }
  UiGfxDrawUnicodeAt(x + 15, footerY, footerStr, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);

  // 8. Draw Scrollbar track & slider
  if (Panel->FileCount > pageSize) {
    UINTN scrollTrackX = x + w - thickness - 6;
    UINTN scrollTrackY = y + headerH + thickness + cellH + 5;
    UINTN scrollTrackH = h - headerH - footerH - 2 * thickness - cellH - 10;
    
    // Draw scroll track background
    UiGfxFillRectRgb(scrollTrackX, scrollTrackY, 4, scrollTrackH, borderR / 4, borderG / 4, borderB / 4);

    // Compute slider size & position
    UINTN sliderH = (scrollTrackH * pageSize) / Panel->FileCount;
    if (sliderH < 10) sliderH = 10;

    UINTN maxTopIndex = Panel->FileCount - pageSize;
    UINTN sliderY = scrollTrackY + ((Panel->TopIndex * (scrollTrackH - sliderH)) / maxTopIndex);

    // Draw slider
    UiGfxFillRectRgb(scrollTrackX, sliderY, 4, sliderH, borderR, borderG, borderB);
  }
}

static VOID DrawAsciiAtScaleRatio(UINTN x, UINTN y, CONST CHAR8* str,
                                  UINT8 r, UINT8 g, UINT8 b,
                                  UINTN numerator, UINTN denominator)
{
  UINTN glyphW, glyphH;
  UiGfxGetGlyphSize(&glyphW, &glyphH);

  UINTN stepW = glyphW * numerator / denominator;
  if (stepW == 0) stepW = 1;
  while (*str) {
    UiGfxDrawGlyphScaledRatio((UINT32)(UINT8)*str, x, y, r, g, b,
                              numerator, denominator);
    x += stepW;
    str++;
  }
}

VOID GuiDrawPanels(
  IN PANEL* LeftPanel,
  IN PANEL* RightPanel,
  IN BOOLEAN LeftActive
) {
  UINTN width, height;
  UiGfxGetDimensions(&width, &height);

  if (width < 200 || height < 100) return;

  UINTN cellW, cellH;
  UiGfxGetCellSize(&cellW, &cellH);
  UINTN pH = GuiPanelHeight();

  // Clear the entire screen background to Black to eliminate all leftovers from the UEFI shell
  UiGfxFillRectRgb(0, 0, width, height, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B);

  // Layout boundaries
  UINTN pW = (width - 30) / 2;

  // Left panel: x=10, y=10
  GuiDrawOnePanel(LeftPanel, 10, 10, pW, pH, LeftActive);

  // Right panel: x=10+pW+10, y=10
  GuiDrawOnePanel(RightPanel, 10 + pW + 10, 10, pW, pH, !LeftActive);
}

#define QUICK_VIEW_BYTES 32768
#define QUICK_VIEW_NAMES 24
#define QUICK_VIEW_NAME_CHARS 96

static CHAR16 gQuickViewPath[MAX_PATH_LEN] = { 0 };
static UINT8 gQuickViewData[QUICK_VIEW_BYTES];
static UINTN gQuickViewBytes = 0;
static UINT64 gQuickViewTotal = 0;
static BOOLEAN gQuickViewDirectory = FALSE;
static EFI_STATUS gQuickViewStatus = EFI_NOT_READY;
static UINTN gQuickViewFiles = 0;
static UINTN gQuickViewDirectories = 0;
static UINT64 gQuickViewImmediateSize = 0;
static UINTN gQuickViewNameCount = 0;
static CHAR16 gQuickViewNames[QUICK_VIEW_NAMES][QUICK_VIEW_NAME_CHARS];
static UINT64 gQuickViewItemSize = 0;
static EFI_TIME gQuickViewItemTime;
static BOOLEAN gQuickViewItemDirectory = FALSE;

VOID GuiQuickViewReset(VOID)
{
  gQuickViewPath[0] = L'\0';
  gQuickViewBytes = 0;
  gQuickViewTotal = 0;
  gQuickViewStatus = EFI_NOT_READY;
  gQuickViewNameCount = 0;
  gQuickViewItemSize = 0;
  ZeroMem(&gQuickViewItemTime, sizeof(gQuickViewItemTime));
  gQuickViewItemDirectory = FALSE;
}

static VOID GuiLoadQuickView(IN PANEL* Panel)
{
  CHAR16 path[MAX_PATH_LEN];
  FS_FILE_ITEM* item;

  if (Panel == NULL || Panel->Files == NULL || Panel->SelectedIndex < 0 ||
      (UINTN)Panel->SelectedIndex >= Panel->FileCount) {
    GuiQuickViewReset();
    return;
  }
  item = &Panel->Files[Panel->SelectedIndex];
  if (StrCmp(item->Name, L"..") == 0) {
    GuiQuickViewReset();
    return;
  }
  FsCombinePath(path, Panel->Path, item->Name);
  if (StrCmp(path, gQuickViewPath) == 0 && item->Size == gQuickViewItemSize &&
      item->IsDirectory == gQuickViewItemDirectory &&
      CompareMem(&item->ModificationTime, &gQuickViewItemTime, sizeof(EFI_TIME)) == 0) return;

  GuiQuickViewReset();
  StrCpyS(gQuickViewPath, ARRAY_SIZE(gQuickViewPath), path);
  gQuickViewItemSize = item->Size;
  CopyMem(&gQuickViewItemTime, &item->ModificationTime, sizeof(EFI_TIME));
  gQuickViewItemDirectory = item->IsDirectory;
  gQuickViewDirectory = item->IsDirectory;
  if (!item->IsDirectory) {
    gQuickViewStatus = FsReadFilePrefix(path, gQuickViewData, sizeof(gQuickViewData),
                                        &gQuickViewBytes, &gQuickViewTotal);
    return;
  }

  {
    FS_FILE_ITEM* files = NULL;
    UINTN count = 0;
    gQuickViewFiles = 0;
    gQuickViewDirectories = 0;
    gQuickViewImmediateSize = 0;
    gQuickViewStatus = FsListDirectory(path, &files, &count);
    if (EFI_ERROR(gQuickViewStatus)) return;
    for (UINTN i = 0; i < count; i++) {
      if (StrCmp(files[i].Name, L"..") == 0) continue;
      if (files[i].IsDirectory) gQuickViewDirectories++;
      else {
        gQuickViewFiles++;
        gQuickViewImmediateSize += files[i].Size;
      }
      if (gQuickViewNameCount < QUICK_VIEW_NAMES) {
        StrnCpyS(gQuickViewNames[gQuickViewNameCount], QUICK_VIEW_NAME_CHARS,
                 files[i].Name, QUICK_VIEW_NAME_CHARS - 1);
        gQuickViewNameCount++;
      }
    }
    gQuickViewTotal = gQuickViewImmediateSize;
    if (files != NULL) FreePool(files);
  }
}

static BOOLEAN GuiQuickViewIsBinary(VOID)
{
  UINTN controls = 0;
  UINTN inspect = gQuickViewBytes < 512 ? gQuickViewBytes : 512;
  for (UINTN i = 0; i < inspect; i++) {
    UINT8 ch = gQuickViewData[i];
    if (ch == 0) return TRUE;
    if (ch < 32 && ch != '\r' && ch != '\n' && ch != '\t') controls++;
  }
  return inspect > 0 && controls * 8 > inspect;
}

VOID GuiDrawQuickView(IN PANEL* SourcePanel, IN BOOLEAN DrawOnLeft)
{
  UINTN width, height;
  UINTN cellW, cellH;
  UINTN pW, pH, x, y = 10;
  UINTN contentX, contentY, maxChars, rows;
  CHAR16 line[256];

  UiGfxGetDimensions(&width, &height);
  UiGfxGetCellSize(&cellW, &cellH);
  if (width < 200 || height < 100 || cellW == 0 || cellH == 0) return;
  GuiLoadQuickView(SourcePanel);
  pW = (width - 30) / 2;
  pH = GuiPanelHeight();
  x = DrawOnLeft ? 10 : 10 + pW + 10;
  contentX = x + 15;
  contentY = y + cellH * 3;
  maxChars = GuiCharsForWidth(pW - 30, cellW);
  rows = pH > cellH * 5 ? (pH - cellH * 5) / cellH : 1;

  UiGfxFillRectRgb(x, y, pW, pH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
  DrawBorder(x, y, pW, pH, COLOR_GRAY_R, COLOR_GRAY_G, COLOR_GRAY_B, 1);
  UiGfxFillRectRgb(x, y + cellH * 2, pW, 1, COLOR_GRAY_R, COLOR_GRAY_G, COLOR_GRAY_B);
  GuiDrawAsciiClippedAt(contentX, y + (cellH > 8 ? (cellH - 8) / 2 : 0), "Quick View", maxChars,
                        COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);

  if (gQuickViewPath[0] == L'\0') {
    GuiDrawUnicodeClippedAt(contentX, contentY, L"No preview for this item.", maxChars,
                            COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
  } else {
    GuiDrawUnicodeClippedAt(contentX, y + cellH, gQuickViewPath, maxChars,
                            COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
    if (EFI_ERROR(gQuickViewStatus)) {
      UnicodeSPrint(line, sizeof(line), L"Read error: %r", gQuickViewStatus);
      GuiDrawUnicodeClippedAt(contentX, contentY, line, maxChars,
                              COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
    } else if (gQuickViewDirectory) {
      CHAR16 sizeText[32];
      FormatFileSize(gQuickViewImmediateSize, sizeText, ARRAY_SIZE(sizeText));
      UnicodeSPrint(line, sizeof(line), L"%d directories, %d files, immediate size %s",
                    (UINT32)gQuickViewDirectories, (UINT32)gQuickViewFiles, sizeText);
      GuiDrawUnicodeClippedAt(contentX, contentY, line, maxChars,
                              COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
      for (UINTN row = 1; row < rows && row - 1 < gQuickViewNameCount; row++) {
        GuiDrawUnicodeClippedAt(contentX, contentY + row * cellH,
                                gQuickViewNames[row - 1], maxChars,
                                COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
      }
    } else if (GuiQuickViewIsBinary()) {
      UINTN offset = 0;
      for (UINTN row = 0; row < rows && offset < gQuickViewBytes; row++, offset += 16) {
        CHAR8 hexLine[96];
        UINTN pos = (UINTN)AsciiSPrint(hexLine, sizeof(hexLine), "%08x  ", (UINT32)offset);
        for (UINTN i = 0; i < 16 && offset + i < gQuickViewBytes && pos + 4 < sizeof(hexLine); i++) {
          pos += (UINTN)AsciiSPrint(&hexLine[pos], sizeof(hexLine) - pos, "%02x ", gQuickViewData[offset + i]);
        }
        GuiDrawAsciiClippedAt(contentX, contentY + row * cellH, hexLine, maxChars,
                              COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
      }
    } else {
      UINTN at = 0;
      for (UINTN row = 0; row < rows && at < gQuickViewBytes; row++) {
        CHAR8 textLine[256];
        UINTN out = 0;
        while (at < gQuickViewBytes && gQuickViewData[at] != '\r' && gQuickViewData[at] != '\n' &&
               out + 1 < sizeof(textLine) && out < maxChars) {
          UINT8 ch = gQuickViewData[at++];
          textLine[out++] = (ch == '\t') ? ' ' : ((ch >= 32 && ch < 127) ? (CHAR8)ch : '.');
        }
        textLine[out] = '\0';
        while (at < gQuickViewBytes && (gQuickViewData[at] == '\r' || gQuickViewData[at] == '\n')) at++;
        GuiDrawAsciiClippedAt(contentX, contentY + row * cellH, textLine, maxChars,
                              COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
      }
    }
  }

  UnicodeSPrint(line, sizeof(line), L"%ld bytes | Ctrl+Q closes", gQuickViewTotal);
  GuiDrawUnicodeClippedAt(contentX, y + pH - cellH - 4, line, maxChars,
                          COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
}

/*
 * Compact Far-style function-key legend.  Keeping each key and action on one
 * line gives the panels back a full text row and makes the legend read as a
 * command strip instead of a second panel footer.  EC keeps its red function
 * keys; the action names use the same cyan as the rest of the interactive UI.
 */

#define MENU_ITEM_COUNT 10

/*
 * Manual function-bar tuning. MENU_FONT_SCALE_PERCENT may be changed in small
 * steps (for example 115, 120, 125) instead of being limited to integer 1x/2x
 * glyph sizes. MENU_PANEL_GAP is the empty space between the panel border and
 * the black command strip; 6 px is 40% less than the previous 10 px.
 */
#define MENU_FONT_SCALE_PERCENT 120
#define MENU_PANEL_GAP 6
#define MENU_VERTICAL_PADDING 6
#define MENU_TEXT_TOP_PADDING 2
#define MENU_KEY_PAD_X 2
#define MENU_KEY_PAD_Y 1
#define MENU_KEY_LABEL_GAP 1

typedef struct {
  CHAR8* Key;
  CHAR8* Label;
} MENU_ITEM;

static CONST MENU_ITEM gMenuItems[MENU_ITEM_COUNT] = {
  { "F1",  "Help"    },
  { "F2",  "Drive"   },
  { "F3",  "View"    },
  { "F4",  "Edit"    },
  { "F5",  "Copy"    },
  { "F6",  "RenMov"  },
  { "F7",  "MkDir"   },
  { "F8",  "Delete"  },
  { "F9",  "Menu"    },
  { "F10", "Quit"    }
};

static UINTN GuiMenuTotalChars(VOID)
{
  UINTN total = 0;
  UINTN i;

  for (i = 0; i < MENU_ITEM_COUNT; i++) {
    total += AsciiStrLen(gMenuItems[i].Key);
    total += AsciiStrLen(gMenuItems[i].Label);
  }
  return total;
}

static UINTN GuiMenuScaledPixels(IN UINTN Pixels)
{
  UINTN Result = Pixels * MENU_FONT_SCALE_PERCENT / 100;
  return Result == 0 ? 1 : Result;
}

UINTN GuiMenuBarHeight(VOID)
{
  UINTN glyphW, glyphH;

  UiGfxGetGlyphSize(&glyphW, &glyphH);
  (VOID)glyphW;

  return GuiMenuScaledPixels(glyphH) + MENU_VERTICAL_PADDING;
}

UINTN GuiPanelHeight(VOID)
{
  UINTN width, height;
  UINTN menuY;

  UiGfxGetDimensions(&width, &height);
  (VOID)width;
  menuY = height - GuiMenuBarHeight();
  if (menuY <= 10 + MENU_PANEL_GAP) return 1;
  return menuY - 10 - MENU_PANEL_GAP;
}

VOID GuiDrawBottomMenu(VOID)
{
  UINTN width, height;
  UINTN glyphW, glyphH;
  UINTN total, used, gap, x;
  UINTN menuH, menuY;
  UINTN i;

  UiGfxGetDimensions(&width, &height);
  UiGfxGetGlyphSize(&glyphW, &glyphH);

  total = GuiMenuTotalChars();
  used = total * GuiMenuScaledPixels(glyphW) +
         MENU_ITEM_COUNT * (MENU_KEY_PAD_X * 2 + MENU_KEY_LABEL_GAP);
  gap = (width > used) ? (width - used) / (MENU_ITEM_COUNT + 1) : 0;

  menuH = GuiMenuBarHeight();
  menuY = height - menuH;
  UiGfxFillRectRgb(0, menuY, width, menuH, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B);

  x = gap;
  for (i = 0; i < MENU_ITEM_COUNT; i++) {
    UINTN keyLen = AsciiStrLen(gMenuItems[i].Key);
    UINTN labelLen = AsciiStrLen(gMenuItems[i].Label);
    UINTN scaledGlyphW = GuiMenuScaledPixels(glyphW);
    UINTN scaledGlyphH = GuiMenuScaledPixels(glyphH);
    UINTN keyW = keyLen * scaledGlyphW;
    UINTN keyBoxW = keyW + MENU_KEY_PAD_X * 2;
    UINTN keyBoxH = scaledGlyphH + MENU_KEY_PAD_Y * 2;
    UINTN labelW = labelLen * scaledGlyphW;
    UINTN cellW = keyBoxW + MENU_KEY_LABEL_GAP + labelW;
    UINTN textY = menuY + MENU_TEXT_TOP_PADDING;

    UiGfxFillRectRgb(x, textY, keyBoxW, keyBoxH,
                     COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    DrawAsciiAtScaleRatio(x + MENU_KEY_PAD_X, textY + MENU_KEY_PAD_Y, gMenuItems[i].Key,
                          COLOR_RED_R, COLOR_RED_G, COLOR_RED_B,
                          MENU_FONT_SCALE_PERCENT, 100);
    DrawAsciiAtScaleRatio(x + keyBoxW + MENU_KEY_LABEL_GAP,
                          textY + MENU_KEY_PAD_Y, gMenuItems[i].Label,
                          COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B,
                          MENU_FONT_SCALE_PERCENT, 100);

    x += cellW + gap;
  }
}

VOID GuiDrawCopyProgress(
  IN CONST CHAR16* SrcPath,
  IN CONST CHAR16* DstPath,
  IN UINT64 BytesCopied,
  IN UINT64 TotalBytes
) {
  UINTN width, height;
  UiGfxGetDimensions(&width, &height);
  UINTN cellW, cellH;
  UiGfxGetCellSize(&cellW, &cellH);

  // Progress box dimensions
  UINTN boxW = GuiDialogWidth(700);
  UINTN boxH = cellH * 8;
  UINTN boxX = (width - boxW) / 2;
  UINTN boxY = (height - boxH) / 2;

  // Draw background & border
  UiGfxFillRectRgb(boxX, boxY, boxW, boxH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
  DrawBorder(boxX, boxY, boxW, boxH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B, 3);

  // Title
  UiGfxDrawAsciiAt(boxX + 20, boxY + 15, "Copying File...", COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

  // Format Src/Dst names
  CHAR16 srcName[128] = { 0 };
  CHAR16 dstName[128] = { 0 };
  StrCpyS(srcName, 128, SrcPath);
  StrCpyS(dstName, 128, DstPath);

  /*
   * The path does not start at the left edge of the box - it starts after the
   * 25 px inset AND the six cells of the "From:"/"To:  " label. The character
   * budget therefore has to be measured from where the text actually begins to
   * the inner right edge, not from the box width.
   *
   * "boxW - 100" ignored that offset: with the usual 700 px box and a 16 px
   * cell (8x16 font at scale 2) it allowed 37 characters starting at +121 px,
   * so the field ended 13 px PAST the right border. A path that filled the last
   * column - any 7-character name ending in a letter, e.g.
   * fs2:\Windows\System32\drivers\afd.sys - drew its final glyph outside the
   * box. The per-frame erase is exactly the box rectangle, so those columns were
   * never repainted and the glyph stayed on screen as a ghost over every later
   * file, hanging past the yellow frame.
   */
  UINTN textX      = boxX + 25 + cellW * 6;
  UINTN innerRight = boxX + boxW - 25;
  UINTN maxChars   = (innerRight > textX) ? GuiCharsForWidth(innerRight - textX, cellW) : 0;

  UiGfxDrawAsciiAt(boxX + 25, boxY + 15 + cellH, "From:", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  GuiDrawUnicodeClippedAt(textX, boxY + 15 + cellH, srcName, maxChars, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

  UiGfxDrawAsciiAt(boxX + 25, boxY + 15 + cellH * 2, "To:  ", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  GuiDrawUnicodeClippedAt(textX, boxY + 15 + cellH * 2, dstName, maxChars, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

  // Draw progress bar outline
  UINTN barX = boxX + 25;
  UINTN barY = boxY + 15 + cellH * 4;
  UINTN barW = boxW - 50;
  UINTN barH = cellH;
  DrawBorder(barX, barY, barW, barH, COLOR_GRAY_R, COLOR_GRAY_G, COLOR_GRAY_B, 1);

  // Draw filled progress bar
  UINTN fillW = 0;
  if (TotalBytes > 0) {
    fillW = (barW * BytesCopied) / TotalBytes;
  }
  // fillW can be 1..3 for a large file early in the copy (e.g. 256 KB of
  // 93 MB -> fillW==1). "fillW - 4" would then underflow UINTN to a huge
  // width and corrupt memory past the framebuffer. Only draw once the fill
  // is wide enough to inset the 2px border on each side.
  if (fillW > 4) {
    UiGfxFillRectRgb(barX + 2, barY + 2, fillW - 4, barH - 4, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
  }

  // Draw status text below bar
  CHAR16 statusStr[128] = { 0 };
  CHAR16 copSize[32] = { 0 };
  CHAR16 totSize[32] = { 0 };
  FormatFileSize(BytesCopied, copSize, 32);
  FormatFileSize(TotalBytes, totSize, 32);
  UINTN percent = TotalBytes > 0 ? (UINTN)((BytesCopied * 100) / TotalBytes) : 0;
  UnicodeSPrint(statusStr, sizeof(statusStr), L"%d%% (%s of %s)", percent, copSize, totSize);

  UiGfxDrawUnicodeAt(boxX + 25, boxY + 15 + cellH * 5 + 5, statusStr, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);

  // Flush to screen
  UiGfxFlush();
}

BOOLEAN GuiDrawInputBox(
  IN  CONST CHAR16* Title,
  IN  CONST CHAR16* Prompt,
  OUT CHAR16* Buffer,
  IN  UINTN BufferSize
) {
  if (Buffer == NULL || BufferSize == 0) return FALSE;

  UINTN width, height;
  UiGfxGetDimensions(&width, &height);
  UINTN cellW, cellH;
  UiGfxGetCellSize(&cellW, &cellH);

  UINTN boxW = GuiDialogWidth(600);
  UINTN boxH = cellH * 8;
  UINTN boxX = (width - boxW) / 2;
  UINTN boxY = (height - boxH) / 2;

  UINTN cursorIndex = StrLen(Buffer);

  // Interactive input loop
  while (TRUE) {
    // 1. Draw Dialog Background
    UiGfxFillRectRgb(boxX, boxY, boxW, boxH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
    DrawBorder(boxX, boxY, boxW, boxH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B, 3);

    // Draw Title
    UiGfxDrawUnicodeAt(boxX + 20, boxY + 15, Title, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

    // Draw Prompt
    GuiDrawUnicodeClippedAt(boxX + 25, boxY + 15 + cellH * 2, Prompt, GuiCharsForWidth(boxW - 50, cellW), COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

    // Draw Input Field Box
    UINTN fieldX = boxX + 25;
    UINTN fieldY = boxY + 15 + cellH * 3 + 5;
    UINTN fieldW = boxW - 50;
    UINTN fieldH = cellH + 8;
    UiGfxFillRectRgb(fieldX, fieldY, fieldW, fieldH, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B);
    DrawBorder(fieldX, fieldY, fieldW, fieldH, COLOR_GRAY_R, COLOR_GRAY_G, COLOR_GRAY_B, 1);

    // Draw Text in Field Box
    GuiDrawUnicodeClippedAt(fieldX + 5, fieldY + 4, Buffer, GuiCharsForWidth(fieldW - 10, cellW), COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

    // Draw Cursor
    UINTN visibleCursorIndex = cursorIndex;
    UINTN maxFieldChars = GuiCharsForWidth(fieldW - 10, cellW);
    if (visibleCursorIndex > maxFieldChars) visibleCursorIndex = maxFieldChars;
    UINTN cursorX = fieldX + 5 + visibleCursorIndex * cellW;
    UiGfxFillRectRgb(cursorX, fieldY + 4, 3, cellH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

    // Draw Buttons Hint
    UiGfxDrawAsciiAt(boxX + 25, boxY + boxH - cellH - 15, "[ Enter: Ok ]   [ ESC: Cancel ]", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);

    // Flush frame to GOP
    UiGfxFlush();

    // Read Key
    EFI_INPUT_KEY key;
    UINTN index;
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
    EFI_STATUS status = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
    if (EFI_ERROR(status)) continue;

    if (key.ScanCode == SCAN_ESC) {
      return FALSE;
    }

    if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      return TRUE;
    }

    if (key.UnicodeChar == CHAR_BACKSPACE) {
      if (cursorIndex > 0) {
        cursorIndex--;
        Buffer[cursorIndex] = L'\0';
      }
    } else if (key.UnicodeChar >= 32 && key.UnicodeChar < 127) {
      if (cursorIndex + 1 < BufferSize) {
        Buffer[cursorIndex] = key.UnicodeChar;
        cursorIndex++;
        Buffer[cursorIndex] = L'\0';
      }
    }
  }
}

VOID GuiDrawMsgBox(
  IN CONST CHAR16* Title,
  IN CONST CHAR16* Message
) {
  UINTN width, height;
  UiGfxGetDimensions(&width, &height);
  UINTN cellW, cellH;
  UiGfxGetCellSize(&cellW, &cellH);

  UINTN boxW = GuiDialogWidth(550);
  UINTN boxH = cellH * 8;
  UINTN boxX = (width - boxW) / 2;
  UINTN boxY = (height - boxH) / 2;

  // Draw background and border
  UiGfxFillRectRgb(boxX, boxY, boxW, boxH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
  DrawBorder(boxX, boxY, boxW, boxH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B, 3);

  // Title
  GuiDrawUnicodeClippedAt(boxX + 20, boxY + 15, Title, GuiCharsForWidth(boxW - 40, cellW), COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

  // Message
  GuiDrawUnicodeWrappedAt(boxX + 25, boxY + 15 + cellH * 2, Message, GuiCharsForWidth(boxW - 50, cellW), 3, cellH, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

  // Enter ok hint
  UiGfxDrawAsciiAt(boxX + 25, boxY + boxH - cellH - 15, "Press any key to close...", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);

  UiGfxFlush();

  // Wait for key
  UINTN index;
  EFI_INPUT_KEY key;
  gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
  gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
}

BOOLEAN GuiDrawListPicker(
  IN  CONST CHAR16* Title,
  IN  CONST CHAR16** Lines,
  IN  UINTN LineCount,
  OUT UINTN* Chosen
) {
  UINTN width, height;
  UINTN cellW, cellH;
  UINTN boxW, boxH, boxX, boxY;
  UINTN topLine = 0;
  UINTN selectedLine;

  if (Lines == NULL || LineCount == 0 || Chosen == NULL) return FALSE;
  selectedLine = (*Chosen < LineCount) ? *Chosen : 0;

  UiGfxGetDimensions(&width, &height);
  UiGfxGetCellSize(&cellW, &cellH);

  boxW = GuiDialogWidth(1100);
  boxH = GuiDialogHeight(21);
  boxX = (width - boxW) / 2;
  boxY = (height - boxH) / 2;

  for (;;) {
    UINTN titleY = boxY + 15;
    UINTN listX = boxX + 25;
    UINTN listY = boxY + 15 + cellH * 2;
    UINTN hintY = boxY + boxH - cellH - 15;
    UINTN listH = (hintY > listY) ? (hintY - listY - 4) : cellH;
    UINTN visibleLines = listH / cellH;
    UINTN maxChars = GuiCharsForWidth(boxW - 70, cellW);
    UINTN row;

    if (visibleLines == 0) visibleLines = 1;
    if (selectedLine >= LineCount) selectedLine = LineCount - 1;
    if (selectedLine < topLine) topLine = selectedLine;
    if (selectedLine >= topLine + visibleLines) {
      topLine = selectedLine - visibleLines + 1;
    }
    if (LineCount > visibleLines && topLine > LineCount - visibleLines) {
      topLine = LineCount - visibleLines;
    } else if (LineCount <= visibleLines) {
      topLine = 0;
    }

    UiGfxFillRectRgb(boxX, boxY, boxW, boxH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
    DrawBorder(boxX, boxY, boxW, boxH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B, 3);
    GuiDrawUnicodeClippedAt(boxX + 20, titleY, Title, maxChars,
                            COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

    for (row = 0; row < visibleLines; row++) {
      UINTN idx = topLine + row;
      UINTN rowY = listY + row * cellH;
      if (idx >= LineCount) break;
      if (idx == selectedLine) {
        UiGfxFillRectRgb(listX - 4, rowY, boxW - 56, cellH,
                         COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
        GuiDrawUnicodeClippedAt(listX, rowY, Lines[idx], maxChars,
                                COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B);
      } else {
        GuiDrawUnicodeClippedAt(listX, rowY, Lines[idx], maxChars,
                                COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
      }
    }

    if (LineCount > visibleLines) {
      UINTN trackX = boxX + boxW - 14;
      UINTN trackY = listY;
      UINTN trackH = visibleLines * cellH;
      UINTN sliderH = (trackH * visibleLines) / LineCount;
      UINTN maxTop = LineCount - visibleLines;
      UINTN sliderY;
      if (sliderH < 10) sliderH = 10;
      if (sliderH > trackH) sliderH = trackH;
      sliderY = trackY + ((topLine * (trackH - sliderH)) / maxTop);
      UiGfxFillRectRgb(trackX, trackY, 4, trackH, COLOR_GRAY_R / 3, COLOR_GRAY_G / 3, COLOR_GRAY_B / 3);
      UiGfxFillRectRgb(trackX, sliderY, 4, sliderH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
    }

    UiGfxDrawAsciiAt(boxX + 25, hintY, "Enter goes there, Esc closes",
                     COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    UiGfxFlush();

    {
      UINTN index;
      EFI_INPUT_KEY key;
      gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
      if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) continue;

      if (key.ScanCode == SCAN_UP) {
        if (selectedLine > 0) selectedLine--;
        continue;
      }
      if (key.ScanCode == SCAN_DOWN) {
        if (selectedLine + 1 < LineCount) selectedLine++;
        continue;
      }
      if (key.ScanCode == SCAN_PAGE_UP) {
        selectedLine = (selectedLine > visibleLines) ? selectedLine - visibleLines : 0;
        continue;
      }
      if (key.ScanCode == SCAN_PAGE_DOWN) {
        selectedLine += visibleLines;
        if (selectedLine >= LineCount) selectedLine = LineCount - 1;
        continue;
      }
      if (key.ScanCode == SCAN_HOME) { selectedLine = 0; continue; }
      if (key.ScanCode == SCAN_END)  { selectedLine = LineCount - 1; continue; }
      if (key.ScanCode == SCAN_ESC)  { return FALSE; }
      if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
        *Chosen = selectedLine;
        return TRUE;
      }
    }
  }
}

VOID GuiDrawSearchProgress(
  IN CONST CHAR16* Root,
  IN CONST CHAR16* Mask,
  IN UINTN DirsVisited
) {
  UINTN width, height;
  UINTN cellW, cellH;
  UINTN boxW, boxH, boxX, boxY;
  CHAR16 line[MAX_PATH_LEN];

  UiGfxGetDimensions(&width, &height);
  UiGfxGetCellSize(&cellW, &cellH);

  boxW = GuiDialogWidth(760);
  boxH = cellH * 6;
  boxX = (width - boxW) / 2;
  boxY = (height - boxH) / 2;

  UiGfxFillRectRgb(boxX, boxY, boxW, boxH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
  DrawBorder(boxX, boxY, boxW, boxH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B, 3);
  UiGfxDrawAsciiAt(boxX + 20, boxY + 12, "Searching", COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

  UnicodeSPrint(line, sizeof(line), L"%s  for  %s", Root, Mask);
  GuiDrawUnicodeClippedAt(boxX + 20, boxY + 12 + cellH * 2, line,
                          GuiCharsForWidth(boxW - 40, cellW),
                          COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

  if (DirsVisited > 0) {
    UnicodeSPrint(line, sizeof(line), L"%d directories", (UINT32)DirsVisited);
    GuiDrawUnicodeClippedAt(boxX + 20, boxY + 12 + cellH * 3, line,
                            GuiCharsForWidth(boxW - 40, cellW),
                            COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
  }

  UiGfxDrawAsciiAt(boxX + 20, boxY + boxH - cellH - 12, "Esc cancels",
                   COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  UiGfxFlush();
}

VOID GuiDrawTreeProgress(
  IN CONST CHAR16* Title,
  IN CONST CHAR16* CurrentPath,
  IN UINTN Files,
  IN UINTN Directories
) {
  UINTN width, height;
  UINTN cellW, cellH;
  UINTN boxW, boxH, boxX, boxY;
  UINTN maxChars;
  CHAR16 line[MAX_PATH_LEN];

  UiGfxGetDimensions(&width, &height);
  UiGfxGetCellSize(&cellW, &cellH);

  boxW = GuiDialogWidth(760);
  boxH = cellH * 7;
  boxX = (width - boxW) / 2;
  boxY = (height - boxH) / 2;
  maxChars = GuiCharsForWidth(boxW - 40, cellW);

  UiGfxFillRectRgb(boxX, boxY, boxW, boxH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
  DrawBorder(boxX, boxY, boxW, boxH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B, 3);
  GuiDrawUnicodeClippedAt(boxX + 20, boxY + 12, Title, maxChars,
                          COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

  // The tail of the path, not its head: the volume and the top directories are
  // the same for every entry, and the part that moves is the one worth showing.
  if (CurrentPath != NULL) {
    CONST CHAR16* shown = CurrentPath;
    UINTN pathChars = StrLen(CurrentPath);
    if (pathChars > maxChars) shown = CurrentPath + (pathChars - maxChars);
    GuiDrawUnicodeClippedAt(boxX + 20, boxY + 12 + cellH * 2, shown, maxChars,
                            COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
  }

  UnicodeSPrint(line, sizeof(line), L"%d files, %d directories",
                (UINT32)Files, (UINT32)Directories);
  GuiDrawUnicodeClippedAt(boxX + 20, boxY + 12 + cellH * 4, line, maxChars,
                          COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);

  UiGfxDrawAsciiAt(boxX + 20, boxY + boxH - cellH - 12, "Esc cancels",
                   COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  UiGfxFlush();
}

VOID GuiDrawHelp(VOID)
{
  UINTN width, height;
  UINTN cellW, cellH;
  UINTN boxW;
  UINTN boxH;
  UINTN boxX;
  UINTN boxY;
  STATIC CONST CHAR16* lines[] = {
    L"F1 Help",
    L"F2 change drive in the active panel",
    L"F3 View",
    L"F4 Edit",
    L"F5 Copy",
    L"F6 Rename/Move",
    L"F7 MkDir",
    L"F8 / Delete removes item(s)",
    L"F9 Menu (drive, find, recursive sync, checksum, settings)",
    L"F10 Quit",
    L"Tab switches the active panel",
    L"Ctrl+Q toggles Quick View in the passive panel",
    L"Up/Down and PgUp/PgDn move through the active panel",
    L"Home / End jump to the first / last item",
    L"Enter opens directories and EFI apps",
    L"Backspace goes to the parent directory",
    L"Alt+F7 recursively finds a file by name or mask",
    L"Alt+F7 also asks for text the file must contain; empty means any",
    L"A content search reads every file the mask lets through, so narrow it",
    L"Esc stops a search, a compare or an update where it stands",
    L"Ctrl+F2 attributes and modification time",
    L"Alt+F1 / Alt+F2 change left/right drive",
    L"Ctrl+F3 sort by name",
    L"Ctrl+F4 sort by extension",
    L"Ctrl+F5 sort by modified date",
    L"Ctrl+F6 sort by size",
    L"Repeat same sort shortcut to toggle asc/desc",
    L"Ctrl+F12 set panel filter; use * to clear",
    L"Alt+Left / Alt+Right navigate path history",
    L"Alt+F10 directory hotlist",
    L"Insert or Space toggles current item",
    L"F5/F6 acts on all selected items, or the item under cursor",
    L"Ctrl+A selects all; Ctrl+U clears selection",
    L"+ select by mask; - unselect by mask",
    L"* inverts selection",
    L"= replaces both selections with panel differences",
    L"Compare uses exact names; files also use size and modified time",
    L"Directories are compared by presence, not by their contents",
    L"F9 recursive compare uses SHA-256 and can update either side",
    L"Recursive update keeps entries found only at the destination",
    L"F9 can calculate SHA-256 and CRC32 for the current file",
    L"Settings can verify every copied file with SHA-256",
    L"F9 can run a selected EFI image with LoadOptions arguments",
    L"F9 UEFI tools shows volumes, drivers and boot entries",
    L"Type letters for quick prefix jump",
    L"/ finds anywhere in name",
    L"N repeats / search",
    L"Press any key to close"
  };
  UINTN lineCount = sizeof(lines) / sizeof(lines[0]);
  UINTN topLine = 0;
  UINTN selectedLine = 0;

  UiGfxGetDimensions(&width, &height);
  UiGfxGetCellSize(&cellW, &cellH);

  boxW = GuiDialogWidth(980);
  boxH = GuiDialogHeight(17);
  boxX = (width - boxW) / 2;
  boxY = (height - boxH) / 2;

  for (;;) {
    UINTN titleY = boxY + 15;
    UINTN listX = boxX + 25;
    UINTN listY = boxY + 15 + cellH * 2;
    UINTN hintY = boxY + boxH - cellH - 15;
    UINTN listH = (hintY > listY) ? (hintY - listY - 4) : cellH;
    UINTN visibleLines = listH / cellH;
    UINTN maxChars = GuiCharsForWidth(boxW - 70, cellW);

    if (visibleLines == 0) visibleLines = 1;
    if (selectedLine >= lineCount) selectedLine = lineCount - 1;
    if (selectedLine < topLine) topLine = selectedLine;
    if (selectedLine >= topLine + visibleLines) {
      topLine = selectedLine - visibleLines + 1;
    }
    if (lineCount > visibleLines && topLine > lineCount - visibleLines) {
      topLine = lineCount - visibleLines;
    } else if (lineCount <= visibleLines) {
      topLine = 0;
    }

    UiGfxFillRectRgb(boxX, boxY, boxW, boxH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
    DrawBorder(boxX, boxY, boxW, boxH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B, 3);
    UiGfxDrawAsciiAt(boxX + 20, titleY, "EC Help", COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

    for (UINTN row = 0; row < visibleLines; row++) {
      UINTN idx = topLine + row;
      UINTN rowY = listY + row * cellH;
      if (idx >= lineCount) break;
      if (idx == selectedLine) {
        UiGfxFillRectRgb(listX - 4, rowY, boxW - 56, cellH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
        GuiDrawUnicodeClippedAt(listX, rowY, lines[idx], maxChars, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B);
      } else {
        GuiDrawUnicodeClippedAt(listX, rowY, lines[idx], maxChars, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
      }
    }

    if (lineCount > visibleLines) {
      UINTN trackX = boxX + boxW - 14;
      UINTN trackY = listY;
      UINTN trackH = visibleLines * cellH;
      UINTN sliderH = (trackH * visibleLines) / lineCount;
      UINTN maxTop = lineCount - visibleLines;
      UINTN sliderY;
      if (sliderH < 10) sliderH = 10;
      if (sliderH > trackH) sliderH = trackH;
      sliderY = trackY + ((topLine * (trackH - sliderH)) / maxTop);
      UiGfxFillRectRgb(trackX, trackY, 4, trackH, COLOR_GRAY_R / 3, COLOR_GRAY_G / 3, COLOR_GRAY_B / 3);
      UiGfxFillRectRgb(trackX, sliderY, 4, sliderH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
    }

    UiGfxDrawAsciiAt(boxX + 25, hintY, "Up/Down/PgUp/PgDn scroll, any other key closes", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);

    UiGfxFlush();

    UINTN index;
    EFI_INPUT_KEY key;
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
    if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) continue;

    if (key.ScanCode == SCAN_UP) {
      if (selectedLine > 0) selectedLine--;
      continue;
    }
    if (key.ScanCode == SCAN_DOWN) {
      if (selectedLine + 1 < lineCount) selectedLine++;
      continue;
    }
    if (key.ScanCode == SCAN_PAGE_UP) {
      if (selectedLine > visibleLines) selectedLine -= visibleLines;
      else selectedLine = 0;
      continue;
    }
    if (key.ScanCode == SCAN_PAGE_DOWN) {
      if (selectedLine + visibleLines < lineCount) selectedLine += visibleLines;
      else selectedLine = lineCount - 1;
      continue;
    }
    return;
  }
}

UINTN GuiDrawConfirmDialog(
  IN CONST CHAR16* Title,
  IN CONST CHAR16* Prompt,
  IN BOOLEAN ShowAllOption
) {
  UINTN width, height;
  UiGfxGetDimensions(&width, &height);
  UINTN cellW, cellH;
  UiGfxGetCellSize(&cellW, &cellH);

  // Compute prompt width
  UINTN promptLen = StrLen(Prompt);
  UINTN boxW = (promptLen + 6) * cellW;
  if (boxW < 380) boxW = 380;
  boxW = GuiDialogWidth(boxW);

  UINTN boxH = cellH * 8;
  UINTN boxX = (width - boxW) / 2;
  UINTN boxY = (height - boxH) / 2;

  // Draw background and border
  UiGfxFillRectRgb(boxX, boxY, boxW, boxH, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
  DrawBorder(boxX, boxY, boxW, boxH, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B, 3);

  // Title
  GuiDrawUnicodeClippedAt(boxX + 20, boxY + 15, Title, GuiCharsForWidth(boxW - 40, cellW), COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);

  // Prompt Message
  GuiDrawUnicodeWrappedAt(boxX + 25, boxY + 15 + cellH * 2, Prompt, GuiCharsForWidth(boxW - 50, cellW), 2, cellH, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

  // Draw Buttons Hints
  if (ShowAllOption) {
    UiGfxDrawAsciiAt(boxX + 25, boxY + boxH - cellH - 15, "[Y] Yes  [N] No  [A] All  [S] Skip all  [C] Cancel", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  } else {
    UiGfxDrawAsciiAt(boxX + 25, boxY + boxH - cellH - 15, "[Y] Yes  [N] No  [C] Cancel", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  }

  UiGfxFlush();

  // Modal key loop
  while (TRUE) {
    UINTN keyIndex;
    EFI_INPUT_KEY keyStroke;
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &keyIndex);
    EFI_STATUS keyStatus = gST->ConIn->ReadKeyStroke(gST->ConIn, &keyStroke);
    if (EFI_ERROR(keyStatus)) continue;

    if (keyStroke.ScanCode != 0) {
      if (keyStroke.ScanCode == SCAN_ESC) {
        return 0; // Cancel
      }
    } else {
      CHAR16 uc = keyStroke.UnicodeChar;
      if (uc == 27 || uc == L'c' || uc == L'C') {
        return 0; // Cancel
      }
      if (uc == L'y' || uc == L'Y') {
        return 1; // Yes
      }
      if (uc == L'n' || uc == L'N') {
        return 2; // No
      }
      if ((uc == L'a' || uc == L'A') && ShowAllOption) {
        return 3; // Yes to All
      }
      if ((uc == L's' || uc == L'S') && ShowAllOption) {
        return 4; // No/Skip to All
      }
    }
  }
}
