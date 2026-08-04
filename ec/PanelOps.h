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
BOOLEAN PanelOpsIsUsableItem(IN CONST FS_FILE_ITEM* Item);
BOOLEAN PanelOpsMatchMask(IN CONST CHAR16* Name, IN CONST CHAR16* Mask);
