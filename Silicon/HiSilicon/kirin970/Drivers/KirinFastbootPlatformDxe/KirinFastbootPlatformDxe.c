/** @file
  Android Fastboot platform layer for the Kirin970 UFS device.

  Implements gAndroidFastbootPlatformProtocolGuid on top of the GPT
  partitions exported by PartitionDxe, so that every partition of the UFS
  GPT - esp, boot, recovery, super, userdata, ... - can be flashed or erased
  with the stock AOSP `fastboot` host tool.

  Partition lookup is done by the GPT partition label (EFI_PARTITION_ENTRY
  .PartitionName), which is what `fastboot flash <name>` passes through.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Protocol/AndroidFastbootPlatform.h>
#include <Protocol/AndroidFastbootTransport.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/BlockIo.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define TAG                       "KirinFb"

#define FASTBOOT_VERSION_STRING   "edk2-kirin970-1.0"
#define FASTBOOT_PRODUCT_STRING   "kirin970"
//
// Must agree with mSerialString in KirinUsbDeviceDxe: this is what
// `fastboot -s <serial> ...` and `fastboot devices` print, so a serialno
// that does not match the USB string reads as a different device.
//
#define FASTBOOT_SERIAL_STRING    "Kirin970UEFI"
#define FASTBOOT_MAX_DOWNLOAD     "0x10000000"

//
// One chunk per WriteBlocks/ReadBlocks call.  Keeps the UFS request size
// sane for multi-gigabyte images (super, userdata, ...).
//
#define FLASH_CHUNK               (1024 * 1024)

STATIC BOOLEAN                    mVerifyFlash = TRUE;

//
// The Fastboot app hands us a value buffer of FASTBOOT_COMMAND_MAX_LENGTH
// minus the "OKAY" prefix, so never write more than that.
//
//
// The Fastboot app gives us Response + 4 of a CHAR8[FASTBOOT_COMMAND_MAX_LENGTH
// + 1] = CHAR8[65] buffer, so 61 bytes including the NUL terminator.
//
#define FASTBOOT_VAR_MAX          61

/**
  Case-insensitive ASCII equality.
**/
STATIC
BOOLEAN
VarNameIs (
  IN CONST CHAR8  *Name,
  IN CONST CHAR8  *Target
  )
{
  while (*Name && *Target) {
    CHAR8  A = *Name;
    CHAR8  B = *Target;

    if (A >= 'A' && A <= 'Z') {
      A = (CHAR8)(A - 'A' + 'a');
    }

    if (B >= 'A' && B <= 'Z') {
      B = (CHAR8)(B - 'A' + 'a');
    }

    if (A != B) {
      return FALSE;
    }

    Name++;
    Target++;
  }

  return (*Name == '\0') && (*Target == '\0');
}

/**
  Compare an ASCII fastboot partition name with a GPT partition label.
**/
STATIC
BOOLEAN
PartitionNameIs (
  IN CONST CHAR8  *Name,
  IN CONST CHAR16 *GptName,
  IN UINTN        GptNameMax
  )
{
  UINTN  Index;

  for (Index = 0; Index < GptNameMax; Index++) {
    CHAR8  A = (CHAR8)Name[Index];
    CHAR8  B;

    if (A >= 'A' && A <= 'Z') {
      A = (CHAR8)(A - 'A' + 'a');
    }

    if (GptName[Index] == L'\0') {
      return (A == '\0');
    }

    if (GptName[Index] > 0x7F) {
      return FALSE;
    }

    B = (CHAR8)GptName[Index];
    if (B >= 'A' && B <= 'Z') {
      B = (CHAR8)(B - 'A' + 'a');
    }

    if (A != B) {
      return FALSE;
    }
  }

  //
  // Name is at least GptNameMax characters long: only a match when the GPT
  // label was also exactly that long (i.e. not NUL terminated within it).
  //
  return (Name[GptNameMax] == '\0');
}

