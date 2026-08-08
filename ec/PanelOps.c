// PanelOps.c - panel selection, wildcard masks, and quick search helpers.
#include "PanelOps.h"

#include <Library/UefiLib.h>

static CHAR16 UpCaseChar(CHAR16 Ch)
{
  if (Ch >= L'a' && Ch <= L'z') {
    return (CHAR16)(Ch - (L'a' - L'A'));
  }
  return Ch;
}

// Same instant, to the second. Sub-second fields and the time zone are left
// out on purpose: FAT keeps two-second resolution and an unset zone, so
// comparing them would report every FAT-to-NTFS pair as different.
static BOOLEAN PanelOpsSameTime(IN CONST EFI_TIME* A, IN CONST EFI_TIME* B)
{
  return (BOOLEAN)(A->Year == B->Year && A->Month == B->Month && A->Day == B->Day &&
                   A->Hour == B->Hour && A->Minute == B->Minute && A->Second == B->Second);
}

// The counterpart of Item on the other side, by name, or NULL.
static CONST FS_FILE_ITEM* PanelOpsFindByName(
  IN CONST PANEL* Panel,
  IN CONST CHAR16* Name
) {
  UINTN i;

  if (Panel == NULL || Panel->Files == NULL) return NULL;
  for (i = 0; i < Panel->FileCount; i++) {
    if (StrCmp(Panel->Files[i].Name, Name) == 0) {
      return &Panel->Files[i];
    }
  }
  return NULL;
}

static BOOLEAN PanelOpsDiffers(IN CONST FS_FILE_ITEM* A, IN CONST FS_FILE_ITEM* B)
{
  if (B == NULL) return TRUE;                       /* not on the other side  */
  if (A->IsDirectory != B->IsDirectory) return TRUE; /* file against folder   */
  if (A->IsDirectory) return FALSE;                  /* presence is the test  */
  if (A->Size != B->Size) return TRUE;
  return (BOOLEAN)(!PanelOpsSameTime(&A->ModificationTime, &B->ModificationTime));
}

static UINTN PanelOpsMarkAgainst(IN OUT PANEL* Panel, IN CONST PANEL* Other)
{
  UINTN marked = 0;
  UINTN i;

  if (Panel == NULL || Panel->Files == NULL) return 0;

  for (i = 0; i < Panel->FileCount; i++) {
    FS_FILE_ITEM* item = &Panel->Files[i];

    item->Selected = FALSE;
    if (!PanelOpsIsUsableItem(item)) continue;
    if (PanelOpsDiffers(item, PanelOpsFindByName(Other, item->Name))) {
      item->Selected = TRUE;
      marked++;
    }
  }
  return marked;
}

UINTN PanelOpsCompareSelect(IN OUT PANEL* Left, IN OUT PANEL* Right)
{
  UINTN marked = 0;

  if (Left == NULL || Right == NULL) return 0;
  marked += PanelOpsMarkAgainst(Left, Right);
  marked += PanelOpsMarkAgainst(Right, Left);
  return marked;
}

BOOLEAN PanelOpsIsUsableItem(IN CONST FS_FILE_ITEM* Item)
{
  return Item != NULL && StrCmp(Item->Name, L"..") != 0;
}

BOOLEAN PanelOpsMatchMask(IN CONST CHAR16* Name, IN CONST CHAR16* Mask)
{
  if (Name == NULL || Mask == NULL || Mask[0] == L'\0') {
    return FALSE;
  }

  while (*Mask != L'\0') {
    if (*Mask == L'*') {
      Mask++;
      if (*Mask == L'\0') {
        return TRUE;
      }
      while (*Name != L'\0') {
        if (PanelOpsMatchMask(Name, Mask)) {
          return TRUE;
        }
        Name++;
      }
      return PanelOpsMatchMask(Name, Mask);
    }

    if (*Name == L'\0') {
      return FALSE;
    }

    if (*Mask != L'?' && UpCaseChar(*Mask) != UpCaseChar(*Name)) {
      return FALSE;
    }

    Mask++;
    Name++;
  }

  return *Name == L'\0';
}

static BOOLEAN ContainsText(IN CONST CHAR16* Name, IN CONST CHAR16* Pattern)
{
  if (Name == NULL || Pattern == NULL || Pattern[0] == L'\0') {
    return FALSE;
  }

  for (UINTN i = 0; Name[i] != L'\0'; i++) {
    UINTN j = 0;
    while (Pattern[j] != L'\0' &&
           Name[i + j] != L'\0' &&
           UpCaseChar(Name[i + j]) == UpCaseChar(Pattern[j])) {
      j++;
    }
    if (Pattern[j] == L'\0') {
      return TRUE;
    }
  }

  return FALSE;
}

