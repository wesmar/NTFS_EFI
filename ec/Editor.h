// Editor.h — Modal HxD-style Binary/Hex Editor.
#pragma once

#include <Uefi.h>

// Launches the modal hex editor for a given file path
VOID EditorShow(
  IN EFI_HANDLE ImageHandle,
  IN CONST CHAR16* Path
);
