#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <Protocol/DevicePath.h>

#define UFS_HC_BASE       0xFF3C0000
#define UFS_SYS_CTRL_BASE 0xFF3E0000
#define SCTRL_BASE        0xFFF0A000

//
// UFSHCI register offsets
//
#define UFS_HC_AHIT_OFFSET  0x0018
#define UFS_HC_IS_OFFSET    0x0020
#define UFS_HC_OCPTHRTL     0x00C0

//
// UFS SYS CTRL register offsets
//
#define UFS_SYS_PSW_POWER_CTRL      0x04
#define UFS_SYS_PHY_ISO_EN          0x08
#define UFS_SYS_HC_LP_CTRL          0x0C
#define UFS_SYS_PHY_CLK_CTRL        0x10
#define UFS_SYS_PSW_CLK_CTRL        0x14
#define UFS_SYS_CLOCK_GATE_BYPASS   0x18
#define UFS_SYS_RESET_CTRL_EN       0x1C
#define UFS_SYS_UFS_SYSCTRL         0x5C
#define UFS_SYS_UFS_DEVICE_RESET    0x60
#define UFS_SYS_UFS_UMECTRL         0x64
#define UFS_SYS_CRG_UFS_CFG         0x7C

//
// Bit definitions
//
#define BIT_UFS_PSW_MTCMOS_EN       BIT0
#define BIT_UFS_PSW_ISO_CTRL        BIT16
#define BIT_UFS_PHY_ISO_CTRL        BIT0
#define BIT_SYSCTRL_LP_ISOL_EN      BIT16
#define BIT_SYSCTRL_PWR_READY       BIT8
#define BIT_SYSCTRL_REF_CLOCK_EN    BIT24
#define BIT_SYSCTRL_PSW_CLK_EN      BIT4
#define BIT_SYSCTRL_LP_RESET_N      BIT0
#define MASK_UFS_CLK_GATE_BYPASS    0x3F
#define MASK_UFS_SYSCRTL_BYPASS     (0x3FU << 16)
#define MASK_UFS_DEVICE_RESET       BIT16
#define BIT_UFS_DEVICE_RESET        BIT0
#define MASK_SYSCTRL_REF_CLOCK_SEL  (0x3U << 8)
#define MASK_SYSCTRL_CFG_CLOCK_FREQ 0xFF

//
// MPHY RHOLD (from efuse)
//
#define SCDEEPSLEEPED_OFFSET        0x008
#define EFUSE_RHOLD_BIT             BIT22
#define MASK_UFS_MPHY_RHOLD         (BIT11 << 16)
#define BIT_UFS_MPHY_RHOLD          BIT11

//
// SCTRL register offsets
//
#define SCTRL_SCPEREN4   0x1B0
#define SCTRL_SCPERDIS4  0x1B4
#define SCTRL_SCCLKDIV9  0x274

//
// UIC command opcodes
//
#define UIC_CMD_DME_SET  0x02
#define UIC_CMD_DME_GET  0x01

//
// UIC_ARG_MIB_SEL: (attr << 16) | gen_selector
//
#define UIC_ARG_MIB_SEL(attr, sel)  (((UINT32)(attr) << 16) | (sel))
#define UIC_ARG_MIB(attr)           UIC_ARG_MIB_SEL(attr, 0)

//
// MPHY TX FSM state
//
#define MPHY_TX_FSM_STATE   0x0041
#define TX_FSM_HIBERN8      0x1

//
// Auto H8 power gating
//
#define LP_AH8_PGE          BIT17

STATIC
VOID
UfsSysCtrlSetBits (
  IN UINT32  Bits,
  IN UINT32  Offset
  )
{
  MmioOr32 (UFS_SYS_CTRL_BASE + Offset, Bits);
}

STATIC
VOID
UfsSysCtrlClrBits (
  IN UINT32  Bits,
  IN UINT32  Offset
  )
{
  MmioAnd32 (UFS_SYS_CTRL_BASE + Offset, ~Bits);
}