static BOOLEAN StartsWithText(IN CONST CHAR16* Name, IN CONST CHAR16* Prefix)
{
  if (Name == NULL || Prefix == NULL || Prefix[0] == L'\0') {
    return FALSE;
  }

  for (UINTN i = 0; Prefix[i] != L'\0'; i++) {
    if (Name[i] == L'\0' || UpCaseChar(Name[i]) != UpCaseChar(Prefix[i])) {
      return FALSE;
    }
  }
  return TRUE;
}

UINTN PanelOpsCountSelected(IN CONST PANEL* Panel)
{
  if (Panel == NULL || Panel->Files == NULL) {
    return 0;
  }

  UINTN count = 0;
  for (UINTN i = 0; i < Panel->FileCount; i++) {
    if (Panel->Files[i].Selected && PanelOpsIsUsableItem(&Panel->Files[i])) {
      count++;
    }
  }
  return count;
}

VOID PanelOpsClearSelection(IN PANEL* Panel)
{
  if (Panel == NULL || Panel->Files == NULL) {
    return;
  }

  for (UINTN i = 0; i < Panel->FileCount; i++) {
    Panel->Files[i].Selected = FALSE;
  }
}

UINTN PanelOpsInvertSelection(IN PANEL* Panel)
{
  if (Panel == NULL || Panel->Files == NULL) {
    return 0;
  }

  UINTN changed = 0;
  for (UINTN i = 0; i < Panel->FileCount; i++) {
    if (PanelOpsIsUsableItem(&Panel->Files[i])) {
      Panel->Files[i].Selected = !Panel->Files[i].Selected;
      changed++;
    }
  }
  return changed;
}

UINTN PanelOpsSelectByMask(IN PANEL* Panel, IN CONST CHAR16* Mask, IN BOOLEAN Select)
{
  if (Panel == NULL || Panel->Files == NULL || Mask == NULL || Mask[0] == L'\0') {
    return 0;
  }

  UINTN changed = 0;
  for (UINTN i = 0; i < Panel->FileCount; i++) {
    if (PanelOpsIsUsableItem(&Panel->Files[i]) && PanelOpsMatchMask(Panel->Files[i].Name, Mask)) {
      Panel->Files[i].Selected = Select;
      changed++;
    }
  }
  return changed;
}

BOOLEAN PanelOpsFindNext(IN OUT PANEL* Panel, IN CONST CHAR16* Pattern, IN BOOLEAN StartAfterCurrent)
{
  if (Panel == NULL || Panel->Files == NULL || Panel->FileCount == 0 || Pattern == NULL || Pattern[0] == L'\0') {
    return FALSE;
  }

  UINTN start = 0;
  if (Panel->SelectedIndex >= 0) {
    start = (UINTN)Panel->SelectedIndex;
    if (StartAfterCurrent) {
      start++;
    }
  }

  for (UINTN pass = 0; pass < 2; pass++) {
    UINTN begin = (pass == 0) ? start : 0;
    UINTN end = (pass == 0) ? Panel->FileCount : start;
    if (begin > Panel->FileCount) {
      begin = Panel->FileCount;
    }
    if (end > Panel->FileCount) {
      end = Panel->FileCount;
    }

    for (UINTN i = begin; i < end; i++) {
      if (PanelOpsIsUsableItem(&Panel->Files[i]) && ContainsText(Panel->Files[i].Name, Pattern)) {
        Panel->SelectedIndex = (INTN)i;
        return TRUE;
      }
    }
  }

  return FALSE;
}

BOOLEAN PanelOpsFindPrefixNext(IN OUT PANEL* Panel, IN CONST CHAR16* Prefix, IN BOOLEAN StartAfterCurrent)
{
  if (Panel == NULL || Panel->Files == NULL || Panel->FileCount == 0 || Prefix == NULL || Prefix[0] == L'\0') {
    return FALSE;
  }

  UINTN start = 0;
  if (Panel->SelectedIndex >= 0) {
    start = (UINTN)Panel->SelectedIndex;
    if (StartAfterCurrent) {
      start++;
    }
  }

  for (UINTN pass = 0; pass < 2; pass++) {
    UINTN begin = (pass == 0) ? start : 0;
    UINTN end = (pass == 0) ? Panel->FileCount : start;
    if (begin > Panel->FileCount) begin = Panel->FileCount;
    if (end > Panel->FileCount) end = Panel->FileCount;

    for (UINTN i = begin; i < end; i++) {
      if (PanelOpsIsUsableItem(&Panel->Files[i]) && StartsWithText(Panel->Files[i].Name, Prefix)) {
        Panel->SelectedIndex = (INTN)i;
        return TRUE;
      }
    }
  }

  return FALSE;
}