/**
  Format every partition handle PartitionDxe has published, one line at a
  time, and hand it to @a Sink.

  Both sinks are host-side: `oem partitions` and `getvar all` put the table
  on the PC, where it can actually be read - the app's getvar value buffer
  is 61 bytes, far too small, and the device screen is not where anyone is
  looking when they typed the command.  "getvar partition-size:<name>"
  coming back empty, "flash" failing with "No such partition" and "erase"
  failing the same way all look identical from the host, but "PartitionInfo
  protocol was never produced" and "the GPT is there, the label is just
  spelled differently" need opposite fixes - hence a line per partition
  rather than just a count.
**/
typedef
VOID
(EFIAPI *PARTITION_LINE_SINK)(IN CONST CHAR8 *Line);

STATIC
VOID
WalkPartitions (
  IN PARTITION_LINE_SINK  Sink
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 *Handles;
  UINTN                      Count;
  UINTN                      Index;
  UINTN                      Char;
  EFI_PARTITION_INFO_PROTOCOL *PartitionInfo;
  EFI_BLOCK_IO_PROTOCOL      *BlockIo;
  CHAR8                      Name[40];
  CHAR8                      Line[160];
  UINT64                     Size;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPartitionInfoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      TAG ": PartitionInfo handles: %r\n",
      Status
      ));
    Sink ("PartitionInfo protocol: not available");
    return;
  }

  AsciiSPrint (Line, sizeof (Line), "%u partition handles", (UINT32)Count);
  Sink (Line);

  for (Index = 0; Index < Count; Index++) {
    PartitionInfo = NULL;
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiPartitionInfoProtocolGuid,
                    (VOID **)&PartitionInfo
                    );
    if (EFI_ERROR (Status) || (PartitionInfo == NULL)) {
      continue;
    }

    if (PartitionInfo->Type != PARTITION_TYPE_GPT) {
      continue;
    }

    for (Char = 0; Char < ARRAY_SIZE (PartitionInfo->Info.Gpt.PartitionName); Char++) {
      if (PartitionInfo->Info.Gpt.PartitionName[Char] == L'\0') {
        break;
      }
      Name[Char] = (PartitionInfo->Info.Gpt.PartitionName[Char] > 0x7F) ?
                     '?' : (CHAR8)PartitionInfo->Info.Gpt.PartitionName[Char];
    }
    Name[Char] = '\0';

    BlockIo = NULL;
    gBS->HandleProtocol (
          Handles[Index],
          &gEfiBlockIoProtocolGuid,
          (VOID **)&BlockIo
          );
    if ((BlockIo != NULL) && (BlockIo->Media != NULL)) {
      Size = MultU64x32 (
               PartitionInfo->Info.Gpt.EndingLBA -
               PartitionInfo->Info.Gpt.StartingLBA + 1,
               BlockIo->Media->BlockSize
               );
    } else {
      Size = 0;
    }

    AsciiSPrint (
      Line,
      sizeof (Line),
      "%a LBA %lx..%lx size 0x%lx",
      Name,
      PartitionInfo->Info.Gpt.StartingLBA,
      PartitionInfo->Info.Gpt.EndingLBA,
      Size
      );
    Sink (Line);
  }

  gBS->FreePool (Handles);
}

STATIC FASTBOOT_TRANSPORT_PROTOCOL  *mTransport;

/**
  One line to the host, as a fastboot INFO response.

  The host prints every INFO line it receives before the final OKAY/FAIL, so
  this is how a payload larger than the app's 61-byte getvar buffer - the
  whole partition table, for instance - reaches the PC instead of a screen
  nobody is looking at.  The platform protocol has no transport of its own,
  so it borrows gAndroidFastbootTransportProtocolGuid; Send() ignores its
  FatalErrorEvent argument anyway, so NULL is safe.
**/
STATIC
EFI_STATUS
SendInfo (
  IN CONST CHAR8  *Text
  )
{
  CHAR8      Line[192];
  EFI_STATUS Status;

  if (mTransport == NULL) {
    Status = gBS->LocateProtocol (
                    &gAndroidFastbootTransportProtocolGuid,
                    NULL,
                    (VOID **)&mTransport
                    );
    if (EFI_ERROR (Status) || (mTransport == NULL)) {
      mTransport = NULL;
      return EFI_DEVICE_ERROR;
    }
  }

  AsciiSPrint (Line, sizeof (Line), "INFO%a", Text);
  return mTransport->Send (AsciiStrLen (Line), Line, NULL);
}

