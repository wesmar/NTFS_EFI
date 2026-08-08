// SelfTest.h - scripted, screenless run of the file operations, for the harness.
#pragma once

#include <Uefi.h>

// EC_SELFTEST is a build switch, not a runtime one. The release build sets it
// to 0, the whole module compiles to nothing and the call below disappears -
// so a test path can never be reached on a rescue stick, and there is nothing
// to strip out by hand later. Turn it on with build.ps1 -SelfTest.
#ifndef EC_SELFTEST
#define EC_SELFTEST 0
#endif

#if EC_SELFTEST

// Runs the scripted checks when the volume EC booted from carries \_ECTEST.on,
// writes \_ECTEST_RESULT.txt beside it and returns TRUE, which tells main to
// exit instead of drawing panels. Without the flag file it does nothing and
// returns FALSE, so a self-test build still works as a normal file manager.
BOOLEAN EcSelfTestMaybeRun(IN EFI_HANDLE ImageHandle);

#else

#define EcSelfTestMaybeRun(ImageHandle)  (FALSE)

#endif
