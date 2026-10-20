/** @file
  Implementation for PlatformBootManagerLib library class interfaces.

  Copyright (C) 2015-2016, Red Hat, Inc.
  Copyright (c) 2014 - 2019, ARM Ltd. All rights reserved.<BR>
  Copyright (c) 2004 - 2018, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2016, Linaro Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Guid/EventGroup.h>
#include <Guid/SerialPortLibVendor.h>
#include <Guid/TtyTerm.h>
#include <IndustryStandard/Pci22.h>
#include <Library/BootLogoLib.h>
#include <Library/CapsuleLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Library/MsPlatformDevicesLib.h>

#include <Protocol/DevicePath.h>
#include <Protocol/BlockIo.h>
#include <Protocol/EsrtManagement.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/PlatformBootManager.h>

#include "PlatformBm.h"

#define DP_NODE_LEN(Type)                                                      \
  {                                                                            \
    (UINT8)sizeof(Type), (UINT8)(sizeof(Type) >> 8)                            \
  }

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH         SerialDxe;
  UART_DEVICE_PATH           Uart;
  VENDOR_DEFINED_DEVICE_PATH TermType;
  EFI_DEVICE_PATH_PROTOCOL   End;
} PLATFORM_SERIAL_CONSOLE;
#pragma pack()

STATIC PLATFORM_SERIAL_CONSOLE mSerialConsole = {
    //
    // VENDOR_DEVICE_PATH SerialDxe
    //
    {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, DP_NODE_LEN(VENDOR_DEVICE_PATH)},
     EDKII_SERIAL_PORT_LIB_VENDOR_GUID},

    //
    // UART_DEVICE_PATH Uart
    //
    {
        {MESSAGING_DEVICE_PATH, MSG_UART_DP, DP_NODE_LEN(UART_DEVICE_PATH)},
        0,                                     // Reserved
        FixedPcdGet64(PcdUartDefaultBaudRate), // BaudRate
        FixedPcdGet8(PcdUartDefaultDataBits),  // DataBits
        FixedPcdGet8(PcdUartDefaultParity),    // Parity
        FixedPcdGet8(PcdUartDefaultStopBits)   // StopBits
    },

    //
    // VENDOR_DEFINED_DEVICE_PATH TermType
    //
    {
        {MESSAGING_DEVICE_PATH, MSG_VENDOR_DP,
         DP_NODE_LEN(VENDOR_DEFINED_DEVICE_PATH)}
        //
        // Guid to be filled in dynamically
        //
    },

    //
    // EFI_DEVICE_PATH_PROTOCOL End
    //
    {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
     DP_NODE_LEN(EFI_DEVICE_PATH_PROTOCOL)}};

#pragma pack(1)
typedef struct {
  USB_CLASS_DEVICE_PATH    Keyboard;
  EFI_DEVICE_PATH_PROTOCOL End;
} PLATFORM_USB_KEYBOARD;
#pragma pack()

STATIC PLATFORM_USB_KEYBOARD mUsbKeyboard = {
    //
    // USB_CLASS_DEVICE_PATH Keyboard
    //
    {
        {MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP,
         DP_NODE_LEN(USB_CLASS_DEVICE_PATH)},
        0xFFFF, // VendorId: any
        0xFFFF, // ProductId: any
        3,      // DeviceClass: HID
        1,      // DeviceSubClass: boot
        1       // DeviceProtocol: keyboard
    },

    //
    // EFI_DEVICE_PATH_PROTOCOL End
    //
    {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
     DP_NODE_LEN(EFI_DEVICE_PATH_PROTOCOL)}};

/**
  Check if the handle satisfies a particular condition.

  @param[in] Handle      The handle to check.
  @param[in] ReportText  A caller-allocated string passed in for reporting
                         purposes. It must never be NULL.

  @retval TRUE   The condition is satisfied.
  @retval FALSE  Otherwise. This includes the case when the condition could not
                 be fully evaluated due to an error.
**/
typedef BOOLEAN(EFIAPI *FILTER_FUNCTION)(
    IN EFI_HANDLE Handle, IN CONST CHAR16 *ReportText);

/**
  Process a handle.

  @param[in] Handle      The handle to process.
  @param[in] ReportText  A caller-allocated string passed in for reporting
                         purposes. It must never be NULL.
**/
typedef VOID(EFIAPI *CALLBACK_FUNCTION)(
    IN EFI_HANDLE Handle, IN CONST CHAR16 *ReportText);