STATIC
VOID
EFIAPI
PartitionLineToHost (
  IN CONST CHAR8  *Line
  )
{
  SendInfo (Line);
}

/**
  Locate a GPT partition by its label.

  @param[in]  Name      ASCII partition name as typed on the host.
  @param[out] BlockIo   Block I/O protocol of the partition.
  @param[out] Info      GPT entry of the partition (optional).

  @retval EFI_SUCCESS    Partition found.
  @retval EFI_NOT_FOUND  No partition with that label.
**/
STATIC
EFI_STATUS
FindPartition (
  IN  CONST CHAR8                *Name,
  OUT EFI_BLOCK_IO_PROTOCOL      **BlockIo,
  OUT EFI_PARTITION_ENTRY        **Info OPTIONAL
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 *Handles;
  UINTN                      Count;
  UINTN                      Index;
  EFI_PARTITION_INFO_PROTOCOL *PartitionInfo;

  if (BlockIo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *BlockIo = NULL;
  if (Info != NULL) {
    *Info = NULL;
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPartitionInfoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      TAG ": lookup '%a': no PartitionInfo handles (%r)\n",
      Name,
      Status
      ));
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < Count; Index++) {
    PartitionInfo = NULL;
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiPartitionInfoProtocolGuid,
                    (VOID **)&PartitionInfo
                    );
    if (EFI_ERROR (Status) || (PartitionInfo == NULL)) {
      continue;
    }

    if (PartitionInfo->Type != PARTITION_TYPE_GPT) {
      continue;
    }

    if (!PartitionNameIs (
           Name,
           PartitionInfo->Info.Gpt.PartitionName,
           ARRAY_SIZE (PartitionInfo->Info.Gpt.PartitionName)
           ))
    {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)BlockIo
                    );
    if (EFI_ERROR (Status) || (*BlockIo == NULL)) {
      *BlockIo = NULL;
      continue;
    }

    if (Info != NULL) {
      *Info = &PartitionInfo->Info.Gpt;
    }

    DEBUG ((
      DEBUG_INFO,
      TAG ": matched '%a' -> LBA %lx..%lx block %u\n",
      Name,
      PartitionInfo->Info.Gpt.StartingLBA,
      PartitionInfo->Info.Gpt.EndingLBA,
      (*BlockIo)->Media->BlockSize
      ));

    gBS->FreePool (Handles);
    return EFI_SUCCESS;
  }

  gBS->FreePool (Handles);

  //
  // One line, not the table: the table now goes to the host (oem partitions,
  // getvar all), and a lookup that misses is the only thing worth putting on
  // the device screen.  Repeated per attempt, because "it missed again" after
  // a fix is exactly what we want to see.
  //
  DEBUG ((
    DEBUG_ERROR,
    TAG ": lookup '%a': not in the GPT\n",
    Name
    ));
  return EFI_NOT_FOUND;
}

/**
  Size of a partition in bytes.
**/
STATIC
UINT64
PartitionSize (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN EFI_PARTITION_ENTRY    *Info
  )
{
  UINT64  Blocks;

  if (Info != NULL) {
    Blocks = Info->EndingLBA - Info->StartingLBA + 1;
    return MultU64x32 (Blocks, BlockIo->Media->BlockSize);
  }

  Blocks = BlockIo->Media->LastBlock + 1;
  return MultU64x32 (Blocks, BlockIo->Media->BlockSize);
}

