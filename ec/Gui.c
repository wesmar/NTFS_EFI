// Gui.c — GUI drawing functions for panels, menus, dialogs, and progress bars.
#include "Gui.h"
#include "UiConsole.h"
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

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

#define COLOR_GRAY_R 170
#define COLOR_GRAY_G 170
#define COLOR_GRAY_B 170

#define COLOR_BLACK_R 0
#define COLOR_BLACK_G 0
#define COLOR_BLACK_B 0

VOID DrawBorder(UINTN x, UINTN y, UINTN w, UINTN h, UINT8 r, UINT8 g, UINT8 b, UINTN thickness)
{
  UiGfxFillRectRgb(x, y, w, thickness, r, g, b); // Top
  UiGfxFillRectRgb(x, y + h - thickness, w, thickness, r, g, b); // Bottom
  UiGfxFillRectRgb(x, y, thickness, h, r, g, b); // Left
  UiGfxFillRectRgb(x + w - thickness, y, thickness, h, r, g, b); // Right
}

static UINTN GuiCharsForWidth(IN UINTN WidthPixels, IN UINTN CellW)
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

static VOID GuiDrawUnicodeClippedAt(
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

  UINTN headerH = cellH * 2;
  UINTN columnHeaderH = cellH;
  UINTN footerH = cellH * 1;
  UINTN listTopPad = 4;
  UINTN listBottomPad = 8;
  if (PanelHeight <= headerH + columnHeaderH + footerH + cellH + listTopPad + listBottomPad) return 1;
  return (PanelHeight - headerH - columnHeaderH - footerH - listTopPad - listBottomPad) / cellH;
}

