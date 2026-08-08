// FileSystem.c — UEFI File System interface, driver loading, and application execution.
#include "FileSystem.h"
#include "Gui.h"
#include "Config.h"
#include "Checksum.h"

#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Library/PrintLib.h>

FS_VOLUME gVolumes[MAX_VOLUMES] = { 0 };
UINTN gVolumeCount = 0;

// Image handle of the ntfs.efi driver we LoadImage/StartImage'd, if any. Its
// DriverBinding is installed on this same handle, so it doubles as the
// DriverImageHandle for gBS->DisconnectController() when we unmount on exit.
EFI_HANDLE gNtfsDriverHandle = NULL;

EFI_GUID gEfiFileSystemInfoGuid = { 0x09576e93, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

static EFI_STATUS ParsePath(
  IN  CONST CHAR16* Path,
  OUT FS_VOLUME** OutVolume,
  OUT CONST CHAR16** OutSubPath
) {
  if (Path == NULL || OutVolume == NULL || OutSubPath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  // Find the colon ':'
  UINTN i = 0;
  while (Path[i] != L'\0' && Path[i] != L':') {
    i++;
  }
  if (Path[i] != L':') {
    return EFI_INVALID_PARAMETER;
  }

  // Match volume name
  for (UINTN v = 0; v < gVolumeCount; v++) {
    if (StrnCmp(gVolumes[v].Name, Path, i + 1) == 0) {
      *OutVolume = &gVolumes[v];
      *OutSubPath = &Path[i + 1];
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS FsGetVolumeInfo(
  IN FS_VOLUME* Vol,
  OUT UINT64* TotalSize,
  OUT UINT64* FreeSize,
  OUT CHAR16* Label,
  IN UINTN LabelSize
) {
  if (Vol == NULL) return EFI_INVALID_PARAMETER;
  
  EFI_FILE_PROTOCOL* root = NULL;
  EFI_STATUS status = Vol->Sfs->OpenVolume(Vol->Sfs, &root);
  if (EFI_ERROR(status)) return status;
  
  UINTN infoSize = 0;
  EFI_FILE_SYSTEM_INFO* info = NULL;
  status = root->GetInfo(root, &gEfiFileSystemInfoGuid, &infoSize, NULL);
  if (status == EFI_BUFFER_TOO_SMALL) {
    info = AllocatePool(infoSize);
    if (info != NULL) {
      status = root->GetInfo(root, &gEfiFileSystemInfoGuid, &infoSize, info);
    }
  }
  
  if (!EFI_ERROR(status) && info != NULL) {
    if (TotalSize) *TotalSize = info->VolumeSize;
    if (FreeSize) *FreeSize = info->FreeSpace;
    if (Label) {
      StrCpyS(Label, LabelSize, info->VolumeLabel);
    }
    FreePool(info);
  } else {
    if (info != NULL) FreePool(info);
  }
  
  root->Close(root);
  return status;
}

EFI_STATUS FsGetVolumeDetails(
  IN FS_VOLUME* Vol,
  OUT UINT64* TotalSize,
  OUT UINT64* FreeSize,
  OUT UINT32* BlockSize,
  OUT BOOLEAN* ReadOnly,
  OUT CHAR16* Label,
  IN UINTN LabelSize
) {
  EFI_FILE_PROTOCOL* root = NULL;
  EFI_FILE_SYSTEM_INFO* info = NULL;
  UINTN infoSize = 0;
  EFI_STATUS status;

  if (Vol == NULL) return EFI_INVALID_PARAMETER;
  status = Vol->Sfs->OpenVolume(Vol->Sfs, &root);
  if (EFI_ERROR(status)) return status;
  status = root->GetInfo(root, &gEfiFileSystemInfoGuid, &infoSize, NULL);
  if (status == EFI_BUFFER_TOO_SMALL) {
    info = AllocatePool(infoSize);
    if (info == NULL) status = EFI_OUT_OF_RESOURCES;
    else status = root->GetInfo(root, &gEfiFileSystemInfoGuid, &infoSize, info);
  }
  if (!EFI_ERROR(status) && info != NULL) {
    if (TotalSize != NULL) *TotalSize = info->VolumeSize;
    if (FreeSize != NULL) *FreeSize = info->FreeSpace;
    if (BlockSize != NULL) *BlockSize = info->BlockSize;
    if (ReadOnly != NULL) *ReadOnly = info->ReadOnly;
    if (Label != NULL && LabelSize > 0) StrCpyS(Label, LabelSize, info->VolumeLabel);
  }
  if (info != NULL) FreePool(info);
  root->Close(root);
  return status;
}

static EFI_STATUS FsOpenParentAndChild(
  IN CONST CHAR16* Path,
  IN UINT64 OpenMode,
  IN UINT64 Attributes,
  OUT EFI_FILE_PROTOCOL** ParentDir,
  OUT EFI_FILE_PROTOCOL** ChildFile,
  OUT CHAR16* ChildName
) {
  if (Path == NULL || ParentDir == NULL || ChildName == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *ParentDir = NULL;
  if (ChildFile) *ChildFile = NULL;
  ChildName[0] = L'\0';

  // 1. Parse volume and subpath
  FS_VOLUME* vol = NULL;
  CONST CHAR16* subPath = NULL;
  EFI_STATUS status = ParsePath(Path, &vol, &subPath);
  if (EFI_ERROR(status)) return status;

  // 2. Open root
  EFI_FILE_PROTOCOL* currentDir = NULL;
  status = vol->Sfs->OpenVolume(vol->Sfs, &currentDir);
  if (EFI_ERROR(status)) return status;

  // Skip leading backslashes in subPath
  while (*subPath == L'\\') {
    subPath++;
  }

  // 3. Walk path to find the parent directory of the final component
  CHAR16 pathCopy[MAX_PATH_LEN];
  StrCpyS(pathCopy, MAX_PATH_LEN, subPath);

  // Find the last backslash in pathCopy
  INTN lastSlashIdx = -1;
  UINTN len = StrLen(pathCopy);
  for (UINTN idx = len; idx > 0; idx--) {
    if (pathCopy[idx - 1] == L'\\') {
      lastSlashIdx = (INTN)(idx - 1);
      break;
    }
  }

  if (lastSlashIdx != -1) {
    // There are parent directories to open first
    pathCopy[lastSlashIdx] = L'\0';
    CHAR16* remainingChildName = &pathCopy[lastSlashIdx + 1];
    StrCpyS(ChildName, 256, remainingChildName);

    EFI_FILE_PROTOCOL* targetDir = NULL;
    status = currentDir->Open(currentDir, &targetDir, pathCopy, EFI_FILE_MODE_READ, 0);
    currentDir->Close(currentDir);
    if (EFI_ERROR(status)) {
      return status;
    }
    currentDir = targetDir;
  } else {
    // No intermediate directories, parent is the root
    StrCpyS(ChildName, 256, pathCopy);
  }

  // If ChildName is empty (e.g. path is volume root)
  if (ChildName[0] == L'\0') {
    *ParentDir = currentDir;
    return EFI_SUCCESS;
  }

  *ParentDir = currentDir;

  // 4. Open final child relative to the parent directory
  if (ChildFile) {
    status = currentDir->Open(currentDir, ChildFile, ChildName, OpenMode, Attributes);
    if (EFI_ERROR(status)) {
      *ChildFile = NULL;
    }
  }

  // Preserve the final component's Open() result. Returning EFI_SUCCESS with
  // ChildFile == NULL made callers count failed creates/opens as successful,
  // hiding the exact point where a large destination directory stopped
  // accepting new entries.
  return status;
}

VOID FsInit(VOID)
{
  gVolumeCount = 0;
  ZeroMem(gVolumes, sizeof(gVolumes));

  UINTN handleCount = 0;
  EFI_HANDLE* handles = NULL;
  EFI_STATUS status = gBS->LocateHandleBuffer(
    ByProtocol,
    &gEfiSimpleFileSystemProtocolGuid,
    NULL,
    &handleCount,
    &handles
  );

  if (EFI_ERROR(status) || handles == NULL) {
    return;
  }

  for (UINTN i = 0; i < handleCount && gVolumeCount < MAX_VOLUMES; i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfs = NULL;
    status = gBS->OpenProtocol(
      handles[i],
      &gEfiSimpleFileSystemProtocolGuid,
      (VOID**)&sfs,
      gImageHandle,
      NULL,
      EFI_OPEN_PROTOCOL_GET_PROTOCOL
    );

    if (!EFI_ERROR(status) && sfs != NULL) {
      gVolumes[gVolumeCount].Handle = handles[i];
      gVolumes[gVolumeCount].Sfs = sfs;
      UnicodeSPrint(gVolumes[gVolumeCount].Name, sizeof(gVolumes[gVolumeCount].Name), L"fs%d:", gVolumeCount);
      gVolumeCount++;
    }
  }

  if (handles != NULL) {
    gBS->FreePool(handles);
  }
}

EFI_STATUS FsListDirectory(
  IN  CONST CHAR16* Path,
  OUT FS_FILE_ITEM** Files,
  OUT UINTN* FileCount
) {
  if (Path == NULL || Files == NULL || FileCount == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Files = NULL;
  *FileCount = 0;

  if (Path[0] == L'\0') {
    // Return list of volumes!
    FS_FILE_ITEM* items = AllocateZeroPool(gVolumeCount * sizeof(FS_FILE_ITEM));
    if (items == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    for (UINTN v = 0; v < gVolumeCount; v++) {
      StrCpyS(items[v].Name, 256, gVolumes[v].Name);
      items[v].IsDirectory = TRUE;
      items[v].Attributes = EFI_FILE_DIRECTORY;
      items[v].Size = 0;
    }
    *Files = items;
    *FileCount = gVolumeCount;
    return EFI_SUCCESS;
  }

  FS_VOLUME* vol = NULL;
  CONST CHAR16* subPath = NULL;
  EFI_STATUS status = ParsePath(Path, &vol, &subPath);
  if (EFI_ERROR(status)) {
    return status;
  }

  EFI_FILE_PROTOCOL* root = NULL;
  status = vol->Sfs->OpenVolume(vol->Sfs, &root);
  if (EFI_ERROR(status)) {
    return status;
  }

  EFI_FILE_PROTOCOL* dir = NULL;
  // If subPath is empty or just "\", open root directly
  if (subPath[0] == L'\0' || (subPath[0] == L'\\' && subPath[1] == L'\0')) {
    dir = root;
  } else {
    status = root->Open(root, &dir, (CHAR16*)subPath, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(status)) {
      return status;
    }
  }

  // First pass: count elements to allocate memory
  UINTN capacity = 64;
  FS_FILE_ITEM* items = AllocateZeroPool(capacity * sizeof(FS_FILE_ITEM));
  if (items == NULL) {
    if (dir != root) dir->Close(dir);
    return EFI_OUT_OF_RESOURCES;
  }

  UINTN count = 0;
  UINTN bufSize = 1024;
  VOID* buffer = AllocateZeroPool(bufSize);
  if (buffer == NULL) {
    FreePool(items);
    if (dir != root) dir->Close(dir);
    return EFI_OUT_OF_RESOURCES;
  }

  // Standard EDK II directories contain "." and ".." in some firmware, or not.
  // We want to guarantee ".." is at the top of the list if we are not at the virtual drive list root.
  if (Path[0] != L'\0') {
    StrCpyS(items[count].Name, 256, L"..");
    items[count].IsDirectory = TRUE;
    items[count].Attributes = EFI_FILE_DIRECTORY;
    items[count].Size = 0;
    count++;
  }

  dir->SetPosition(dir, 0);

  while (TRUE) {
    UINTN currentBufSize = bufSize;
    status = dir->Read(dir, &currentBufSize, buffer);
    if (status == EFI_BUFFER_TOO_SMALL) {
      FreePool(buffer);
      bufSize = currentBufSize;
      buffer = AllocateZeroPool(bufSize);
      if (buffer == NULL) {
        FreePool(items);
        if (dir != root) dir->Close(dir);
        return EFI_OUT_OF_RESOURCES;
      }
      continue;
    }

    if (EFI_ERROR(status) || currentBufSize == 0) {
      break; // End of directory or error
    }

    EFI_FILE_INFO* fileInfo = (EFI_FILE_INFO*)buffer;

    // Skip "." and ".." entry if returned by firmware
    if (StrCmp(fileInfo->FileName, L".") == 0 || StrCmp(fileInfo->FileName, L"..") == 0) {
      continue;
    }

    // Grow list capacity if needed
    if (count >= capacity) {
      capacity *= 2;
      FS_FILE_ITEM* newItems = ReallocatePool(count * sizeof(FS_FILE_ITEM), capacity * sizeof(FS_FILE_ITEM), items);
      if (newItems == NULL) {
        FreePool(items);
        FreePool(buffer);
        if (dir != root) dir->Close(dir);
        return EFI_OUT_OF_RESOURCES;
      }
      items = newItems;
    }

    StrCpyS(items[count].Name, 256, fileInfo->FileName);
    items[count].Size = fileInfo->FileSize;
    items[count].Attributes = fileInfo->Attribute;
    items[count].IsDirectory = (fileInfo->Attribute & EFI_FILE_DIRECTORY) != 0;
    CopyMem(&items[count].ModificationTime, &fileInfo->ModificationTime, sizeof(EFI_TIME));

    count++;
  }

  FreePool(buffer);
  if (dir != root) {
    dir->Close(dir);
  } else {
    root->Close(root);
  }

  *Files = items;
  *FileCount = count;
  return EFI_SUCCESS;
}

static BOOLEAN gCopyAbortRequested = FALSE;
static BOOLEAN gCopyOverwriteAll = FALSE;
static BOOLEAN gCopySkipAll = FALSE;
static UINTN gCopyCountSuccess = 0;
static UINTN gCopyCountFailed = 0;
static EFI_STATUS gCopyFirstFailureStatus = EFI_SUCCESS;
static CHAR16 gCopyFirstFailureName[128] = { 0 };
static CONST CHAR16* FsGetFileName(IN CONST CHAR16* Path);

static VOID FsRememberCopyFailure(IN CONST CHAR16* Path, IN EFI_STATUS Status) {
  if (!EFI_ERROR(gCopyFirstFailureStatus) && EFI_ERROR(Status)) {
    CONST CHAR16* name = FsGetFileName(Path);
    StrnCpyS(gCopyFirstFailureName, ARRAY_SIZE(gCopyFirstFailureName), name,
             ARRAY_SIZE(gCopyFirstFailureName) - 1);
    gCopyFirstFailureStatus = Status;
  }
}

static BOOLEAN CheckAbortKey(VOID) {
  EFI_INPUT_KEY key;
  EFI_STATUS status = gBS->CheckEvent(gST->ConIn->WaitForKey);
  if (status == EFI_SUCCESS) {
    status = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
    if (!EFI_ERROR(status)) {
      if (key.ScanCode == SCAN_ESC || key.UnicodeChar == 27) {
        return TRUE;
      }
    }
  }
  return FALSE;
}

static CONST CHAR16* FsGetFileName(IN CONST CHAR16* Path) {
  UINTN len = StrLen(Path);
  for (UINTN i = len; i > 0; i--) {
    if (Path[i - 1] == L'\\' || Path[i - 1] == L':') {
      return &Path[i];
    }
  }
  return Path;
}

EFI_STATUS FsCopyFile(
  IN  CONST CHAR16* SrcPath,
  IN  CONST CHAR16* DstPath,
  IN  FS_COPY_PROGRESS ProgressCallback
) {
  if (SrcPath == NULL || DstPath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (gCopyAbortRequested) return EFI_ABORTED;

  // 1. Check for file conflict
  BOOLEAN isDestDir = FALSE;
  if (FsFileExists(DstPath, &isDestDir)) {
    if (!gEcConfig.ConfirmOverwrite) {
      EFI_STATUS DelStatus = FsDeleteFileOrDir(DstPath);
      if (EFI_ERROR(DelStatus)) {
        return DelStatus;
      }
    } else {
    if (gCopySkipAll) {
      return EFI_SUCCESS; // Skip file
    }
    if (!gCopyOverwriteAll) {
      CHAR16 prompt[256];
      UnicodeSPrint(prompt, 256 * sizeof(CHAR16), L"File \"%s\" already exists. Overwrite?", FsGetFileName(DstPath));
      UINTN response = GuiDrawConfirmDialog(L"File Conflict", prompt, TRUE);
      if (response == 0) { // Cancel
        gCopyAbortRequested = TRUE;
        return EFI_ABORTED;
      } else if (response == 2) { // No (Skip)
        return EFI_SUCCESS;
      } else if (response == 3) { // Yes to All
        gCopyOverwriteAll = TRUE;
      } else if (response == 4) { // Skip All
        gCopySkipAll = TRUE;
        return EFI_SUCCESS;
      }
      // Yes (1) or Yes to All (3) -> proceed to overwrite
    }
    }
  }

  EFI_FILE_PROTOCOL* srcParent = NULL;
  EFI_FILE_PROTOCOL* srcFile = NULL;
  CHAR16 srcChildName[256];
  EFI_STATUS status = FsOpenParentAndChild(SrcPath, EFI_FILE_MODE_READ, 0, &srcParent, &srcFile, srcChildName);
  if (EFI_ERROR(status) || srcFile == NULL) {
    if (srcParent) srcParent->Close(srcParent);
    return status;
  }

  // Retrieve source file size
  UINTN infoSize = 0;
  EFI_FILE_INFO* fileInfo = NULL;
  status = srcFile->GetInfo(srcFile, &gEfiFileInfoGuid, &infoSize, NULL);
  if (status == EFI_BUFFER_TOO_SMALL) {
    fileInfo = AllocatePool(infoSize);
    if (fileInfo != NULL) {
      status = srcFile->GetInfo(srcFile, &gEfiFileInfoGuid, &infoSize, fileInfo);
    }
  }
  
  UINT64 fileSize = 0;
  if (!EFI_ERROR(status) && fileInfo != NULL) {
    fileSize = fileInfo->FileSize;
  }
  if (fileInfo != NULL) {
    FreePool(fileInfo);
  }

  // Open/Create destination file component-wise
  EFI_FILE_PROTOCOL* dstParent = NULL;
  EFI_FILE_PROTOCOL* dstFile = NULL;
  CHAR16 dstChildName[256];
  status = FsOpenParentAndChild(
    DstPath,
    EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
    0,
    &dstParent,
    &dstFile,
    dstChildName
  );

  if (EFI_ERROR(status) || dstFile == NULL) {
    srcFile->Close(srcFile);
    srcParent->Close(srcParent);
    if (dstParent) dstParent->Close(dstParent);
    return status;
  }

  // Copy loop
  #define COPY_BUF_SIZE (256 * 1024) // 256 KB buffer
  VOID* buffer = AllocatePool(COPY_BUF_SIZE);
  if (buffer == NULL) {
    srcFile->Close(srcFile);
    srcParent->Close(srcParent);
    dstFile->Close(dstFile);
    dstParent->Close(dstParent);
    return EFI_OUT_OF_RESOURCES;
  }

  UINT64 totalCopied = 0;
  EFI_STATUS copyStatus = EFI_SUCCESS;
  if (ProgressCallback) {
    ProgressCallback(0, fileSize);
  }

  while (TRUE) {
    // Check for user cancel via ESC key
    if (CheckAbortKey()) {
      gCopyAbortRequested = TRUE;
      copyStatus = EFI_ABORTED;
      break;
    }

    UINTN readSize = COPY_BUF_SIZE;
    status = srcFile->Read(srcFile, &readSize, buffer);
    if (EFI_ERROR(status)) {
      copyStatus = status;
      break;
    }
    if (readSize == 0) {
      break;
    }

    UINTN writeSize = readSize;
    status = dstFile->Write(dstFile, &writeSize, buffer);
    if (EFI_ERROR(status)) {
      copyStatus = status;
      break;
    }
    if (writeSize != readSize) {
      copyStatus = EFI_DEVICE_ERROR;
      break;
    }

    totalCopied += writeSize;
    if (ProgressCallback) {
      ProgressCallback(totalCopied, fileSize);
    }
  }

  FreePool(buffer);
  srcFile->Close(srcFile);
  srcParent->Close(srcParent);

  // Flush and close the destination before trying to remove a partial file.
  // Preserve the original read/write error: a cleanup failure must not hide
  // the reason the copy failed.
  status = dstFile->Flush(dstFile);
  if (!EFI_ERROR(copyStatus) && EFI_ERROR(status)) {
    copyStatus = status;
  }
  dstFile->Close(dstFile);
  dstParent->Close(dstParent);

  // A failed source read used to leave the destination entry behind (usually
  // as a misleading zero-byte file).  No partial copy is a valid result, so
  // remove it for every failure, not only for an ESC abort.  Also treat an
  // early clean EOF as corruption when GetInfo advertised more source bytes.
  if (!EFI_ERROR(copyStatus) && totalCopied != fileSize) {
    copyStatus = EFI_DEVICE_ERROR;
  }
  if (!EFI_ERROR(copyStatus) && gEcConfig.VerifyAfterCopy) {
    EC_FILE_CHECKSUM sourceChecksum;
    EC_FILE_CHECKSUM destinationChecksum;
    EFI_STATUS sourceStatus = ChecksumFile(SrcPath, &sourceChecksum);
    EFI_STATUS destinationStatus = ChecksumFile(DstPath, &destinationChecksum);
    if (EFI_ERROR(sourceStatus)) copyStatus = sourceStatus;
    else if (EFI_ERROR(destinationStatus)) copyStatus = destinationStatus;
    else if (!ChecksumEqual(&sourceChecksum, &destinationChecksum)) copyStatus = EFI_CRC_ERROR;
  }
  if (EFI_ERROR(copyStatus)) {
    FsDeleteFileOrDir(DstPath);
  }

  return copyStatus;
}

static EFI_STATUS FsCopyRecursiveInternal(
  IN  CONST CHAR16* SrcPath,
  IN  CONST CHAR16* DstPath,
  IN  FS_COPY_PROGRESS ProgressCallback
) {
  if (gCopyAbortRequested) return EFI_ABORTED;

  BOOLEAN isDir = FALSE;
  if (!FsFileExists(SrcPath, &isDir)) {
    FsRememberCopyFailure(SrcPath, EFI_NOT_FOUND);
    gCopyCountFailed++;
    return EFI_NOT_FOUND;
  }

  if (!isDir) {
    EFI_STATUS status = FsCopyFile(SrcPath, DstPath, ProgressCallback);
    if (EFI_ERROR(status)) {
      FsRememberCopyFailure(SrcPath, status);
      if (status != EFI_ABORTED) gCopyCountFailed++;
      return status;
    }
    gCopyCountSuccess++;
    return EFI_SUCCESS;
  }

  // Create target directory.  Continue only when it already exists as a
  // directory; the old code ignored every create error and then returned the
  // successful source-listing status, so a whole failed subtree was reported
  // as copied and became unselected in the panel.
  EFI_STATUS status = FsCreateDir(DstPath);
  if (EFI_ERROR(status)) {
    BOOLEAN dstIsDir = FALSE;
    if (!(FsFileExists(DstPath, &dstIsDir) && dstIsDir)) {
      FsRememberCopyFailure(DstPath, status);
      gCopyCountFailed++;
      return status;
    }
  }
  gCopyCountSuccess++; // Directory itself processed

  // List source directory content
  FS_FILE_ITEM* files = NULL;
  UINTN count = 0;
  status = FsListDirectory(SrcPath, &files, &count);
  if (EFI_ERROR(status)) {
    FsRememberCopyFailure(SrcPath, status);
    gCopyCountFailed++;
    return status;
  }

  EFI_STATUS overallStatus = EFI_SUCCESS;
  if (files != NULL) {
    for (UINTN i = 0; i < count; i++) {
      if (StrCmp(files[i].Name, L".") == 0 || StrCmp(files[i].Name, L"..") == 0) {
        continue;
      }
      if (gCopyAbortRequested) {
        status = EFI_ABORTED;
        break;
      }
      CHAR16 nextSrc[MAX_PATH_LEN] = { 0 };
      CHAR16 nextDst[MAX_PATH_LEN] = { 0 };
      FsCombinePath(nextSrc, SrcPath, files[i].Name);
      FsCombinePath(nextDst, DstPath, files[i].Name);
      
      EFI_STATUS subStatus = FsCopyRecursiveInternal(nextSrc, nextDst, ProgressCallback);
      if (subStatus == EFI_ABORTED) {
        overallStatus = EFI_ABORTED;
        break;
      }
      if (EFI_ERROR(subStatus) && !EFI_ERROR(overallStatus)) {
        overallStatus = subStatus;
      }
    }
    FreePool(files);
  }

  return overallStatus;
}

EFI_STATUS FsCopyRecursive(
  IN  CONST CHAR16* SrcPath,
  IN  CONST CHAR16* DstPath,
  IN  FS_COPY_PROGRESS ProgressCallback
) {
  gCopyAbortRequested = FALSE;
  gCopyOverwriteAll = FALSE;
  gCopySkipAll = FALSE;
  gCopyCountSuccess = 0;
  gCopyCountFailed = 0;
  gCopyFirstFailureStatus = EFI_SUCCESS;
  gCopyFirstFailureName[0] = L'\0';

  EFI_STATUS status = FsCopyRecursiveInternal(SrcPath, DstPath, ProgressCallback);

  if (status == EFI_ABORTED && gEcConfig.ShowOperationSummary) {
    GuiDrawMsgBox(L"Copy Aborted", L"Copy operation was cancelled by user.");
  } else if (gCopyCountFailed > 0 && gEcConfig.ShowOperationSummary) {
    CHAR16 msg[256];
    UnicodeSPrint(msg, sizeof(msg),
      L"First failure: %s\nStatus: %r",
      gCopyFirstFailureName, gCopyFirstFailureStatus);
    GuiDrawMsgBox(L"Copy Complete", msg);
  }

  return status;
}

EFI_STATUS FsReadFileToBuffer(
  IN  CONST CHAR16* Path,
  OUT VOID** Buffer,
  OUT UINT64* Size
) {
  if (Path == NULL || Buffer == NULL || Size == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Buffer = NULL;
  *Size = 0;

  FS_VOLUME* vol = NULL;
  CONST CHAR16* subPath = NULL;
  EFI_STATUS status = ParsePath(Path, &vol, &subPath);
  if (EFI_ERROR(status)) return status;

  EFI_FILE_PROTOCOL* root = NULL;
  status = vol->Sfs->OpenVolume(vol->Sfs, &root);
  if (EFI_ERROR(status)) return status;

  EFI_FILE_PROTOCOL* file = NULL;
  status = root->Open(root, &file, (CHAR16*)subPath, EFI_FILE_MODE_READ, 0);
  root->Close(root);
  if (EFI_ERROR(status)) return status;

  // Get file size
  UINTN infoSize = 0;
  EFI_FILE_INFO* info = NULL;
  status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, NULL);
  if (status == EFI_BUFFER_TOO_SMALL) {
    info = AllocatePool(infoSize);
    if (info != NULL) {
      status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, info);
    }
  }

  if (EFI_ERROR(status) || info == NULL) {
    if (info != NULL) FreePool(info);
    file->Close(file);
    return EFI_DEVICE_ERROR;
  }

  UINT64 fileSize = info->FileSize;
  FreePool(info);

  // Handle 0-byte files gracefully
  if (fileSize == 0) {
    file->Close(file);
    *Buffer = AllocatePool(1); // Return an allocated empty buffer
    *Size = 0;
    return EFI_SUCCESS;
  }

  VOID* buf = AllocatePool((UINTN)fileSize);
  if (buf == NULL) {
    file->Close(file);
    return EFI_OUT_OF_RESOURCES;
  }

  UINTN readSize = (UINTN)fileSize;
  status = file->Read(file, &readSize, buf);
  file->Close(file);

  if (EFI_ERROR(status)) {
    FreePool(buf);
    return status;
  }

  *Buffer = buf;
  *Size = fileSize;
  return EFI_SUCCESS;
}

EFI_STATUS FsReadFilePrefix(
  IN CONST CHAR16* Path,
  OUT VOID* Buffer,
  IN UINTN Capacity,
  OUT UINTN* BytesRead,
  OUT UINT64* TotalSize
) {
  FS_VOLUME* vol = NULL;
  CONST CHAR16* subPath = NULL;
  EFI_FILE_PROTOCOL* root = NULL;
  EFI_FILE_PROTOCOL* file = NULL;
  EFI_FILE_INFO* info = NULL;
  UINTN infoSize = 0;
  EFI_STATUS status;

  if (Path == NULL || Buffer == NULL || Capacity == 0 || BytesRead == NULL || TotalSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *BytesRead = 0;
  *TotalSize = 0;
  status = ParsePath(Path, &vol, &subPath);
  if (EFI_ERROR(status)) return status;
  status = vol->Sfs->OpenVolume(vol->Sfs, &root);
  if (EFI_ERROR(status)) return status;
  status = root->Open(root, &file, (CHAR16*)subPath, EFI_FILE_MODE_READ, 0);
  root->Close(root);
  if (EFI_ERROR(status) || file == NULL) return status;

  status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, NULL);
  if (status == EFI_BUFFER_TOO_SMALL) {
    info = AllocatePool(infoSize);
    if (info == NULL) status = EFI_OUT_OF_RESOURCES;
    else status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, info);
  }
  if (!EFI_ERROR(status) && info != NULL) *TotalSize = info->FileSize;
  if (info != NULL) FreePool(info);
  if (!EFI_ERROR(status)) {
    UINTN readSize = Capacity;
    status = file->Read(file, &readSize, Buffer);
    if (!EFI_ERROR(status)) *BytesRead = readSize;
  }
  file->Close(file);
  return status;
}

static VOID FsConnectAll(VOID)
{
  UINTN handleCount = 0;
  EFI_HANDLE* handles = NULL;
  EFI_STATUS status = gBS->LocateHandleBuffer(
    AllHandles,
    NULL,
    NULL,
    &handleCount,
    &handles
  );

  if (!EFI_ERROR(status) && handles != NULL) {
    for (UINTN i = 0; i < handleCount; i++) {
      gBS->ConnectController(handles[i], NULL, NULL, TRUE);
    }
    FreePool(handles);
  }
}

VOID FsRescanDevices(VOID)
{
  FsConnectAll();
  FsInit();
}

static EFI_STATUS FsLoadNtfsDriverFromRoot(
  IN EFI_HANDLE ImageHandle,
  IN EFI_FILE_PROTOCOL* Root,
  IN CONST CHAR16* DriverPath
) {
  EFI_FILE_PROTOCOL* file = NULL;
  EFI_STATUS status;

  if (Root == NULL || DriverPath == NULL || DriverPath[0] == L'\0') {
    return EFI_INVALID_PARAMETER;
  }

  while (*DriverPath == L'\\' || *DriverPath == L'/') DriverPath++;
  status = Root->Open(Root, &file, (CHAR16*)DriverPath, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status) || file == NULL) return status;

  UINTN infoSize = 0;
  EFI_FILE_INFO* info = NULL;
  status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, NULL);
  if (status == EFI_BUFFER_TOO_SMALL) {
    info = AllocatePool(infoSize);
    if (info != NULL) {
      status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, info);
    }
  }

  UINT64 fileSize = 0;
  if (!EFI_ERROR(status) && info != NULL) {
    fileSize = info->FileSize;
  }
  if (info != NULL) FreePool(info);

  if (fileSize == 0) {
    file->Close(file);
    return EFI_NOT_FOUND;
  }

  VOID* buffer = AllocatePool((UINTN)fileSize);
  if (buffer == NULL) {
    file->Close(file);
    return EFI_OUT_OF_RESOURCES;
  }

  UINTN readSize = (UINTN)fileSize;
  status = file->Read(file, &readSize, buffer);
  file->Close(file);
  if (EFI_ERROR(status)) {
    FreePool(buffer);
    return status;
  }

  EFI_HANDLE driverHandle = NULL;
  status = gBS->LoadImage(
    FALSE,
    ImageHandle,
    NULL,
    buffer,
    (UINTN)fileSize,
    &driverHandle
  );

  if (!EFI_ERROR(status)) {
    status = gBS->StartImage(driverHandle, NULL, NULL);
    if (!EFI_ERROR(status)) {
      gNtfsDriverHandle = driverHandle; // remember it so we can unmount on exit
    }
    FreePool(buffer);
    FsConnectAll();
    FsInit();
    return status;
  }

  FreePool(buffer);
  return status;
}

static EFI_STATUS FsTryLoadNtfsFromBootVolume(IN EFI_HANDLE ImageHandle)
{
  EFI_LOADED_IMAGE_PROTOCOL* loadedImage = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfs = NULL;
  EFI_FILE_PROTOCOL* root = NULL;
  EFI_STATUS status;
  EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

  status = gBS->OpenProtocol(
    ImageHandle,
    &loadedImageGuid,
    (VOID**)&loadedImage,
    ImageHandle,
    NULL,
    EFI_OPEN_PROTOCOL_GET_PROTOCOL
  );
  if (EFI_ERROR(status) || loadedImage == NULL) return status;

  status = gBS->HandleProtocol(loadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&sfs);
  if (EFI_ERROR(status) || sfs == NULL) return status;

  status = sfs->OpenVolume(sfs, &root);
  if (EFI_ERROR(status) || root == NULL) return status;

  if (gEcConfig.NtfsDriverPath[0] != L'\0') {
    status = FsLoadNtfsDriverFromRoot(ImageHandle, root, gEcConfig.NtfsDriverPath);
    if (!EFI_ERROR(status)) {
      root->Close(root);
      return status;
    }
  }

  status = FsLoadNtfsDriverFromRoot(ImageHandle, root, L"EFI\\BOOT\\ntfs.efi");
  if (EFI_ERROR(status)) {
    status = FsLoadNtfsDriverFromRoot(ImageHandle, root, L"ntfs.efi");
  }

  root->Close(root);
  return status;
}

EFI_STATUS FsLoadNtfsDriver(IN EFI_HANDLE ImageHandle)
{
  EFI_STATUS bootStatus = FsTryLoadNtfsFromBootVolume(ImageHandle);
  if (!EFI_ERROR(bootStatus)) return bootStatus;

  // Fallback for removable setups where the driver is on another readable ESP.
  for (UINTN v = 0; v < gVolumeCount; v++) {
    EFI_FILE_PROTOCOL* root = NULL;
    EFI_STATUS status = gVolumes[v].Sfs->OpenVolume(gVolumes[v].Sfs, &root);
    if (EFI_ERROR(status)) continue;

    status = FsLoadNtfsDriverFromRoot(ImageHandle, root, gEcConfig.NtfsDriverPath);
    if (EFI_ERROR(status)) {
      status = FsLoadNtfsDriverFromRoot(ImageHandle, root, L"EFI\\BOOT\\ntfs.efi");
    }
    if (EFI_ERROR(status)) {
      status = FsLoadNtfsDriverFromRoot(ImageHandle, root, L"ntfs.efi");
    }
    root->Close(root);
    if (!EFI_ERROR(status)) return status;
  }

  return bootStatus;
}

VOID FsUnmountAllNtfs(VOID)
{
  // Cleanly disconnect the ntfs.efi driver from every volume it bound. That
  // drives its DriverBinding.Stop -> NtfsEfiUnmountVolume, which clears the
  // NTFS $Volume dirty flag and issues a final BlockIo->FlushBlocks. Without
  // this, EC leaves the volume marked dirty on exit and Windows offers a
  // chkdsk on the next boot even though every write was already durable.
  // Passing gNtfsDriverHandle as the DriverImageHandle scopes the disconnect
  // to OUR driver only, so FAT volumes served by firmware are left untouched;
  // volumes our driver never bound simply return an error we ignore.
  if (gNtfsDriverHandle == NULL) {
    return;
  }
  for (UINTN v = 0; v < gVolumeCount; v++) {
    if (gVolumes[v].Handle != NULL) {
      gBS->DisconnectController(gVolumes[v].Handle, gNtfsDriverHandle, NULL);
    }
  }
}

EFI_STATUS FsStartEfiAppWithArgs(
  IN EFI_HANDLE ImageHandle,
  IN CONST CHAR16* Path,
  IN CONST CHAR16* Arguments
)
{
  FS_VOLUME* vol = NULL;
  CONST CHAR16* subPath = NULL;
  EFI_STATUS status = ParsePath(Path, &vol, &subPath);
  if (EFI_ERROR(status)) return status;

  // We should create a Device Path for the executable file.
  // Using DevicePathLib: FileDevicePath
  EFI_DEVICE_PATH_PROTOCOL* devPath = FileDevicePath(vol->Handle, (CHAR16*)subPath);
  if (devPath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  EFI_HANDLE appHandle = NULL;
  status = gBS->LoadImage(
    FALSE,
    ImageHandle,
    devPath,
    NULL,
    0,
    &appHandle
  );

  FreePool(devPath);

  if (!EFI_ERROR(status)) {
    VOID* loadOptions = NULL;
    if (Arguments != NULL && Arguments[0] != L'\0') {
      EFI_LOADED_IMAGE_PROTOCOL* loadedImage = NULL;
      EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
      UINTN optionBytes = (StrLen(Arguments) + 1) * sizeof(CHAR16);
      loadOptions = AllocateCopyPool(optionBytes, Arguments);
      // An image that was loaded but never started stays in memory until it is
      // unloaded explicitly; StartImage only cleans up after itself.
      if (loadOptions == NULL) {
        gBS->UnloadImage(appHandle);
        return EFI_OUT_OF_RESOURCES;
      }
      status = gBS->HandleProtocol(appHandle, &loadedImageGuid, (VOID**)&loadedImage);
      if (EFI_ERROR(status) || loadedImage == NULL) {
        FreePool(loadOptions);
        gBS->UnloadImage(appHandle);
        return EFI_ERROR(status) ? status : EFI_NOT_FOUND;
      }
      loadedImage->LoadOptions = loadOptions;
      loadedImage->LoadOptionsSize = (UINT32)optionBytes;
    }
    status = gBS->StartImage(appHandle, NULL, NULL);
    if (loadOptions != NULL) FreePool(loadOptions);
  }

  return status;
}

EFI_STATUS FsStartEfiApp(IN EFI_HANDLE ImageHandle, IN CONST CHAR16* Path)
{
  return FsStartEfiAppWithArgs(ImageHandle, Path, NULL);
}

EFI_STATUS FsStartEfiDriver(IN EFI_HANDLE ImageHandle, IN CONST CHAR16* Path)
{
  FS_VOLUME* vol = NULL;
  CONST CHAR16* subPath = NULL;
  EFI_DEVICE_PATH_PROTOCOL* devPath;
  EFI_HANDLE driverHandle = NULL;
  EFI_STATUS status = ParsePath(Path, &vol, &subPath);
  if (EFI_ERROR(status)) return status;
  devPath = FileDevicePath(vol->Handle, (CHAR16*)subPath);
  if (devPath == NULL) return EFI_OUT_OF_RESOURCES;
  status = gBS->LoadImage(FALSE, ImageHandle, devPath, NULL, 0, &driverHandle);
  FreePool(devPath);
  if (!EFI_ERROR(status)) status = gBS->StartImage(driverHandle, NULL, NULL);
  if (!EFI_ERROR(status)) FsRescanDevices();
  return status;
}

BOOLEAN FsFileExists(IN CONST CHAR16* Path, OUT BOOLEAN* IsDirectory)
{
  FS_VOLUME* vol = NULL;
  CONST CHAR16* subPath = NULL;
  EFI_STATUS status = ParsePath(Path, &vol, &subPath);
  if (EFI_ERROR(status)) return FALSE;

  EFI_FILE_PROTOCOL* root = NULL;
  status = vol->Sfs->OpenVolume(vol->Sfs, &root);
  if (EFI_ERROR(status)) return FALSE;

  EFI_FILE_PROTOCOL* file = NULL;
  status = root->Open(root, &file, (CHAR16*)subPath, EFI_FILE_MODE_READ, 0);
  root->Close(root);

  if (EFI_ERROR(status)) return FALSE;

  // Query details
  UINTN infoSize = 0;
  EFI_FILE_INFO* info = NULL;
  status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, NULL);
  if (status == EFI_BUFFER_TOO_SMALL) {
    info = AllocatePool(infoSize);
    if (info != NULL) {
      status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, info);
    }
  }

  if (!EFI_ERROR(status) && info != NULL) {
    if (IsDirectory) {
      *IsDirectory = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
    }
    FreePool(info);
    file->Close(file);
    return TRUE;
  }

  if (info != NULL) FreePool(info);
  file->Close(file);
  return FALSE;
}

VOID FsCombinePath(OUT CHAR16* Dest, IN CONST CHAR16* Base, IN CONST CHAR16* Sub)
{
  if (Dest == NULL || Base == NULL || Sub == NULL) return;

  // Copy base path
  StrCpyS(Dest, MAX_PATH_LEN, Base);

  // If sub is L"..", go up one directory
  if (StrCmp(Sub, L"..") == 0) {
    UINTN len = StrLen(Dest);
    if (len == 0) return;

    // If we are already at root ("fs0:\" or "fs0:"), going up takes us to empty path L"" (Drives list)
    if ((len == 5 && Dest[4] == L'\\' && Dest[3] == L':') ||
        (len == 4 && Dest[3] == L':')) {
      Dest[0] = L'\0';
      return;
    }

    // Find the last backslash (ignoring trailing backslash unless it is the root volume backslash)
    UINTN lastSlash = len;
    for (UINTN i = len; i > 0; i--) {
      if (Dest[i - 1] == L'\\') {
        lastSlash = i - 1;
        break;
      }
    }

    // Check if the slash is the root slash (e.g. "fs0:\")
    // If it is, keep the slash and terminate after it.
    if (lastSlash > 0 && Dest[lastSlash - 1] == L':') {
      Dest[lastSlash + 1] = L'\0';
    } else if (lastSlash < len) {
      Dest[lastSlash] = L'\0';
      if (Dest[lastSlash - 1] == L':') {
        // Safe measure for fs0: (make it fs0:\)
        Dest[lastSlash] = L'\\';
        Dest[lastSlash + 1] = L'\0';
      }
    }
    return;
  }

  // Standard combine
  UINTN len = StrLen(Dest);
  if (len > 0 && Dest[len - 1] != L'\\') {
    StrCatS(Dest, MAX_PATH_LEN, L"\\");
  }

  // Skip leading backslash of sub path if present
  if (Sub[0] == L'\\') {
    StrCatS(Dest, MAX_PATH_LEN, &Sub[1]);
  } else {
    StrCatS(Dest, MAX_PATH_LEN, Sub);
  }
}

EFI_STATUS FsDeleteFileOrDir(IN CONST CHAR16* Path)
{
  EFI_FILE_PROTOCOL* parentDir = NULL;
  EFI_FILE_PROTOCOL* targetFile = NULL;
  CHAR16 childName[256];
  
  EFI_STATUS status = FsOpenParentAndChild(
    Path,
    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
    0,
    &parentDir,
    &targetFile,
    childName
  );
  
  if (parentDir) parentDir->Close(parentDir);
  if (EFI_ERROR(status) || targetFile == NULL) {
    return status;
  }

  status = targetFile->Delete(targetFile);   // closes the handle either way

  // EFI_WARN_DELETE_FAILURE means "the handle was closed, but the file was NOT
  // deleted" - a non-empty directory being the usual reason. It is a WARNING
  // code, so EFI_ERROR() is FALSE for it: every caller below would otherwise
  // count a failed delete as a success and tell the user the item was removed
  // while it is still on disk. Normalize it into a real error here, once.
  if (status == EFI_WARN_DELETE_FAILURE) {
    return EFI_ACCESS_DENIED;
  }
  return status;
}

EFI_STATUS FsCreateDir(IN CONST CHAR16* Path)
{
  EFI_FILE_PROTOCOL* parentDir = NULL;
  EFI_FILE_PROTOCOL* newDir = NULL;
  CHAR16 childName[256];
  
  EFI_STATUS status = FsOpenParentAndChild(
    Path,
    EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
    EFI_FILE_DIRECTORY,
    &parentDir,
    &newDir,
    childName
  );
  
  if (parentDir) parentDir->Close(parentDir);
  if (newDir) newDir->Close(newDir);

  // Make the new directory durable on bare metal.
  if (!EFI_ERROR(status)) {
    FsFlushVolumeForPath(Path);
  }
  return status;
}

EFI_STATUS FsWriteFileFromBuffer(
  IN CONST CHAR16* Path,
  IN VOID* Buffer,
  IN UINT64 Size
) {
  if (Path == NULL || Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_FILE_PROTOCOL* parentDir = NULL;
  EFI_FILE_PROTOCOL* file = NULL;
  CHAR16 childName[256];
  
  EFI_STATUS status = FsOpenParentAndChild(
    Path,
    EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
    0,
    &parentDir,
    &file,
    childName
  );
  
  if (EFI_ERROR(status) || file == NULL) {
    if (parentDir) parentDir->Close(parentDir);
    return status;
  }

  // Truncate file first
  UINTN infoSize = 0;
  EFI_FILE_INFO* info = NULL;
  status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, NULL);
  if (status == EFI_BUFFER_TOO_SMALL) {
    info = AllocatePool(infoSize);
    if (info != NULL) {
      status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, info);
    }
  }

  if (!EFI_ERROR(status) && info != NULL) {
    info->FileSize = Size;
    status = file->SetInfo(file, &gEfiFileInfoGuid, infoSize, info);
    FreePool(info);
  } else {
    if (info != NULL) FreePool(info);
    file->Close(file);
    parentDir->Close(parentDir);
    return status;
  }

  // Write new content
  if (Size > 0) {
    UINTN writeSize = (UINTN)Size;
    status = file->Write(file, &writeSize, Buffer);
  }

  file->Flush(file);
  file->Close(file);
  parentDir->Close(parentDir);

  return status;
}

static BOOLEAN gDeleteAbortRequested = FALSE;
static UINTN gDeleteCountSuccess = 0;
static UINTN gDeleteCountFailed = 0;

static EFI_STATUS FsDeleteRecursiveInternal(IN CONST CHAR16* Path) {
  if (gDeleteAbortRequested) return EFI_ABORTED;

  if (CheckAbortKey()) {
    gDeleteAbortRequested = TRUE;
    return EFI_ABORTED;
  }

  BOOLEAN isDir = FALSE;
  if (!FsFileExists(Path, &isDir)) {
    gDeleteCountFailed++;
    return EFI_NOT_FOUND;
  }

  if (!isDir) {
    EFI_STATUS status = FsDeleteFileOrDir(Path);
    if (EFI_ERROR(status)) {
      gDeleteCountFailed++;
      return status;
    }
    gDeleteCountSuccess++;
    return EFI_SUCCESS;
  }

  // List directory content
  FS_FILE_ITEM* files = NULL;
  UINTN count = 0;
  EFI_STATUS status = FsListDirectory(Path, &files, &count);
  if (!EFI_ERROR(status) && files != NULL) {
    for (UINTN i = 0; i < count; i++) {
      if (StrCmp(files[i].Name, L".") == 0 || StrCmp(files[i].Name, L"..") == 0) {
        continue;
      }
      if (gDeleteAbortRequested) {
        status = EFI_ABORTED;
        break;
      }
      CHAR16 childPath[MAX_PATH_LEN] = { 0 };
      FsCombinePath(childPath, Path, files[i].Name);
      EFI_STATUS subStatus = FsDeleteRecursiveInternal(childPath);
      if (subStatus == EFI_ABORTED) {
        status = EFI_ABORTED;
        break;
      }
    }
    FreePool(files);
  }

  if (status == EFI_ABORTED) return EFI_ABORTED;

  // Finally delete this directory itself
  status = FsDeleteFileOrDir(Path);
  if (EFI_ERROR(status)) {
    gDeleteCountFailed++;
    return status;
  }
  gDeleteCountSuccess++;
  return EFI_SUCCESS;
}

// Forces the volume that Path lives on to flush buffered writes through to the
// physical medium. Delete/rename leave no file handle to Flush() afterwards,
// and on real firmware the underlying BlockIo may hold writes in a write-back
// cache that only drains on FlushBlocks (the ntfs.efi driver wires its
// EFI_FILE_PROTOCOL.Flush to BlockIo->FlushBlocks). Without this a "delete
// then power off" on bare metal could silently lose the change even though the
// operation returned success - the exact symptom seen on large real volumes,
// while cache=writethrough VMs (Hyper-V/QEMU) masked it.
EFI_STATUS FsFlushVolumeForPath(IN CONST CHAR16* Path) {
  FS_VOLUME* vol = NULL;
  CONST CHAR16* sub = NULL;
  EFI_STATUS status = ParsePath(Path, &vol, &sub);
  if (EFI_ERROR(status) || vol == NULL) return status;

  EFI_FILE_PROTOCOL* root = NULL;
  status = vol->Sfs->OpenVolume(vol->Sfs, &root);
  if (EFI_ERROR(status) || root == NULL) return status;

  status = root->Flush(root);
  root->Close(root);
  return status;
}

EFI_STATUS FsDeleteRecursive(IN CONST CHAR16* Path) {
  gDeleteAbortRequested = FALSE;
  gDeleteCountSuccess = 0;
  gDeleteCountFailed = 0;

  EFI_STATUS status = FsDeleteRecursiveInternal(Path);

  // Make the deletion durable on bare metal before returning to the UI.
  FsFlushVolumeForPath(Path);

  if (status == EFI_ABORTED && gEcConfig.ShowOperationSummary) {
    GuiDrawMsgBox(L"Delete Aborted", L"Delete operation was cancelled by user.");
  } else if (gDeleteCountFailed > 0 && gEcConfig.ShowOperationSummary) {
    CHAR16 msg[128];
    UnicodeSPrint(msg, sizeof(msg), L"Delete finished. %d items deleted, %d failed.", gDeleteCountSuccess, gDeleteCountFailed);
    GuiDrawMsgBox(L"Delete Complete", msg);
  }

  return status;
}

/*
 * Metadata is read and written through one EFI_FILE_INFO round trip: GetInfo
 * hands back the whole record, the caller's fields replace what they asked to
 * change, and SetInfo puts it back. Anything the caller passes as NULL is
 * copied through untouched - the driver writes every field of the record it
 * receives, so a partial update has to preserve the rest here.
 *
 * The file is opened for write because SetInfo on a read-only handle is
 * refused, and a directory is opened the same way its own entry is stored.
 */
static EFI_STATUS FsOpenForMeta(
  IN  CONST CHAR16* Path,
  OUT EFI_FILE_PROTOCOL** Parent,
  OUT EFI_FILE_PROTOCOL** File,
  IN  BOOLEAN ForWrite
) {
  CHAR16 childName[256];
  UINT64 mode = ForWrite ? (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE) : EFI_FILE_MODE_READ;
  EFI_STATUS status;
  BOOLEAN isDir = FALSE;

  if (!FsFileExists(Path, &isDir)) {
    return EFI_NOT_FOUND;
  }
  status = FsOpenParentAndChild(Path, mode, isDir ? EFI_FILE_DIRECTORY : 0,
                                Parent, File, childName);
  if (EFI_ERROR(status) && ForWrite) {
    /* a ReadOnly file refuses a write handle - that is exactly the file whose
     * attribute the user is trying to clear, so fall back to a read handle and
     * let SetInfo decide */
    status = FsOpenParentAndChild(Path, EFI_FILE_MODE_READ, isDir ? EFI_FILE_DIRECTORY : 0,
                                  Parent, File, childName);
  }
  return status;
}

static EFI_FILE_INFO* FsReadFileInfo(IN EFI_FILE_PROTOCOL* File, OUT UINTN* InfoSize)
{
  EFI_FILE_INFO* info = NULL;
  UINTN size = 0;
  EFI_STATUS status;

  *InfoSize = 0;
  status = File->GetInfo(File, &gEfiFileInfoGuid, &size, NULL);
  if (status != EFI_BUFFER_TOO_SMALL || size == 0) {
    return NULL;
  }
  info = AllocatePool(size);
  if (info == NULL) {
    return NULL;
  }
  if (EFI_ERROR(File->GetInfo(File, &gEfiFileInfoGuid, &size, info))) {
    FreePool(info);
    return NULL;
  }
  *InfoSize = size;
  return info;
}

EFI_STATUS FsGetFileMeta(
  IN  CONST CHAR16* Path,
  OUT UINT64* Attributes,
  OUT EFI_TIME* CreateTime,
  OUT EFI_TIME* ModificationTime,
  OUT EFI_TIME* LastAccessTime
) {
  EFI_FILE_PROTOCOL* parent = NULL;
  EFI_FILE_PROTOCOL* file = NULL;
  EFI_FILE_INFO* info;
  UINTN infoSize = 0;
  EFI_STATUS status;

  if (Path == NULL) return EFI_INVALID_PARAMETER;

  status = FsOpenForMeta(Path, &parent, &file, FALSE);
  if (EFI_ERROR(status) || file == NULL) {
    if (parent) parent->Close(parent);
    return EFI_ERROR(status) ? status : EFI_NOT_FOUND;
  }

  info = FsReadFileInfo(file, &infoSize);
  if (info == NULL) {
    file->Close(file);
    if (parent) parent->Close(parent);
    return EFI_DEVICE_ERROR;
  }

  if (Attributes)       *Attributes = info->Attribute;
  if (CreateTime)       *CreateTime = info->CreateTime;
  if (ModificationTime) *ModificationTime = info->ModificationTime;
  if (LastAccessTime)   *LastAccessTime = info->LastAccessTime;

  FreePool(info);
  file->Close(file);
  if (parent) parent->Close(parent);
  return EFI_SUCCESS;
}

EFI_STATUS FsSetFileMeta(
  IN CONST CHAR16* Path,
  IN CONST UINT64* Attributes,
  IN CONST EFI_TIME* ModificationTime
) {
  EFI_FILE_PROTOCOL* parent = NULL;
  EFI_FILE_PROTOCOL* file = NULL;
  EFI_FILE_INFO* info;
  UINTN infoSize = 0;
  EFI_STATUS status;

  if (Path == NULL || (Attributes == NULL && ModificationTime == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  status = FsOpenForMeta(Path, &parent, &file, TRUE);
  if (EFI_ERROR(status) || file == NULL) {
    if (parent) parent->Close(parent);
    return EFI_ERROR(status) ? status : EFI_NOT_FOUND;
  }

  info = FsReadFileInfo(file, &infoSize);
  if (info == NULL) {
    file->Close(file);
    if (parent) parent->Close(parent);
    return EFI_DEVICE_ERROR;
  }

  if (Attributes != NULL) {
    /* only the four DOS bits are the caller's to set; EFI_FILE_DIRECTORY and
     * anything the firmware keeps in the high bits stay as they were */
    UINT64 keep = info->Attribute & ~(UINT64)(EFI_FILE_READ_ONLY | EFI_FILE_HIDDEN |
                                              EFI_FILE_SYSTEM | EFI_FILE_ARCHIVE);
    info->Attribute = keep | (*Attributes & (EFI_FILE_READ_ONLY | EFI_FILE_HIDDEN |
                                             EFI_FILE_SYSTEM | EFI_FILE_ARCHIVE));
  }
  if (ModificationTime != NULL) {
    info->ModificationTime = *ModificationTime;
  }

  status = file->SetInfo(file, &gEfiFileInfoGuid, infoSize, info);

  FreePool(info);
  file->Flush(file);
  file->Close(file);
  if (parent) parent->Close(parent);

  if (!EFI_ERROR(status)) {
    FsFlushVolumeForPath(Path);
  }
  return status;
}

FS_VOLUME* FsFindVolumeForPath(IN CONST CHAR16* Path)
{
  UINTN colIdx = 0;
  UINTN v;

  if (Path == NULL || Path[0] == L'\0') return NULL;
  while (Path[colIdx] != L'\0' && Path[colIdx] != L':') colIdx++;
  if (Path[colIdx] != L':') return NULL;

  for (v = 0; v < gVolumeCount; v++) {
    if (StrnCmp(gVolumes[v].Name, Path, colIdx + 1) == 0) {
      return &gVolumes[v];
    }
  }
  return NULL;
}

EFI_STATUS FsRenameOrMove(IN CONST CHAR16* SrcPath, IN CONST CHAR16* DstPath)
{
  // Open file
  FS_VOLUME* srcVol = NULL;
  CONST CHAR16* srcSubPath = NULL;
  EFI_STATUS status = ParsePath(SrcPath, &srcVol, &srcSubPath);
  if (EFI_ERROR(status)) return status;

  FS_VOLUME* dstVol = NULL;
  CONST CHAR16* dstSubPath = NULL;
  status = ParsePath(DstPath, &dstVol, &dstSubPath);
  if (EFI_ERROR(status)) return status;

  if (srcVol != dstVol) {
    return EFI_UNSUPPORTED; // Cannot rename across different volumes
  }

  EFI_FILE_PROTOCOL* root = NULL;
  status = srcVol->Sfs->OpenVolume(srcVol->Sfs, &root);
  if (EFI_ERROR(status)) return status;

  EFI_FILE_PROTOCOL* file = NULL;
  status = root->Open(root, &file, (CHAR16*)srcSubPath, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  root->Close(root);
  if (EFI_ERROR(status)) return status;

  // Get FileInfo
  UINTN infoSize = 0;
  EFI_FILE_INFO* info = NULL;
  status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, NULL);
  if (status == EFI_BUFFER_TOO_SMALL) {
    info = AllocatePool(infoSize);
    if (info != NULL) {
      status = file->GetInfo(file, &gEfiFileInfoGuid, &infoSize, info);
    }
  }

  if (EFI_ERROR(status) || info == NULL) {
    if (info != NULL) FreePool(info);
    file->Close(file);
    return status;
  }

  // Allocate new FileInfo structure with enough space for dstSubPath
  UINTN newInfoSize = sizeof(EFI_FILE_INFO) + StrSize(dstSubPath);
  EFI_FILE_INFO* newInfo = AllocateZeroPool(newInfoSize);
  if (newInfo == NULL) {
    FreePool(info);
    file->Close(file);
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem(newInfo, info, sizeof(EFI_FILE_INFO));
  newInfo->Size = newInfoSize;
  StrCpyS(newInfo->FileName, StrLen(dstSubPath) + 1, dstSubPath);

  status = file->SetInfo(file, &gEfiFileInfoGuid, newInfoSize, newInfo);

  FreePool(info);
  FreePool(newInfo);
  file->Close(file);

  // Make the rename/move durable on bare metal (SetInfo mutates metadata but
  // leaves nothing to Flush() through afterwards).
  if (!EFI_ERROR(status)) {
    FsFlushVolumeForPath(DstPath);
  }
  return status;
}

CHAR16* FsFindWindowsBootManager(VOID)
{
  static CHAR16 path[MAX_PATH_LEN];
  for (UINTN i = 0; i < gVolumeCount; i++) {
    UnicodeSPrint(path, sizeof(path), L"%s\\EFI\\Microsoft\\Boot\\bootmgfw.efi", gVolumes[i].Name);
    BOOLEAN isDir = FALSE;
    if (FsFileExists(path, &isDir) && !isDir) {
      return path;
    }
  }
  return NULL;
}
