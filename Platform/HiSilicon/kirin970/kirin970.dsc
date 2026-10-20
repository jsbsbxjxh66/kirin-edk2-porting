[Defines]
  SOC_PLATFORM         = KIRIN970
  USE_PHYSICAL_TIMER   = TRUE
  SECURE_BOOT_ENABLE   = FALSE
  AB_SLOTS_SUPPORT     = FALSE
  ENABLE_LINUX_UTILS   = 1
  MAINLINE_KERNEL_IN_FV = 0

!include Silicon/HiSilicon/kirin970/KirinPkg/KirinCommonDsc.inc

[PcdsFixedAtBuild.common]
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x00000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0xE0000000

  gArmTokenSpaceGuid.PcdCpuVectorBaseAddress|0x6FB8C000
  gArmTokenSpaceGuid.PcdArmArchTimerFreqInHz|1920000
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26
  gArmTokenSpaceGuid.PcdGicDistributorBase|0xE82B1000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0xE82B2000

  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x00000970

  gEmbeddedTokenSpaceGuid.PcdPrePiStackBase|0x6FB90000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackSize|0x00040000
  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|44

  gKirinTokenSpaceGuid.PcdUefiMemPoolBase|0x40100000
  gKirinTokenSpaceGuid.PcdUefiMemPoolSize|0x2EF00000

  gKirinTokenSpaceGuid.PcdMipiFrameBufferAddress|0x10600000

  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|2

  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterBase|0xFDF02000
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|115200
  gArmPlatformTokenSpaceGuid.PL011UartClkInHz|19200000

  gKirinTokenSpaceGuid.PcdBootShimInfo1|0xB0000000

  gSimpleInitTokenSpaceGuid.PcdDeviceTreeStore|0xB0010000

  # SimpleInit boot-menu pointer speed.
  # UsbMouseDxe hardcodes ResolutionX/Y = 8 counts/mm (UsbMouse.c:676) and
  # uefi_pointer.c converts a report with
  #   dx = RelativeMovementX * gui_mouse_scale / ResolutionX
  # in integer arithmetic.  The stock default of 1 therefore collapses eight
  # mouse counts into a single pixel and truncates the rest to zero, so the
  # cursor barely creeps across a 1080x2244 panel.  16 = 2 px per count;
  # it can still be tuned live from Settings -> mouse (range 1..64).
  gSimpleInitTokenSpaceGuid.PcdGuiDefaultMouseScale|16

  # Fastboot USB identity (EmbeddedPkg AndroidFastbootTransportUsbDxe).
  # The host tool matches on the interface class (0xff/0x42/0x03) rather than
  # these, but Windows picks the driver from the hardware ID, and the
  # android_winusb.inf section that yields "Android Bootloader Interface" is
  # USB\VID_18D1&PID_D00D - the ID the stock bootloader reports.  The previous
  # 0x4ee2 matched nothing installed here, so Windows bound the ADB driver
  # instead and fastboot could list the device (serial comes from the device
  # instance ID) but every bulk pipe write failed.
  gEmbeddedTokenSpaceGuid.PcdAndroidFastbootUsbVendorId|0x18d1
  gEmbeddedTokenSpaceGuid.PcdAndroidFastbootUsbProductId|0xd00d

[LibraryClasses.common]
  PlatformMemoryMapLib|Platform/$(VENDOR_NAME)/kirin970/Library/$(PLATFORM_NAME)/PlatformMemoryMapLib/PlatformMemoryMapLib.inf
  PlatformPeiLib|Silicon/HiSilicon/kirin970/Library/PlatformPeiLib/PlatformPeiLib.inf
  PlatformPrePiLib|Silicon/HiSilicon/kirin970/Library/PlatformPrePiLib/PlatformPrePiLib.inf
  MsPlatformDevicesLib|Silicon/HiSilicon/kirin970/Library/MsPlatformDevicesLib/MsPlatformDevicesLib.inf
  SOCSmbiosInfoLib|Silicon/HiSilicon/kirin970/Library/SOCSmbiosInfoLib/SOCSmbiosInfoLib.inf

[Components.common]
  # SimpleInit reboot-mode markers: PMIC HRST_REG13 (0x25A) over SPMI
  # plus the misc BCB, read by the Kirin970 boot chain after the reset.
  Silicon/HiSilicon/kirin970/Drivers/KirinRebootModeDxe/KirinRebootModeDxe.inf

  # In-Uefi fastboot: GPT-wide flash/erase of every UFS partition.
  #  * KirinUsbDeviceDxe       - gUsbDeviceProtocolGuid (DWC3 gadget)
  #  * KirinFastbootPlatformDxe  - gAndroidFastbootPlatformProtocolGuid
  #  * FastbootTransportUsbDxe   - USB transport (needs a USB_DEVICE producer)
  #  * KirinFastbootApp         - the AOSP fastboot 0.4 command engine,
  #                               plus flashing:/set_active:* (our copy of
  #                               EmbeddedPkg's AndroidFastbootApp; see its
  #                               .inf for why the FILE_GUID is unchanged)
  Silicon/HiSilicon/kirin970/Drivers/KirinUsbDeviceDxe/KirinUsbDeviceDxe.inf
  Silicon/HiSilicon/kirin970/Drivers/KirinFastbootPlatformDxe/KirinFastbootPlatformDxe.inf
  EmbeddedPkg/Drivers/AndroidFastbootTransportUsbDxe/FastbootTransportUsbDxe.inf

  Silicon/HiSilicon/kirin970/Application/KirinFastbootApp/KirinFastbootApp.inf

  # "USB 模式" boot menu folder: every way to use the DWC3 port.
  #  * KirinUsbMassStorage - BOT target backed by the UFS LUN0 whole-disk
  #                          BlockIo (the same GPT fastboot flashes)
  #  * KirinUsbHost        - hand the block back to XhciDxe (host mode)
  #  * KirinUsbOtg         - back to the default peripheral gadget
  Silicon/HiSilicon/kirin970/Application/KirinUsbMassStorage/KirinUsbMassStorage.inf
  Silicon/HiSilicon/kirin970/Application/KirinUsbHost/KirinUsbHost.inf
  Silicon/HiSilicon/kirin970/Application/KirinUsbOtg/KirinUsbOtg.inf
