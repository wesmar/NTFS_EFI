// FileSystem.h — UEFI File System interface, driver loading, and application execution.
#pragma once

#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>

#define MAX_VOLUMES 32
#define MAX_PATH_LEN 512

typedef struct {
  EFI_HANDLE Handle;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Sfs;
  CHAR16 Name[16]; // e.g. "fs0:"
} FS_VOLUME;

typedef struct {
  CHAR16 Name[256];
  UINT64 Size;
  UINT64 Attributes;
  BOOLEAN IsDirectory;
  EFI_TIME ModificationTime;
  BOOLEAN Selected;
} FS_FILE_ITEM;

extern FS_VOLUME gVolumes[MAX_VOLUMES];
extern UINTN gVolumeCount;

EFI_STATUS FsGetVolumeInfo(
  IN FS_VOLUME* Vol,
  OUT UINT64* TotalSize,
  OUT UINT64* FreeSize,
  OUT CHAR16* Label,
  IN UINTN LabelSize
);

EFI_STATUS FsGetVolumeDetails(
  IN FS_VOLUME* Vol,
  OUT UINT64* TotalSize,
  OUT UINT64* FreeSize,
  OUT UINT32* BlockSize,
  OUT BOOLEAN* ReadOnly,
  OUT CHAR16* Label,
  IN UINTN LabelSize
);

// Initializes and refreshes the volume list
VOID FsInit(VOID);

// Lists a directory. Memory for Files array is allocated inside and must be freed with FreePool.
EFI_STATUS FsListDirectory(
  IN  CONST CHAR16* Path,
  OUT FS_FILE_ITEM** Files,
  OUT UINTN* FileCount
);

// Copies a file with a progress callback
typedef VOID (*FS_COPY_PROGRESS)(UINT64 BytesCopied, UINT64 TotalBytes);

EFI_STATUS FsCopyFile(
  IN  CONST CHAR16* SrcPath,
  IN  CONST CHAR16* DstPath,
  IN  FS_COPY_PROGRESS ProgressCallback
);

// Copies files or directories recursively
EFI_STATUS FsCopyRecursive(
  IN  CONST CHAR16* SrcPath,
  IN  CONST CHAR16* DstPath,
  IN  FS_COPY_PROGRESS ProgressCallback
);

// Reads a file into a newly allocated pool buffer. Buffer must be freed with FreePool.
EFI_STATUS FsReadFileToBuffer(
  IN  CONST CHAR16* Path,
  OUT VOID** Buffer,
  OUT UINT64* Size
);

// Reads at most Capacity bytes without allocating the whole file. Used by the
// passive-panel quick view for large files.
EFI_STATUS FsReadFilePrefix(
  IN CONST CHAR16* Path,
  OUT VOID* Buffer,
  IN UINTN Capacity,
  OUT UINTN* BytesRead,
  OUT UINT64* TotalSize
);

// Tries to find and load ntfs.efi
EFI_STATUS FsLoadNtfsDriver(IN EFI_HANDLE ImageHandle);

// Cleanly unmounts every NTFS volume our loaded ntfs.efi bound (clears the
// $Volume dirty flag + final FlushBlocks). Call once on EC exit.
VOID FsUnmountAllNtfs(VOID);

// Runs another EFI program
EFI_STATUS FsStartEfiApp(IN EFI_HANDLE ImageHandle, IN CONST CHAR16* Path);
EFI_STATUS FsStartEfiAppWithArgs(
  IN EFI_HANDLE ImageHandle,
  IN CONST CHAR16* Path,
  IN CONST CHAR16* Arguments
);

// Starts a selected EFI image as a driver, reconnects controllers and rescans.
EFI_STATUS FsStartEfiDriver(IN EFI_HANDLE ImageHandle, IN CONST CHAR16* Path);
VOID FsRescanDevices(VOID);

// Finds the path to Windows Boot Manager on any mounted volume
CHAR16* FsFindWindowsBootManager(VOID);

// Helper to check if file or directory exists
BOOLEAN FsFileExists(IN CONST CHAR16* Path, OUT BOOLEAN* IsDirectory);

// Helper to combine two paths
VOID FsCombinePath(OUT CHAR16* Dest, IN CONST CHAR16* Base, IN CONST CHAR16* Sub);

// Deletes a file or directory
EFI_STATUS FsDeleteFileOrDir(IN CONST CHAR16* Path);

// Creates a directory
EFI_STATUS FsCreateDir(IN CONST CHAR16* Path);

// Writes a buffer back to a file (creates or overwrites)
EFI_STATUS FsWriteFileFromBuffer(
  IN CONST CHAR16* Path,
  IN VOID* Buffer,
  IN UINT64 Size
);

// Deletes a file or directory recursively
EFI_STATUS FsDeleteRecursive(IN CONST CHAR16* Path);

// Forces the volume that Path lives on to flush buffered writes to the medium
// (durability for delete/rename/mkdir, which leave no file handle to Flush()).
EFI_STATUS FsFlushVolumeForPath(IN CONST CHAR16* Path);

// Reads the DOS attributes and the three timestamps of a file or directory.
EFI_STATUS FsGetFileMeta(
  IN  CONST CHAR16* Path,
  OUT UINT64* Attributes,
  OUT EFI_TIME* CreateTime,
  OUT EFI_TIME* ModificationTime,
  OUT EFI_TIME* LastAccessTime
);

// Writes DOS attributes and/or the modification time back. A NULL pointer
// leaves that part of the file's metadata alone, so clearing ReadOnly does not
// disturb the timestamps and setting a date does not disturb the attributes.
EFI_STATUS FsSetFileMeta(
  IN CONST CHAR16* Path,
  IN CONST UINT64* Attributes,
  IN CONST EFI_TIME* ModificationTime
);

// The volume a path belongs to, or NULL when the path names none. Several
// places had their own copy of this loop.
FS_VOLUME* FsFindVolumeForPath(IN CONST CHAR16* Path);

// Renames or moves a file or directory within the same volume
EFI_STATUS FsRenameOrMove(IN CONST CHAR16* SrcPath, IN CONST CHAR16* DstPath);
