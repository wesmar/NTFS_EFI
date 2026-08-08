// Config.c - small EC.ini parser loaded from the boot ESP.
#include "Config.h"

#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Guid/FileInfo.h>

EC_CONFIG gEcConfig;

static EFI_HANDLE gConfigDeviceHandle = NULL;
static CHAR16 gConfigIniPath[MAX_PATH_LEN];

static VOID ConfigSetDefaults(VOID)
{
  ZeroMem(&gEcConfig, sizeof(gEcConfig));
  gEcConfig.FilterLeft[0] = L'*';
  gEcConfig.FilterRight[0] = L'*';
}

static VOID ConfigCopyPath(OUT CHAR16* Dest, IN UINTN DestChars, IN CONST CHAR16* Src)
{
  UINTN Out = 0;

  if (Dest == NULL || DestChars == 0) return;
  if (Src == NULL) {
    Dest[0] = L'\0';
    return;
  }

  while (Out + 1 < DestChars && Src[Out] != L'\0') {
    CHAR16 Ch = Src[Out];
    Dest[Out] = (Ch == L'/') ? L'\\' : Ch;
    Out++;
  }
  Dest[Out] = L'\0';
}

static VOID ConfigTrimLeadingSlash(IN OUT CHAR16* Path)
{
  UINTN In = 0;
  UINTN Out = 0;

  if (Path == NULL) return;
  while (Path[In] == L'\\' || Path[In] == L'/') In++;
  if (In == 0) return;

  while (Path[In] != L'\0') {
    Path[Out++] = Path[In++];
  }
  Path[Out] = L'\0';
}

static BOOLEAN ConfigPathStartsWithSlash(IN CONST CHAR16* Path)
{
  return Path != NULL && (Path[0] == L'\\' || Path[0] == L'/');
}

static VOID ConfigJoinPath(
  OUT CHAR16* Dest,
  IN UINTN DestChars,
  IN CONST CHAR16* Dir,
  IN CONST CHAR16* FileName
) {
  UINTN Out = 0;

  if (Dest == NULL || DestChars == 0) return;
  Dest[0] = L'\0';
  if (FileName == NULL) return;

  if (Dir != NULL && Dir[0] != L'\0') {
    while (Out + 1 < DestChars && Dir[Out] != L'\0') {
      CHAR16 Ch = Dir[Out];
      Dest[Out] = (Ch == L'/') ? L'\\' : Ch;
      Out++;
    }
    if (Out > 0 && Out + 1 < DestChars && Dest[Out - 1] != L'\\') {
      Dest[Out++] = L'\\';
    }
  }

  for (UINTN i = 0; Out + 1 < DestChars && FileName[i] != L'\0'; i++) {
    CHAR16 Ch = FileName[i];
    Dest[Out++] = (Ch == L'/') ? L'\\' : Ch;
  }
  Dest[Out] = L'\0';
}

static VOID ConfigSetAppDir(IN EFI_LOADED_IMAGE_PROTOCOL* LoadedImage)
{
  EFI_DEVICE_PATH_PROTOCOL* Node;
  CHAR16 ImagePath[MAX_PATH_LEN];
  INTN LastSlash = -1;

  gEcConfig.AppDir[0] = L'\0';
  if (LoadedImage == NULL || LoadedImage->FilePath == NULL) return;

  Node = LoadedImage->FilePath;
  while (!IsDevicePathEnd(Node)) {
    if (DevicePathType(Node) == MEDIA_DEVICE_PATH && DevicePathSubType(Node) == MEDIA_FILEPATH_DP) {
      FILEPATH_DEVICE_PATH* FilePath = (FILEPATH_DEVICE_PATH*)Node;
      ConfigCopyPath(ImagePath, MAX_PATH_LEN, FilePath->PathName);
      ConfigTrimLeadingSlash(ImagePath);

      for (UINTN i = 0; ImagePath[i] != L'\0'; i++) {
        if (ImagePath[i] == L'\\') LastSlash = (INTN)i;
      }

      if (LastSlash > 0) {
        ImagePath[LastSlash] = L'\0';
        ConfigCopyPath(gEcConfig.AppDir, MAX_PATH_LEN, ImagePath);
      }
    }
    Node = NextDevicePathNode(Node);
  }
}

