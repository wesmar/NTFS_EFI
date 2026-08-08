// Viewer.c — modal ASCII and HEX file viewer implementation
#include "Viewer.h"
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
  VIEW_MODE_TEXT,
  VIEW_MODE_HEX
} VIEWER_MODE;

typedef struct {
  UINT8* Data;
  UINTN Length;
} TEXT_LINE;

static TEXT_LINE* gTextLines = NULL;
static UINTN gTextLineCount = 0;

static VOID ParseTextLines(UINT8* Buffer, UINT64 FileSize)
{
  if (gTextLines != NULL) {
    FreePool(gTextLines);
    gTextLines = NULL;
  }
  gTextLineCount = 0;

  if (FileSize == 0) return;

  UINTN capacity = 1024;
  gTextLines = AllocateZeroPool(capacity * sizeof(TEXT_LINE));
  if (gTextLines == NULL) return;

  UINTN lineStart = 0;
  for (UINTN i = 0; i < (UINTN)FileSize; i++) {
    if (Buffer[i] == '\r' || Buffer[i] == '\n') {
      if (gTextLineCount >= capacity) {
        capacity *= 2;
        TEXT_LINE* newLines = ReallocatePool(gTextLineCount * sizeof(TEXT_LINE), capacity * sizeof(TEXT_LINE), gTextLines);
        if (newLines == NULL) {
          FreePool(gTextLines);
          gTextLines = NULL;
          gTextLineCount = 0;
          return;
        }
        gTextLines = newLines;
      }

      gTextLines[gTextLineCount].Data = &Buffer[lineStart];
      gTextLines[gTextLineCount].Length = i - lineStart;
      gTextLineCount++;

      // Skip carriage return linefeed pair
      if (Buffer[i] == '\r' && i + 1 < (UINTN)FileSize && Buffer[i + 1] == '\n') {
        i++;
      }
      lineStart = i + 1;
    }
  }

  // Record trailing line
  if (lineStart < (UINTN)FileSize) {
    if (gTextLineCount >= capacity) {
      capacity++;
      TEXT_LINE* newLines = ReallocatePool(gTextLineCount * sizeof(TEXT_LINE), capacity * sizeof(TEXT_LINE), gTextLines);
      if (newLines != NULL) {
        gTextLines = newLines;
        gTextLines[gTextLineCount].Data = &Buffer[lineStart];
        gTextLines[gTextLineCount].Length = (UINTN)FileSize - lineStart;
        gTextLineCount++;
      }
    } else {
      gTextLines[gTextLineCount].Data = &Buffer[lineStart];
      gTextLines[gTextLineCount].Length = (UINTN)FileSize - lineStart;
      gTextLineCount++;
    }
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

static VOID DrawTextLine(
  UINTN x, UINTN y,
  UINT8* data, UINTN len,
  UINTN maxChars,
  UINT8 r, UINT8 g, UINT8 b,
  UINTN scale
) {
  UINTN charCount = 0;
  CHAR8 temp[512] = { 0 };

  for (UINTN i = 0; i < len && charCount + 1 < sizeof(temp) && charCount < maxChars; i++) {
    UINT8 c = data[i];
    if (c == '\t') {
      // Expand tabs to 4 spaces
      UINTN tabSpaces = 4 - (charCount % 4);
      for (UINTN t = 0; t < tabSpaces && charCount + 1 < sizeof(temp) && charCount < maxChars; t++) {
        temp[charCount++] = ' ';
      }
    } else if (c >= 32 && c < 127) {
      temp[charCount++] = c;
    } else {
      temp[charCount++] = '.';
    }
  }
  temp[charCount] = '\0';
  DrawAsciiAtScale(x, y, temp, r, g, b, scale);
}

static VOID DrawHexLine(
  UINTN x, UINTN y,
  UINT64 offset,
  UINT8* data, UINTN len,
  UINT8 r, UINT8 g, UINT8 b,
  UINTN scale
) {
  CHAR8 temp[256] = { 0 };

  // 1. Format offset (8 hex digits)
  AsciiSPrint(temp, sizeof(temp), "%08X  ", (UINT32)offset);

  // 2. Format 16 bytes as hex
  for (UINTN i = 0; i < 16; i++) {
    if (i < len) {
      AsciiSPrint(&temp[AsciiStrLen(temp)], sizeof(temp) - AsciiStrLen(temp), "%02X ", data[i]);
    } else {
      AsciiStrCatS(temp, sizeof(temp), "   ");
    }

    if (i == 7) {
      AsciiStrCatS(temp, sizeof(temp), "- ");
    }
  }
  AsciiStrCatS(temp, sizeof(temp), " ");

  // 3. Format printable ASCII representations on the right
  UINTN asciiStart = AsciiStrLen(temp);
  for (UINTN i = 0; i < 16; i++) {
    if (i < len) {
      UINT8 c = data[i];
      temp[asciiStart + i] = (c >= 32 && c < 127) ? c : '.';
    } else {
      temp[asciiStart + i] = ' ';
    }
  }
  temp[asciiStart + 16] = '\0';

  DrawAsciiAtScale(x, y, temp, r, g, b, scale);
}

static BOOLEAN DetectIsBinary(UINT8* Buffer, UINT64 FileSize)
{
  if (FileSize == 0) return FALSE;

  UINTN scanLen = FileSize > 1024 ? 1024 : (UINTN)FileSize;
  UINTN nonPrintableCount = 0;

  for (UINTN i = 0; i < scanLen; i++) {
    UINT8 c = Buffer[i];
    if (c == '\0') {
      return TRUE; // Null bytes mean it's binary (e.g. PE executables)
    }

    if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c > 127) {
      nonPrintableCount++;
    }
  }

  // If more than 15% non-printable characters, it's binary
  if ((nonPrintableCount * 100) / scanLen > 15) {
    return TRUE;
  }

  return FALSE;
}

/*
 * Finding a string in the open file. What the viewer is for in a rescue is
 * reading a log or a hive dump, and both are answered by "where does this word
 * appear", not by scrolling.
 *
 * The needle is typed as text and matched case-insensitively against the raw
 * bytes, which covers ASCII in a text file and ASCII embedded in a binary -
 * the two cases a pre-boot viewer actually meets. The file is already in
 * memory, so the search is a plain scan with no allocation and no index.
 */
static UINT8 ViewerUpper(UINT8 Ch)
{
  return (Ch >= 'a' && Ch <= 'z') ? (UINT8)(Ch - ('a' - 'A')) : Ch;
}

// Returns TRUE and the offset of the first match at or after From.
BOOLEAN ViewerFindBytes(
  IN  CONST UINT8* Data,
  IN  UINT64 Size,
  IN  CONST UINT8* Needle,
  IN  UINTN NeedleLen,
  IN  UINT64 From,
  OUT UINT64* Found
) {
  UINT64 i;

  if (Data == NULL || Needle == NULL || NeedleLen == 0 || NeedleLen > Size) return FALSE;

  for (i = From; i + NeedleLen <= Size; i++) {
    UINTN j = 0;
    while (j < NeedleLen && ViewerUpper(Data[i + j]) == ViewerUpper(Needle[j])) {
      j++;
    }
    if (j == NeedleLen) {
      *Found = i;
      return TRUE;
    }
  }
  return FALSE;
}

// The typed needle is UCS-2; the file is bytes. Narrow it, refusing anything
// that is not plain ASCII rather than truncating it into a wrong match.
BOOLEAN ViewerNeedleToBytes(
  IN  CONST CHAR16* Text,
  OUT UINT8* Bytes,
  IN  UINTN MaxBytes,
  OUT UINTN* Length
) {
  UINTN n = 0;

  while (Text[n] != L'\0') {
    if (n >= MaxBytes || Text[n] > 0x7F) return FALSE;
    Bytes[n] = (UINT8)Text[n];
    n++;
  }
  *Length = n;
  return (BOOLEAN)(n > 0);
}

// Put the byte at Offset on screen, in whichever mode is showing: the hex view
// scrolls to its 16-byte row, the text view to the line the byte falls in.
static VOID ViewerGoToOffset(
  IN  UINT64 Offset,
  IN  VIEWER_MODE Mode,
  OUT UINTN* TextTopLine,
  OUT UINT64* HexTopOffset,
  IN  VOID* FileBuffer
) {
  if (Mode == VIEW_MODE_HEX) {
    *HexTopOffset = (Offset / 16) * 16;
    return;
  }
  if (gTextLines == NULL || gTextLineCount == 0) {
    *TextTopLine = 0;
    return;
  }
  {
    UINTN i;
    UINTN best = 0;
    for (i = 0; i < gTextLineCount; i++) {
      UINT64 lineOffset = (UINT64)(gTextLines[i].Data - (UINT8*)FileBuffer);
      if (lineOffset > Offset) break;
      best = i;
    }
    *TextTopLine = best;
  }
}

VOID ViewerShow(
  IN EFI_HANDLE ImageHandle,
  IN CONST CHAR16* Path
) {
  (VOID)ImageHandle;

  VOID* fileBuffer = NULL;
  UINT64 fileSize = 0;

  // Render a loading box
  UINTN width, height;
  UiGfxGetDimensions(&width, &height);
  UiGfxFillRectRgb(0, 0, width, height, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);
  UiGfxDrawAsciiAt(20, 20, "Loading file...", COLOR_YELLOW_R, COLOR_YELLOW_G, COLOR_YELLOW_B);
  UiGfxFlush();

  EFI_STATUS status = FsReadFileToBuffer(Path, &fileBuffer, &fileSize);
  if (EFI_ERROR(status) || fileBuffer == NULL) {
    GuiDrawMsgBox(L"Error", L"Failed to read file into memory!");
    return;
  }

  // Parse lines for text mode
  ParseTextLines((UINT8*)fileBuffer, fileSize);

  // Auto-detect mode
  VIEWER_MODE mode = DetectIsBinary((UINT8*)fileBuffer, fileSize) ? VIEW_MODE_HEX : VIEW_MODE_TEXT;

  // Scrolling metrics
  UINTN textTopLine = 0;
  UINT64 hexTopOffset = 0;
  CHAR16 findText[128] = { 0 };
  UINT8 findBytes[128];
  UINTN findLen = 0;
  UINT64 findFrom = 0;

  BOOLEAN quit = FALSE;
  while (!quit) {
    // Re-query dimensions in case resolution changes
    UiGfxGetDimensions(&width, &height);
    
    UINTN glyphW, glyphH;
    UiGfxGetGlyphSize(&glyphW, &glyphH);

    // Compute custom scale: hex mode line needs 78 characters. 
    // If 78 * glyphW * 2 is wider than screen, force 1x scale!
    UINTN viewerScale = 2;
    if (width < 78 * glyphW * 2) {
      viewerScale = 1;
    }
    
    UINTN viewerCellW = glyphW * viewerScale;
    UINTN viewerCellH = glyphH * viewerScale;

    // Layout
    UINTN headerY = 0;
    UINTN headerH = viewerCellH + 6;
    UINTN footerH = viewerCellH + 6;
    UINTN footerY = height - footerH;
    UINTN contentY = headerH + 10;
    UINTN contentH = height - headerH - footerH - 20;
    UINTN pageSize = contentH / viewerCellH;

    // Clear entire frame to blue
    UiGfxFillRectRgb(0, 0, width, height, COLOR_BLUE_R, COLOR_BLUE_G, COLOR_BLUE_B);

    // 1. Draw top status bar
    UiGfxFillRectRgb(0, headerY, width, headerH, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    
    // Title text: Path and File Size
    CHAR16 titleStr[MAX_PATH_LEN + 64];
    UnicodeSPrint(
      titleStr,
      sizeof(titleStr),
      L"Viewer: %s (%ld bytes) | Mode: %s",
      Path,
      fileSize,
      mode == VIEW_MODE_TEXT ? L"TEXT" : L"HEX"
    );
    DrawUnicodeAtScale(15, headerY + 3, titleStr, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B, viewerScale);

    // 2. Draw contents
    UINTN maxChars = (width - 30) / viewerCellW;
    if (mode == VIEW_MODE_TEXT) {
      // Draw visible text lines
      for (UINTN i = 0; i < pageSize; i++) {
        UINTN lineIdx = textTopLine + i;
        if (lineIdx >= gTextLineCount) {
          break;
        }
        UINTN yPos = contentY + i * viewerCellH;
        DrawTextLine(
          15, yPos,
          gTextLines[lineIdx].Data,
          gTextLines[lineIdx].Length,
          maxChars,
          COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B,
          viewerScale
        );
      }
    } else {
      // Draw visible hex lines
      UINTN totalHexRows = (UINTN)((fileSize + 15) / 16);
      UINTN startRow = (UINTN)(hexTopOffset / 16);
      
      for (UINTN i = 0; i < pageSize; i++) {
        UINTN rowIdx = startRow + i;
        if (rowIdx >= totalHexRows) {
          break;
        }
        UINTN yPos = contentY + i * viewerCellH;
        UINT64 lineOffset = (UINT64)rowIdx * 16;
        UINTN lineLen = 16;
        if (lineOffset + 16 > fileSize) {
          lineLen = (UINTN)(fileSize - lineOffset);
        }
        DrawHexLine(
          15, yPos,
          lineOffset,
          (UINT8*)fileBuffer + lineOffset,
          lineLen,
          COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B,
          viewerScale
        );
      }
    }

    // 3. Draw bottom menu bar
    UiGfxFillRectRgb(0, footerY, width, footerH, COLOR_BLACK_R, COLOR_BLACK_G, COLOR_BLACK_B);
    DrawAsciiAtScale(
      15, footerY + 3,
      "[ F4: Text/Hex ]  [ F7: Find ]  [ F3 or N: Next ]  [ Esc: Close ]  [ Arrows/PgUp/PgDn ]",
      COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B,
      viewerScale
    );

    // Flush frame to GOP
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
          if (mode == VIEW_MODE_TEXT) {
            if (textTopLine > 0) textTopLine--;
          } else {
            if (hexTopOffset >= 16) hexTopOffset -= 16;
          }
          break;

        case SCAN_DOWN:
          if (mode == VIEW_MODE_TEXT) {
            if (textTopLine + pageSize < gTextLineCount) textTopLine++;
          } else {
            UINTN totalHexRows = (UINTN)((fileSize + 15) / 16);
            if ((hexTopOffset / 16) + pageSize < totalHexRows) hexTopOffset += 16;
          }
          break;

        case SCAN_PAGE_UP:
          if (mode == VIEW_MODE_TEXT) {
            if (textTopLine >= pageSize) {
              textTopLine -= pageSize;
            } else {
              textTopLine = 0;
            }
          } else {
            UINT64 pageOffset = (UINT64)pageSize * 16;
            if (hexTopOffset >= pageOffset) {
              hexTopOffset -= pageOffset;
            } else {
              hexTopOffset = 0;
            }
          }
          break;

        case SCAN_PAGE_DOWN:
          if (mode == VIEW_MODE_TEXT) {
            if (textTopLine + pageSize < gTextLineCount) {
              textTopLine += pageSize;
              if (textTopLine + pageSize > gTextLineCount) {
                textTopLine = gTextLineCount > pageSize ? gTextLineCount - pageSize : 0;
              }
            }
          } else {
            UINT64 pageOffset = (UINT64)pageSize * 16;
            UINTN totalHexRows = (UINTN)((fileSize + 15) / 16);
            UINT64 maxOffset = (UINT64)totalHexRows * 16;
            if (hexTopOffset + pageOffset < maxOffset) {
              hexTopOffset += pageOffset;
            }
          }
          break;

        case SCAN_F4:
          // Toggle Mode
          if (mode == VIEW_MODE_TEXT) {
            mode = VIEW_MODE_HEX;
            // Map textTopLine to hexOffset approximately
            if (textTopLine < gTextLineCount && gTextLines != NULL) {
              hexTopOffset = (UINT64)(gTextLines[textTopLine].Data - (UINT8*)fileBuffer);
              hexTopOffset = (hexTopOffset / 16) * 16; // Align to 16 bytes
            } else {
              hexTopOffset = 0;
            }
          } else {
            mode = VIEW_MODE_TEXT;
            // Map hexOffset back to line index
            textTopLine = 0;
            if (gTextLines != NULL) {
              for (UINTN i = 0; i < gTextLineCount; i++) {
                UINT64 lineOffset = (UINT64)(gTextLines[i].Data - (UINT8*)fileBuffer);
                if (lineOffset >= hexTopOffset) {
                  textTopLine = i;
                  break;
                }
              }
            }
          }
          break;

        case SCAN_F7: {
          CHAR16 prompt[128];
          StrCpyS(prompt, ARRAY_SIZE(prompt), findText);
          if (GuiDrawInputBox(L"Find", L"Text to look for:", prompt, ARRAY_SIZE(prompt))) {
            UINTN len = 0;
            if (!ViewerNeedleToBytes(prompt, findBytes, sizeof(findBytes), &len)) {
              GuiDrawMsgBox(L"Find", L"Type plain ASCII text to look for.");
            } else {
              UINT64 hit = 0;
              StrCpyS(findText, ARRAY_SIZE(findText), prompt);
              findLen = len;
              if (ViewerFindBytes((UINT8*)fileBuffer, fileSize, findBytes, findLen, 0, &hit)) {
                findFrom = hit + 1;
                ViewerGoToOffset(hit, mode, &textTopLine, &hexTopOffset, fileBuffer);
              } else {
                GuiDrawMsgBox(L"Find", L"Not found.");
              }
            }
          }
          break;
        }

        case SCAN_F3: {
          // find next, from just past the previous hit
          UINT64 hit = 0;
          if (findLen == 0) break;
          if (ViewerFindBytes((UINT8*)fileBuffer, fileSize, findBytes, findLen, findFrom, &hit)) {
            findFrom = hit + 1;
            ViewerGoToOffset(hit, mode, &textTopLine, &hexTopOffset, fileBuffer);
          } else {
            GuiDrawMsgBox(L"Find", L"No further match; searching from the top again.");
            findFrom = 0;
          }
          break;
        }

        case SCAN_ESC:
          quit = TRUE;
          break;
      }
    } else {
      // Handle printable character keys inside viewer
      if (key.UnicodeChar == L'4') { // Alternative toggle key
        if (mode == VIEW_MODE_TEXT) {
          mode = VIEW_MODE_HEX;
          if (textTopLine < gTextLineCount && gTextLines != NULL) {
            hexTopOffset = (UINT64)(gTextLines[textTopLine].Data - (UINT8*)fileBuffer);
            hexTopOffset = (hexTopOffset / 16) * 16;
          } else {
            hexTopOffset = 0;
          }
        } else {
          mode = VIEW_MODE_TEXT;
          textTopLine = 0;
          if (gTextLines != NULL) {
            for (UINTN i = 0; i < gTextLineCount; i++) {
              UINT64 lineOffset = (UINT64)(gTextLines[i].Data - (UINT8*)fileBuffer);
              if (lineOffset >= hexTopOffset) {
                textTopLine = i;
                break;
              }
            }
          }
        }
      } else if (key.UnicodeChar == L'n' || key.UnicodeChar == L'N') {
        UINT64 hit = 0;
        if (findLen != 0) {
          if (ViewerFindBytes((UINT8*)fileBuffer, fileSize, findBytes, findLen, findFrom, &hit)) {
            findFrom = hit + 1;
            ViewerGoToOffset(hit, mode, &textTopLine, &hexTopOffset, fileBuffer);
          } else {
            GuiDrawMsgBox(L"Find", L"No further match; searching from the top again.");
            findFrom = 0;
          }
        }
      } else if (key.UnicodeChar == 27) { // ESC char
        quit = TRUE;
      }
    }
  }

  // Free buffers
  if (fileBuffer != NULL) {
    FreePool(fileBuffer);
  }
  if (gTextLines != NULL) {
    FreePool(gTextLines);
    gTextLines = NULL;
    gTextLineCount = 0;
  }
}
