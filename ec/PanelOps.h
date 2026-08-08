// PanelOps.h - panel selection, masks, and quick search helpers.
#pragma once

#include <Uefi.h>
#include "Panel.h"

UINTN PanelOpsCountSelected(IN CONST PANEL* Panel);
VOID  PanelOpsClearSelection(IN PANEL* Panel);
UINTN PanelOpsInvertSelection(IN PANEL* Panel);
UINTN PanelOpsSelectByMask(IN PANEL* Panel, IN CONST CHAR16* Mask, IN BOOLEAN Select);
BOOLEAN PanelOpsFindNext(IN OUT PANEL* Panel, IN CONST CHAR16* Pattern, IN BOOLEAN StartAfterCurrent);
BOOLEAN PanelOpsFindPrefixNext(IN OUT PANEL* Panel, IN CONST CHAR16* Prefix, IN BOOLEAN StartAfterCurrent);
/*
 * Select, on both sides at once, everything that differs between the two
 * panels: an entry the other side does not have at all, or a file that is
 * there under the same name with a different size or a different modification
 * time. Identical pairs are left unselected, so what stays lit is exactly what
 * would need copying to make the two directories agree.
 *
 * Directories are compared by presence only - a directory's own size and date
 * say nothing about what is inside it, and pretending otherwise would light up
 * every folder on a freshly copied tree.
 *
 * Returns the number of entries selected across both panels.
 */
UINTN PanelOpsCompareSelect(IN OUT PANEL* Left, IN OUT PANEL* Right);

BOOLEAN PanelOpsIsUsableItem(IN CONST FS_FILE_ITEM* Item);
BOOLEAN PanelOpsMatchMask(IN CONST CHAR16* Name, IN CONST CHAR16* Mask);