/**
  Locate all handles that carry the specified protocol, filter them with a
  callback function, and pass each handle that passes the filter to another
  callback.

  @param[in] ProtocolGuid  The protocol to look for.

  @param[in] Filter        The filter function to pass each handle to. If this
                           parameter is NULL, then all handles are processed.

  @param[in] Process       The callback function to pass each handle to that
                           clears the filter.
**/
STATIC
VOID FilterAndProcess(
    IN EFI_GUID *ProtocolGuid, IN FILTER_FUNCTION Filter OPTIONAL,
    IN CALLBACK_FUNCTION Process)
{
  EFI_STATUS  Status;
  EFI_HANDLE *Handles;
  UINTN       NoHandles;
  UINTN       Idx;

  Status = gBS->LocateHandleBuffer(
      ByProtocol, ProtocolGuid, NULL /* SearchKey */, &NoHandles, &Handles);
  if (EFI_ERROR(Status)) {
    //
    // This is not an error, just an informative condition.
    //
    DEBUG((EFI_D_VERBOSE, "%a: %g: %r\n", __FUNCTION__, ProtocolGuid, Status));
    return;
  }

  ASSERT(NoHandles > 0);
  for (Idx = 0; Idx < NoHandles; ++Idx) {
    CHAR16 *      DevicePathText;
    STATIC CHAR16 Fallback[] = L"<device path unavailable>";

    //
    // The ConvertDevicePathToText() function handles NULL input transparently.
    //
    DevicePathText = ConvertDevicePathToText(
        DevicePathFromHandle(Handles[Idx]),
        FALSE, // DisplayOnly
        FALSE  // AllowShortcuts
    );
    if (DevicePathText == NULL) {
      DevicePathText = Fallback;
    }

    if (Filter == NULL || Filter(Handles[Idx], DevicePathText)) {
      Process(Handles[Idx], DevicePathText);
    }

    if (DevicePathText != Fallback) {
      FreePool(DevicePathText);
    }
  }
  gBS->FreePool(Handles);
}

/**
  This FILTER_FUNCTION checks if a handle corresponds to a PCI display device.
**/
STATIC
BOOLEAN
EFIAPI
IsPciDisplay(IN EFI_HANDLE Handle, IN CONST CHAR16 *ReportText)
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL *PciIo;
  PCI_TYPE00           Pci;

  Status = gBS->HandleProtocol(Handle, &gEfiPciIoProtocolGuid, (VOID **)&PciIo);
  if (EFI_ERROR(Status)) {
    //
    // This is not an error worth reporting.
    //
    return FALSE;
  }

  Status = PciIo->Pci.Read(
      PciIo, EfiPciIoWidthUint32, 0 /* Offset */, sizeof Pci / sizeof(UINT32),
      &Pci);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "%a: %s: %r\n", __FUNCTION__, ReportText, Status));
    return FALSE;
  }

  return IS_PCI_DISPLAY(&Pci);
}

/**
  This CALLBACK_FUNCTION attempts to connect a handle non-recursively, asking
  the matching driver to produce all first-level child handles.
**/
STATIC
VOID EFIAPI Connect(IN EFI_HANDLE Handle, IN CONST CHAR16 *ReportText)
{
  EFI_STATUS Status;

  Status = gBS->ConnectController(
      Handle, // ControllerHandle
      NULL,   // DriverImageHandle
      NULL,   // RemainingDevicePath -- produce all children
      FALSE   // Recursive
  );
  DEBUG(
      (EFI_ERROR(Status) ? EFI_D_ERROR : EFI_D_VERBOSE, "%a: %s: %r\n",
       __FUNCTION__, ReportText, Status));
}

/**
  This CALLBACK_FUNCTION retrieves the EFI_DEVICE_PATH_PROTOCOL from the
  handle, and adds it to ConOut and ErrOut.
**/
STATIC
VOID EFIAPI AddOutput(IN EFI_HANDLE Handle, IN CONST CHAR16 *ReportText)
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;

  DevicePath = DevicePathFromHandle(Handle);
  if (DevicePath == NULL) {
    DEBUG(
        (EFI_D_ERROR, "%a: %s: handle %p: device path not found\n",
         __FUNCTION__, ReportText, Handle));
    return;
  }

  Status = EfiBootManagerUpdateConsoleVariable(ConOut, DevicePath, NULL);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (EFI_D_ERROR, "%a: %s: adding to ConOut: %r\n", __FUNCTION__,
         ReportText, Status));
    return;
  }

  Status = EfiBootManagerUpdateConsoleVariable(ErrOut, DevicePath, NULL);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (EFI_D_ERROR, "%a: %s: adding to ErrOut: %r\n", __FUNCTION__,
         ReportText, Status));
    return;
  }

  DEBUG(
      (EFI_D_VERBOSE, "%a: %s: added to ConOut and ErrOut\n", __FUNCTION__,
       ReportText));
}