STATIC
VOID
KirinUfsSocInit (
  VOID
  )
{
  UINT32  Reg;

  DEBUG ((DEBUG_INFO, "KirinUfsHc: SoC init start\n"));

  //
  // Step 1: Configure UFS subsystem clock (207MHz / div)
  //
  MmioWrite32 (SCTRL_BASE + SCTRL_SCPERDIS4, BIT14);
  MmioWrite32 (SCTRL_BASE + SCTRL_SCCLKDIV9, 0x003F0007);
  MmioWrite32 (SCTRL_BASE + SCTRL_SCPEREN4, BIT14);

  //
  // Step 2: Enable UFS IES mask
  //
  MmioWrite32 (UFS_SYS_CTRL_BASE + UFS_SYS_UFS_UMECTRL, BIT0);

  //
  // Step 3: Assert UFS reset (ip_rst_ufs = 0, with mask bit16)
  //
  MmioWrite32 (UFS_SYS_CTRL_BASE + UFS_SYS_CRG_UFS_CFG, (BIT16 | 0));

  //
  // Step 3b: set_rhold from efuse
  //
  Reg = MmioRead32 (SCTRL_BASE + SCDEEPSLEEPED_OFFSET);
  if (Reg & EFUSE_RHOLD_BIT) {
    MmioWrite32 (UFS_SYS_CTRL_BASE + UFS_SYS_UFS_DEVICE_RESET,
      MASK_UFS_MPHY_RHOLD | BIT_UFS_MPHY_RHOLD);
    DEBUG ((DEBUG_INFO, "KirinUfsHc: MPHY RHOLD enabled (efuse)\n"));
  }

  //
  // Step 4: PSW power up
  //
  UfsSysCtrlSetBits (BIT_UFS_PSW_MTCMOS_EN, UFS_SYS_PSW_POWER_CTRL);
  MicroSecondDelay (10);

  //
  // Step 5: Notify power ready
  //
  UfsSysCtrlSetBits (BIT_SYSCTRL_PWR_READY, UFS_SYS_HC_LP_CTRL);

  //
  // Step 6: Close MPHY ref clock, configure divider
  //
  UfsSysCtrlClrBits (BIT_SYSCTRL_REF_CLOCK_EN, UFS_SYS_PHY_CLK_CTRL);

  Reg = (0x3U << 2) | (0x7U << (2 + 16));
  MmioWrite32 (UFS_SYS_CTRL_BASE + UFS_SYS_CRG_UFS_CFG, Reg);

  Reg = MmioRead32 (UFS_SYS_CTRL_BASE + UFS_SYS_PHY_CLK_CTRL);
  Reg &= ~(MASK_SYSCTRL_REF_CLOCK_SEL | MASK_SYSCTRL_CFG_CLOCK_FREQ);
  Reg |= 0x34;
  MmioWrite32 (UFS_SYS_CTRL_BASE + UFS_SYS_PHY_CLK_CTRL, Reg);

  //
  // Step 7: Enable ref_clk_en override (bit5 & bit4), with mask (bit21 & bit20)
  //
  MmioWrite32 (UFS_SYS_CTRL_BASE + UFS_SYS_UFS_DEVICE_RESET, 0x00300030);

  //
  // Step 8: Bypass UFS clock gate
  //
  UfsSysCtrlSetBits (MASK_UFS_CLK_GATE_BYPASS, UFS_SYS_CLOCK_GATE_BYPASS);
  UfsSysCtrlSetBits (MASK_UFS_SYSCRTL_BYPASS, UFS_SYS_UFS_SYSCTRL);

  //
  // Step 9: Open PSW clock
  //
  UfsSysCtrlSetBits (BIT_SYSCTRL_PSW_CLK_EN, UFS_SYS_PSW_CLK_CTRL);

  //
  // Step 10: Disable isolations
  //
  UfsSysCtrlClrBits (BIT_UFS_PSW_ISO_CTRL, UFS_SYS_PSW_POWER_CTRL);
  UfsSysCtrlClrBits (BIT_UFS_PHY_ISO_CTRL, UFS_SYS_PHY_ISO_EN);
  UfsSysCtrlClrBits (BIT_SYSCTRL_LP_ISOL_EN, UFS_SYS_HC_LP_CTRL);

  //
  // Step 11: Release AXI reset (ip_arst_ufs = 1, with mask bit17)
  //
  MmioWrite32 (UFS_SYS_CTRL_BASE + UFS_SYS_CRG_UFS_CFG,
    (BIT17 | BIT1));

  //
  // Step 12: Enable LP reset
  //
  UfsSysCtrlSetBits (BIT_SYSCTRL_LP_RESET_N, UFS_SYS_RESET_CTRL_EN);
  MicroSecondDelay (1000);

  //
  // Step 13: Open MPHY reference clock
  //
  UfsSysCtrlSetBits (BIT_SYSCTRL_REF_CLOCK_EN, UFS_SYS_PHY_CLK_CTRL);

  //
  // Step 14: Device reset sequence
  //
  MmioWrite32 (UFS_SYS_CTRL_BASE + UFS_SYS_UFS_DEVICE_RESET,
    MASK_UFS_DEVICE_RESET | 0);

  MmioWrite32 (SCTRL_BASE + SCTRL_SCPEREN4, BIT12);

  MicroSecondDelay (1000);

  MmioWrite32 (UFS_SYS_CTRL_BASE + UFS_SYS_UFS_DEVICE_RESET,
    MASK_UFS_DEVICE_RESET | BIT_UFS_DEVICE_RESET);

  //
  // Step 15: Wait for device to be ready (40ms per kernel driver)
  //
  MicroSecondDelay (40000);

  //
  // Step 16: Release UFS controller reset (ip_rst_ufs = 1, with mask bit16)
  //
  MmioWrite32 (UFS_SYS_CTRL_BASE + UFS_SYS_CRG_UFS_CFG,
    (BIT16 | BIT0));

  Reg = MmioRead32 (UFS_SYS_CTRL_BASE + UFS_SYS_CRG_UFS_CFG);
  if (Reg & BIT0) {
    MicroSecondDelay (1000);
  }

  DEBUG ((DEBUG_INFO, "KirinUfsHc: SoC init done, CRG_CFG=0x%08x\n", Reg));
}