static EFI_STATUS ConfigOpenFromRoot(
  IN EFI_FILE_PROTOCOL* Root,
  IN CONST CHAR16* Path,
  OUT EFI_FILE_PROTOCOL** File
) {
  EFI_STATUS Status;
  CHAR16 CleanPath[MAX_PATH_LEN];

  if (Root == NULL || Path == NULL || File == NULL || Path[0] == L'\0') return EFI_INVALID_PARAMETER;
  ConfigCopyPath(CleanPath, MAX_PATH_LEN, Path);
  ConfigTrimLeadingSlash(CleanPath);
  Status = Root->Open(Root, File, CleanPath, EFI_FILE_MODE_READ, 0);
  return Status;
}

static VOID ConfigResolveNtfsDriverPath(VOID)
{
  CHAR16 RawPath[MAX_PATH_LEN];

  if (gEcConfig.NtfsDriverSetting[0] == L'\0') {
    ConfigJoinPath(gEcConfig.NtfsDriverPath, MAX_PATH_LEN, gEcConfig.AppDir, L"ntfs.efi");
    return;
  }

  ConfigCopyPath(RawPath, MAX_PATH_LEN, gEcConfig.NtfsDriverSetting);
  if (ConfigPathStartsWithSlash(RawPath)) {
    ConfigCopyPath(gEcConfig.NtfsDriverPath, MAX_PATH_LEN, RawPath);
    ConfigTrimLeadingSlash(gEcConfig.NtfsDriverPath);
    return;
  }

  ConfigJoinPath(gEcConfig.NtfsDriverPath, MAX_PATH_LEN, gEcConfig.AppDir, RawPath);
}

static BOOLEAN AsciiEqNoCase(IN CONST CHAR8* A, IN CONST CHAR8* B, IN UINTN Len)
{
  for (UINTN i = 0; i < Len; i++) {
    CHAR8 Ca = A[i];
    CHAR8 Cb = B[i];
    if (Ca >= 'a' && Ca <= 'z') Ca = (CHAR8)(Ca - ('a' - 'A'));
    if (Cb >= 'a' && Cb <= 'z') Cb = (CHAR8)(Cb - ('a' - 'A'));
    if (Ca != Cb) return FALSE;
  }
  return TRUE;
}

static BOOLEAN ParseBool(IN CONST CHAR8* Value, IN UINTN Len, IN BOOLEAN DefaultValue)
{
  while (Len > 0 && (*Value == ' ' || *Value == '\t')) {
    Value++;
    Len--;
  }
  while (Len > 0 && (Value[Len - 1] == ' ' || Value[Len - 1] == '\t' || Value[Len - 1] == '\r')) {
    Len--;
  }

  if (Len == 1 && (Value[0] == '1' || Value[0] == 'Y' || Value[0] == 'y')) return TRUE;
  if (Len == 1 && (Value[0] == '0' || Value[0] == 'N' || Value[0] == 'n')) return FALSE;
  if (Len == 4 && AsciiEqNoCase(Value, "TRUE", 4)) return TRUE;
  if (Len == 5 && AsciiEqNoCase(Value, "FALSE", 5)) return FALSE;
  if (Len == 3 && AsciiEqNoCase(Value, "YES", 3)) return TRUE;
  if (Len == 2 && AsciiEqNoCase(Value, "NO", 2)) return FALSE;
  if (Len == 2 && AsciiEqNoCase(Value, "ON", 2)) return TRUE;
  if (Len == 3 && AsciiEqNoCase(Value, "OFF", 3)) return FALSE;
  return DefaultValue;
}

static VOID ConfigAsciiToUnicode(IN CONST CHAR8* Value, IN UINTN Len, OUT CHAR16* Dest, IN UINTN DestChars)
{
  UINTN Out = 0;
  if (Dest == NULL || DestChars == 0) return;
  if (Value == NULL) {
    Dest[0] = L'\0';
    return;
  }

  while (Out + 1 < DestChars && Out < Len && Value[Out] != '\0') {
    Dest[Out] = (CHAR16)(UINT8)Value[Out];
    Out++;
  }
  Dest[Out] = L'\0';
}

