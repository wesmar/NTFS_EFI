// Console.h - the Ctrl+O command line: panels out of the way, a prompt in.
#pragma once

#include <Uefi.h>
#include "Panel.h"

// Most a command line is allowed to hold, and the most words it is split into.
// Both are bounds rather than budgets: a longer line is refused with a message,
// never truncated into a command that means something else.
#define CONSOLE_LINE_CHARS 256
#define CONSOLE_MAX_ARGS   12
#define CONSOLE_HISTORY    16

/*
 * Hands the screen to a text-mode prompt and runs commands until the user
 * leaves with "exit" or a second Ctrl+O. TextMode is the console mode EC saved
 * before it took the screen with GOP; it is restored for the duration.
 *
 * The active panel's path is the working directory: "cd" moves it, and the
 * panel is left standing wherever the console finished, so leaving the prompt
 * does not undo the navigation done at it.
 *
 * Commands EC does not recognise are handed to EFI_SHELL_PROTOCOL when the
 * firmware has one, which it does when EC was itself started from the UEFI
 * Shell. Booted straight as BOOTX64.EFI there is no shell, and the built-in
 * commands are all there is - which is the case this exists for.
 */
VOID ConsoleRun(
  IN     EFI_HANDLE ImageHandle,
  IN     INT32 TextMode,
  IN OUT PANEL* ActivePanel
);

/*
 * Splits a command line into words. Double quotes group a word containing
 * spaces and are dropped from it; everything else is whitespace-separated.
 * Returns the number of words, at most CONSOLE_MAX_ARGS. Line is written
 * through - the words point into it.
 *
 * Exposed because it is pure text work with no screen behind it, which is
 * exactly the part the self-test can drive.
 */
UINTN ConsoleSplitArgs(
  IN OUT CHAR16* Line,
  OUT    CHAR16** Args,
  IN     UINTN MaxArgs
);

/*
 * Turns a typed path into a full EC path. A word holding ':' is taken as it
 * stands; one starting with '\' is taken from the root of the working
 * directory's volume; anything else is joined to the working directory itself.
 * Returns FALSE when the result would not fit.
 */
BOOLEAN ConsoleResolvePath(
  IN  CONST CHAR16* WorkingDir,
  IN  CONST CHAR16* Typed,
  OUT CHAR16* Out,
  IN  UINTN OutChars
);