//
// ============ UFS Host Controller Protocol ============
//

STATIC
EFI_STATUS
EFIAPI
KirinUfsHcGetMmioBar (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT    UINTN                               *MmioBar
  )
{
  if (MmioBar == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *MmioBar = UFS_HC_BASE;
  return EFI_SUCCESS;
}

//
// ============ UTP descriptor common buffer pool ============
//
// UfsAllocateAlignCommonBuffer() takes a common buffer for every UFS command
// descriptor, plus one for the transfer request list and one for the task
// management list.  The straightforward implementation - AllocatePages(),
// tag the range EFI_MEMORY_UC, FreePages() - is wrong in two ways:
//
//   1. gDS->SetMemorySpaceAttributes(EFI_MEMORY_UC) splits the GCD map entry
//      and reaches down into gCpu->SetMemoryAttributes() (Gcd.c:912) to
//      rewrite the page tables and shoot down the TLB.  Paid on every
//      descriptor allocation, so every ReadBlocks() carried two GCD walks -
//      and loading a kernel from the GRUB menu issues thousands of them.
//
//   2. FreeBuffer() never put EFI_MEMORY_WB back.  The pages returned to the
//      DXE core still uncached, so whoever got them next - GRUB's own image,
//      the kernel/initrd load buffer, a FatDxe read buffer - silently ran
//      uncached.  That is a large, hard-to-attribute slowdown of exactly the
//      "select a Linux entry in GRUB and wait" phase.
//
// The descriptor memory is now allocated once, marked uncached once, and
// sub-allocated from a bitmap inside this driver.  It never goes back to the
// DXE core, so there is no attribute to restore and no GCD or page-table work
// left on the per-command path.
//
#define UFS_COMMON_POOL_PAGES  16
#define UFS_COMMON_POOL_SIZE   (UFS_COMMON_POOL_PAGES * EFI_PAGE_SIZE)

STATIC VOID    *mUfsCommonPool      = NULL;
STATIC UINT32   mUfsCommonPoolInUse = 0;  // bit N set => page N is allocated
STATIC BOOLEAN  mUfsCommonPoolReady = FALSE;

STATIC
EFI_STATUS
KirinUfsHcCommonPoolInit (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Address;

  //
  // Keep the pool below 4G: UfsAllocateAlignCommonBuffer() rejects a
  // descriptor whose bus address does not fit in 32 bits when the controller
  // does not advertise UFS_HC_CAP_64ADDR.
  //
  Address = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  EFI_SIZE_TO_PAGES (UFS_COMMON_POOL_SIZE),
                  &Address
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinUfsHc: UTP common pool allocate failed: %r\n", Status));
    return Status;
  }

  //
  // Once here instead of once per command.  A failure is reported but is not
  // fatal - it leaves the descriptors write-back, exactly as they were before
  // this pool existed.  DEBUG_ERROR rather than DEBUG_WARN because RELEASE
  // only passes EFI_D_ERROR through, and a descriptor buffer that is not
  // uncached will silently hand the controller stale data.
  //
  Status = gDS->SetMemorySpaceAttributes (Address, UFS_COMMON_POOL_SIZE, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUfsHc: UTP common pool SetMemorySpaceAttributes UC failed: %r (addr=0x%lx size=0x%x)\n",
      Status, Address, (UINT32)UFS_COMMON_POOL_SIZE
      ));
  }

  mUfsCommonPool      = (VOID *)(UINTN)Address;
  mUfsCommonPoolInUse = 0;
  mUfsCommonPoolReady = TRUE;

  DEBUG ((
    DEBUG_INFO,
    "KirinUfsHc: UTP common pool 0x%lx + 0x%x bytes (%u pages) reserved\n",
    (UINT64)Address, (UINT32)UFS_COMMON_POOL_SIZE, (UINT32)UFS_COMMON_POOL_PAGES
    ));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KirinUfsHcAllocateBuffer (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN     EFI_ALLOCATE_TYPE                    Type,
  IN     EFI_MEMORY_TYPE                      MemoryType,
  IN     UINTN                               Pages,
  OUT    VOID                                **HostAddress,
  IN     UINT64                               Attributes
  )
{
  UINTN  Index;
  UINTN  Start;
  UINTN  Run;

  if ((HostAddress == NULL) || (Pages == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // UfsAllocateAlignCommonBuffer() is the only caller and always asks for
  // AllocateAnyPages out of this pool; the other allocate types would need a
  // reservation policy that does not exist here.
  //
  if (Type != AllocateAnyPages) {
    return EFI_UNSUPPORTED;
  }

  if (!mUfsCommonPoolReady || (Pages > UFS_COMMON_POOL_PAGES)) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUfsHc: UTP common pool can't satisfy %u page(s), ready=%d\n",
      (UINT32)Pages, mUfsCommonPoolReady
      ));
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // First fit of Pages consecutive free pages.
  //
  Run   = 0;
  Start = 0;
  for (Index = 0; Index < UFS_COMMON_POOL_PAGES; Index++) {
    if ((mUfsCommonPoolInUse & (UINT32)(1U << Index)) == 0) {
      if (Run == 0) {
        Start = Index;
      }
      Run++;
      if (Run >= Pages) {
        break;
      }
    } else {
      Run = 0;
    }
  }
  if (Run < Pages) {
    DEBUG ((DEBUG_ERROR, "KirinUfsHc: UTP common pool exhausted, need %u page(s)\n", (UINT32)Pages));
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = Start; Index < Start + Pages; Index++) {
    mUfsCommonPoolInUse |= (UINT32)(1U << Index);
  }

  *HostAddress = (UINT8 *)mUfsCommonPool + Start * EFI_PAGE_SIZE;

  //
  // UfsAllocateAlignCommonBuffer() zeroes the buffer itself right after Map(),
  // and Map() does the cache maintenance, so neither is repeated here.
  //
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KirinUfsHcFreeBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN     UINTN                               Pages,
  IN     VOID                                *HostAddress
  )
{
  UINTN  Start;
  UINTN  Offset;
  UINTN  Index;

  if ((HostAddress == NULL) || (Pages == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mUfsCommonPoolReady ||
      ((UINT8 *)HostAddress <  (UINT8 *)mUfsCommonPool) ||
      ((UINT8 *)HostAddress >= (UINT8 *)mUfsCommonPool + UFS_COMMON_POOL_SIZE)) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUfsHc: FreeBuffer of %p which is not in the UTP common pool\n",
      HostAddress
      ));
    return EFI_INVALID_PARAMETER;
  }

  Offset = (UINTN)((UINT8 *)HostAddress - (UINT8 *)mUfsCommonPool);
  Start  = Offset / EFI_PAGE_SIZE;
  if (((Offset % EFI_PAGE_SIZE) != 0) || ((Start + Pages) > UFS_COMMON_POOL_PAGES)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = Start; Index < Start + Pages; Index++) {
    if ((mUfsCommonPoolInUse & (UINT32)(1U << Index)) == 0) {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUfsHc: FreeBuffer of %p is a double free (page %u)\n",
        HostAddress, (UINT32)Index
        ));
      return EFI_INVALID_PARAMETER;
    }
  }

  for (Index = Start; Index < Start + Pages; Index++) {
    mUfsCommonPoolInUse &= ~(UINT32)(1U << Index);
  }

  //
  // Deliberately no FreePages(): nothing returns to the DXE core, so no page
  // can leak out of here still marked uncached - which is exactly what the old
  // FreePages() path did to every subsequent owner of these pages.
  //
  return EFI_SUCCESS;
}

