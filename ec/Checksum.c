// Checksum.c - self-contained SHA-256 and CRC32, streamed from EFI files.
#include "Checksum.h"
#include "FileSystem.h"

#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

typedef struct {
  UINT32 State[8];
  UINT64 TotalBytes;
  UINT8  Block[64];
  UINTN  BlockUsed;
} SHA256_CONTEXT;

static CONST UINT32 gSha256K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static UINT32 Ror32(UINT32 Value, UINTN Count)
{
  return (Value >> Count) | (Value << (32 - Count));
}

static VOID Sha256Transform(IN OUT SHA256_CONTEXT* Context, IN CONST UINT8 Block[64])
{
  UINT32 w[64];
  UINT32 a, b, c, d, e, f, g, h;

  for (UINTN i = 0; i < 16; i++) {
    w[i] = ((UINT32)Block[i * 4] << 24) | ((UINT32)Block[i * 4 + 1] << 16) |
           ((UINT32)Block[i * 4 + 2] << 8) | (UINT32)Block[i * 4 + 3];
  }
  for (UINTN i = 16; i < 64; i++) {
    UINT32 s0 = Ror32(w[i - 15], 7) ^ Ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    UINT32 s1 = Ror32(w[i - 2], 17) ^ Ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  a = Context->State[0]; b = Context->State[1]; c = Context->State[2]; d = Context->State[3];
  e = Context->State[4]; f = Context->State[5]; g = Context->State[6]; h = Context->State[7];
  for (UINTN i = 0; i < 64; i++) {
    UINT32 s1 = Ror32(e, 6) ^ Ror32(e, 11) ^ Ror32(e, 25);
    UINT32 choose = (e & f) ^ ((~e) & g);
    UINT32 t1 = h + s1 + choose + gSha256K[i] + w[i];
    UINT32 s0 = Ror32(a, 2) ^ Ror32(a, 13) ^ Ror32(a, 22);
    UINT32 majority = (a & b) ^ (a & c) ^ (b & c);
    UINT32 t2 = s0 + majority;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  Context->State[0] += a; Context->State[1] += b; Context->State[2] += c; Context->State[3] += d;
  Context->State[4] += e; Context->State[5] += f; Context->State[6] += g; Context->State[7] += h;
}

static VOID Sha256Init(OUT SHA256_CONTEXT* Context)
{
  STATIC CONST UINT32 Initial[8] = {
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
  };
  ZeroMem(Context, sizeof(*Context));
  CopyMem(Context->State, Initial, sizeof(Initial));
}

static VOID Sha256Update(IN OUT SHA256_CONTEXT* Context, IN CONST UINT8* Data, IN UINTN Length)
{
  Context->TotalBytes += Length;
  while (Length > 0) {
    UINTN room = 64 - Context->BlockUsed;
    UINTN take = Length < room ? Length : room;
    CopyMem(&Context->Block[Context->BlockUsed], Data, take);
    Context->BlockUsed += take;
    Data += take;
    Length -= take;
    if (Context->BlockUsed == 64) {
      Sha256Transform(Context, Context->Block);
      Context->BlockUsed = 0;
    }
  }
}

static VOID Sha256Final(IN OUT SHA256_CONTEXT* Context, OUT UINT8 Digest[32])
{
  UINT64 bitLength = Context->TotalBytes * 8;
  Context->Block[Context->BlockUsed++] = 0x80;
  if (Context->BlockUsed > 56) {
    while (Context->BlockUsed < 64) Context->Block[Context->BlockUsed++] = 0;
    Sha256Transform(Context, Context->Block);
    Context->BlockUsed = 0;
  }
  while (Context->BlockUsed < 56) Context->Block[Context->BlockUsed++] = 0;
  for (UINTN i = 0; i < 8; i++) {
    Context->Block[56 + i] = (UINT8)(bitLength >> (56 - i * 8));
  }
  Sha256Transform(Context, Context->Block);
  for (UINTN i = 0; i < 8; i++) {
    Digest[i * 4]     = (UINT8)(Context->State[i] >> 24);
    Digest[i * 4 + 1] = (UINT8)(Context->State[i] >> 16);
    Digest[i * 4 + 2] = (UINT8)(Context->State[i] >> 8);
    Digest[i * 4 + 3] = (UINT8)Context->State[i];
  }
}

static UINT32 Crc32Update(UINT32 Crc, IN CONST UINT8* Data, UINTN Length)
{
  for (UINTN i = 0; i < Length; i++) {
    Crc ^= Data[i];
    for (UINTN bit = 0; bit < 8; bit++) {
      Crc = (Crc >> 1) ^ (0xedb88320U & (UINT32)(0 - (Crc & 1)));
    }
  }
  return Crc;
}

EFI_STATUS ChecksumFile(IN CONST CHAR16* Path, OUT EC_FILE_CHECKSUM* Result)
{
  FS_VOLUME* volume;
  EFI_FILE_PROTOCOL* root = NULL;
  EFI_FILE_PROTOCOL* file = NULL;
  VOID* buffer = NULL;
  CONST CHAR16* subPath;
  EFI_STATUS status;
  SHA256_CONTEXT sha;
  UINT32 crc = 0xffffffffU;

  if (Path == NULL || Result == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(Result, sizeof(*Result));
  volume = FsFindVolumeForPath(Path);
  if (volume == NULL) return EFI_NOT_FOUND;
  subPath = Path;
  while (*subPath != L'\0' && *subPath != L':') subPath++;
  if (*subPath != L':') return EFI_INVALID_PARAMETER;
  subPath++;

  status = volume->Sfs->OpenVolume(volume->Sfs, &root);
  if (EFI_ERROR(status)) return status;
  status = root->Open(root, &file, (CHAR16*)subPath, EFI_FILE_MODE_READ, 0);
  root->Close(root);
  if (EFI_ERROR(status) || file == NULL) return status;

  buffer = AllocatePool(64 * 1024);
  if (buffer == NULL) {
    file->Close(file);
    return EFI_OUT_OF_RESOURCES;
  }
  Sha256Init(&sha);
  for (;;) {
    UINTN readSize = 64 * 1024;
    status = file->Read(file, &readSize, buffer);
    if (EFI_ERROR(status) || readSize == 0) break;
    Sha256Update(&sha, buffer, readSize);
    crc = Crc32Update(crc, buffer, readSize);
    Result->Size += readSize;
  }
  FreePool(buffer);
  file->Close(file);
  if (EFI_ERROR(status)) return status;
  Sha256Final(&sha, Result->Sha256);
  Result->Crc32 = ~crc;
  return EFI_SUCCESS;
}

BOOLEAN ChecksumEqual(IN CONST EC_FILE_CHECKSUM* A, IN CONST EC_FILE_CHECKSUM* B)
{
  return A != NULL && B != NULL && A->Size == B->Size &&
         CompareMem(A->Sha256, B->Sha256, sizeof(A->Sha256)) == 0;
}

VOID ChecksumSha256ToText(IN CONST UINT8 Digest[32], OUT CHAR16 Text[65])
{
  STATIC CONST CHAR16 Hex[] = L"0123456789abcdef";
  if (Digest == NULL || Text == NULL) return;
  for (UINTN i = 0; i < 32; i++) {
    Text[i * 2] = Hex[Digest[i] >> 4];
    Text[i * 2 + 1] = Hex[Digest[i] & 15];
  }
  Text[64] = L'\0';
}