static VOID ApplyKeyValue(IN CONST CHAR8* Key, IN UINTN KeyLen, IN CONST CHAR8* Value, IN UINTN ValueLen)
{
  while (KeyLen > 0 && (*Key == ' ' || *Key == '\t')) {
    Key++;
    KeyLen--;
  }
  while (KeyLen > 0 && (Key[KeyLen - 1] == ' ' || Key[KeyLen - 1] == '\t')) {
    KeyLen--;
  }

  while (ValueLen > 0 && (*Value == ' ' || *Value == '\t')) {
    Value++;
    ValueLen--;
  }
  while (ValueLen > 0 && (Value[ValueLen - 1] == ' ' || Value[ValueLen - 1] == '\t' || Value[ValueLen - 1] == '\r')) {
    ValueLen--;
  }

  if (KeyLen == 13 && AsciiEqNoCase(Key, "ConfirmDelete", 13)) {
    gEcConfig.ConfirmDelete = ParseBool(Value, ValueLen, gEcConfig.ConfirmDelete);
  } else if (KeyLen == 16 && AsciiEqNoCase(Key, "ConfirmOverwrite", 16)) {
    gEcConfig.ConfirmOverwrite = ParseBool(Value, ValueLen, gEcConfig.ConfirmOverwrite);
  } else if (KeyLen == 19 && AsciiEqNoCase(Key, "ShowSuccessMessages", 19)) {
    gEcConfig.ShowSuccessMessages = ParseBool(Value, ValueLen, gEcConfig.ShowSuccessMessages);
  } else if (KeyLen == 20 && AsciiEqNoCase(Key, "ShowOperationSummary", 20)) {
    gEcConfig.ShowOperationSummary = ParseBool(Value, ValueLen, gEcConfig.ShowOperationSummary);
  } else if (KeyLen == 15 && AsciiEqNoCase(Key, "VerifyAfterCopy", 15)) {
    gEcConfig.VerifyAfterCopy = ParseBool(Value, ValueLen, gEcConfig.VerifyAfterCopy);
  } else if (KeyLen == 11 && AsciiEqNoCase(Key, "DefaultLeft", 11)) {
    ConfigAsciiToUnicode(Value, ValueLen, gEcConfig.DefaultLeft, MAX_PATH_LEN);
  } else if (KeyLen == 12 && AsciiEqNoCase(Key, "DefaultRight", 12)) {
    ConfigAsciiToUnicode(Value, ValueLen, gEcConfig.DefaultRight, MAX_PATH_LEN);
  } else if (KeyLen == 10 && AsciiEqNoCase(Key, "FilterLeft", 10)) {
    ConfigAsciiToUnicode(Value, ValueLen, gEcConfig.FilterLeft, 128);
  } else if (KeyLen == 11 && AsciiEqNoCase(Key, "FilterRight", 11)) {
    ConfigAsciiToUnicode(Value, ValueLen, gEcConfig.FilterRight, 128);
  } else if (KeyLen == 14 && AsciiEqNoCase(Key, "NtfsDriverPath", 14)) {
    ConfigAsciiToUnicode(Value, ValueLen, gEcConfig.NtfsDriverSetting, MAX_PATH_LEN);
  } else if (KeyLen == 10 && AsciiEqNoCase(Key, "NtfsDriver", 10)) {
    ConfigAsciiToUnicode(Value, ValueLen, gEcConfig.NtfsDriverSetting, MAX_PATH_LEN);
  } else if (KeyLen == 7 && AsciiEqNoCase(Key, "HotDir", 6) && Key[6] >= '1' && Key[6] <= '9') {
    UINTN Index = (UINTN)(Key[6] - '1');
    ConfigAsciiToUnicode(Value, ValueLen, gEcConfig.HotDirs[Index], MAX_PATH_LEN);
    if (gEcConfig.HotDirs[Index][0] != L'\0' && Index + 1 > gEcConfig.HotDirCount) {
      gEcConfig.HotDirCount = Index + 1;
    }
  }
}

