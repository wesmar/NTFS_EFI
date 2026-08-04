// Viewer.h — Modal file viewer supporting Text (ASCII) and Hex display modes.
#pragma once

#include <Uefi.h>

// Launches the modal file viewer for a given file path
VOID ViewerShow(
  IN EFI_HANDLE ImageHandle,
  IN CONST CHAR16* Path
);
