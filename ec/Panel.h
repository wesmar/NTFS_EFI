// Panel.h — Panel state and directory navigation logic.
#pragma once

#include <Uefi.h>
#include "FileSystem.h"

typedef enum {
  PANEL_SORT_NAME = 0,
  PANEL_SORT_EXTENSION,
  PANEL_SORT_MODIFIED,
  PANEL_SORT_SIZE
} PANEL_SORT_MODE;

typedef struct {
  CHAR16 Path[MAX_PATH_LEN];
  CHAR16 FilterMask[128];
  FS_FILE_ITEM* Files;
  UINTN FileCount;
  INTN SelectedIndex; // Index of the currently highlighted file, -1 if empty
  UINTN TopIndex;     // Scroll offset (index of the file drawn at the top of the panel)
  PANEL_SORT_MODE SortMode;
  BOOLEAN SortDescending;

  /*
   * Volume figures for the status line, read when the listing is read rather
   * than on every frame. The footer used to ask the volume for its free space
   * on each redraw, which is once per keystroke per panel - two round trips
   * through the file system driver to print a number that only changes when
   * something is written, and every write path refreshes the panel anyway.
   */
  UINT64 VolumeTotal;
  UINT64 VolumeFree;
  CHAR16 VolumeLabel[32];
} PANEL;

// Initializes a panel with a default path
VOID PanelInit(PANEL* Panel, IN CONST CHAR16* DefaultPath);

// Frees the allocated memory for files in a panel
VOID PanelFree(PANEL* Panel);

// Re-reads directory contents and updates panel state
EFI_STATUS PanelRefresh(PANEL* Panel);

// Re-reads directory and tries PreferredName first; if missing, keeps nearest old index.
EFI_STATUS PanelRefreshKeep(PANEL* Panel, IN CONST CHAR16* PreferredName, IN INTN FallbackIndex);

// Changes sort mode and reorders current panel contents
VOID PanelSetSortMode(PANEL* Panel, PANEL_SORT_MODE SortMode);

// Sets a wildcard display filter such as "*.efi"; "*" or empty disables it.
VOID PanelSetFilter(PANEL* Panel, IN CONST CHAR16* Mask);

// Moves selection up
VOID PanelNavigateUp(PANEL* Panel);

// Moves selection down
VOID PanelNavigateDown(PANEL* Panel, UINTN PageSize);

// Pages selection up
VOID PanelPageUp(PANEL* Panel, UINTN PageSize);

// Pages selection down
VOID PanelPageDown(PANEL* Panel, UINTN PageSize);

// Handles Enter press on the selected item (enters dir or starts EFI app)
VOID PanelEnter(PANEL* Panel, EFI_HANDLE ImageHandle);