/**
  Write @a Size bytes of @a Source to @a BlockIo starting at @a Lba.

  Handles a buffer whose size is not a whole number of blocks by padding the
  final block with zeroes (the standard fastboot behaviour), and honours
  Media->IoAlign by going through a page-aligned bounce buffer when needed.

  @retval EFI_SUCCESS            Written.
  @retval EFI_BAD_BUFFER_SIZE    Size/alignment could not be satisfied.
  @retval EFI_DEVICE_ERROR       Media error.
**/
STATIC
EFI_STATUS
WriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN UINT64                 Lba,
  IN UINTN                  Size,
  IN CONST VOID             *Source
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;
  UINT32      BlockSize = BlockIo->Media->BlockSize;
  UINT8       *Bounce = NULL;
  BOOLEAN     OwnBounce = FALSE;
  CONST UINT8 *Cursor;
  UINT64      CurrentLba;
  UINTN       Remaining;
  UINTN       LastSize = 0;

  if (Source == NULL || Size == 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (BlockIo->Media->ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }

  //
  // A bounce buffer is only needed when the caller's buffer does not satisfy
  // IoAlign.  Page allocation gives 4 KiB alignment, which covers every
  // IoAlign value a block device is allowed to report.
  //
  if ((BlockIo->Media->IoAlign > 1) &&
      (((UINTN)Source & (BlockIo->Media->IoAlign - 1)) != 0))
  {
    Bounce = AllocatePages (EFI_SIZE_TO_PAGES (FLASH_CHUNK));
    if (Bounce == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    OwnBounce = TRUE;
  }

  CurrentLba = Lba;
  Cursor     = (CONST UINT8 *)Source;
  Remaining  = Size;

  while (Remaining > 0) {
    UINTN  ThisSize  = (Remaining > FLASH_CHUNK) ? FLASH_CHUNK : Remaining;
    UINTN  ThisBlocks = (ThisSize + BlockSize - 1) / BlockSize;
    UINTN  ThisBytes  = ThisBlocks * BlockSize;
    VOID   *Payload;

    LastSize = ThisSize;

    if (Bounce != NULL) {
      ZeroMem (Bounce, ThisBytes);
      CopyMem (Bounce, Cursor, ThisSize);
      Payload = Bounce;
    } else if ((ThisSize % BlockSize) != 0) {
      //
      // Final partial block: pad a scratch block with zeroes so the media
      // only ever sees whole blocks.
      //
      Payload = AllocatePages (EFI_SIZE_TO_PAGES (ThisBytes));
      if (Payload == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        goto Done;
      }

      ZeroMem (Payload, ThisBytes);
      CopyMem (Payload, Cursor, ThisSize);
      Status = BlockIo->WriteBlocks (
                          BlockIo,
                          BlockIo->Media->MediaId,
                          CurrentLba,
                          ThisBytes,
                          Payload
                          );
      FreePages (Payload, EFI_SIZE_TO_PAGES (ThisBytes));
      if (EFI_ERROR (Status)) {
        goto Fail;
      }

      CurrentLba += ThisBlocks;
      Cursor      += ThisSize;
      Remaining   -= ThisSize;
      continue;
    } else {
      Payload = (VOID *)Cursor;
    }

    Status = BlockIo->WriteBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        CurrentLba,
                        ThisBytes,
                        Payload
                        );
    if (EFI_ERROR (Status)) {
      goto Fail;
    }

    CurrentLba += ThisBlocks;
    Cursor      += ThisSize;
    Remaining   -= ThisSize;
  }

  goto Done;

Fail:
  DEBUG ((
    DEBUG_ERROR,
    TAG ": WriteBlocks LBA %lx +%x failed: %r\n",
    CurrentLba,
    LastSize,
    Status
    ));

Done:
  if (OwnBounce) {
    FreePages (Bounce, EFI_SIZE_TO_PAGES (FLASH_CHUNK));
  }

  return Status;
}