STATIC
UINT16
PlatformRegisterFvBootOption(
    CONST EFI_GUID *FileGuid, CHAR16 *Description, UINT32 Attributes)
{
  EFI_STATUS                        Status;
  INTN                              OptionIndex;
  EFI_BOOT_MANAGER_LOAD_OPTION      NewOption;
  EFI_BOOT_MANAGER_LOAD_OPTION *    BootOptions;
  UINTN                             BootOptionCount;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH FileNode;
  EFI_LOADED_IMAGE_PROTOCOL *       LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL *        DevicePath;
  UINT16                            OptionNumber;

  Status = gBS->HandleProtocol(
      gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  ASSERT_EFI_ERROR(Status);

  EfiInitializeFwVolDevicepathNode(&FileNode, FileGuid);
  DevicePath = DevicePathFromHandle(LoadedImage->DeviceHandle);
  ASSERT(DevicePath != NULL);
  DevicePath =
      AppendDevicePathNode(DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  ASSERT(DevicePath != NULL);

  Status = EfiBootManagerInitializeLoadOption(
      &NewOption, LoadOptionNumberUnassigned, LoadOptionTypeBoot, Attributes,
      Description, DevicePath, NULL, 0);
  ASSERT_EFI_ERROR(Status);
  FreePool(DevicePath);

  BootOptions =
      EfiBootManagerGetLoadOptions(&BootOptionCount, LoadOptionTypeBoot);

  OptionIndex =
      EfiBootManagerFindLoadOption(&NewOption, BootOptions, BootOptionCount);

  if (OptionIndex == -1) {
    Status = EfiBootManagerAddLoadOptionVariable(&NewOption, MAX_UINTN);
    ASSERT_EFI_ERROR(Status);
  }
  OptionNumber = NewOption.OptionNumber;
  EfiBootManagerFreeLoadOption(&NewOption);
  EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);
  return OptionNumber;
}

STATIC
VOID GetPlatformOptions(VOID)
{
  EFI_STATUS                      Status;
  EFI_BOOT_MANAGER_LOAD_OPTION *  CurrentBootOptions;
  EFI_BOOT_MANAGER_LOAD_OPTION *  BootOptions;
  EFI_INPUT_KEY *                 BootKeys;
  PLATFORM_BOOT_MANAGER_PROTOCOL *PlatformBootManager;
  UINTN                           CurrentBootOptionCount;
  UINTN                           Index;
  UINTN                           BootCount;

  Status = gBS->LocateProtocol(
      &gPlatformBootManagerProtocolGuid, NULL, (VOID **)&PlatformBootManager);
  if (EFI_ERROR(Status)) {
    return;
  }
  Status = PlatformBootManager->GetPlatformBootOptionsAndKeys(
      &BootCount, &BootOptions, &BootKeys);
  if (EFI_ERROR(Status)) {
    return;
  }
  //
  // Fetch the existent boot options. If there are none, CurrentBootCount
  // will be zeroed.
  //
  CurrentBootOptions =
      EfiBootManagerGetLoadOptions(&CurrentBootOptionCount, LoadOptionTypeBoot);
  //
  // Process the platform boot options.
  //
  for (Index = 0; Index < BootCount; Index++) {
    INTN  Match;
    UINTN BootOptionNumber;

    //
    // If there are any preexistent boot options, and the subject platform boot
    // option is already among them, then don't try to add it. Just get its
    // assigned boot option number so we can associate a hotkey with it. Note
    // that EfiBootManagerFindLoadOption() deals fine with (CurrentBootOptions
    // == NULL) if (CurrentBootCount == 0).
    //
    Match = EfiBootManagerFindLoadOption(
        &BootOptions[Index], CurrentBootOptions, CurrentBootOptionCount);
    if (Match >= 0) {
      BootOptionNumber = CurrentBootOptions[Match].OptionNumber;
    }
    else {
      //
      // Add the platform boot options as a new one, at the end of the boot
      // order. Note that if the platform provided this boot option with an
      // unassigned option number, then the below function call will assign a
      // number.
      //
      Status =
          EfiBootManagerAddLoadOptionVariable(&BootOptions[Index], MAX_UINTN);
      if (EFI_ERROR(Status)) {
        DEBUG(
            (DEBUG_ERROR, "%a: failed to register \"%s\": %r\n", __FUNCTION__,
             BootOptions[Index].Description, Status));
        continue;
      }
      BootOptionNumber = BootOptions[Index].OptionNumber;
    }

    //
    // Register a hotkey with the boot option, if requested.
    //
    if (BootKeys[Index].UnicodeChar == L'\0') {
      continue;
    }

    Status = EfiBootManagerAddKeyOptionVariable(
        NULL, BootOptionNumber, 0, &BootKeys[Index], NULL);
    if (EFI_ERROR(Status)) {
      DEBUG(
          (DEBUG_ERROR, "%a: failed to register hotkey for \"%s\": %r\n",
           __FUNCTION__, BootOptions[Index].Description, Status));
    }
  }
  EfiBootManagerFreeLoadOptions(CurrentBootOptions, CurrentBootOptionCount);
  EfiBootManagerFreeLoadOptions(BootOptions, BootCount);
  FreePool(BootKeys);
}

STATIC
VOID PlatformRegisterOptionsAndKeys(VOID)
{
  EFI_STATUS                   Status;
  EFI_INPUT_KEY                Enter;
  EFI_INPUT_KEY                UP;
  EFI_INPUT_KEY                Esc;
  EFI_BOOT_MANAGER_LOAD_OPTION BootOption;

  GetPlatformOptions();

  //
  // Register ENTER as CONTINUE key
  //
  Enter.ScanCode    = SCAN_NULL;
  Enter.UnicodeChar = CHAR_CARRIAGE_RETURN;
  Status            = EfiBootManagerRegisterContinueKeyOption(0, &Enter, NULL);
  ASSERT_EFI_ERROR(Status);

  //
  // Map UP and ESC to Boot Manager Menu or SimpleInitGUI
  //
  UP.ScanCode     = SCAN_UP;
  UP.UnicodeChar  = CHAR_NULL;
  Esc.ScanCode    = SCAN_ESC;
  Esc.UnicodeChar = CHAR_NULL;
  Status          = EfiBootManagerGetBootManagerMenu(&BootOption);
  ASSERT_EFI_ERROR(Status);
#ifdef ENABLE_SIMPLE_INIT

  //
  // Register Simple Init GUI APP
  //
  UINT16 OptionSimpleInit = PlatformRegisterFvBootOption(
      &gSimpleInitFileGuid, L"Simple Init", LOAD_OPTION_ACTIVE);
  Status = EfiBootManagerAddKeyOptionVariable(
      NULL, (UINT16)OptionSimpleInit, 0, &UP, NULL);
#else
  Status = EfiBootManagerAddKeyOptionVariable(
      NULL, (UINT16)BootOption.OptionNumber, 0, &UP, NULL);
#endif
  ASSERT(Status == EFI_SUCCESS || Status == EFI_ALREADY_STARTED);
}

//
// BDS Platform Functions
//
/**
  Do the platform init, can be customized by OEM/IBV
  Possible things that can be done in PlatformBootManagerBeforeConsole:
  > Update console variable: 1. include hot-plug devices;
  >                          2. Clear ConIn and add SOL for AMT
  > Register new Driver#### or Boot####
  > Register new Key####: e.g.: F12
  > Signal ReadyToLock event
  > Authentication action: 1. connect Auth devices;
  >                        2. Identify auto logon user.
**/
VOID EFIAPI PlatformBootManagerBeforeConsole(VOID)
{
  //
  // Signal EndOfDxe PI Event
  //
  EfiEventGroupSignal(&gEfiEndOfDxeEventGroupGuid);

  //
  // Dispatch deferred images after EndOfDxe event.
  //
  EfiBootManagerDispatchDeferredImages();

  //
  // Locate the PCI root bridges and make the PCI bus driver connect each,
  // non-recursively. This will produce a number of child handles with PciIo on
  // them.
  //
  FilterAndProcess(&gEfiPciRootBridgeIoProtocolGuid, NULL, Connect);

  //
  // Find all display class PCI devices (using the handles from the previous
  // step), and connect them non-recursively. This should produce a number of
  // child handles with GOPs on them.
  //
  FilterAndProcess(&gEfiPciIoProtocolGuid, IsPciDisplay, Connect);

  //
  // Now add the device path of all handles with GOP on them to ConOut and
  // ErrOut.
  //
  FilterAndProcess(&gEfiGraphicsOutputProtocolGuid, NULL, AddOutput);

  //
  // Add the hardcoded short-form USB keyboard device path to ConIn.
  //
  EfiBootManagerUpdateConsoleVariable(
      ConIn, (EFI_DEVICE_PATH_PROTOCOL *)&mUsbKeyboard, NULL);

  EFI_HANDLE *              handles;
  UINTN                     NoHandles;
  EFI_DEVICE_PATH_PROTOCOL *devicehandle;
  /*CHAR16 *devicepathtxt;*/
  // EfiBootManagerUpdateConsoleVariable(ConIn,
  //  (EFI_DEVICE_PATH_PROTOCOL*)&gQcomKeypadDeviceGuid, NULL);
  gBS->LocateHandleBuffer(
      ByProtocol, &gEfiSimpleTextInputExProtocolGuid, NULL, &NoHandles,
      &handles);
  devicehandle = DevicePathFromHandle(handles[1]);
  EfiBootManagerUpdateConsoleVariable(
      ConIn, devicehandle, NULL); /*
            devicepathtxt = ConvertDevicePathToText(devicehandle,TRUE,TRUE);
            DEBUG((DEBUG_ERROR,"There are %s handles\n",devicepathtxt));
            ASSERT(0);*/

  //
  // Add the hardcoded serial console device path to ConIn, ConOut, ErrOut.
  //
  ASSERT(FixedPcdGet8(PcdDefaultTerminalType) == 4);
  CopyGuid(&mSerialConsole.TermType.Guid, &gEfiTtyTermGuid);

  EfiBootManagerUpdateConsoleVariable(
      ConIn, (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);
  EfiBootManagerUpdateConsoleVariable(
      ConOut, (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);
  EfiBootManagerUpdateConsoleVariable(
      ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole, NULL);

  //
  // Register platform-specific boot options and keyboard shortcuts.
  //
  PlatformRegisterOptionsAndKeys();
}

STATIC
VOID HandleCapsules(VOID)
{
  ESRT_MANAGEMENT_PROTOCOL *EsrtManagement;
  EFI_PEI_HOB_POINTERS      HobPointer;
  EFI_CAPSULE_HEADER *      CapsuleHeader;
  BOOLEAN                   NeedReset;
  EFI_STATUS                Status;

  DEBUG((DEBUG_INFO, "%a: processing capsules ...\n", __FUNCTION__));

  Status = gBS->LocateProtocol(
      &gEsrtManagementProtocolGuid, NULL, (VOID **)&EsrtManagement);
  if (!EFI_ERROR(Status)) {
    EsrtManagement->SyncEsrtFmp();
  }

  //
  // Find all capsule images from hob
  //
  HobPointer.Raw = GetHobList();
  NeedReset      = FALSE;
  while ((HobPointer.Raw =
              GetNextHob(EFI_HOB_TYPE_UEFI_CAPSULE, HobPointer.Raw)) != NULL) {
    CapsuleHeader = (VOID *)(UINTN)HobPointer.Capsule->BaseAddress;

    Status = ProcessCapsuleImage(CapsuleHeader);
    if (EFI_ERROR(Status)) {
      DEBUG(
          (DEBUG_ERROR, "%a: failed to process capsule %p - %r\n", __FUNCTION__,
           CapsuleHeader, Status));
      return;
    }

    NeedReset      = TRUE;
    HobPointer.Raw = GET_NEXT_HOB(HobPointer);
  }

  if (NeedReset) {
    DEBUG(
        (DEBUG_WARN, "%a: capsule update successful, resetting ...\n",
         __FUNCTION__));

    gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    CpuDeadLoop();
  }
}

#define VERSION_STRING_PREFIX L"Tianocore/EDK2 firmware version "

//
// FAT boot-sector health check and light repair, mirroring the cheap
// items fsck.fat -a does.
//
// Hard power-offs leave the FAT "dirty" bit set (bit0 of the state
// byte: FAT16 offset 0x25, FAT32 offset 0x41; kernel fs/fat sets it
// on mount and clears it on clean unmount) and can corrupt the
// FAT32 FSInfo signatures. A leftover dirty bit makes hosts treat
// the volume as damaged; clear it and restore the FSInfo signatures
// so a power-cycled ESP still mounts cleanly. Cross-linked clusters
// still need a real fsck.vfat from Linux/TWRP - the summary message
// says so.
//
#define FAT16_STATE_OFFSET   0x25
#define FAT32_STATE_OFFSET   0x41
#define FAT32_INFOSEC_OFFSET 48     // BPB: FSInfo sector number (u16)
#define FAT_DIRTY_BIT        0x01
#define FSINFO_LEAD_SIG      0x41615252UL   // "RRaA"
#define FSINFO_STRUC_SIG     0x61417272UL   // "rrAa" @484

STATIC
VOID
EspFatHealthCheck (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             *Handles;
  UINTN                  HandleCount;
  UINTN                  Index;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_BLOCK_IO_MEDIA     *Media;
  UINT8                  *Sector;
  UINT8                  *FsInfo;
  UINT16                 InfoSector;
  UINTN                  DirtyCleared   = 0;
  UINTN                  FsInfoRepaired = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiBlockIoProtocolGuid,
                  NULL, &HandleCount, &Handles
                  );
  if (EFI_ERROR (Status) || (HandleCount == 0)) {
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (
                      Handles[Index], &gEfiBlockIoProtocolGuid,
                      (VOID **)&BlockIo)))
    {
      continue;
    }
    Media = BlockIo->Media;
    if ((Media == NULL) || !Media->MediaPresent ||
        !Media->LogicalPartition || (Media->BlockSize < 512))
    {
      continue;  // whole-disk handles; partitions only
    }

    Sector = (UINT8 *)AllocatePool (Media->BlockSize);
    if (Sector == NULL) {
      break;
    }

    do {
      Status = BlockIo->ReadBlocks (
                          BlockIo, Media->MediaId, 0, Media->BlockSize, Sector);
      if (EFI_ERROR (Status)) {
        break;
      }

      //
      // Valid FAT boot sector? (0x55AA at 510 and a FAT type string)
      //
      if ((Sector[510] != 0x55) || (Sector[511] != 0xAA)) {
        break;
      }

      if (CompareMem (Sector + 82, "FAT32", 5) == 0) {
        //
        // FAT32: dirty bit + FSInfo signature repair.
        //
        if (Sector[FAT32_STATE_OFFSET] & FAT_DIRTY_BIT) {
          Sector[FAT32_STATE_OFFSET] &= (UINT8)~FAT_DIRTY_BIT;
          Status = BlockIo->WriteBlocks (
                              BlockIo, Media->MediaId, 0, Media->BlockSize,
                              Sector);
          if (!EFI_ERROR (Status)) {
            DirtyCleared++;
            DEBUG ((DEBUG_ERROR,
                    "FAT check: dirty bit cleared (hard power-off detected)"
                    "\n"));
          }
        }

        InfoSector = *(UINT16 *)(Sector + FAT32_INFOSEC_OFFSET);
        if ((InfoSector != 0) && (InfoSector != 0xFFFF) &&
            ((UINT64)InfoSector < Media->LastBlock))
        {
          FsInfo = (UINT8 *)AllocatePool (Media->BlockSize);
          if (FsInfo != NULL) {
            Status = BlockIo->ReadBlocks (
                                BlockIo, Media->MediaId, InfoSector,
                                Media->BlockSize, FsInfo);
            if (!EFI_ERROR (Status) &&
                ((*(UINT32 *)(FsInfo + 0)   != FSINFO_LEAD_SIG) ||
                 (*(UINT32 *)(FsInfo + 484) != FSINFO_STRUC_SIG)))
            {
              //
              // Broken FSInfo: restore the signatures and mark the
              // cluster counts unknown (what fsck.fat does), then
              // fix the trail signature.
              //
              *(UINT32 *)(FsInfo + 0)   = FSINFO_LEAD_SIG;
              *(UINT32 *)(FsInfo + 484) = FSINFO_STRUC_SIG;
              *(UINT32 *)(FsInfo + 488) = 0xFFFFFFFF;  // free count
              *(UINT32 *)(FsInfo + 492) = 0xFFFFFFFF;  // next free
              FsInfo[508] = 0x55;
              FsInfo[509] = 0xAA;
              if (!EFI_ERROR (BlockIo->WriteBlocks (
                                BlockIo, Media->MediaId, InfoSector,
                                Media->BlockSize, FsInfo)))
              {
                FsInfoRepaired++;
                DEBUG ((DEBUG_ERROR, "FAT check: FSInfo repaired\n"));
              }
            }
            FreePool (FsInfo);
          }
        }
      } else if (CompareMem (Sector + 54, "FAT", 3) == 0) {
        //
        // FAT12/FAT16: dirty bit only.
        //
        if (Sector[FAT16_STATE_OFFSET] & FAT_DIRTY_BIT) {
          Sector[FAT16_STATE_OFFSET] &= (UINT8)~FAT_DIRTY_BIT;
          Status = BlockIo->WriteBlocks (
                              BlockIo, Media->MediaId, 0, Media->BlockSize,
                              Sector);
          if (!EFI_ERROR (Status)) {
            DirtyCleared++;
            DEBUG ((DEBUG_ERROR,
                    "FAT check: dirty bit cleared (hard power-off detected)"
                    "\n"));
          }
        }
      }
    } while (FALSE);

    FreePool (Sector);
  }

  FreePool (Handles);

  if ((DirtyCleared != 0) || (FsInfoRepaired != 0)) {
    DEBUG ((DEBUG_ERROR,
            "FAT check: %u dirty flag(s) cleared, %u FSInfo repaired"
            " - if files still fail, run fsck.vfat from Linux/TWRP\n",
            (UINT32)DirtyCleared, (UINT32)FsInfoRepaired));
  } else {
    DEBUG ((DEBUG_INFO, "FAT check: all volumes clean\n"));
  }
}