static VOID ParseIni(IN CONST CHAR8* Data, IN UINTN Size)
{
  UINTN Pos = 0;
  while (Pos < Size) {
    UINTN LineStart = Pos;
    UINTN LineEnd;
    UINTN Eq;

    while (Pos < Size && Data[Pos] != '\n') Pos++;
    LineEnd = Pos;
    if (Pos < Size && Data[Pos] == '\n') Pos++;

    while (LineStart < LineEnd && (Data[LineStart] == ' ' || Data[LineStart] == '\t')) LineStart++;
    if (LineStart >= LineEnd || Data[LineStart] == '#' || Data[LineStart] == ';' || Data[LineStart] == '[') {
      continue;
    }

    Eq = LineStart;
    while (Eq < LineEnd && Data[Eq] != '=') Eq++;
    if (Eq >= LineEnd) continue;

    ApplyKeyValue(&Data[LineStart], Eq - LineStart, &Data[Eq + 1], LineEnd - Eq - 1);
  }
}

VOID ConfigLoad(IN EFI_HANDLE ImageHandle)
{
  EFI_LOADED_IMAGE_PROTOCOL* LoadedImage = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Sfs = NULL;
  EFI_FILE_PROTOCOL* Root = NULL;
  EFI_FILE_PROTOCOL* File = NULL;
  EFI_STATUS Status;
  EFI_GUID LoadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_GUID FileInfoGuid = EFI_FILE_INFO_ID;
  UINTN InfoSize = 0;
  EFI_FILE_INFO* Info = NULL;
  CHAR16 IniPath[MAX_PATH_LEN];

  ConfigSetDefaults();
  gConfigDeviceHandle = NULL;
  gConfigIniPath[0] = L'\0';

  Status = gBS->OpenProtocol(
    ImageHandle,
    &LoadedImageGuid,
    (VOID**)&LoadedImage,
    ImageHandle,
    NULL,
    EFI_OPEN_PROTOCOL_GET_PROTOCOL
  );
  if (EFI_ERROR(Status) || LoadedImage == NULL) return;
  ConfigSetAppDir(LoadedImage);
  gConfigDeviceHandle = LoadedImage->DeviceHandle;

  Status = gBS->HandleProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Sfs);
  if (EFI_ERROR(Status) || Sfs == NULL) return;

  Status = Sfs->OpenVolume(Sfs, &Root);
  if (EFI_ERROR(Status) || Root == NULL) return;

  ConfigJoinPath(IniPath, MAX_PATH_LEN, gEcConfig.AppDir, L"EC.ini");
  ConfigCopyPath(gConfigIniPath, MAX_PATH_LEN, IniPath);
  Status = ConfigOpenFromRoot(Root, IniPath, &File);
  if (EFI_ERROR(Status) && gEcConfig.AppDir[0] != L'\0') {
    Status = ConfigOpenFromRoot(Root, L"EC.ini", &File);
    if (!EFI_ERROR(Status)) ConfigCopyPath(gConfigIniPath, MAX_PATH_LEN, L"EC.ini");
  }
  if (EFI_ERROR(Status) && StrCmp(gEcConfig.AppDir, L"EFI\\BOOT") != 0) {
    Status = ConfigOpenFromRoot(Root, L"EFI\\BOOT\\EC.ini", &File);
    if (!EFI_ERROR(Status)) ConfigCopyPath(gConfigIniPath, MAX_PATH_LEN, L"EFI\\BOOT\\EC.ini");
  }
  Root->Close(Root);
  if (EFI_ERROR(Status) || File == NULL) {
    ConfigResolveNtfsDriverPath();
    return;
  }

  Status = File->GetInfo(File, &FileInfoGuid, &InfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Info = AllocatePool(InfoSize);
    if (Info != NULL) {
      Status = File->GetInfo(File, &FileInfoGuid, &InfoSize, Info);
    }
  }

  if (!EFI_ERROR(Status) && Info != NULL && Info->FileSize > 0 && Info->FileSize < 65536) {
    CHAR8* Buffer = AllocatePool((UINTN)Info->FileSize + 1);
    if (Buffer != NULL) {
      UINTN ReadSize = (UINTN)Info->FileSize;
      Status = File->Read(File, &ReadSize, Buffer);
      if (!EFI_ERROR(Status)) {
        Buffer[ReadSize] = '\0';
        ParseIni(Buffer, ReadSize);
      }
      FreePool(Buffer);
    }
  }

  if (Info != NULL) FreePool(Info);
  File->Close(File);

  ConfigResolveNtfsDriverPath();
}

