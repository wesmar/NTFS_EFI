// Viewer.h — Modal file viewer supporting Text (ASCII) and Hex display modes.
#pragma once

#include <Uefi.h>

// Launches the modal file viewer for a given file path
VOID ViewerShow(
  IN EFI_HANDLE ImageHandle,
  IN CONST CHAR16* Path
);

/*
 * The find-in-file engine on its own. It is pure byte work with no screen and
 * no allocation, which is exactly the part worth testing without a keyboard,
 * so the self-test drives these two directly.
 */

// First case-insensitive match of Needle at or after From. TRUE when found.
BOOLEAN ViewerFindBytes(
  IN  CONST UINT8* Data,
  IN  UINT64 Size,
  IN  CONST UINT8* Needle,
  IN  UINTN NeedleLen,
  IN  UINT64 From,
  OUT UINT64* Found
);

// Narrows a typed UCS-2 needle to bytes. FALSE for an empty needle or for
// anything outside ASCII, which would otherwise truncate into a wrong match.
BOOLEAN ViewerNeedleToBytes(
  IN  CONST CHAR16* Text,
  OUT UINT8* Bytes,
  IN  UINTN MaxBytes,
  OUT UINTN* Length
);
