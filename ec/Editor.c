// Editor.c — Modal Hex and Text Editor.
#include "Editor.h"
#include "Colors.h"
#include "UiConsole.h"
#include "FileSystem.h"
#include "Gui.h"

#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>

typedef enum {
  EDIT_MODE_TEXT,
  EDIT_MODE_HEX
} EDITOR_MODE;

typedef struct {
  CHAR8* Data;
  UINTN Length;
  UINTN Capacity;
} EDIT_LINE;

static EDIT_LINE* gEditLines = NULL;
static UINTN gEditLineCount = 0;
static UINTN gEditLineCapacity = 0;

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

static VOID DrawUnicodeAtScale(UINTN x, UINTN y, CONST CHAR16* str, UINT8 r, UINT8 g, UINT8 b, UINTN scale)
{
  UINTN glyphW, glyphH;
  UiGfxGetGlyphSize(&glyphW, &glyphH);
  
  UINTN stepW = glyphW * scale;
  while (*str) {
    UiGfxDrawGlyphScaled((UINT32)*str, x, y, r, g, b, scale);
    x += stepW;
    str++;
  }
}

// Parse buffer into text lines
static VOID ParseEditLines(UINT8* Buffer, UINT64 FileSize)
{
  if (gEditLines != NULL) {
    for (UINTN i = 0; i < gEditLineCount; i++) {
      if (gEditLines[i].Data) FreePool(gEditLines[i].Data);
    }
    FreePool(gEditLines);
    gEditLines = NULL;
  }
  gEditLineCount = 0;
  gEditLineCapacity = 128;
  gEditLines = AllocateZeroPool(gEditLineCapacity * sizeof(EDIT_LINE));

  if (FileSize == 0 || Buffer == NULL) {
    gEditLines[0].Length = 0;
    gEditLines[0].Capacity = 64;
    gEditLines[0].Data = AllocateZeroPool(64);
    gEditLineCount = 1;
    return;
  }

  UINTN lineStart = 0;
  for (UINTN i = 0; i < (UINTN)FileSize; i++) {
    if (Buffer[i] == '\r' || Buffer[i] == '\n') {
      if (gEditLineCount >= gEditLineCapacity) {
        UINTN oldCap = gEditLineCapacity;
        gEditLineCapacity *= 2;
        gEditLines = ReallocatePool(oldCap * sizeof(EDIT_LINE), gEditLineCapacity * sizeof(EDIT_LINE), gEditLines);
      }

      UINTN len = i - lineStart;
      gEditLines[gEditLineCount].Length = len;
      gEditLines[gEditLineCount].Capacity = len + 64;
      gEditLines[gEditLineCount].Data = AllocateZeroPool(gEditLines[gEditLineCount].Capacity);
      if (len > 0) {
        CopyMem(gEditLines[gEditLineCount].Data, &Buffer[lineStart], len);
      }
      gEditLineCount++;

      // Skip CR/LF pair
      if (Buffer[i] == '\r' && i + 1 < (UINTN)FileSize && Buffer[i + 1] == '\n') {
        i++;
      }
      lineStart = i + 1;
    }
  }

  // Trailing line
  if (lineStart < (UINTN)FileSize) {
    if (gEditLineCount >= gEditLineCapacity) {
      UINTN oldCap = gEditLineCapacity;
      gEditLineCapacity++;
      gEditLines = ReallocatePool(oldCap * sizeof(EDIT_LINE), gEditLineCapacity * sizeof(EDIT_LINE), gEditLines);
    }
    UINTN len = (UINTN)FileSize - lineStart;
    gEditLines[gEditLineCount].Length = len;
    gEditLines[gEditLineCount].Capacity = len + 64;
    gEditLines[gEditLineCount].Data = AllocateZeroPool(gEditLines[gEditLineCount].Capacity);
    CopyMem(gEditLines[gEditLineCount].Data, &Buffer[lineStart], len);
    gEditLineCount++;
  }
}

