// UiConsole.c — GOP framebuffer text/graphics console. Double-buffered.
#include "UiConsole.h"

#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>     // gST
#include <Protocol/GraphicsOutput.h>

#include "BmpFont.h"

// Scalable Screen Font (https://gitlab.com/bztsrc/scalable-font2)
typedef struct {
  unsigned char  magic[4];
  unsigned int   size;
  unsigned char  type;
  unsigned char  features;
  unsigned char  fbWidth;
  unsigned char  fbHeight;
  unsigned char  baseline;
  unsigned char  underline;
  unsigned short fragments_offs;
  unsigned int   characters_offs;
  unsigned int   ligature_offs;
  unsigned int   kerning_offs;
  unsigned int   cmap_offs;
} ssfn_font_t;

static ssfn_font_t* gFont = (ssfn_font_t*)&_bmp_font[0];

#define UI_TEXT_SCALE_LARGE 2
#define UI_TEXT_SCALE_SMALL 1

static EFI_GRAPHICS_OUTPUT_PROTOCOL* gGop = NULL;
static UINT32* gBackBuffer = NULL;
static UINTN gFbWidth = 0;
static UINTN gFbHeight = 0;
static UINTN gPitchPixels = 0;
static UINTN gCellW = 0;
static UINTN gCellH = 0;
static UINTN gGlyphW = 0;
static UINTN gGlyphH = 0;
static UINTN gTextScaleX = UI_TEXT_SCALE_LARGE;
static UINTN gTextScaleY = UI_TEXT_SCALE_LARGE;
static UINTN gCols = 0;
static UINTN gRows = 0;
static UINTN gCursorX = 0;
static UINTN gCursorY = 0;
static BOOLEAN gGfxReady = FALSE;
static BOOLEAN gPendingCR = FALSE;
static BOOLEAN gPixelBgr = TRUE;
static UINT32 gFgColor = 0x00FFFFFF;
static UINT32 gBgColor = 0x00000000;
static UINT32 gColorLut[16] = { 0 };

static UINT32 MakeColor(UINT8 r, UINT8 g, UINT8 b)
{
  if (gPixelBgr) {
    return ((UINT32)r << 16) | ((UINT32)g << 8) | (UINT32)b;
  }
  return ((UINT32)b << 16) | ((UINT32)g << 8) | (UINT32)r;
}

static VOID InitColorLut(VOID)
{
  gColorLut[EFI_BLACK]        = MakeColor(0x00, 0x00, 0x00);
  gColorLut[EFI_BLUE]         = MakeColor(0x00, 0x00, 0xAA);
  gColorLut[EFI_GREEN]        = MakeColor(0x00, 0xAA, 0x00);
  gColorLut[EFI_CYAN]         = MakeColor(0x00, 0xAA, 0xAA);
  gColorLut[EFI_RED]          = MakeColor(0xAA, 0x00, 0x00);
  gColorLut[EFI_MAGENTA]      = MakeColor(0xAA, 0x00, 0xAA);
  gColorLut[EFI_BROWN]        = MakeColor(0xAA, 0x55, 0x00);
  gColorLut[EFI_LIGHTGRAY]    = MakeColor(0xAA, 0xAA, 0xAA);
  gColorLut[EFI_DARKGRAY]     = MakeColor(0x55, 0x55, 0x55);
  gColorLut[EFI_LIGHTBLUE]    = MakeColor(0x55, 0x55, 0xFF);
  gColorLut[EFI_LIGHTGREEN]   = MakeColor(0x55, 0xFF, 0x55);
  gColorLut[EFI_LIGHTCYAN]    = MakeColor(0x55, 0xFF, 0xFF);
  gColorLut[EFI_LIGHTRED]     = MakeColor(0xFF, 0x55, 0x55);
  gColorLut[EFI_LIGHTMAGENTA] = MakeColor(0xFF, 0x55, 0xFF);
  gColorLut[EFI_YELLOW]       = MakeColor(0xFF, 0xFF, 0x55);
  gColorLut[EFI_WHITE]        = MakeColor(0xFF, 0xFF, 0xFF);
}

