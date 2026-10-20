[BuildOptions.common]
  *_*_*_CC_FLAGS = -DENABLE_SIMPLE_INIT

[Defines]
  VENDOR_NAME                      = Huawei
  PLATFORM_NAME                    = bla-al00
  PLATFORM_GUID                    = 7c3e8b1a-4d5f-4e2a-9b6c-1a2d3e4f5678
  PLATFORM_VERSION                 = 0.1
  DSC_SPECIFICATION                = 0x00010019
  OUTPUT_DIRECTORY                 = Build/bla-al00
  SUPPORTED_ARCHITECTURES          = AARCH64
  BUILD_TARGETS                    = DEBUG|RELEASE
  SKUID_IDENTIFIER                 = DEFAULT
  FLASH_DEFINITION                 = Platform/HiSilicon/kirin970/kirin970.fdf
  DEVICE_DXE_FV_COMPONENTS         = Platform/Huawei/kirin970/bla-al00.fdf.inc

!include Platform/HiSilicon/kirin970/kirin970.dsc

[PcdsFixedAtBuild.common]
  gArmTokenSpaceGuid.PcdSystemMemorySize|0xC0000000

  gKirinTokenSpaceGuid.PcdMipiFrameBufferWidth|1080
  gKirinTokenSpaceGuid.PcdMipiFrameBufferHeight|2160

  # AMOLED CMD mode: DSS_BASE(0xE8600000) + DSS_LDI0_OFFSET(0x7D000)
  gKirinTokenSpaceGuid.PcdDssLdiBase|0xE867D000

  gRenegadePkgTokenSpaceGuid.PcdDeviceVendor|"Huawei"
  gRenegadePkgTokenSpaceGuid.PcdDeviceProduct|"Huawei Mate 10 Pro"
  gRenegadePkgTokenSpaceGuid.PcdDeviceCodeName|"BLA-AL00"