// Convert text lines back to a single buffer
static VOID SerializeEditLines(OUT VOID** Buffer, OUT UINT64* Size)
{
  UINT64 totalSize = 0;
  for (UINTN i = 0; i < gEditLineCount; i++) {
    totalSize += gEditLines[i].Length;
    if (i + 1 < gEditLineCount) {
      totalSize += 2; // \r\n
    }
  }

  UINT8* buf = AllocateZeroPool((UINTN)totalSize);
  UINTN offset = 0;
  for (UINTN i = 0; i < gEditLineCount; i++) {
    if (gEditLines[i].Length > 0) {
      CopyMem(&buf[offset], gEditLines[i].Data, gEditLines[i].Length);
      offset += gEditLines[i].Length;
    }
    if (i + 1 < gEditLineCount) {
      buf[offset++] = '\r';
      buf[offset++] = '\n';
    }
  }

  *Buffer = buf;
  *Size = totalSize;
}

static EFI_STATUS SaveEditorBuffer(
  IN CONST CHAR16* Path,
  IN EDITOR_MODE Mode,
  IN VOID* FileBuffer,
  IN UINT64 FileSize
) {
  VOID* saveBuf = NULL;
  UINT64 saveSize = 0;

  if (Mode == EDIT_MODE_TEXT) {
    SerializeEditLines(&saveBuf, &saveSize);
  } else {
    saveBuf = FileBuffer;
    saveSize = FileSize;
  }

  EFI_STATUS saveStatus = FsWriteFileFromBuffer(Path, saveBuf, saveSize);
  if (Mode == EDIT_MODE_TEXT && saveBuf != NULL) {
    FreePool(saveBuf);
  }

  return saveStatus;
}

static BOOLEAN ConfirmEditorExit(
  IN CONST CHAR16* Path,
  IN EDITOR_MODE Mode,
  IN VOID* FileBuffer,
  IN UINT64 FileSize,
  IN OUT BOOLEAN* IsModified
) {
  if (IsModified == NULL || !*IsModified) {
    return TRUE;
  }

  UINTN response = GuiDrawConfirmDialog(L"Save Changes", L"File modified. Save changes?", FALSE);
  if (response == 0) {
    return FALSE;
  }
  if (response == 2) {
    return TRUE;
  }
  if (response == 1) {
    EFI_STATUS saveStatus = SaveEditorBuffer(Path, Mode, FileBuffer, FileSize);
    if (EFI_ERROR(saveStatus)) {
      GuiDrawMsgBox(L"Save Error", L"Failed to write changes to disk!");
      return FALSE;
    }
    *IsModified = FALSE;
    return TRUE;
  }

  return FALSE;
}

