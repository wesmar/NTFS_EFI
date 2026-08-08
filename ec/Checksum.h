// Checksum.h - streaming file checksums used by copy verification and tools.
#pragma once

#include <Uefi.h>

typedef struct {
  UINT8  Sha256[32];
  UINT32 Crc32;
  UINT64 Size;
} EC_FILE_CHECKSUM;

EFI_STATUS ChecksumFile(IN CONST CHAR16* Path, OUT EC_FILE_CHECKSUM* Result);
BOOLEAN ChecksumEqual(IN CONST EC_FILE_CHECKSUM* A, IN CONST EC_FILE_CHECKSUM* B);
VOID ChecksumSha256ToText(IN CONST UINT8 Digest[32], OUT CHAR16 Text[65]);