/**
  Do the platform specific action after the console is ready
  Possible things that can be done in PlatformBootManagerAfterConsole:
  > Console post action:
    > Dynamically switch output mode from 100x31 to 80x25 for certain scenario
    > Signal console ready platform customized event
  > Run diagnostics like memory testing
  > Connect certain devices
  > Dispatch additional option roms
  > Special boot: e.g.: USB boot, enter UI
**/
VOID EFIAPI PlatformBootManagerAfterConsole(VOID)
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *GraphicsOutput;
  UINTN                         FirmwareVerLength;
  UINTN                         PosX;
  UINTN                         PosY;

  FirmwareVerLength = StrLen(PcdGetPtr(PcdFirmwareVersionString));

  //
  // Show the splash screen.
  //
  Status = BootLogoEnableLogo();
  if (EFI_ERROR(Status)) {
    if (FirmwareVerLength > 0) {
      Print(VERSION_STRING_PREFIX L"%s\n", PcdGetPtr(PcdFirmwareVersionString));
    }
#ifdef ENABLE_SIMPLE_INIT
    Print(L"Press any side button for SimpleInitGUI");
#else
    Print(L"Press any side button for Boot Options");
#endif
  }
  else if (FirmwareVerLength > 0) {
    Status = gBS->HandleProtocol(
        gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid,
        (VOID **)&GraphicsOutput);
    if (!EFI_ERROR(Status)) {
      PosX = (GraphicsOutput->Mode->Info->HorizontalResolution -
              (StrLen(VERSION_STRING_PREFIX) + FirmwareVerLength) *
                  EFI_GLYPH_WIDTH) /
             2;
      PosY = 0;

      PrintXY(
          PosX, PosY, NULL, NULL, VERSION_STRING_PREFIX L"%s",
          PcdGetPtr(PcdFirmwareVersionString));
    }
  }

  //
  // Connect the rest of the devices. The UFS -> ScsiBus -> ScsiDisk ->
  // Partition -> FAT chain binds here; SimpleInit's boot prober scans
  // exactly once and never rescans, so a FAT binding that slipped this
  // window leaves the GUI without boot entries even though the UEFI
  // Shell (which runs its own map -r / ConnectAll on entry) sees the
  // filesystem fine. Verify the bind actually happened and retry.
  //
  {
    UINTN   SfsHandleCount = 0;
    EFI_HANDLE *SfsHandles = NULL;
    UINTN   Retry;

    EfiBootManagerConnectAll();
    for (Retry = 0; Retry < 5; Retry++) {
      SfsHandleCount = 0;
      SfsHandles = NULL;
      if (gBS->LocateHandleBuffer(
            ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
            NULL, &SfsHandleCount, &SfsHandles) == EFI_SUCCESS) {
        if (SfsHandles != NULL) {
          FreePool(SfsHandles);
        }
        if (SfsHandleCount > 0) {
          break;
        }
      }
      gBS->Stall(100 * 1000);  // 100ms between attempts
      EfiBootManagerConnectAll();
    }

    DEBUG((
      SfsHandleCount > 0 ? DEBUG_INFO : DEBUG_ERROR,
      "BmConnect: %u SimpleFileSystem handle(s) after ConnectAll\n",
      (UINT32)SfsHandleCount
      ));
  }

  //
  // FAT health check / light repair on all partitions (dirty bit
  // from hard power-offs, broken FSInfo signatures).
  //
  EspFatHealthCheck();

  //
  // On ARM, there is currently no reason to use the phased capsule
  // update approach where some capsules are dispatched before EndOfDxe
  // and some are dispatched after. So just handle all capsules here,
  // when the console is up and we can actually give the user some
  // feedback about what is going on.
  //
  HandleCapsules();

  //
  // Enumerate all possible boot options.
  //
  EfiBootManagerRefreshAllBootOption();

  //
  // Register UEFI Shell
  //
  PlatformRegisterFvBootOption(
      &gUefiShellFileGuid, L"UEFI Shell", LOAD_OPTION_ACTIVE);

