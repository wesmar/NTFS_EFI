#pragma once
#include <Uefi.h>

// Set to 1 to enable logging system, or 0 to completely compile it out.
#ifndef ENABLE_DEBUG_LOG
#define ENABLE_DEBUG_LOG 0
#endif

#if ENABLE_DEBUG_LOG

EFI_STATUS DebugLog_Init(IN EFI_HANDLE ImageHandle, IN BOOLEAN Enable);
VOID       DebugLog_Close(VOID);
VOID       DebugLog_Reopen(VOID);
VOID       DebugLog_Write(IN CONST CHAR8 *Text);
VOID       DebugLog_Print(IN CONST CHAR8 *Fmt, ...);
VOID       DebugLog_SetNoFlush(IN BOOLEAN NoFlush);
VOID       DebugLog_Flush(VOID);

#else

#define DebugLog_Init(ImageHandle, Enable)   (EFI_SUCCESS)
#define DebugLog_Close()                     ((VOID)0)
#define DebugLog_Reopen()                    ((VOID)0)
#define DebugLog_Write(Text)                 ((VOID)0)
#define DebugLog_Print(Fmt, ...)             ((VOID)0)
#define DebugLog_SetNoFlush(NoFlush)         ((VOID)0)
#define DebugLog_Flush()                     ((VOID)0)

#endif
