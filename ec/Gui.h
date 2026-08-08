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

// Replaces the passive panel with a read-only preview of the active item.
VOID GuiDrawQuickView(IN PANEL* SourcePanel, IN BOOLEAN DrawOnLeft);
VOID GuiQuickViewReset(VOID);

// Draws the bottom function-key legend: key name above, action below
VOID GuiDrawBottomMenu(VOID);

// How tall that legend is at the current resolution. Anything that lays out
// against the bottom of the screen asks here rather than guessing.
UINTN GuiMenuBarHeight(VOID);

// Height passed to panel drawing/page calculations. Kept in one place so the
// tunable gap above the function bar cannot drift between Gui.c and Main.c.
UINTN GuiPanelHeight(VOID);

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

// Draws a scrollable list and lets one entry be chosen. *Chosen supplies the
// initial row and receives the row accepted with Enter; Esc returns FALSE.
BOOLEAN GuiDrawListPicker(
  IN  CONST CHAR16* Title,
  IN  CONST CHAR16** Lines,
  IN  UINTN LineCount,
  OUT UINTN* Chosen
);

// Draws the "searching" overlay. Shown once before a tree walk, which has no
// progress to report beyond the fact that it is running and cancellable.
VOID GuiDrawSearchProgress(
  IN CONST CHAR16* Root,
  IN CONST CHAR16* Mask,
  IN UINTN DirsVisited
);

// Returns the page size (number of visible files) for a given panel height
UINTN GuiGetPageSize(UINTN PanelHeight);

// Text helpers shared with the dialogs that live outside Gui.c: how many
// characters fit a pixel width, and a clipped single-line draw.
UINTN GuiCharsForWidth(IN UINTN WidthPixels, IN UINTN CellW);

VOID GuiDrawUnicodeClippedAt(
  IN UINTN X,
  IN UINTN Y,
  IN CONST CHAR16* Text,
  IN UINTN MaxChars,
  IN UINT8 R,
  IN UINT8 G,
  IN UINT8 B
);

// The width a dialog should use: what it asks for, capped to what the screen
// can spare with a margin kept clear.
UINTN GuiDialogWidth(IN UINTN Preferred);

// The height a dialog should use, given how many text rows it wants.
UINTN GuiDialogHeight(IN UINTN Rows);

// Draws a rectangle border line
VOID DrawBorder(UINTN x, UINTN y, UINTN w, UINTN h, UINT8 r, UINT8 g, UINT8 b, UINTN thickness);
