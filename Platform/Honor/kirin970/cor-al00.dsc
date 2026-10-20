[BuildOptions.common]
  *_*_*_CC_FLAGS = -DENABLE_SIMPLE_INIT

[Defines]
  VENDOR_NAME                      = Honor
  PLATFORM_NAME                    = cor-al00
  PLATFORM_GUID                    = 28f1a3bf-193a-47e3-a7b9-5a435eaab2ee
  PLATFORM_VERSION                 = 0.1
  DSC_SPECIFICATION                = 0x00010019
  OUTPUT_DIRECTORY                 = Build/cor-al00
  SUPPORTED_ARCHITECTURES          = AARCH64
  BUILD_TARGETS                    = DEBUG|RELEASE
  SKUID_IDENTIFIER                 = DEFAULT
  FLASH_DEFINITION                 = Platform/HiSilicon/kirin970/kirin970.fdf
  DEVICE_DXE_FV_COMPONENTS         = Platform/Honor/kirin970/cor-al00.fdf.inc

!include Platform/HiSilicon/kirin970/kirin970.dsc

[PcdsFixedAtBuild.common]
  gKirinTokenSpaceGuid.PcdMipiFrameBufferWidth|1080
  # Panel native yres is 2340, but the bootloader's scanout window is 1080x2244
  # and going to 2340 shifts/cuts the picture on device.  Keep the bootloader's.
  gKirinTokenSpaceGuid.PcdMipiFrameBufferHeight|2244
  
  gRenegadePkgTokenSpaceGuid.PcdDeviceVendor|"Honor"
  gRenegadePkgTokenSpaceGuid.PcdDeviceProduct|"Honor Play"
  gRenegadePkgTokenSpaceGuid.PcdDeviceCodeName|"COR-AL00"