typedef struct {
  VOID    *HostAddress;
  UINTN    NumberOfBytes;
} UFS_MAP_INFO;

STATIC
EFI_STATUS
EFIAPI
KirinUfsHcMap (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN     EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN     VOID                                 *HostAddress,
  IN OUT UINTN                                *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS                  *DeviceAddress,
  OUT    VOID                                 **Mapping
  )
{
  UFS_MAP_INFO  *MapInfo;

  if (HostAddress == NULL || NumberOfBytes == NULL || DeviceAddress == NULL || Mapping == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  MapInfo = AllocatePool (sizeof (UFS_MAP_INFO));
  if (MapInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  MapInfo->HostAddress   = HostAddress;
  MapInfo->NumberOfBytes = *NumberOfBytes;

  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  *Mapping = MapInfo;

  WriteBackInvalidateDataCacheRange (HostAddress, *NumberOfBytes);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KirinUfsHcUnmap (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  VOID                                *Mapping
  )
{
  UFS_MAP_INFO  *MapInfo;

  if (Mapping != NULL) {
    MapInfo = (UFS_MAP_INFO *)Mapping;
    WriteBackInvalidateDataCacheRange (MapInfo->HostAddress, MapInfo->NumberOfBytes);
    FreePool (MapInfo);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KirinUfsHcFlush (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  MemoryFence ();
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KirinUfsHcRead (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH   Width,
  IN     UINT64                                     Offset,
  IN     UINTN                                      Count,
  IN OUT VOID                                       *Buffer
  )
{
  UINTN  Index;
  UINTN  Addr;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Addr = UFS_HC_BASE + (UINTN)Offset;

  for (Index = 0; Index < Count; Index++) {
    switch (Width) {
    case EfiUfsHcWidthUint8:
      ((UINT8 *)Buffer)[Index] = MmioRead8 (Addr);
      Addr += 1;
      break;
    case EfiUfsHcWidthUint16:
      ((UINT16 *)Buffer)[Index] = MmioRead16 (Addr);
      Addr += 2;
      break;
    case EfiUfsHcWidthUint32:
      ((UINT32 *)Buffer)[Index] = MmioRead32 (Addr);
      Addr += 4;
      break;
    case EfiUfsHcWidthUint64:
      ((UINT64 *)Buffer)[Index] = MmioRead64 (Addr);
      Addr += 8;
      break;
    default:
      return EFI_INVALID_PARAMETER;
    }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KirinUfsHcWrite (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH   Width,
  IN     UINT64                                     Offset,
  IN     UINTN                                      Count,
  IN OUT VOID                                       *Buffer
  )
{
  UINTN  Index;
  UINTN  Addr;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Addr = UFS_HC_BASE + (UINTN)Offset;

  for (Index = 0; Index < Count; Index++) {
    switch (Width) {
    case EfiUfsHcWidthUint8:
      MmioWrite8 (Addr, ((UINT8 *)Buffer)[Index]);
      Addr += 1;
      break;
    case EfiUfsHcWidthUint16:
      MmioWrite16 (Addr, ((UINT16 *)Buffer)[Index]);
      Addr += 2;
      break;
    case EfiUfsHcWidthUint32:
      MmioWrite32 (Addr, ((UINT32 *)Buffer)[Index]);
      Addr += 4;
      break;
    case EfiUfsHcWidthUint64:
      MmioWrite64 (Addr, ((UINT64 *)Buffer)[Index]);
      Addr += 8;
      break;
    default:
      return EFI_INVALID_PARAMETER;
    }
  }
  return EFI_SUCCESS;
}

STATIC EDKII_UFS_HOST_CONTROLLER_PROTOCOL mKirinUfsHcProtocol = {
  KirinUfsHcGetMmioBar,
  KirinUfsHcAllocateBuffer,
  KirinUfsHcFreeBuffer,
  KirinUfsHcMap,
  KirinUfsHcUnmap,
  KirinUfsHcFlush,
  KirinUfsHcRead,
  KirinUfsHcWrite
};

//
// ============ UFS HC Platform Protocol (MPHY configuration) ============
//

STATIC
EFI_STATUS
DmeSet (
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *DriverIntf,
  IN UINT32                          Attr,
  IN UINT32                          Sel,
  IN UINT32                          Value
  )
{
  EDKII_UIC_COMMAND  Cmd;

  Cmd.Opcode = UIC_CMD_DME_SET;
  Cmd.Arg1   = UIC_ARG_MIB_SEL (Attr, Sel);
  Cmd.Arg2   = 0;
  Cmd.Arg3   = Value;

  return DriverIntf->UfsExecUicCommand (DriverIntf, &Cmd);
}

STATIC
EFI_STATUS
DmeGet (
  IN  EDKII_UFS_HC_DRIVER_INTERFACE  *DriverIntf,
  IN  UINT32                          Attr,
  IN  UINT32                          Sel,
  OUT UINT32                         *Value
  )
{
  EDKII_UIC_COMMAND  Cmd;
  EFI_STATUS         Status;

  Cmd.Opcode = UIC_CMD_DME_GET;
  Cmd.Arg1   = UIC_ARG_MIB_SEL (Attr, Sel);
  Cmd.Arg2   = 0;
  Cmd.Arg3   = 0;

  Status = DriverIntf->UfsExecUicCommand (DriverIntf, &Cmd);
  if (!EFI_ERROR (Status)) {
    *Value = Cmd.Arg3;
  }
  return Status;
}

STATIC
EFI_STATUS
MphyWrite (
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *DriverIntf,
  IN UINT16                          Addr,
  IN UINT16                          Value
  )
{
  EFI_STATUS  Status;

  Status = DmeSet (DriverIntf, 0x8117, 0, (Addr >> 8) & 0xFF);
  if (EFI_ERROR (Status)) return Status;

  Status = DmeSet (DriverIntf, 0x8116, 0, Addr & 0xFF);
  if (EFI_ERROR (Status)) return Status;

  Status = DmeSet (DriverIntf, 0x8119, 0, (Value >> 8) & 0xFF);
  if (EFI_ERROR (Status)) return Status;

  Status = DmeSet (DriverIntf, 0x8118, 0, Value & 0xFF);
  if (EFI_ERROR (Status)) return Status;

  return DmeSet (DriverIntf, 0x811C, 0, 1);
}

STATIC
EFI_STATUS
CheckHibern8 (
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *DriverIntf
  )
{
  UINT32  Lane0Fsm;
  UINT32  Lane1Fsm;
  UINTN   Retry;

  for (Retry = 0; Retry < 100; Retry++) {
    if (EFI_ERROR (DmeGet (DriverIntf, MPHY_TX_FSM_STATE, 0, &Lane0Fsm))) {
      return EFI_DEVICE_ERROR;
    }
    if (EFI_ERROR (DmeGet (DriverIntf, MPHY_TX_FSM_STATE, 1, &Lane1Fsm))) {
      return EFI_DEVICE_ERROR;
    }
    if (Lane0Fsm == TX_FSM_HIBERN8 && Lane1Fsm == TX_FSM_HIBERN8) {
      DEBUG ((DEBUG_INFO, "KirinUfsHc: PHY in Hibern8 after %d polls\n", Retry + 1));
      return EFI_SUCCESS;
    }
    MicroSecondDelay (200);
  }

  DEBUG ((DEBUG_ERROR, "KirinUfsHc: Hibern8 timeout, lane0=%d lane1=%d\n", Lane0Fsm, Lane1Fsm));
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
KirinUfsSetupSnpsAsicMphy (
  IN EDKII_UFS_HC_DRIVER_INTERFACE  *DriverIntf
  )
{
  EFI_STATUS  Status;

  DEBUG ((DEBUG_INFO, "KirinUfsHc: SNPS ASIC MPHY setup (Rate B)\n"));

  // VS_mphy_disable = 1
  Status = DmeSet (DriverIntf, 0xD0C1, 0, 0x1);
  if (EFI_ERROR (Status)) return Status;

  //
  // Rate B path (from DTS: ufs-kirin-use-rate-B)
  //
  Status = DmeSet (DriverIntf, 0x156A, 0, 0x2);  // PA_HSSeries = B
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x8114, 0, 0x1);  // CBRATESEL = 1
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x8121, 0, 0x2D); // CBOVRCTRL2
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x8122, 0, 0x1);  // CBOVRCTRL3
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x8127, 0, 0x98); // CBOVRCTRL4
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x8128, 0, 0x1);  // CBOVRCTRL5
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0xD085, 0, 0x1);  // VS_MphyCfgUpdt
  if (EFI_ERROR (Status)) return Status;

  Status = DmeSet (DriverIntf, 0x800D, 0x4, 0x58); // RXOVRCTRL4 rx0
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x800D, 0x5, 0x58); // RXOVRCTRL4 rx1
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x800E, 0x4, 0xB);  // RXOVRCTRL5 rx0
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x800E, 0x5, 0xB);  // RXOVRCTRL5 rx1
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x8009, 0x4, 0x1);  // RXSQCONTROL rx0
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x8009, 0x5, 0x1);  // RXSQCONTROL rx1
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0xD085, 0, 0x1);    // VS_MphyCfgUpdt
  if (EFI_ERROR (Status)) return Status;

  Status = DmeSet (DriverIntf, 0x8113, 0, 0x1);
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0xD085, 0, 0x1);
  if (EFI_ERROR (Status)) return Status;

  // RX Hibern8 and ActivateTime
  Status = DmeSet (DriverIntf, 0x0092, 0x4, 0xA);
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x0092, 0x5, 0xA);
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x008F, 0x4, 0xA);
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x008F, 0x5, 0xA);
  if (EFI_ERROR (Status)) return Status;

  // Sync lengths (Gear 1/2/3)
  Status = DmeSet (DriverIntf, 0x0095, 0x4, 0x4F);
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x0095, 0x5, 0x4F);
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x0094, 0x4, 0x4F);
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x0094, 0x5, 0x4F);
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x008B, 0x4, 0x4F);
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x008B, 0x5, 0x4F);
  if (EFI_ERROR (Status)) return Status;

  // Thibernate Tx
  Status = DmeSet (DriverIntf, 0x000F, 0x0, 0x5);
  if (EFI_ERROR (Status)) return Status;
  Status = DmeSet (DriverIntf, 0x000F, 0x1, 0x5);
  if (EFI_ERROR (Status)) return Status;

  Status = DmeSet (DriverIntf, 0xD085, 0, 0x1);
  if (EFI_ERROR (Status)) return Status;

  // VS_mphy_disable = 0 (re-enable)
  Status = DmeSet (DriverIntf, 0xD0C1, 0, 0x0);
  if (EFI_ERROR (Status)) return Status;

  // Wait for PHY to enter Hibern8
  Status = CheckHibern8 (DriverIntf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinUfsHc: Hibern8 check failed after MPHY enable\n"));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "KirinUfsHc: SNPS ASIC MPHY setup done\n"));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KirinUfsHcPlatformCallback (
  IN     EFI_HANDLE                            ControllerHandle,
  IN     EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                                  *CallbackData
  )
{
  EDKII_UFS_HC_DRIVER_INTERFACE  *DriverIntf;
  EFI_STATUS                      Status;
  UINT32                          Reg;

  if (CallbackData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  DriverIntf = (EDKII_UFS_HC_DRIVER_INTERFACE *)CallbackData;

  switch (CallbackPhase) {
  case EdkiiUfsHcPreHce:
    DEBUG ((DEBUG_INFO, "KirinUfsHc: PreHce callback\n"));
    return EFI_SUCCESS;

  case EdkiiUfsHcPostHce:
    DEBUG ((DEBUG_INFO, "KirinUfsHc: PostHce callback\n"));
    return EFI_SUCCESS;

  case EdkiiUfsHcPreLinkStartup:
    DEBUG ((DEBUG_INFO, "KirinUfsHc: PreLinkStartup - configuring MPHY\n"));

    Status = KirinUfsSetupSnpsAsicMphy (DriverIntf);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "KirinUfsHc: MPHY setup failed: %r\n", Status));
      return Status;
    }

    // Disable auto H8
    Reg = MmioRead32 (UFS_HC_BASE + UFS_HC_AHIT_OFFSET);
    Reg &= ~0xFFFF;
    MmioWrite32 (UFS_HC_BASE + UFS_HC_AHIT_OFFSET, Reg);

    // PA_Local_TX_LCC_Enable = 0
    Status = DmeSet (DriverIntf, 0x155E, 0, 0x0);
    if (EFI_ERROR (Status)) return Status;

    // Close VS_Mk2ExtnSupport
    Status = DmeSet (DriverIntf, 0xD0AB, 0, 0x0);
    if (EFI_ERROR (Status)) return Status;

    // TX equalizer 35dB via MPHY register writes
    Status = MphyWrite (DriverIntf, 0x1002, 0xAC78);
    if (EFI_ERROR (Status)) return Status;
    Status = MphyWrite (DriverIntf, 0x1102, 0xAC78);
    if (EFI_ERROR (Status)) return Status;
    Status = MphyWrite (DriverIntf, 0x1003, 0x2440);
    if (EFI_ERROR (Status)) return Status;
    Status = MphyWrite (DriverIntf, 0x1103, 0x2440);
    if (EFI_ERROR (Status)) return Status;

    DEBUG ((DEBUG_INFO, "KirinUfsHc: PreLinkStartup done\n"));
    return EFI_SUCCESS;

  case EdkiiUfsHcPostLinkStartup:
    DEBUG ((DEBUG_INFO, "KirinUfsHc: PostLinkStartup - configuring Unipro DL\n"));

    // Unipro Data Link thresholds (critical for UTP traffic)
    Status = DmeSet (DriverIntf, 0x2044, 0, 0x0);  // DL_AFC0CreditThreshold
    if (EFI_ERROR (Status)) return Status;
    Status = DmeSet (DriverIntf, 0x2045, 0, 0x0);  // DL_TC0OutAckThreshold
    if (EFI_ERROR (Status)) return Status;
    Status = DmeSet (DriverIntf, 0x2040, 0, 0x9);  // DL_TC0TXFCThreshold
    if (EFI_ERROR (Status)) return Status;

    // BROKEN_CLK_GATE_BYPASS: stop bypassing clock gate after link is up
    UfsSysCtrlClrBits (MASK_UFS_CLK_GATE_BYPASS, UFS_SYS_CLOCK_GATE_BYPASS);
    UfsSysCtrlClrBits (MASK_UFS_SYSCRTL_BYPASS, UFS_SYS_UFS_SYSCTRL);

    // Disable power-gating in auto H8
    Reg = MmioRead32 (UFS_HC_BASE + UFS_HC_OCPTHRTL);
    Reg &= ~LP_AH8_PGE;
    MmioWrite32 (UFS_HC_BASE + UFS_HC_OCPTHRTL, Reg);

    // Debug counters
    Status = DmeSet (DriverIntf, 0xD09A, 0, 0x80000000);
    if (EFI_ERROR (Status)) return Status;
    Status = DmeSet (DriverIntf, 0xD09C, 0, 0x00000005);
    if (EFI_ERROR (Status)) return Status;

    // Clear stale interrupt status
    Reg = MmioRead32 (UFS_HC_BASE + UFS_HC_IS_OFFSET);
    if (Reg) {
      MmioWrite32 (UFS_HC_BASE + UFS_HC_IS_OFFSET, Reg);
    }

    DEBUG ((DEBUG_INFO, "KirinUfsHc: PostLinkStartup done\n"));
    return EFI_SUCCESS;

  default:
    return EFI_INVALID_PARAMETER;
  }
}

STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL mKirinUfsHcPlatform = {
  EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION,
  NULL,
  KirinUfsHcPlatformCallback
};

//
// ============ Device Path ============
//

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} UFS_HC_DEVICE_PATH;
#pragma pack()

STATIC UFS_HC_DEVICE_PATH mUfsHcDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    { 0xa3f1640b, 0x537c, 0x4f0e, { 0xb4, 0x9d, 0x8e, 0x3c, 0x7a, 0x5d, 0x9e, 0x12 } }
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8)(sizeof (EFI_DEVICE_PATH_PROTOCOL)),
      (UINT8)((sizeof (EFI_DEVICE_PATH_PROTOCOL)) >> 8)
    }
  }
};

STATIC EFI_HANDLE mUfsHcHandle = NULL;

EFI_STATUS
EFIAPI
KirinUfsHcEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Ver;
  UINT32      Hce;

  KirinUfsSocInit ();

  Ver = MmioRead32 (UFS_HC_BASE + 0x08);
  Hce = MmioRead32 (UFS_HC_BASE + 0x34);
  DEBUG ((DEBUG_INFO, "KirinUfsHc: UFSHCI Version 0x%08x, HCE=0x%x, HCS=0x%x\n",
          Ver, Hce, MmioRead32 (UFS_HC_BASE + 0x30)));

  //
  // Has to be up before the host controller protocol is published: connecting
  // the controller immediately runs UfsPassThruDxe, which asks for the
  // transfer request list through AllocateBuffer().
  //
  Status = KirinUfsHcCommonPoolInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEdkiiUfsHcPlatformProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mKirinUfsHcPlatform
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinUfsHc: Failed to install platform protocol: %r\n", Status));
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mUfsHcHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mUfsHcDevicePath,
                  &gEdkiiUfsHostControllerProtocolGuid,
                  &mKirinUfsHcProtocol,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinUfsHc: Failed to install protocols: %r\n", Status));
    return Status;
  }

  Status = gBS->ConnectController (mUfsHcHandle, NULL, NULL, TRUE);

  return EFI_SUCCESS;
}