#ifdef ENABLE_LINUX_SIMPLE_MASS_STORAGE
  //
  // Register Built-in Linux Kernel
  //
  PlatformRegisterFvBootOption(
      &gLinuxSimpleMassStorageGuid, L"USB Attached SCSI (UAS) Storage", LOAD_OPTION_ACTIVE);
#endif

#ifdef AB_SLOTS_SUPPORT
  //
  // Register Switch Slots App
  //
  PlatformRegisterFvBootOption(
      &gSwitchSlotsAppFileGuid, L"Reboot to other slot", LOAD_OPTION_ACTIVE);
#endif

  PlatformSetup();
}

/**
  This function is called each second during the boot manager waits the
  timeout.

  @param TimeoutRemain  The remaining timeout.
**/
VOID EFIAPI PlatformBootManagerWaitCallback(UINT16 TimeoutRemain)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION Black;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION White;
  UINT16                              Timeout;
  EFI_STATUS                          Status;

  Timeout = PcdGet16(PcdPlatformBootTimeOut);

  if (Timeout != 0 && TimeoutRemain <= 0) {
    gST->ConOut->ClearScreen(gST->ConOut);
    BootLogoEnableLogo ();
    return;
  }

  Black.Raw = 0x00000000;
  White.Raw = 0x00FFFFFF;

  Status = BootLogoUpdateProgress(
      White.Pixel, Black.Pixel,
#ifdef ENABLE_SIMPLE_INIT
      L"Press any side button for SimpleInitGUI",
#else
      L"Press any side button for Boot Options",
#endif
      White.Pixel, (Timeout - TimeoutRemain) * 100 / Timeout, 0);
  if (EFI_ERROR(Status)) {
    Print(L".");
  }
}

