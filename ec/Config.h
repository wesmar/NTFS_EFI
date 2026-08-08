// Config.h - EC.ini runtime options.
#pragma once

#include <Uefi.h>
#include "FileSystem.h"

#define EC_MAX_HOTDIRS 9

typedef struct {
  BOOLEAN ConfirmDelete;
  BOOLEAN ConfirmOverwrite;
  BOOLEAN ShowSuccessMessages;
  BOOLEAN ShowOperationSummary;
  BOOLEAN VerifyAfterCopy;
  CHAR16 DefaultLeft[MAX_PATH_LEN];
  CHAR16 DefaultRight[MAX_PATH_LEN];
  CHAR16 FilterLeft[128];
  CHAR16 FilterRight[128];
  CHAR16 AppDir[MAX_PATH_LEN];
  // Value as entered in EC.ini. NtfsDriverPath below is its ESP-resolved form.
  CHAR16 NtfsDriverSetting[MAX_PATH_LEN];
  CHAR16 NtfsDriverPath[MAX_PATH_LEN];
  CHAR16 HotDirs[EC_MAX_HOTDIRS][MAX_PATH_LEN];
  UINTN HotDirCount;
} EC_CONFIG;

extern EC_CONFIG gEcConfig;

VOID ConfigLoad(IN EFI_HANDLE ImageHandle);
EFI_STATUS ConfigSave(VOID);