static VOID FormatFileSize(UINT64 Size, CHAR16* Buffer, UINTN BufferSize)
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

  UINTN headerH = cellH * 2;
  UINTN footerH = cellH;
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
  if (Panel->Path[0] != L'\0') {
    FS_VOLUME* vol = NULL;
    UINTN colIdx = 0;
    while (Panel->Path[colIdx] != L'\0' && Panel->Path[colIdx] != L':') colIdx++;
    if (Panel->Path[colIdx] == L':') {
      for (UINTN v = 0; v < gVolumeCount; v++) {
        if (StrnCmp(gVolumes[v].Name, Panel->Path, colIdx + 1) == 0) {
          vol = &gVolumes[v];
          break;
        }
      }
    }

    if (vol != NULL) {
      UINT64 totalSize = 0, freeSize = 0;
      CHAR16 label[32] = { 0 };
      if (!EFI_ERROR(FsGetVolumeInfo(vol, &totalSize, &freeSize, label, 32))) {
        CHAR16 freeStr[32] = { 0 };
        CHAR16 totalStr[32] = { 0 };
        FormatFileSize(freeSize, freeStr, 32);
        FormatFileSize(totalSize, totalStr, 32);
        UnicodeSPrint(volInfoStr, sizeof(volInfoStr), L" | Free: %s/%s (%s)", freeStr, totalStr, label);
      }
    }
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

static VOID DrawAsciiAtScale(UINTN x, UINTN y, CONST CHAR8* str, UINT8 r, UINT8 g, UINT8 b, UINTN scale)
{
  UINTN glyphW, glyphH;
  UiGfxGetGlyphSize(&glyphW, &glyphH);
  
  UINTN stepW = glyphW * scale;
  while (*str) {
    UiGfxDrawGlyphScaled((UINT32)(UINT8)*str, x, y, r, g, b, scale);
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
  UINTN menuH = cellH + 15;
  UINTN menuY = height - menuH;
  UINTN pH = menuY - 20;

  // Clear the entire screen background to Black to eliminate all leftovers from the UEFI shell
  UiGfxFillRectRgb(0, 0, width, height, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B);

  // Layout boundaries
  UINTN pW = (width - 30) / 2;

  // Left panel: x=10, y=10
  GuiDrawOnePanel(LeftPanel, 10, 10, pW, pH, LeftActive);

  // Right panel: x=10+pW+10, y=10
  GuiDrawOnePanel(RightPanel, 10 + pW + 10, 10, pW, pH, !LeftActive);
}

VOID GuiDrawBottomMenu(VOID)
{
  UINTN width, height;
  UiGfxGetDimensions(&width, &height);
  UINTN cellW, cellH;
  UiGfxGetCellSize(&cellW, &cellH);

  UINTN menuH = cellH + 15;
  UINTN menuY = height - menuH;
  
  // Clear menu area
  UiGfxFillRectRgb(0, menuY, width, menuH, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B);

  // Draw standard function keys: F1..F10
  typedef struct {
    CHAR8* Num;
    CHAR8* Label;
  } MENU_ITEM;

  static MENU_ITEM items[10] = {
    {"1", "Help"},
    {"2", "Drive"},
    {"3", "View"},
    {"4", "Edit"},
    {"5", "Copy"},
    {"6", "RenMov"},
    {"7", "MkDir"},
    {"8", "Delete"},
    {"9", "Refresh"},
    {"10", "Quit"}
  };

  // Determine scale for bottom menu
  // On narrow screens (width < 1024), we force 1x scale to prevent overlapping labels.
  UINTN menuTextScale = 1;
  UINTN glyphW, glyphH;
  UiGfxGetGlyphSize(&glyphW, &glyphH);
  
  if (width >= 1600) {
    menuTextScale = 2;
  }

  UINTN menuCellW = glyphW * menuTextScale;
  UINTN menuCellH = glyphH * menuTextScale;

  UINTN itemW = width / 10;
  for (UINTN i = 0; i < 10; i++) {
    UINTN itemX = i * itemW;

    // Center contents vertically inside menu bar
    UINTN itemY = menuY + (menuH - menuCellH) / 2;

    // Draw background for key number
    UiGfxFillRectRgb(itemX, itemY - 2, menuCellW * 3, menuCellH + 4, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    
    // Draw key number
    DrawAsciiAtScale(itemX + menuCellW, itemY, items[i].Num, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B, menuTextScale);

    // Draw label
    DrawAsciiAtScale(itemX + menuCellW * 3 + 5, itemY, items[i].Label, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B, menuTextScale);
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
  UINTN boxW = width > 800 ? 700 : width - 100;
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

  UINTN maxChars = GuiCharsForWidth(boxW - 100, cellW);

  UiGfxDrawAsciiAt(boxX + 25, boxY + 15 + cellH, "From:", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  GuiDrawUnicodeClippedAt(boxX + 25 + cellW * 6, boxY + 15 + cellH, srcName, maxChars, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

  UiGfxDrawAsciiAt(boxX + 25, boxY + 15 + cellH * 2, "To:  ", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
  GuiDrawUnicodeClippedAt(boxX + 25 + cellW * 6, boxY + 15 + cellH * 2, dstName, maxChars, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);

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

  UINTN boxW = width > 700 ? 600 : width - 100;
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

  UINTN boxW = width > 700 ? 550 : width - 100;
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
    L"F2 Drive",
    L"F3 View",
    L"F4 Edit",
    L"F5 Copy",
    L"F6 Rename/Move",
    L"F7 MkDir",
    L"F8 Delete",
    L"F9 Refresh",
    L"F10 Quit",
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
    L"Ctrl+A selects all; Ctrl+U clears selection",
    L"+ select by mask; - unselect by mask",
    L"* invert selection",
    L"Home / End jump in current panel",
    L"Type letters for quick prefix jump",
    L"/ finds anywhere in name",
    L"N repeats / search",
    L"Enter opens directories and EFI apps",
    L"Backspace goes up",
    L"Press any key to close"
  };
  UINTN lineCount = sizeof(lines) / sizeof(lines[0]);
  UINTN topLine = 0;
  UINTN selectedLine = 0;

  UiGfxGetDimensions(&width, &height);
  UiGfxGetCellSize(&cellW, &cellH);

  boxW = width > 1200 ? 980 : width - 80;
  boxH = height > cellH * 18 ? cellH * 17 : height - 80;
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
  if (boxW > width - 80) boxW = width - 80;

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