static UINTN ComputeAsciiGlyphWidth(VOID)
{
  if (!gFont || gFont->characters_offs == 0 || gFont->size == 0) {
    return 8;
  }

  UINT8* ptr = (UINT8*)gFont + gFont->characters_offs;
  UINT8* end = (UINT8*)gFont + gFont->size;

  UINTN cp = 0;
  UINTN maxW = 0;

  while (cp < 0x110000 && ptr < end) {
    UINT8 c0 = ptr[0];

    if (c0 == 0xFF) {
      cp += 65536;
      ptr += 1;
      continue;
    } else if ((c0 & 0xC0) == 0xC0) {
      if (ptr + 1 >= end) {
        break;
      }
      UINTN j = (((UINTN)c0 & 0x3F) << 8) | ptr[1];
      cp += j;
      ptr += 2;
      continue;
    } else if ((c0 & 0xC0) == 0x80) {
      UINTN j = (c0 & 0x3F);
      cp += j;
      ptr += 1;
      continue;
    }

    if (cp >= 32 && cp <= 126) {
      UINTN w = ptr[4];
      if (w > maxW) {
        maxW = w;
      }
    }

    UINTN segs = ptr[1];
    UINTN step = 6 + segs * ((c0 & 0x40) ? 6 : 5);
    ptr += step;
    cp++;
  }

  if (maxW == 0) {
    maxW = (gFont && gFont->fbWidth) ? gFont->fbWidth : 8;
  }

  return maxW;
}

static VOID GfxFillRect(UINTN x, UINTN y, UINTN w, UINTN h, UINT32 color)
{
  if (!gGfxReady || !gBackBuffer || w == 0 || h == 0) {
    return;
  }

  if (x >= gFbWidth || y >= gFbHeight) {
    return;
  }

  // Compare against the available span instead of "x + w > gFbWidth": if a
  // caller passes a wildly oversized w (e.g. an unsigned underflow), x + w
  // overflows UINTN and wraps below gFbWidth, defeating the clamp and letting
  // the inner loop write past the framebuffer. gFbWidth - x is safe here
  // because x < gFbWidth is already guaranteed above.
  if (w > gFbWidth - x) {
    w = gFbWidth - x;
  }
  if (h > gFbHeight - y) {
    h = gFbHeight - y;
  }

  UINT32* row = gBackBuffer + y * gFbWidth + x;
  for (UINTN ry = 0; ry < h; ry++) {
    for (UINTN rx = 0; rx < w; rx++) {
      row[rx] = color;
    }
    row += gFbWidth;
  }
}

static VOID GfxClearScreen(VOID)
{
  GfxFillRect(0, 0, gFbWidth, gFbHeight, gBgColor);
}

static VOID GfxClearLine(UINTN line)
{
  if (line >= gRows) {
    return;
  }
  GfxFillRect(0, line * gCellH, gFbWidth, gCellH, gBgColor);
}

static VOID GfxScroll(VOID)
{
  if (gCellH == 0 || gFbHeight <= gCellH) {
    gCursorX = 0;
    gCursorY = 0;
    GfxClearScreen();
    return;
  }

  UINTN rowPixels = gCellH;
  UINTN copyPixels = (gFbHeight - rowPixels) * gFbWidth;

  if (copyPixels > 0) {
    CopyMem(gBackBuffer, gBackBuffer + rowPixels * gFbWidth, copyPixels * sizeof(UINT32));
  }

  GfxFillRect(0, gFbHeight - rowPixels, gFbWidth, rowPixels, gBgColor);

  if (gRows > 0) {
    gCursorY = gRows - 1;
  } else {
    gCursorY = 0;
  }
  gCursorX = 0;
}

static unsigned char* FindGlyph(UINT32 codepoint)
{
  unsigned char* ptr = (unsigned char*)gFont + gFont->characters_offs;
  unsigned char* chr = 0;

  for (UINT32 i = 0; i < 0x110000; i++) {
    if (ptr[0] == 0xFF) {
      i += 65535;
      ptr++;
    } else if ((ptr[0] & 0xC0) == 0xC0) {
      UINT32 j = (((ptr[0] & 0x3F) << 8) | ptr[1]);
      i += j;
      ptr += 2;
    } else if ((ptr[0] & 0xC0) == 0x80) {
      UINT32 j = (ptr[0] & 0x3F);
      i += j;
      ptr++;
    } else {
      if (i == codepoint) {
        chr = ptr;
        break;
      }
      ptr += 6 + ptr[1] * (ptr[0] & 0x40 ? 6 : 5);
    }
  }

  return chr;
}