#define CONFIG_SAVE_CAPACITY 32768

static BOOLEAN ConfigAppendAscii(
  IN OUT CHAR8* Buffer,
  IN UINTN Capacity,
  IN OUT UINTN* Position,
  IN CONST CHAR8* Text
) {
  UINTN Pos;

  if (Buffer == NULL || Position == NULL || Text == NULL) return FALSE;
  Pos = *Position;
  while (*Text != '\0') {
    if (Pos + 1 >= Capacity) return FALSE;
    Buffer[Pos++] = *Text++;
  }
  Buffer[Pos] = '\0';
  *Position = Pos;
  return TRUE;
}

static BOOLEAN ConfigAppendUnicode(
  IN OUT CHAR8* Buffer,
  IN UINTN Capacity,
  IN OUT UINTN* Position,
  IN CONST CHAR16* Text
) {
  UINTN Pos;

  if (Buffer == NULL || Position == NULL || Text == NULL) return FALSE;
  Pos = *Position;
  while (*Text != L'\0') {
    if (Pos + 1 >= Capacity) return FALSE;
    // EC paths and masks are firmware-facing and normally ASCII. Keep the INI
    // valid even if a firmware input method supplies a wider character.
    Buffer[Pos++] = (*Text <= 0x7f) ? (CHAR8)*Text : '?';
    Text++;
  }
  Buffer[Pos] = '\0';
  *Position = Pos;
  return TRUE;
}

static BOOLEAN ConfigAppendValue(
  IN OUT CHAR8* Buffer,
  IN UINTN Capacity,
  IN OUT UINTN* Position,
  IN CONST CHAR8* Key,
  IN CONST CHAR16* Value
) {
  return ConfigAppendAscii(Buffer, Capacity, Position, Key) &&
         ConfigAppendAscii(Buffer, Capacity, Position, "=") &&
         ConfigAppendUnicode(Buffer, Capacity, Position, Value) &&
         ConfigAppendAscii(Buffer, Capacity, Position, "\r\n");
}