/**
  The function is called when no boot option could be launched,
  including platform recovery options and options pointing to applications
  built into firmware volumes.

  There is no ESP or OS boot entry on this device, so ending up here is
  the normal state after the SimpleInit GUI exits. Keep relaunching the
  GUI from the firmware volume (with the failure status printed) instead
  of returning into the BDS dead loop, which freezes the screen.

  If this function returns, BDS attempts to enter an infinite loop.
**/
VOID EFIAPI PlatformBootManagerUnableToBoot(VOID)
{
#ifdef ENABLE_SIMPLE_INIT
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL       *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL        *FvDevicePath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_DEVICE_PATH_PROTOCOL        *DevicePath;
  EFI_HANDLE                       ImageHandle;

  Status = gBS->HandleProtocol (
                  gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  if (EFI_ERROR (Status)) {
    return;
  }

  EfiInitializeFwVolDevicepathNode (&FileNode, &gSimpleInitFileGuid);

  FvDevicePath = DevicePathFromHandle (LoadedImage->DeviceHandle);
  if (FvDevicePath == NULL) {
    return;
  }

  DevicePath = AppendDevicePathNode (
                 FvDevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  if (DevicePath == NULL) {
    return;
  }

  //
  // Relaunch SimpleInit whenever it exits. Applications stay loaded after
  // StartImage, so unload the image to avoid leaking one image allocation
  // per loop.
  //
  for (;;) {
    Status = gBS->LoadImage (
                    FALSE, gImageHandle, DevicePath, NULL, 0, &ImageHandle);
    if (EFI_ERROR (Status)) {
      DEBUG ((
          DEBUG_ERROR,
          "UnableToBoot: LoadImage(SimpleInit) failed: %r\n",
          Status));
      return;
    }

    Status = gBS->StartImage (ImageHandle, NULL, NULL);
    if (EFI_ERROR (Status)) {
      DEBUG ((
          DEBUG_ERROR,
          "UnableToBoot: StartImage(SimpleInit) returned: %r\n",
          Status));
    }
    gBS->UnloadImage (ImageHandle);
  }
#endif
}