static BOOLEAN DrawCopyrightGlyph(UINTN x, UINTN y, UINT32 color, UINTN scaleX, UINTN scaleY)
{
  static const UINT8 kCopyright8[8] = {
    0x3C, 0x42, 0x84, 0x80, 0x80, 0x84, 0x42, 0x3C
  };

  UINTN gw = gGlyphW ? gGlyphW : 8;
  UINTN gh = gGlyphH ? gGlyphH : 16;
  UINTN offX = (gw > 8) ? (gw - 8) / 2 : 0;
  UINTN offY = (gh > 8) ? (gh - 8) / 2 : 0;

  for (UINTN row = 0; row < 8; row++) {
    UINT8 bits = kCopyright8[row];
    for (UINTN col = 0; col < 8; col++) {
      if (bits & (0x80 >> col)) {
        GfxFillRect(x + (offX + col) * scaleX,
                    y + (offY + row) * scaleY,
                    scaleX, scaleY, color);
      }
    }
  }

  return TRUE;
}

static UINT32 FallbackGlyph(UINT32 codepoint);
static CONST CHAR8* FallbackText(UINT32 codepoint);

static BOOLEAN GfxDrawGlyphScaledRatioXY(UINT32 codepoint, UINTN x, UINTN y,
                                         UINT32 color,
                                         UINTN scaleXNum, UINTN scaleXDen,
                                         UINTN scaleYNum, UINTN scaleYDen)
{
  if (!gGfxReady || !gBackBuffer || x >= gFbWidth || y >= gFbHeight) {
    return FALSE;
  }

  if (scaleXNum == 0) scaleXNum = 1;
  if (scaleXDen == 0) scaleXDen = 1;
  if (scaleYNum == 0) scaleYNum = 1;
  if (scaleYDen == 0) scaleYDen = 1;

  unsigned char* chr = FindGlyph(codepoint);
  if (!chr) {
    if (codepoint == 0x00A9) {
      return DrawCopyrightGlyph(x, y, color,
                                (scaleXNum + scaleXDen - 1) / scaleXDen,
                                (scaleYNum + scaleYDen - 1) / scaleYDen);
    }
    UINT32 fallback = FallbackGlyph(codepoint);
    if (fallback) {
      chr = FindGlyph(fallback);
    }
    if (!chr) {
      return FALSE;
    }
  }

  unsigned char* ptr = chr + 6;
  unsigned char* frg;
  int i, j, k, l, m, n;

  for (i = n = 0; i < chr[1]; i++, ptr += chr[0] & 0x40 ? 6 : 5) {
    if (ptr[0] == 255 && ptr[1] == 255) {
      continue;
    }

    frg = (unsigned char*)gFont + (chr[0] & 0x40 ?
      ((ptr[5] << 24) | (ptr[4] << 16) | (ptr[3] << 8) | ptr[2]) :
      ((ptr[4] << 16) | (ptr[3] << 8) | ptr[2]));

    if ((frg[0] & 0xE0) != 0x80) {
      continue;
    }

    n = (int)ptr[1];
    k = ((frg[0] & 0x1F) + 1) << 3;
    j = (int)frg[1] + 1;
    frg += 2;

    for (m = 1; j; j--, n++) {
      for (l = 0; l < k; l++, m <<= 1) {
        if (m > 0x80) { frg++; m = 1; }
        if ((*frg & m) && n >= 0) {
          UINTN left = (UINTN)l * scaleXNum / scaleXDen;
          UINTN right = ((UINTN)l + 1) * scaleXNum / scaleXDen;
          UINTN top = (UINTN)n * scaleYNum / scaleYDen;
          UINTN bottom = ((UINTN)n + 1) * scaleYNum / scaleYDen;
          if (right <= left) right = left + 1;
          if (bottom <= top) bottom = top + 1;
          GfxFillRect(x + left, y + top, right - left, bottom - top, color);
        }
      }
    }
  }

  return TRUE;
}