static VOID DrawHexEditorLine(
  UINTN x, UINTN y,
  UINT64 offset,
  UINT8* data, UINTN len,
  UINT64 cursorOffset,
  UINTN cursorNibble,
  UINTN scale,
  UINTN cellW,
  UINTN cellH
) {
  CHAR8 temp[256];

  // 1. Draw Offset
  AsciiSPrint(temp, sizeof(temp), "%08X  ", (UINT32)offset);
  DrawAsciiAtScale(x, y, temp, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B, scale);

  UINTN hexStartCol = 10;

  // 2. Draw Hex bytes
  for (UINTN i = 0; i < 16; i++) {
    UINTN byteCol = hexStartCol + i * 3 + (i >= 8 ? 2 : 0);
    UINTN byteX = x + byteCol * cellW;

    if (i < len) {
      UINT64 byteOffset = offset + i;
      BOOLEAN isCursorByte = (byteOffset == cursorOffset);

      AsciiSPrint(temp, sizeof(temp), "%02X", data[i]);

      if (isCursorByte) {
        UiGfxFillRectRgb(byteX, y, cellW * 2, cellH, COLOR_GREEN_R, COLOR_GREEN_G, COLOR_GREEN_B);
        DrawAsciiAtScale(byteX, y, temp, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B, scale);

        UINTN activeNibbleX = byteX + (cursorNibble == 1 ? cellW : 0);
        UiGfxFillRectRgb(activeNibbleX, y + cellH - 3, cellW, 3, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
      } else {
        DrawAsciiAtScale(byteX, y, temp, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B, scale);
      }
    }

    UINTN sepX = byteX + cellW * 2;
    if (i == 7) {
      DrawAsciiAtScale(sepX, y, " - ", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B, scale);
    } else if (i < 15) {
      DrawAsciiAtScale(sepX, y, " ", COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B, scale);
    }
  }

  // 3. Draw ASCII preview
  UINTN asciiStartCol = hexStartCol + 16 * 3 + 2;
  UINTN asciiStartX = x + asciiStartCol * cellW;

  for (UINTN i = 0; i < 16; i++) {
    UINTN charX = asciiStartX + i * cellW;
    if (i < len) {
      UINT64 byteOffset = offset + i;
      BOOLEAN isCursorByte = (byteOffset == cursorOffset);

      UINT8 c = data[i];
      temp[0] = (c >= 32 && c < 127) ? c : '.';
      temp[1] = '\0';

      if (isCursorByte) {
        UiGfxFillRectRgb(charX, y, cellW, cellH, COLOR_GREEN_R, COLOR_GREEN_G, COLOR_GREEN_B);
        DrawAsciiAtScale(charX, y, temp, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B, scale);
      } else {
        DrawAsciiAtScale(charX, y, temp, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B, scale);
      }
    }
  }
}

static VOID DrawTextEditorLine(
  UINTN x, UINTN y,
  EDIT_LINE* line,
  UINTN maxChars,
  UINTN cursorCol,
  BOOLEAN isActiveRow,
  UINTN scale,
  UINTN cellW,
  UINTN cellH
) {
  CHAR8 temp[512] = { 0 };
  UINTN charCount = 0;

  for (UINTN i = 0; i < line->Length && charCount < maxChars; i++) {
    UINT8 c = line->Data[i];
    if (c == '\t') {
      UINTN tabSpaces = 4 - (charCount % 4);
      for (UINTN t = 0; t < tabSpaces && charCount < maxChars; t++) {
        temp[charCount++] = ' ';
      }
    } else {
      temp[charCount++] = (c >= 32 && c < 127) ? c : '.';
    }
  }
  temp[charCount] = '\0';

  DrawAsciiAtScale(x, y, temp, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B, scale);

  if (isActiveRow) {
    UINTN screenCol = 0;
    for (UINTN i = 0; i < cursorCol && i < line->Length; i++) {
      if (line->Data[i] == '\t') {
        screenCol += 4 - (screenCol % 4);
      } else {
        screenCol++;
      }
    }

    if (screenCol < maxChars) {
      UINTN cursorX = x + screenCol * cellW;
      UINT8 cursorChar = (cursorCol < line->Length) ? line->Data[cursorCol] : ' ';
      CHAR8 cursorStr[2] = { (cursorChar >= 32 && cursorChar < 127) ? cursorChar : '.', '\0' };

      UiGfxFillRectRgb(cursorX, y, cellW, cellH, COLOR_GREEN_R, COLOR_GREEN_G, COLOR_GREEN_B);
      DrawAsciiAtScale(cursorX, y, cursorStr, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B, scale);

      UiGfxFillRectRgb(cursorX, y + cellH - 3, cellW, 3, COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
    }
  }
}

static BOOLEAN ParseHexChar(CHAR16 Char, OUT UINT8* Val)
{
  if (Char >= L'0' && Char <= L'9') {
    *Val = (UINT8)(Char - L'0');
    return TRUE;
  }
  if (Char >= L'a' && Char <= L'f') {
    *Val = (UINT8)(10 + (Char - L'a'));
    return TRUE;
  }
  if (Char >= L'A' && Char <= L'F') {
    *Val = (UINT8)(10 + (Char - L'A'));
    return TRUE;
  }
  return FALSE;
}

static BOOLEAN DetectIsBinary(UINT8* Buffer, UINT64 FileSize)
{
  if (FileSize == 0) return FALSE;

  UINTN scanLen = FileSize > 1024 ? 1024 : (UINTN)FileSize;
  UINTN nonPrintableCount = 0;

  for (UINTN i = 0; i < scanLen; i++) {
    UINT8 c = Buffer[i];
    if (c == '\0') return TRUE;

    if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c > 127) {
      nonPrintableCount++;
    }
  }

  if ((nonPrintableCount * 100) / scanLen > 15) {
    return TRUE;
  }

  return FALSE;
}

VOID EditorShow(
  IN EFI_HANDLE ImageHandle,
  IN CONST CHAR16* Path
) {
  (VOID)ImageHandle;

  VOID* fileBuffer = NULL;
  UINT64 fileSize = 0;

  // Draw loading screen
  UINTN width, height;
  UiGfxGetDimensions(&width, &height);
  UiGfxFillRectRgb(0, 0, width, height, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
  UiGfxDrawAsciiAt(20, 20, "Loading file for editing...", COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
  UiGfxFlush();

  EFI_STATUS status = FsReadFileToBuffer(Path, &fileBuffer, &fileSize);
  if (EFI_ERROR(status) || fileBuffer == NULL) {
    GuiDrawMsgBox(L"Error", L"Failed to read file into memory!");
    return;
  }

  // Determine starting mode
  EDITOR_MODE mode = DetectIsBinary((UINT8*)fileBuffer, fileSize) ? EDIT_MODE_HEX : EDIT_MODE_TEXT;

  // Hex Mode Editor states
  UINT64 hexCursorOffset = 0;
  UINTN hexCursorNibble = 0;
  UINT64 hexTopOffset = 0;

  // Text Mode Editor states
  UINTN textCursorRow = 0;
  UINTN textCursorCol = 0;
  UINTN textTopLine = 0;

  BOOLEAN isModified = FALSE;

  // If initial mode is text, parse buffer
  if (mode == EDIT_MODE_TEXT) {
    ParseEditLines((UINT8*)fileBuffer, fileSize);
    FreePool(fileBuffer);
    fileBuffer = NULL;
  }

  BOOLEAN quit = FALSE;
  while (!quit) {
    UiGfxGetDimensions(&width, &height);
    
    UINTN glyphW, glyphH;
    UiGfxGetGlyphSize(&glyphW, &glyphH);

    // Dynamic scale fallback (1x if layout doesn't fit)
    UINTN editorScale = 2;
    if (width < 78 * glyphW * 2) {
      editorScale = 1;
    }

    UINTN cellW = glyphW * editorScale;
    UINTN cellH = glyphH * editorScale;

    // Layout boundaries
    UINTN headerY = 0;
    UINTN headerH = cellH + 6;
    UINTN footerH = cellH + 6;
    UINTN footerY = height - footerH;
    UINTN contentY = headerH + 10;
    UINTN contentH = height - headerH - footerH - 20;
    UINTN pageSize = contentH / cellH;

    // View boundaries adjustment
    if (mode == EDIT_MODE_HEX) {
      UINT64 cursorRowOffset = (hexCursorOffset / 16) * 16;
      if (cursorRowOffset < hexTopOffset) {
        hexTopOffset = cursorRowOffset;
      } else if (cursorRowOffset >= hexTopOffset + pageSize * 16) {
        hexTopOffset = cursorRowOffset - (pageSize - 1) * 16;
      }
    } else {
      if (textCursorRow < textTopLine) {
        textTopLine = textCursorRow;
      } else if (textCursorRow >= textTopLine + pageSize) {
        textTopLine = textCursorRow - pageSize + 1;
      }
    }

    // Draw background
    UiGfxFillRectRgb(0, 0, width, height, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);

    // 1. Draw top status bar
    UiGfxFillRectRgb(0, headerY, width, headerH, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    
    CHAR16 titleStr[MAX_PATH_LEN + 128];
    if (mode == EDIT_MODE_HEX) {
      UnicodeSPrint(
        titleStr, sizeof(titleStr),
        L"HxD-HexEditor: %s (%ld bytes)%s | Offset: %08X",
        Path, fileSize, isModified ? L" *" : L"", (UINT32)hexCursorOffset
      );
    } else {
      UnicodeSPrint(
        titleStr, sizeof(titleStr),
        L"TextEditor: %s%s | Line: %d Col: %d",
        Path, isModified ? L" *" : L"", textCursorRow + 1, textCursorCol + 1
      );
    }
    DrawUnicodeAtScale(15, headerY + 3, titleStr, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B, editorScale);

    // 2. Draw content grid
    if (mode == EDIT_MODE_HEX) {
      UINTN totalHexRows = (UINTN)((fileSize + 15) / 16);
      UINTN startRow = (UINTN)(hexTopOffset / 16);

      for (UINTN i = 0; i < pageSize; i++) {
        UINTN rowIdx = startRow + i;
        if (rowIdx >= totalHexRows) break;

        UINTN yPos = contentY + i * cellH;
        UINT64 lineOffset = (UINT64)rowIdx * 16;
        UINTN lineLen = 16;
        if (lineOffset + 16 > fileSize) {
          lineLen = (UINTN)(fileSize - lineOffset);
        }

        DrawHexEditorLine(
          15, yPos,
          lineOffset,
          (UINT8*)fileBuffer + lineOffset,
          lineLen,
          hexCursorOffset,
          hexCursorNibble,
          editorScale,
          cellW, cellH
        );
      }
    } else {
      UINTN maxChars = (width - 30) / cellW;
      for (UINTN i = 0; i < pageSize; i++) {
        UINTN lineIdx = textTopLine + i;
        if (lineIdx >= gEditLineCount) break;

        UINTN yPos = contentY + i * cellH;
        DrawTextEditorLine(
          15, yPos,
          &gEditLines[lineIdx],
          maxChars,
          textCursorCol,
          (lineIdx == textCursorRow),
          editorScale,
          cellW, cellH
        );
      }
    }

    // 3. Draw bottom menu bar
    UiGfxFillRectRgb(0, footerY, width, footerH, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B);
    DrawAsciiAtScale(
      15, footerY + 3,
      "[ F2: Save ]  [ F4: Toggle Text/Hex ]  [ ESC: Exit ]  [ Arrows: Navigate ]",
      COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B,
      editorScale
    );

    UiGfxFlush();

    // 4. Keyboard interaction
    EFI_INPUT_KEY key;
    UINTN eventIndex;
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &eventIndex);
    EFI_STATUS keyStatus = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
    if (EFI_ERROR(keyStatus)) continue;

    if (key.ScanCode != 0) {
      switch (key.ScanCode) {
        case SCAN_UP:
          if (mode == EDIT_MODE_HEX) {
            if (hexCursorOffset >= 16) hexCursorOffset -= 16;
          } else {
            if (textCursorRow > 0) {
              textCursorRow--;
              if (textCursorCol > gEditLines[textCursorRow].Length) {
                textCursorCol = gEditLines[textCursorRow].Length;
              }
            }
          }
          break;

        case SCAN_DOWN:
          if (mode == EDIT_MODE_HEX) {
            if (hexCursorOffset + 16 < fileSize) {
              hexCursorOffset += 16;
            } else {
              hexCursorOffset = fileSize - 1;
            }
          } else {
            if (textCursorRow + 1 < gEditLineCount) {
              textCursorRow++;
              if (textCursorCol > gEditLines[textCursorRow].Length) {
                textCursorCol = gEditLines[textCursorRow].Length;
              }
            }
          }
          break;

        case SCAN_LEFT:
          if (mode == EDIT_MODE_HEX) {
            if (hexCursorNibble == 1) {
              hexCursorNibble = 0;
            } else {
              if (hexCursorOffset > 0) {
                hexCursorOffset--;
                hexCursorNibble = 1;
              }
            }
          } else {
            if (textCursorCol > 0) {
              textCursorCol--;
            } else if (textCursorRow > 0) {
              textCursorRow--;
              textCursorCol = gEditLines[textCursorRow].Length;
            }
          }
          break;

        case SCAN_RIGHT:
          if (mode == EDIT_MODE_HEX) {
            if (hexCursorNibble == 0) {
              hexCursorNibble = 1;
            } else {
              if (hexCursorOffset + 1 < fileSize) {
                hexCursorOffset++;
                hexCursorNibble = 0;
              }
            }
          } else {
            if (textCursorCol < gEditLines[textCursorRow].Length) {
              textCursorCol++;
            } else if (textCursorRow + 1 < gEditLineCount) {
              textCursorRow++;
              textCursorCol = 0;
            }
          }
          break;

        case SCAN_PAGE_UP:
          if (mode == EDIT_MODE_HEX) {
            UINT64 pageOffset = (UINT64)pageSize * 16;
            if (hexCursorOffset >= pageOffset) {
              hexCursorOffset -= pageOffset;
            } else {
              hexCursorOffset = hexCursorOffset % 16;
            }
          } else {
            if (textCursorRow >= pageSize) {
              textCursorRow -= pageSize;
            } else {
              textCursorRow = 0;
            }
            if (textCursorCol > gEditLines[textCursorRow].Length) {
              textCursorCol = gEditLines[textCursorRow].Length;
            }
          }
          break;

        case SCAN_PAGE_DOWN:
          if (mode == EDIT_MODE_HEX) {
            UINT64 pageOffset = (UINT64)pageSize * 16;
            if (hexCursorOffset + pageOffset < fileSize) {
              hexCursorOffset += pageOffset;
            } else {
              hexCursorOffset = ((fileSize - 1) / 16) * 16 + (hexCursorOffset % 16);
              if (hexCursorOffset >= fileSize) hexCursorOffset = fileSize - 1;
            }
          } else {
            if (textCursorRow + pageSize < gEditLineCount) {
              textCursorRow += pageSize;
            } else {
              textCursorRow = gEditLineCount - 1;
            }
            if (textCursorCol > gEditLines[textCursorRow].Length) {
              textCursorCol = gEditLines[textCursorRow].Length;
            }
          }
          break;

        case SCAN_F2: {
          // Save changes to disk
          EFI_STATUS saveStatus = SaveEditorBuffer(Path, mode, fileBuffer, fileSize);
          if (EFI_ERROR(saveStatus)) {
            GuiDrawMsgBox(L"Save Error", L"Failed to write changes to disk!");
          } else {
            isModified = FALSE;
            GuiDrawMsgBox(L"Saved", L"File saved successfully!");
          }
          break;
        }

        case SCAN_F4: {
          // Toggle Editor Mode (Text <-> Hex)
          if (mode == EDIT_MODE_HEX) {
            // Hex -> Text
            ParseEditLines((UINT8*)fileBuffer, fileSize);
            FreePool(fileBuffer);
            fileBuffer = NULL;

            // Map hexCursorOffset to row/col
            textCursorRow = 0;
            textCursorCol = 0;
            UINT64 offsetCounter = 0;
            for (UINTN i = 0; i < gEditLineCount; i++) {
              UINT64 lineEnd = offsetCounter + gEditLines[i].Length;
              if (hexCursorOffset >= offsetCounter && hexCursorOffset <= lineEnd) {
                textCursorRow = i;
                textCursorCol = (UINTN)(hexCursorOffset - offsetCounter);
                break;
              }
              offsetCounter += gEditLines[i].Length + 2; // account for CR/LF
            }
            mode = EDIT_MODE_TEXT;
          } else {
            // Text -> Hex
            UINT64 targetOffset = 0;
            for (UINTN i = 0; i < textCursorRow; i++) {
              targetOffset += gEditLines[i].Length + 2;
            }
            targetOffset += textCursorCol;

            SerializeEditLines(&fileBuffer, &fileSize);
            // Free text structures
            for (UINTN i = 0; i < gEditLineCount; i++) {
              if (gEditLines[i].Data) FreePool(gEditLines[i].Data);
            }
            FreePool(gEditLines);
            gEditLines = NULL;
            gEditLineCount = 0;

            // Map row/col back to hexCursorOffset
            if (targetOffset >= fileSize) {
              targetOffset = fileSize > 0 ? fileSize - 1 : 0;
            }
            hexCursorOffset = targetOffset;
            hexCursorNibble = 0;

            mode = EDIT_MODE_HEX;
          }
          break;
        }

        case SCAN_ESC: {
          if (ConfirmEditorExit(Path, mode, fileBuffer, fileSize, &isModified)) {
            quit = TRUE;
          }
          break;
        }
      }
    } else {
      // Character Key Input
      if (mode == EDIT_MODE_HEX) {
        UINT8 val = 0;
        if (ParseHexChar(key.UnicodeChar, &val)) {
          UINT8* buf = (UINT8*)fileBuffer;
          if (hexCursorNibble == 0) {
            buf[hexCursorOffset] = (buf[hexCursorOffset] & 0x0F) | (val << 4);
            hexCursorNibble = 1;
          } else {
            buf[hexCursorOffset] = (buf[hexCursorOffset] & 0xF0) | val;
            hexCursorNibble = 0;
            if (hexCursorOffset + 1 < fileSize) {
              hexCursorOffset++;
            }
          }
          isModified = TRUE;
        }
      } else {
        // Text Editor Character Entry
        if (key.UnicodeChar >= 32 && key.UnicodeChar <= 126) {
          EDIT_LINE* line = &gEditLines[textCursorRow];
          // Ensure capacity
          if (line->Length + 1 >= line->Capacity) {
            UINTN oldCap = line->Capacity;
            line->Capacity += 64;
            line->Data = ReallocatePool(oldCap, line->Capacity, line->Data);
          }
          // Shift right
          for (UINTN i = line->Length; i > textCursorCol; i--) {
            line->Data[i] = line->Data[i - 1];
          }
          line->Data[textCursorCol] = (CHAR8)key.UnicodeChar;
          line->Length++;
          textCursorCol++;
          isModified = TRUE;
        } else if (key.UnicodeChar == L'\t') {
          EDIT_LINE* line = &gEditLines[textCursorRow];
          if (line->Length + 1 >= line->Capacity) {
            UINTN oldCap = line->Capacity;
            line->Capacity += 64;
            line->Data = ReallocatePool(oldCap, line->Capacity, line->Data);
          }
          for (UINTN i = line->Length; i > textCursorCol; i--) {
            line->Data[i] = line->Data[i - 1];
          }
          line->Data[textCursorCol] = '\t';
          line->Length++;
          textCursorCol++;
          isModified = TRUE;
        } else if (key.UnicodeChar == 8) { // Backspace
          EDIT_LINE* line = &gEditLines[textCursorRow];
          if (textCursorCol > 0) {
            // Shift left
            for (UINTN i = textCursorCol - 1; i + 1 < line->Length; i++) {
              line->Data[i] = line->Data[i + 1];
            }
            line->Length--;
            textCursorCol--;
            isModified = TRUE;
          } else if (textCursorRow > 0) {
            // Merge with previous line
            EDIT_LINE* prev = &gEditLines[textCursorRow - 1];
            UINTN prevOrigLen = prev->Length;
            UINTN newLen = prev->Length + line->Length;

            if (newLen >= prev->Capacity) {
              UINTN oldCap = prev->Capacity;
              prev->Capacity = newLen + 64;
              prev->Data = ReallocatePool(oldCap, prev->Capacity, prev->Data);
            }

            CopyMem(&prev->Data[prevOrigLen], line->Data, line->Length);
            prev->Length = newLen;

            // Free current line buffer
            if (line->Data) FreePool(line->Data);

            // Shift lines up
            for (UINTN i = textCursorRow; i + 1 < gEditLineCount; i++) {
              gEditLines[i] = gEditLines[i + 1];
            }
            gEditLineCount--;

            textCursorRow--;
            textCursorCol = prevOrigLen;
            isModified = TRUE;
          }
        } else if (key.UnicodeChar == 13) { // Enter (Newline split)
          EDIT_LINE* current = &gEditLines[textCursorRow];
          UINTN splitLen = current->Length - textCursorCol;

          // Allocate new line structure
          EDIT_LINE newLine;
          newLine.Length = splitLen;
          newLine.Capacity = splitLen + 64;
          newLine.Data = AllocateZeroPool(newLine.Capacity);
          if (splitLen > 0) {
            CopyMem(newLine.Data, &current->Data[textCursorCol], splitLen);
          }

          // Truncate current line
          current->Length = textCursorCol;

          // Insert new line after current
          if (gEditLineCount >= gEditLineCapacity) {
            UINTN oldCap = gEditLineCapacity;
            gEditLineCapacity += 32;
            gEditLines = ReallocatePool(oldCap * sizeof(EDIT_LINE), gEditLineCapacity * sizeof(EDIT_LINE), gEditLines);
          }

          // Shift lines down
          for (UINTN i = gEditLineCount; i > textCursorRow + 1; i--) {
            gEditLines[i] = gEditLines[i - 1];
          }

          gEditLines[textCursorRow + 1] = newLine;
          gEditLineCount++;

          textCursorRow++;
          textCursorCol = 0;
          isModified = TRUE;
        }
      }
      
      if (key.UnicodeChar == 27) { // ESC ASCII
        if (ConfirmEditorExit(Path, mode, fileBuffer, fileSize, &isModified)) {
          quit = TRUE;
        }
      }
    }
  }

  // Cleanup
  if (fileBuffer != NULL) {
    FreePool(fileBuffer);
  }
  if (gEditLines != NULL) {
    for (UINTN i = 0; i < gEditLineCount; i++) {
      if (gEditLines[i].Data) FreePool(gEditLines[i].Data);
    }
    FreePool(gEditLines);
    gEditLines = NULL;
    gEditLineCount = 0;
    gEditLineCapacity = 0;
  }
}