/**
  Read back and compare what was just written.
**/
STATIC
EFI_STATUS
VerifyPartition (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN UINT64                 Lba,
  IN UINTN                  Size,
  IN CONST VOID             *Expected
  )
{
  EFI_STATUS  Status;
  UINT32      BlockSize = BlockIo->Media->BlockSize;
  UINT8       *Buffer;
  CONST UINT8 *Cursor;
  UINT64      CurrentLba;
  UINTN       Remaining;

  if (!BlockIo->Media->MediaPresent) {
    return EFI_SUCCESS;
  }

  Buffer   = AllocatePages (EFI_SIZE_TO_PAGES (FLASH_CHUNK));
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CurrentLba = Lba;
  Cursor     = (CONST UINT8 *)Expected;
  Remaining  = Size;

  while (Remaining > 0) {
    UINTN  ThisSize = Remaining;
    UINTN  ThisBlocks;

    if (ThisSize > FLASH_CHUNK) {
      ThisSize = FLASH_CHUNK;
    }

    ThisBlocks = (ThisSize + BlockSize - 1) / BlockSize;

    Status = BlockIo->ReadBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        CurrentLba,
                        ThisBlocks * BlockSize,
                        Buffer
                        );
    if (EFI_ERROR (Status)) {
      FreePool (Buffer);
      return Status;
    }

    if (CompareMem (Buffer, Cursor, ThisSize) != 0) {
      DEBUG ((DEBUG_ERROR, TAG ": verify mismatch at LBA %lx\n", CurrentLba));
      FreePool (Buffer);
      return EFI_DEVICE_ERROR;
    }

    CurrentLba += ThisBlocks;
    Cursor      += ThisSize;
    Remaining   -= ThisSize;
  }

  FreePool (Buffer);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FastbootPlatformInit (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, TAG ": fastboot platform initialised (verify=%d)\n", mVerifyFlash));
  //
  // No partition dump here: the transport has not started yet, so there is
  // nowhere to send it, and the device screen is not where a partition table
  // belongs.  "oem partitions" and "getvar all" both put it on the host.
  //
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
FastbootPlatformUnInit (
  VOID
  )
{
}