static BOOLEAN GfxDrawGlyphScaledXY(UINT32 codepoint, UINTN x, UINTN y,
                                    UINT32 color, UINTN scaleX, UINTN scaleY)
{
  return GfxDrawGlyphScaledRatioXY(codepoint, x, y, color,
                                   scaleX == 0 ? 1 : scaleX, 1,
                                   scaleY == 0 ? 1 : scaleY, 1);
}

static UINT32 FallbackGlyph(UINT32 codepoint)
{
  switch (codepoint) {
    case 0x2554: // ╔
    case 0x2557: // ╗
    case 0x255A: // ╚
    case 0x255D: // ╝
    case 0x2566: // ╦
    case 0x2569: // ╩
    case 0x2560: // ╠
    case 0x2563: // ╣
    case 0x256C: // ╬
    case 0x253C: // ┼
    case 0x252C: // ┬
    case 0x2534: // ┴
    case 0x251C: // ├
    case 0x2524: // ┤
    case 0x250C: // ┌
    case 0x2510: // ┐
    case 0x2514: // └
    case 0x2518: // ┘
      return '+';
    case 0x2550: // ═
    case 0x2500: // ─
      return '-';
    case 0x2551: // ║
    case 0x2502: // │
      return '|';
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3:
    case 0x00E4: case 0x00E5: case 0x0101: case 0x0103:
    case 0x0105: case 0x01CE: return 'a';
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3:
    case 0x00C4: case 0x00C5: case 0x0100: case 0x0102:
    case 0x0104: case 0x01CD: return 'A';
    case 0x00E6: return 'a';
    case 0x00C6: return 'A';
    case 0x00E7: case 0x0107: case 0x0109: case 0x010D: return 'c';
    case 0x00C7: case 0x0106: case 0x0108: case 0x010C: return 'C';
    case 0x010F: case 0x0111: return 'd';
    case 0x010E: case 0x0110: return 'D';
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
    case 0x0113: case 0x0117: case 0x0119: case 0x011B: return 'e';
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
    case 0x0112: case 0x0116: case 0x0118: case 0x011A: return 'E';
    case 0x011F: case 0x0123: return 'g';
    case 0x011E: case 0x0122: return 'G';
    case 0x0125: return 'h';
    case 0x0124: return 'H';
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
    case 0x012B: case 0x012F: case 0x0131: return 'i';
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
    case 0x012A: case 0x012E: case 0x0130: return 'I';
    case 0x0135: return 'j';
    case 0x0134: return 'J';
    case 0x0137: return 'k';
    case 0x0136: return 'K';
    case 0x013A: case 0x013C: case 0x013E: case 0x0140: return 'l';
    case 0x0139: case 0x013B: case 0x013D: case 0x013F: return 'L';
    case 0x00F1: case 0x0144: case 0x0146: case 0x0148: return 'n';
    case 0x00D1: case 0x0143: case 0x0145: case 0x0147: return 'N';
    case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5:
    case 0x00F6: case 0x00F8: case 0x014D: case 0x0151: return 'o';
    case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5:
    case 0x00D6: case 0x00D8: case 0x014C: case 0x0150: return 'O';
    case 0x0155: case 0x0159: return 'r';
    case 0x0154: case 0x0158: return 'R';
    case 0x015B: case 0x015D: case 0x015F: case 0x0161: return 's';
    case 0x015A: case 0x015C: case 0x015E: case 0x0160: return 'S';
    case 0x00DF: return 's';
    case 0x0163: case 0x0165: return 't';
    case 0x0162: case 0x0164: return 'T';
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
    case 0x016B: case 0x016F: case 0x0171: case 0x0173: return 'u';
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
    case 0x016A: case 0x016E: case 0x0170: case 0x0172: return 'U';
    case 0x00FD: case 0x00FF: return 'y';
    case 0x00DD: case 0x0178: return 'Y';
    case 0x017A: case 0x017C: case 0x017E: return 'z';
    case 0x0179: case 0x017B: case 0x017D: return 'Z';
    case 0x00DE: return 'T';
    case 0x00FE: return 't';
    default:
      return 0;
  }
}