EFI_STATUS ConfigSave(VOID)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Sfs = NULL;
  EFI_FILE_PROTOCOL* Root = NULL;
  EFI_FILE_PROTOCOL* File = NULL;
  EFI_FILE_INFO* Info = NULL;
  EFI_GUID FileInfoGuid = EFI_FILE_INFO_ID;
  EFI_STATUS Status;
  CHAR8* Buffer;
  UINTN Position = 0;
  UINTN InfoSize = 0;
  UINTN WriteSize;
  CHAR8 HotDirKey[] = "HotDir1";

  if (gConfigDeviceHandle == NULL || gConfigIniPath[0] == L'\0') return EFI_NOT_READY;

  Buffer = AllocateZeroPool(CONFIG_SAVE_CAPACITY);
  if (Buffer == NULL) return EFI_OUT_OF_RESOURCES;

  if (!ConfigAppendAscii(Buffer, CONFIG_SAVE_CAPACITY, &Position,
        "; EC.ini - saved by the EC F9 settings menu.\r\n"
        "; Values: 0/1. Paths use UEFI volume names such as fs0:\\\r\n\r\n") ||
      !ConfigAppendAscii(Buffer, CONFIG_SAVE_CAPACITY, &Position,
        gEcConfig.ConfirmDelete ? "ConfirmDelete=1\r\n" : "ConfirmDelete=0\r\n") ||
      !ConfigAppendAscii(Buffer, CONFIG_SAVE_CAPACITY, &Position,
        gEcConfig.ConfirmOverwrite ? "ConfirmOverwrite=1\r\n" : "ConfirmOverwrite=0\r\n") ||
      !ConfigAppendAscii(Buffer, CONFIG_SAVE_CAPACITY, &Position,
        gEcConfig.ShowSuccessMessages ? "ShowSuccessMessages=1\r\n" : "ShowSuccessMessages=0\r\n") ||
      !ConfigAppendAscii(Buffer, CONFIG_SAVE_CAPACITY, &Position,
        gEcConfig.ShowOperationSummary ? "ShowOperationSummary=1\r\n" : "ShowOperationSummary=0\r\n") ||
      !ConfigAppendAscii(Buffer, CONFIG_SAVE_CAPACITY, &Position,
        gEcConfig.VerifyAfterCopy ? "VerifyAfterCopy=1\r\n\r\n" : "VerifyAfterCopy=0\r\n\r\n") ||
      !ConfigAppendValue(Buffer, CONFIG_SAVE_CAPACITY, &Position, "NtfsDriverPath", gEcConfig.NtfsDriverSetting) ||
      !ConfigAppendAscii(Buffer, CONFIG_SAVE_CAPACITY, &Position, "\r\n") ||
      !ConfigAppendValue(Buffer, CONFIG_SAVE_CAPACITY, &Position, "DefaultLeft", gEcConfig.DefaultLeft) ||
      !ConfigAppendValue(Buffer, CONFIG_SAVE_CAPACITY, &Position, "DefaultRight", gEcConfig.DefaultRight) ||
      !ConfigAppendAscii(Buffer, CONFIG_SAVE_CAPACITY, &Position, "\r\n") ||
      !ConfigAppendValue(Buffer, CONFIG_SAVE_CAPACITY, &Position, "FilterLeft", gEcConfig.FilterLeft) ||
      !ConfigAppendValue(Buffer, CONFIG_SAVE_CAPACITY, &Position, "FilterRight", gEcConfig.FilterRight) ||
      !ConfigAppendAscii(Buffer, CONFIG_SAVE_CAPACITY, &Position, "\r\n")) {
    FreePool(Buffer);
    return EFI_BUFFER_TOO_SMALL;
  }

  for (UINTN Index = 0; Index < EC_MAX_HOTDIRS; Index++) {
    HotDirKey[6] = (CHAR8)('1' + Index);
    if (!ConfigAppendValue(Buffer, CONFIG_SAVE_CAPACITY, &Position,
                           HotDirKey, gEcConfig.HotDirs[Index])) {
      FreePool(Buffer);
      return EFI_BUFFER_TOO_SMALL;
    }
  }

  Status = gBS->HandleProtocol(gConfigDeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Sfs);
  if (EFI_ERROR(Status) || Sfs == NULL) {
    FreePool(Buffer);
    return EFI_ERROR(Status) ? Status : EFI_NOT_FOUND;
  }

  Status = Sfs->OpenVolume(Sfs, &Root);
  if (EFI_ERROR(Status) || Root == NULL) {
    FreePool(Buffer);
    return EFI_ERROR(Status) ? Status : EFI_NOT_FOUND;
  }

  Status = Root->Open(Root, &File, gConfigIniPath,
                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR(Status) || File == NULL) {
    Root->Close(Root);
    FreePool(Buffer);
    return EFI_ERROR(Status) ? Status : EFI_NOT_FOUND;
  }

  Status = File->GetInfo(File, &FileInfoGuid, &InfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Info = AllocatePool(InfoSize);
    if (Info == NULL) Status = EFI_OUT_OF_RESOURCES;
    else Status = File->GetInfo(File, &FileInfoGuid, &InfoSize, Info);
  }
  if (!EFI_ERROR(Status) && Info != NULL) {
    Info->FileSize = 0;
    Status = File->SetInfo(File, &FileInfoGuid, InfoSize, Info);
  }
  if (Info != NULL) FreePool(Info);

  if (!EFI_ERROR(Status)) Status = File->SetPosition(File, 0);
  WriteSize = Position;
  if (!EFI_ERROR(Status)) Status = File->Write(File, &WriteSize, Buffer);
  if (!EFI_ERROR(Status) && WriteSize != Position) Status = EFI_DEVICE_ERROR;
  if (!EFI_ERROR(Status)) Status = File->Flush(File);

  File->Close(File);
  Root->Close(Root);
  FreePool(Buffer);
  return Status;
}