/**
  Write the downloaded image to the named GPT partition.
**/
STATIC
EFI_STATUS
EFIAPI
FastbootPlatformFlashPartition (
  IN CHAR8  *PartitionName,
  IN UINTN  BufferSize,
  IN VOID   *Buffer
  )
{
  EFI_STATUS           Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  EFI_PARTITION_ENTRY   *Info;
  UINT64               PartSize;
  UINT64               StartLba;

  if (PartitionName == NULL || Buffer == NULL || BufferSize == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FindPartition (PartitionName, &BlockIo, &Info);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, TAG ": no partition '%a'\n", PartitionName));
    return EFI_NOT_FOUND;
  }

  PartSize = PartitionSize (BlockIo, Info);
  if ((UINT64)BufferSize > PartSize) {
    DEBUG ((
      DEBUG_ERROR,
      TAG ": image %x larger than partition '%a' (%lx)\n",
      BufferSize,
      PartitionName,
      PartSize
      ));
    return EFI_BAD_BUFFER_SIZE;
  }

  StartLba = (Info != NULL) ? Info->StartingLBA : 0;

  DEBUG ((
    DEBUG_INFO,
    TAG ": flashing '%a' %x bytes @LBA %lx\n",
    PartitionName,
    BufferSize,
    StartLba
    ));

  Status = WriteBlocks (BlockIo, StartLba, BufferSize, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (mVerifyFlash) {
    Status = VerifyPartition (BlockIo, StartLba, BufferSize, Buffer);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, TAG ": '%a' failed verification\n", PartitionName));
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/**
  Zero fill the named GPT partition.
**/
STATIC
EFI_STATUS
EFIAPI
FastbootPlatformErasePartition (
  IN CHAR8  *PartitionName
  )
{
  EFI_STATUS           Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  EFI_PARTITION_ENTRY   *Info;
  UINT64               PartSize;
  UINT64               StartLba;
  UINT8                *Zeros;
  UINT64               Remaining;

  if (PartitionName == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FindPartition (PartitionName, &BlockIo, &Info);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  PartSize = PartitionSize (BlockIo, Info);
  StartLba = (Info != NULL) ? Info->StartingLBA : 0;

  Zeros = AllocateZeroPool (FLASH_CHUNK);
  if (Zeros == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((
    DEBUG_INFO,
    TAG ": erasing '%a' %lx bytes @LBA %lx\n",
    PartitionName,
    PartSize,
    StartLba
    ));

  Remaining = PartSize;
  while (Remaining > 0) {
    UINTN  ThisSize = (Remaining > FLASH_CHUNK) ? FLASH_CHUNK : (UINTN)Remaining;

    Status = WriteBlocks (BlockIo, StartLba, ThisSize, Zeros);
    if (EFI_ERROR (Status)) {
      FreePool (Zeros);
      return Status;
    }

    StartLba += (ThisSize + BlockIo->Media->BlockSize - 1) / BlockIo->Media->BlockSize;
    Remaining -= ThisSize;
  }

  FreePool (Zeros);
  return EFI_SUCCESS;
}

//
// Every fastboot variable this bootloader can answer on its own, in the
// order the stock tool prints them under `getvar all`.  "version" is
// answered by the app before we are ever called, and the per-partition
// variables (partition-size:, partition-type:, has-slot:, is-logical:) are
// answered from the GPT instead of this table.
//
STATIC CONST CHAR8  *mScalarVarNames[] = {
  "version-bootloader",
  "version-baseband",
  "product",
  "serialno",
  "max-download-size",
  "variant",
  "secure",
  "unlocked",
  "tampered",
  "rollback",
  "is-userspace",
  "slot-count",
  "current-slot",
  "snapshot-update",
  "verify"
};

/**
  Answer one non-partition fastboot variable into @a Value.

  @param[in]      Name      Variable name as the host typed it.
  @param[out]     Value     Destination, NUL terminated.
  @param[in]      ValueSize Size of @a Value.

  @retval TRUE   Recognised; @a Value was filled in.
  @retval FALSE  Unknown name; @a Value untouched, which the caller reports
                 as an empty value - what the protocol uses for "does not
                 exist".
**/
STATIC
BOOLEAN
ScalarVar (
  IN  CONST CHAR8  *Name,
  OUT CHAR8        *Value,
  IN  UINTN        ValueSize
  )
{
  if (VarNameIs (Name, "version-bootloader")) {
    AsciiSPrint (Value, ValueSize, "%a", FASTBOOT_VERSION_STRING);
  } else if (VarNameIs (Name, "version-baseband")) {
    //
    // The modem image is not mapped into UEFI.  Empty means "exists, no
    // content here", which keeps the host from treating the modem as
    // absent.
    //
    Value[0] = '\0';
  } else if (VarNameIs (Name, "product")) {
    AsciiSPrint (Value, ValueSize, "%a", FASTBOOT_PRODUCT_STRING);
  } else if (VarNameIs (Name, "serialno")) {
    AsciiSPrint (Value, ValueSize, "%a", FASTBOOT_SERIAL_STRING);
  } else if (VarNameIs (Name, "max-download-size")) {
    AsciiSPrint (Value, ValueSize, "%a", FASTBOOT_MAX_DOWNLOAD);
  } else if (VarNameIs (Name, "variant")) {
    AsciiStrCpyS (Value, ValueSize, "kirin970");
  } else if (VarNameIs (Name, "secure")) {
    AsciiStrCpyS (Value, ValueSize, "no");
  } else if (VarNameIs (Name, "unlocked")) {
    AsciiStrCpyS (Value, ValueSize, "yes");
  } else if (VarNameIs (Name, "tampered")) {
    AsciiStrCpyS (Value, ValueSize, "false");
  } else if (VarNameIs (Name, "rollback")) {
    AsciiStrCpyS (Value, ValueSize, "0");
  } else if (VarNameIs (Name, "is-userspace")) {
    AsciiStrCpyS (Value, ValueSize, "no");
  } else if (VarNameIs (Name, "slot-count")) {
    //
    // One GPT, no A/B.  Reporting 2 would make the host walk has-slot for
    // every partition and expect a slot we do not have.
    //
    AsciiStrCpyS (Value, ValueSize, "1");
  } else if (VarNameIs (Name, "current-slot")) {
    AsciiStrCpyS (Value, ValueSize, "a");
  } else if (VarNameIs (Name, "snapshot-update")) {
    AsciiStrCpyS (Value, ValueSize, "no");
  } else if (VarNameIs (Name, "verify")) {
    AsciiStrCpyS (Value, ValueSize, mVerifyFlash ? "on" : "off");
  } else if (AsciiStrnCmp (Name, "has-slot:", sizeof ("has-slot:") - 1) == 0) {
    AsciiStrCpyS (Value, ValueSize, "no");
  } else if (AsciiStrnCmp (Name, "is-logical:", sizeof ("is-logical:") - 1) == 0) {
    // Everything on the UFS GPT is a physical partition.
    AsciiStrCpyS (Value, ValueSize, "no");
  } else {
    return FALSE;
  }

  return TRUE;
}

/**
  Implement the subset of fastboot variables that matter for flashing.

  Anything unknown is reported as an empty value, which is what the protocol
  says to do when a variable does not exist.
**/
STATIC
EFI_STATUS
EFIAPI
FastbootPlatformGetVar (
  IN  CHAR8  *Name,
  OUT CHAR8  *Value
  )
{
  EFI_STATUS           Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  EFI_PARTITION_ENTRY   *Info;

  if (Name == NULL || Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Value[0] = '\0';

  //
  // "partition-size:<name>" / "partition-type:<name>"
  //
  if (AsciiStrnCmp (Name, "partition-size:", sizeof ("partition-size:") - 1) == 0) {
    Status = FindPartition (Name + sizeof ("partition-size:") - 1, &BlockIo, &Info);
    if (EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    AsciiSPrint (
      Value,
      FASTBOOT_VAR_MAX,
      "0x%lx",
      PartitionSize (BlockIo, Info)
      );
    return EFI_SUCCESS;
  }

  if (AsciiStrnCmp (Name, "partition-type:", sizeof ("partition-type:") - 1) == 0) {
    Status = FindPartition (Name + sizeof ("partition-type:") - 1, &BlockIo, &Info);
    if (EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    AsciiStrCpyS (Value, FASTBOOT_VAR_MAX, "basic");
    return EFI_SUCCESS;
  }

  //
  // "getvar all" - the protocol's dump-everything request.  The app hands
  // this function a 61-byte value buffer, which cannot hold a table, so the
  // content goes out first as INFO lines (the host prints each one before
  // the final OKAY) and the app then closes the command with an empty
  // OKAY.  Without this the host sees "all:" and nothing else, which reads
  // as "the bootloader cannot see its own partition table".
  //
  if (VarNameIs (Name, "all")) {
    CHAR8   Line[160];
    CHAR8   Pair[200];
    UINTN   Index;

    for (Index = 0; Index < ARRAY_SIZE (mScalarVarNames); Index++) {
      if (ScalarVar (mScalarVarNames[Index], Line, sizeof (Line))) {
        AsciiSPrint (Pair, sizeof (Pair), "%a:%a", mScalarVarNames[Index], Line);
        SendInfo (Pair);
      }
    }

    WalkPartitions (PartitionLineToHost);
    return EFI_SUCCESS;
  }

  //
  // A partition GUID is a real GPT field but nobody flashes by UUID here,
  // and leaving it out entirely makes some hosts fall back to the name -
  // which is the path that works.  Empty, not a made-up value.
  //
  if (AsciiStrnCmp (Name, "partition-uuid:", sizeof ("partition-uuid:") - 1) == 0) {
    return EFI_SUCCESS;
  }

  ScalarVar (Name, Value, FASTBOOT_VAR_MAX);
  return EFI_SUCCESS;
}

/**
  OEM commands.  Only the ones this driver actually implements.

    oem verify on|off   - toggle the read-back verification after flash
**/
STATIC
EFI_STATUS
EFIAPI
FastbootPlatformDoOemCommand (
  IN CHAR8  *Command
  )
{
  CHAR8  *Cursor = Command;

  if (Command == NULL) {
    return EFI_NOT_FOUND;
  }

  while (*Cursor == ' ') {
    Cursor++;
  }

  if (AsciiStrnCmp (Cursor, "verify", sizeof ("verify") - 1) == 0) {
    Cursor += sizeof ("verify") - 1;
    while (*Cursor == ' ') {
      Cursor++;
    }

    if (VarNameIs (Cursor, "on")) {
      mVerifyFlash = TRUE;
    } else if (VarNameIs (Cursor, "off")) {
      mVerifyFlash = FALSE;
    } else {
      return EFI_NOT_FOUND;
    }

    DEBUG ((DEBUG_INFO, TAG ": verify now %d\n", mVerifyFlash));
    return EFI_SUCCESS;
  }

  //
  // oem partitions - the table goes to the host as INFO lines, which is
  // where it is readable: getvar all has a 61-byte value buffer, far too
  // small for a table, and the device screen is not the place anyone is
  // looking when they typed the command.
  //
  if (VarNameIs (Cursor, "partitions")) {
    WalkPartitions (PartitionLineToHost);
    return EFI_SUCCESS;
  }

  //
  // oem device-info - the lock-state lines the stock tool prints, and the
  // first thing anyone runs to find out whether this bootloader will take
  // a flash at all.
  //
  if (VarNameIs (Cursor, "device-info")) {
    SendInfo ("Device unlocked: yes");
    SendInfo ("Device unlocked critical: yes");
    SendInfo ("Device tampered: false");
    SendInfo ("Write protected: no");
    SendInfo ("Bootloader version: " FASTBOOT_VERSION_STRING);
    return EFI_SUCCESS;
  }

  //
  // oem unlock / oem lock - reported, not enforced.  There is no verified
  // boot on this side yet, so claiming a state change that did not happen
  // would be worse than saying so.
  //
  if (VarNameIs (Cursor, "unlock") || VarNameIs (Cursor, "lock")) {
    SendInfo ("edk2-kirin970 does not enforce a lock state");
    SendInfo ("Device unlocked: yes");
    return EFI_SUCCESS;
  }

  //
  // oem enable-verity / oem disable-verity - hosts pass these when given
  // --disable-verity.  Nothing to toggle until Android is booted from here,
  // so answer rather than fail the whole command.
  //
  if (VarNameIs (Cursor, "enable-verity") || VarNameIs (Cursor, "disable-verity")) {
    SendInfo ("dm-verity is not managed by this bootloader");
    return EFI_SUCCESS;
  }

  //
  // oem help - what this bootloader actually implements, so the next
  // unknown command can be checked against a list instead of guessed at.
  //
  if (VarNameIs (Cursor, "help")) {
    SendInfo ("oem partitions          - list the GPT on the host");
    SendInfo ("oem device-info         - lock / write-protect state");
    SendInfo ("oem unlock | lock       - reported, not enforced");
    SendInfo ("oem enable|disable-verity - no-op, Android not booted from here");
    SendInfo ("oem verify on|off       - read the partition back after flashing");
    SendInfo ("oem help                - this list");
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

STATIC FASTBOOT_PLATFORM_PROTOCOL  mFastbootPlatform = {
  FastbootPlatformInit,
  FastbootPlatformUnInit,
  FastbootPlatformFlashPartition,
  FastbootPlatformErasePartition,
  FastbootPlatformGetVar,
  FastbootPlatformDoOemCommand
};

EFI_STATUS
EFIAPI
KirinFastbootPlatformEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gAndroidFastbootPlatformProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mFastbootPlatform
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, TAG ": installing platform protocol failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, TAG ": platform protocol installed\n"));
  return EFI_SUCCESS;
}