static CONST CHAR8* FallbackText(UINT32 codepoint)
{
  switch (codepoint) {
    case 0x00DF: return "ss";
    case 0x00E6: return "ae";
    case 0x00C6: return "AE";
    case 0x0153: return "oe";
    case 0x0152: return "OE";
    case 0x00DE: return "Th";
    case 0x00FE: return "th";
    case 0x00D0: return "D";
    case 0x00F0: return "d";
    default:
      break;
  }

  UINT32 glyph = FallbackGlyph(codepoint);
  static CHAR8 text[2];
  if (glyph >= 0x20 && glyph <= 0x7E) {
    text[0] = (CHAR8)glyph;
    text[1] = '\0';
    return text;
  }
  return NULL;
}

static VOID GfxPutChar(UINT32 codepoint)
{
  if (!gGfxReady) {
    return;
  }

  if (codepoint == '\r') {
    gCursorX = 0;
    gPendingCR = TRUE;
    return;
  }

  if (codepoint == '\n') {
    gCursorX = 0;
    gPendingCR = FALSE;
    gCursorY++;
    if (gCursorY >= gRows) {
      GfxScroll();
    }
    return;
  }

  if (codepoint == '\t') {
    UINTN spaces = 4 - (gCursorX % 4);
    for (UINTN i = 0; i < spaces; i++) {
      GfxPutChar(' ');
    }
    return;
  }

  if (codepoint < 0x20) {
    return;
  }

  if (codepoint > 0x7E) {
    CONST CHAR8* fallback = FallbackText(codepoint);
    if (fallback != NULL) {
      while (*fallback) {
        GfxPutChar((UINT8)*fallback);
        fallback++;
      }
      return;
    }
  }

  if (gPendingCR) {
    GfxClearLine(gCursorY);
    gPendingCR = FALSE;
  }

  if (gCursorY >= gRows) {
    GfxScroll();
  }

  UINTN x = gCursorX * gCellW;
  UINTN y = gCursorY * gCellH;
  GfxFillRect(x, y, gCellW, gCellH, gBgColor);

  GfxDrawGlyphScaledXY(codepoint, x, y, gFgColor, gTextScaleX, gTextScaleY);

  gCursorX++;
  if (gCursorX >= gCols) {
    gCursorX = 0;
    gCursorY++;
    if (gCursorY >= gRows) {
      GfxScroll();
    }
  }
}

static VOID GfxWriteUnicode(IN CONST CHAR16* Str)
{
  if (!Str) {
    return;
  }
  while (*Str) {
    GfxPutChar((UINT32)(*Str));
    Str++;
  }
}

static VOID GfxWriteAscii(IN CONST CHAR8* Str)
{
  if (!Str) {
    return;
  }
  while (*Str) {
    GfxPutChar((UINT8)(*Str));
    Str++;
  }
}

#define EC_PREFERRED_GOP_WIDTH  1920U
#define EC_PREFERRED_GOP_HEIGHT 1080U

static VOID TrySelectPreferredGopMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL* Gop)
{
  UINT32 BestMode = Gop->Mode->Mode;
  UINT64 BestArea = 0;
  BOOLEAN FoundPreferred = FALSE;

  if (Gop == NULL || Gop->Mode == NULL) return;

  for (UINT32 Mode = 0; Mode < Gop->Mode->MaxMode; Mode++) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info = NULL;
    UINTN InfoSize = 0;
    EFI_STATUS Status = Gop->QueryMode(Gop, Mode, &InfoSize, &Info);
    if (EFI_ERROR(Status) || Info == NULL) {
      continue;
    }

    if (Info->PixelFormat != PixelBltOnly) {
      UINT64 Area = (UINT64)Info->HorizontalResolution * Info->VerticalResolution;
      if (Info->HorizontalResolution == EC_PREFERRED_GOP_WIDTH &&
          Info->VerticalResolution == EC_PREFERRED_GOP_HEIGHT) {
        BestMode = Mode;
        FoundPreferred = TRUE;
      } else if (!FoundPreferred &&
                 Info->HorizontalResolution <= EC_PREFERRED_GOP_WIDTH &&
                 Info->VerticalResolution <= 1200 &&
                 Area > BestArea) {
        BestArea = Area;
        BestMode = Mode;
      }
    }

    gBS->FreePool(Info);
    if (FoundPreferred) break;
  }

  if (BestMode != Gop->Mode->Mode) {
    (VOID)Gop->SetMode(Gop, BestMode);
  }
}

