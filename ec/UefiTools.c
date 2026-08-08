// UefiTools.c - compact, read-only view of BootOrder/Boot#### variables.
#include "UefiTools.h"
#include "Gui.h"

#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

#define BOOT_LINE_CHARS 192

static EFI_GUID gEcGlobalVariableGuid = {
  0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }
};

static EFI_STATUS ReadGlobalVariable(
  IN CONST CHAR16* Name,
  OUT VOID** Data,
  OUT UINTN* Size
) {
  EFI_STATUS status;
  UINTN bytes = 0;
  VOID* buffer;
  if (Name == NULL || Data == NULL || Size == NULL) return EFI_INVALID_PARAMETER;
  *Data = NULL;
  *Size = 0;
  status = gRT->GetVariable((CHAR16*)Name, &gEcGlobalVariableGuid, NULL, &bytes, NULL);
  if (status != EFI_BUFFER_TOO_SMALL || bytes == 0) return status;
  buffer = AllocatePool(bytes);
  if (buffer == NULL) return EFI_OUT_OF_RESOURCES;
  status = gRT->GetVariable((CHAR16*)Name, &gEcGlobalVariableGuid, NULL, &bytes, buffer);
  if (EFI_ERROR(status)) {
    FreePool(buffer);
    return status;
  }
  *Data = buffer;
  *Size = bytes;
  return EFI_SUCCESS;
}

static CONST CHAR16* BootDescription(IN CONST UINT8* Data, IN UINTN Size)
{
  CONST CHAR16* description;
  UINTN maxChars;
  if (Data == NULL || Size < sizeof(UINT32) + sizeof(UINT16) + sizeof(CHAR16)) return L"<invalid>";
  description = (CONST CHAR16*)(Data + sizeof(UINT32) + sizeof(UINT16));
  maxChars = (Size - sizeof(UINT32) - sizeof(UINT16)) / sizeof(CHAR16);
  for (UINTN i = 0; i < maxChars; i++) {
    if (description[i] == L'\0') return description;
  }
  return L"<unterminated description>";
}

VOID UefiToolsShowBootEntries(VOID)
{
  VOID* orderData = NULL;
  UINTN orderSize = 0;
  UINT16 bootNext = 0xffff;
  UINTN bootNextSize = sizeof(bootNext);
  UINT16* order;
  UINTN count;
  CHAR16** lines = NULL;
  CHAR16* storage = NULL;
  UINTN chosen = 0;
  EFI_STATUS status;

  status = ReadGlobalVariable(L"BootOrder", &orderData, &orderSize);
  if (EFI_ERROR(status) || orderSize < sizeof(UINT16)) {
    GuiDrawMsgBox(L"UEFI boot entries", L"BootOrder is unavailable.");
    if (orderData != NULL) FreePool(orderData);
    return;
  }
  order = (UINT16*)orderData;
  count = orderSize / sizeof(UINT16);
  lines = AllocateZeroPool(count * sizeof(CHAR16*));
  storage = AllocateZeroPool(count * BOOT_LINE_CHARS * sizeof(CHAR16));
  if (lines == NULL || storage == NULL) {
    if (lines != NULL) FreePool(lines);
    if (storage != NULL) FreePool(storage);
    FreePool(orderData);
    GuiDrawMsgBox(L"UEFI boot entries", L"Not enough memory.");
    return;
  }
  gRT->GetVariable(L"BootNext", &gEcGlobalVariableGuid, NULL, &bootNextSize, &bootNext);

  for (UINTN i = 0; i < count; i++) {
    CHAR16 variableName[12];
    VOID* optionData = NULL;
    UINTN optionSize = 0;
    UINT32 attributes = 0;
    CONST CHAR16* description = L"<unreadable>";
    lines[i] = &storage[i * BOOT_LINE_CHARS];
    UnicodeSPrint(variableName, sizeof(variableName), L"Boot%04x", order[i]);
    if (!EFI_ERROR(ReadGlobalVariable(variableName, &optionData, &optionSize))) {
      if (optionSize >= sizeof(UINT32)) CopyMem(&attributes, optionData, sizeof(attributes));
      description = BootDescription(optionData, optionSize);
    }
    UnicodeSPrint(lines[i], BOOT_LINE_CHARS * sizeof(CHAR16), L"%s %s Boot%04x  %s",
                  order[i] == bootNext ? L">" : L" ",
                  (attributes & 1) != 0 ? L"A" : L"-", order[i], description);
    if (optionData != NULL) FreePool(optionData);
  }

  while (GuiDrawListPicker(L"BootOrder: > BootNext, A active (read-only)",
                           (CONST CHAR16**)lines, count, &chosen)) {
    CHAR16 message[192];
    UnicodeSPrint(message, sizeof(message), L"Order position: %d\n%s",
                  (UINT32)(chosen + 1), lines[chosen]);
    GuiDrawMsgBox(L"UEFI boot entry", message);
  }
  FreePool(storage);
  FreePool(lines);
  FreePool(orderData);
}

