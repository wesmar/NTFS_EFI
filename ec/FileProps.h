// FileProps.h - view and edit the DOS attributes and modification time of a file.
#pragma once

#include <Uefi.h>

// Shows what the file record actually holds and lets the four DOS attribute
// bits and the modification time be changed. Returns TRUE when something was
// written, so the caller knows to refresh its panel.
BOOLEAN FilePropsEdit(IN CONST CHAR16* Path);

// Parses "YYYY-MM-DD HH:MM:SS" (seconds optional) into an EFI_TIME. Split out
// from the dialog so the self-test can exercise it without a screen.
BOOLEAN FilePropsParseTime(IN CONST CHAR16* Text, OUT EFI_TIME* Time);