BOOLEAN UiConsoleInit(IN EFI_SYSTEM_TABLE* SystemTable)
{
  if (gGfxReady || SystemTable == NULL) {
    return gGfxReady;
  }

  EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
  EFI_STATUS status = SystemTable->BootServices->LocateProtocol(
    &gEfiGraphicsOutputProtocolGuid, NULL, (VOID**)&gop);

  if (EFI_ERROR(status) || gop == NULL || gop->Mode == NULL || gop->Mode->Info == NULL) {
    return FALSE;
  }

  TrySelectPreferredGopMode(gop);

  if (gop->Mode->Info->PixelFormat == PixelBltOnly) {
    return FALSE;
  }

  gGop = gop;
  gFbWidth = gop->Mode->Info->HorizontalResolution;
  gFbHeight = gop->Mode->Info->VerticalResolution;
  gPitchPixels = gop->Mode->Info->PixelsPerScanLine;

  gPixelBgr = (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor);
  InitColorLut();

  // Allocate back buffer in RAM
  status = SystemTable->BootServices->AllocatePool(
    EfiBootServicesData,
    gFbWidth * gFbHeight * sizeof(UINT32),
    (VOID**)&gBackBuffer
  );
  if (EFI_ERROR(status) || gBackBuffer == NULL) {
    return FALSE;
  }

  gGlyphW = ComputeAsciiGlyphWidth();
  gGlyphH = (gFont && gFont->fbHeight) ? gFont->fbHeight : 16;
  if (gGlyphW == 0) gGlyphW = 8;
  if (gGlyphH == 0) gGlyphH = 16;

  if (gFbHeight < 700 || gFbWidth < 1024) {
    gTextScaleX = UI_TEXT_SCALE_SMALL;
    gTextScaleY = UI_TEXT_SCALE_SMALL;
  } else {
    gTextScaleX = UI_TEXT_SCALE_LARGE;
    gTextScaleY = UI_TEXT_SCALE_LARGE;
  }

  gCellW = gGlyphW * gTextScaleX;
  gCellH = gGlyphH * gTextScaleY;
  gCols = (gCellW > 0) ? (gFbWidth / gCellW) : 0;
  gRows = (gCellH > 0) ? (gFbHeight / gCellH) : 0;

  gCursorX = 0;
  gCursorY = 0;
  gPendingCR = FALSE;
  gFgColor = gColorLut[EFI_WHITE];
  gBgColor = gColorLut[EFI_BLUE]; // default to blue background!

  gGfxReady = TRUE;
  GfxClearScreen();
  return TRUE;
}

VOID UiConsoleShutdown(VOID)
{
  if (gBackBuffer) {
    gBS->FreePool(gBackBuffer);
    gBackBuffer = NULL;
  }
  gGfxReady = FALSE;
}

UINTN UiPrint(IN CONST CHAR16* Format, ...)
{
  VA_LIST marker;
  CHAR16 buffer[4096];

  VA_START(marker, Format);
  UnicodeVSPrint(buffer, sizeof(buffer), Format, marker);
  VA_END(marker);

  if (gGfxReady) {
    GfxWriteUnicode(buffer);
  } else {
    Print(L"%s", buffer);
  }

  return StrLen(buffer);
}

UINTN UiAsciiPrint(IN CONST CHAR8* Format, ...)
{
  VA_LIST marker;
  CHAR8 buffer[4096];

  VA_START(marker, Format);
  AsciiVSPrint(buffer, sizeof(buffer), Format, marker);
  VA_END(marker);

  if (gGfxReady) {
    GfxWriteAscii(buffer);
  } else {
    AsciiPrint("%a", buffer);
  }

  return AsciiStrLen(buffer);
}

VOID UiSetAttribute(IN UINTN Attribute)
{
  UINTN fg = Attribute & 0x0F;
  UINTN bg = (Attribute >> 4) & 0x0F;

  if (gGfxReady) {
    if (fg < 16) {
      gFgColor = gColorLut[fg];
    }
    if (bg < 16) {
      gBgColor = gColorLut[bg];
    }
  }

  if (gST && gST->ConOut) {
    gST->ConOut->SetAttribute(gST->ConOut, Attribute);
  }
}

