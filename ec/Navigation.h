// Navigation.h - per-panel path history and EC.ini hotlist support.
#pragma once

#include <Uefi.h>
#include "Panel.h"

#define NAV_HISTORY_MAX 16

typedef struct {
  CHAR16 Entries[NAV_HISTORY_MAX][MAX_PATH_LEN];
  UINTN Count;
  UINTN Current;
} NAV_HISTORY;

VOID NavHistoryInit(OUT NAV_HISTORY* History, IN CONST CHAR16* InitialPath);
VOID NavHistoryPush(IN OUT NAV_HISTORY* History, IN CONST CHAR16* Path);
BOOLEAN NavHistoryBack(IN OUT NAV_HISTORY* History, OUT CHAR16* Path);
BOOLEAN NavHistoryForward(IN OUT NAV_HISTORY* History, OUT CHAR16* Path);
BOOLEAN NavApplyPath(IN OUT PANEL* Panel, IN OUT NAV_HISTORY* History, IN CONST CHAR16* Path);
BOOLEAN NavShowHotlist(IN OUT PANEL* Panel, IN OUT NAV_HISTORY* History);
