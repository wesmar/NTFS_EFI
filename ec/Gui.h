// Gui.h — GUI drawing functions for panels, menus, dialogs, and progress bars.
#pragma once

#include <Uefi.h>
#include "Panel.h"

// Draws the main dual panels
VOID GuiDrawPanels(
  IN PANEL* LeftPanel,
  IN PANEL* RightPanel,
  IN BOOLEAN LeftActive
);

// Draws the bottom function keys menu (F1..F10)
VOID GuiDrawBottomMenu(VOID);

// Draws a copying progress overlay box
VOID GuiDrawCopyProgress(
  IN CONST CHAR16* SrcPath,
  IN CONST CHAR16* DstPath,
  IN UINT64 BytesCopied,
  IN UINT64 TotalBytes
);

// Draws an interactive text input box dialog (returns TRUE if submitted, FALSE if cancelled)
BOOLEAN GuiDrawInputBox(
  IN  CONST CHAR16* Title,
  IN  CONST CHAR16* Prompt,
  OUT CHAR16* Buffer,
  IN  UINTN BufferSize
);

// Draws a simple message dialog box
VOID GuiDrawMsgBox(
  IN CONST CHAR16* Title,
  IN CONST CHAR16* Message
);

// Draws the built-in help dialog
VOID GuiDrawHelp(VOID);

// Draws a confirmation dialog. Returns: 1 = Yes, 2 = No, 3 = Yes to All, 4 = No/Skip to All, 0 = Cancel / Esc
UINTN GuiDrawConfirmDialog(
  IN CONST CHAR16* Title,
  IN CONST CHAR16* Prompt,
  IN BOOLEAN ShowAllOption
);

// Returns the page size (number of visible files) for a given panel height
UINTN GuiGetPageSize(UINTN PanelHeight);

// Draws a rectangle border line
VOID DrawBorder(UINTN x, UINTN y, UINTN w, UINTN h, UINT8 r, UINT8 g, UINT8 b, UINTN thickness);