BOOLEAN UiGfxIsReady(VOID)
{
  return gGfxReady;
}

VOID UiGfxGetDimensions(OUT UINTN* Width, OUT UINTN* Height)
{
  if (Width)  *Width  = gFbWidth;
  if (Height) *Height = gFbHeight;
}

VOID UiGfxGetCellSize(OUT UINTN* CellW, OUT UINTN* CellH)
{
  if (CellW) *CellW = gCellW;
  if (CellH) *CellH = gCellH;
}

VOID UiGfxGetGlyphSize(OUT UINTN* GlyphW, OUT UINTN* GlyphH)
{
  if (GlyphW) *GlyphW = gGlyphW;
  if (GlyphH) *GlyphH = gGlyphH;
}

VOID UiGfxSetCursor(UINTN Col, UINTN Row)
{
  gCursorX = Col;
  gCursorY = Row;
}

VOID UiGfxFillRectRgb(UINTN x, UINTN y, UINTN w, UINTN h, UINT8 r, UINT8 g, UINT8 b)
{
  GfxFillRect(x, y, w, h, MakeColor(r, g, b));
}

VOID UiGfxSetPixel(UINTN x, UINTN y, UINT8 r, UINT8 g, UINT8 b)
{
  if (!gGfxReady || !gBackBuffer || x >= gFbWidth || y >= gFbHeight) {
    return;
  }
  gBackBuffer[y * gFbWidth + x] = MakeColor(r, g, b);
}

VOID UiGfxDrawGlyphScaled(UINT32 codepoint, UINTN x, UINTN y,
                           UINT8 r, UINT8 g, UINT8 b, UINTN scale)
{
  GfxDrawGlyphScaledXY(codepoint, x, y, MakeColor(r, g, b), scale == 0 ? 1 : scale, scale == 0 ? 1 : scale);
}

VOID UiGfxDrawGlyphScaledRatio(UINT32 codepoint, UINTN x, UINTN y,
                               UINT8 r, UINT8 g, UINT8 b,
                               UINTN numerator, UINTN denominator)
{
  GfxDrawGlyphScaledRatioXY(codepoint, x, y, MakeColor(r, g, b),
                            numerator, denominator, numerator, denominator);
}

VOID UiGfxGetCursor(OUT UINTN* Col, OUT UINTN* Row)
{
  if (Col) *Col = gCursorX;
  if (Row) *Row = gCursorY;
}

VOID UiGfxDrawAsciiAt(UINTN x, UINTN y, CONST CHAR8* str, UINT8 r, UINT8 g, UINT8 b)
{
  if (!str || !gGfxReady || !gBackBuffer) {
    return;
  }
  UINT32 color = MakeColor(r, g, b);
  while (*str) {
    GfxDrawGlyphScaledXY((UINT32)(UINT8)*str, x, y, color, gTextScaleX, gTextScaleY);
    x += gCellW;
    str++;
  }
}

VOID UiGfxDrawUnicodeAt(UINTN x, UINTN y, CONST CHAR16* str, UINT8 r, UINT8 g, UINT8 b)
{
  if (!str || !gGfxReady || !gBackBuffer) {
    return;
  }
  UINT32 color = MakeColor(r, g, b);
  while (*str) {
    UINT32 codepoint = (UINT32)*str;
    CONST CHAR8* fallback = (codepoint > 0x7E) ? FallbackText(codepoint) : NULL;
    if (fallback != NULL) {
      while (*fallback) {
        GfxDrawGlyphScaledXY((UINT32)(UINT8)*fallback, x, y, color, gTextScaleX, gTextScaleY);
        x += gCellW;
        fallback++;
      }
    } else {
      GfxDrawGlyphScaledXY(codepoint, x, y, color, gTextScaleX, gTextScaleY);
      x += gCellW;
    }
    str++;
  }
}

VOID UiGfxFlush(VOID)
{
  if (!gGfxReady || !gGop || !gBackBuffer) {
    return;
  }
  gGop->Blt(
    gGop,
    (EFI_GRAPHICS_OUTPUT_BLT_PIXEL*)gBackBuffer,
    EfiBltBufferToVideo,
    0, 0,
    0, 0,
    gFbWidth, gFbHeight,
    gFbWidth * sizeof(UINT32)
  );
}
