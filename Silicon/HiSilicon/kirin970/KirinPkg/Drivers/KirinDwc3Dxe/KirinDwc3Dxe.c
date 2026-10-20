#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/KirinHi6523Lib.h>

//
// Base addresses
//
#define PERI_CRG_BASE       0xFFF35000
#define SCTRL_BASE          0xFFF0A000
#define PCTRL_BASE          0xE8A09000
#define USB_MISC_BASE       0xFF200000
#define DWC3_BASE           0xFF100000
#define DWC3_SIZE           0x100000

//
// PERI_CRG clock gate registers (write-1-to-set)
//
#define CRG_PEREN0          0x000
#define CRG_PEREN4          0x040
#define CRG_PERRSTEN4       0x090
#define CRG_PERRSTDIS4      0x094
#define CRG_PERRSTSTAT4     0x098
#define CRG_ISODIS          0x148
#define CRG_PEREN16         0x410

#define GT_HCLK_USB3OTG     BIT25
#define GT_CLK_USB3OTG_REF  BIT0
#define GT_ACLK_USB3OTG     BIT1
#define GT_CLK_USB2PHY_REF  BIT19
#define IP_RST_USB3OTG_32K  BIT6
#define IP_RST_USB3OTG_MISC BIT7
#define USB_REFCLK_ISO_EN   BIT25

//
// SCTRL
//
#define SCTRL_SCDEEPSLEEPED 0x08

//
// PCTRL
//
#define PCTRL_PERI_CTRL24   0x064
#define PCTRL_CLK_GATE      0x010

//
// hisi_usb clocks[2] (clk_usb3_tcxo_en) is a hiword gate at PCTRL+0x10,
// bit index 1; enabling it is a single write of (mask<<16)|value.
//
#define PCTRL_CLK_TCXO_EN   (((1u << 1) << 16) | (1u << 1))

//
// USB MISC ctrl registers (base 0xFF200000)
//
#define MISC_CTRL0          0x00
#define MISC_CTRL3          0x0C
#define MISC_CTRL4          0x10
#define MISC_CTRL5          0x14
#define MISC_CTRL7          0x1C
#define MISC_CFG50          0x50
#define MISC_CFG54          0x54
#define MISC_CFG5C          0x5C
#define MISC_CFGA0          0xA0
#define MISC_LINK_CTRL      0x7C
#define MISC_TCA_208        0x208
#define MISC_TCA_204        0x204
#define MISC_TCA_200        0x200
#define MISC_TCA_210        0x210
#define MISC_TCA_214        0x214
#define MISC_TCA_240        0x240

#define EYE_DIAGRAM_HOST_PARAM  0x33de7e7

//
// DWC3 registers (offsets mirror drivers/usb/dwc3/core.h)
//
#define DWC3_GSNPSID        0xC120
#define DWC3_GHWPARAMS1     0xC144
#define DWC3_GUCTL          0xC12C
#define DWC3_GUCTL1         0xC11C
#define DWC3_GUCTL2         0xC19C
#define DWC3_GUCTL3         0xC60C
#define DWC3_GEVNTADRLO0    0xC400
#define DWC3_GEVNTADRHI0    0xC404
#define DWC3_GEVNTSIZ0      0xC408
#define DWC3_GEVNTCOUNT0    0xC40C
#define DWC3_DCFG           0xC700
#define DWC3_DCTL           0xC704
#define DWC3_DEVTEN         0xC708

#define DWC3_LLUCTL         0xD024
#define DWC3_LSCDTIM1       0xD02C
#define DWC3_LSCDTIM2       0xD030
#define DWC3_LU1LFPSTXTIM   0xD004
#define DWC3_LSKIPFREQ      0xD020

#define DWC3_GCTL           0xC110
#define DWC3_GCTL_PRTCAP_MASK     (3u << 12)
#define DWC3_GCTL_PRTCAPDIR(n)    ((n) << 12)
#define DWC3_GCTL_PRTCAP_DEVICE   2
#define DWC3_GCTL_PRTCAP_HOST     1
#define DWC3_GCTL_CORESOFTRESET   BIT11
#define DWC3_GCTL_SCALEDOWN_MASK  (3u << 4)
#define DWC3_GCTL_GBLHIBERNATIONEN BIT1
#define DWC3_GCTL_DSBLCLKGTNG     BIT0
#define DWC3_GCTL_DISSCRAMBLE     BIT3
#define DWC3_GCTL_U2RSTECN        BIT16

#define DWC3_GHWPARAMS1_EN_PWROPT(n)   (((n) >> 24) & 3u)
#define DWC3_GHWPARAMS1_EN_PWROPT_CLK  1
#define DWC3_GHWPARAMS1_EN_PWROPT_HIB  2

#define DWC3_GUSB2PHYCFG0   0xC200
#define DWC3_GUSB2PHYCFG_SUSPHY      BIT6
#define DWC3_GUSB2PHYCFG_ULPI_UTMI   BIT4
#define DWC3_GUSB2PHYCFG_PHYIF_MASK  BIT3
#define DWC3_GUSB2PHYCFG_USBTRDTIM_MASK (0xFu << 10)
#define DWC3_GUSB2PHYCFG_PHYIF(n)      ((n) << 3)
#define DWC3_GUSB2PHYCFG_USBTRDTIM(n)  ((n) << 10)
#define USBTRDTIM_UTMI_8_BIT           9

#define DWC3_GUSB3PIPECTL0  0xC2C0
#define DWC3_GUSB3PIPECTL_SUSPHY  BIT17
#define DWC3_GUSB3PIPECTL_UX_EXIT_PX  BIT27
#define DWC3_GUSB3PIPECTL_TX_DEEPH(n)  ((n) << 1)

#define DWC3_DCTL_RUN_STOP  BIT31
#define DWC3_DCTL_CSFTRST   BIT30

#define DWC3_GEVNTSIZ_INTMASK  (1u << 31)

//
// dwc3_lscdtimer_set(), CONFIG_USB_DWC3_FEB branch (dwc3-usb-interface.c:248).
// Applies to device mode too - dwc3_otg_start_peripheral() calls it - so both
// roles need these HiSilicon link timers programmed before any endpoint work.
//
#define DWC3_GUCTL_ENOVERLAPCHK         (1u << 13)
#define DWC3_GUCTL1_HW_LPM_CAP_DISABLE  (1u << 13)
#define DWC31_GUCTL2_SVC_OPP_PER_HS_MASK   (3u << 5)
#define DWC3_GUCTL3_SPLITDISABLE           (1u << 14)
#define DWC3_GUCTL3_SVC_OPP_PER_HS_SEP(n)  ((n) << 9)
#define DWC3_GUCTL3_SVC_OPP_PER_HS_SEP_MASK (0xFu << 9)
#define DWC3_LLUCTL_PENDING_HP_TIMER_US_MASK (0x1Fu << 16)
#define DWC3_LLUCTL_PENDING_HP_TIMER_US(v)   ((v) << 16)
#define DWC3_LLUCTL_ENABLE_HP_TIMER          (1u << 16)

STATIC
VOID
MiscRegSetBit (
  IN UINT32  Bit,
  IN UINT32  Offset
  )
{
  UINT32 Val = MmioRead32 (USB_MISC_BASE + Offset);
  Val |= (1u << Bit);
  MmioWrite32 (USB_MISC_BASE + Offset, Val);
}

STATIC
VOID
MiscRegClrBit (
  IN UINT32  Bit,
  IN UINT32  Offset
  )
{
  UINT32 Val = MmioRead32 (USB_MISC_BASE + Offset);
  Val &= ~(1u << Bit);
  MmioWrite32 (USB_MISC_BASE + Offset, Val);
}

STATIC
VOID
MiscRegSetValue (
  IN UINT32  Mask,
  IN UINT32  Offset
  )
{
  UINT32 Val = MmioRead32 (USB_MISC_BASE + Offset);
  Val |= Mask;
  MmioWrite32 (USB_MISC_BASE + Offset, Val);
}

//
// dwc3_soft_reset() (core.c:198).  RUNSTOP has to come down first or the core
// can ignore CSFTRST while it is still running - which is exactly the state a
// previous function leaves behind.
//
STATIC
EFI_STATUS
Dwc3DctlSoftReset (
  VOID
  )
{
  UINT32  Reg;
  UINTN   Retry;

  Reg = MmioRead32 (DWC3_BASE + DWC3_DCTL);
  Reg &= ~DWC3_DCTL_RUN_STOP;
  Reg |= DWC3_DCTL_CSFTRST;
  MmioWrite32 (DWC3_BASE + DWC3_DCTL, Reg);

  for (Retry = 500000; Retry > 0; Retry--) {
    if ((MmioRead32 (DWC3_BASE + DWC3_DCTL) & DWC3_DCTL_CSFTRST) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }

  DEBUG ((
    DEBUG_ERROR,
    "KirinDwc3: DCTL soft reset stuck, DCTL 0x%08x GCTL 0x%08x PRTCAP=%u\n",
    MmioRead32 (DWC3_BASE + DWC3_DCTL),
    MmioRead32 (DWC3_BASE + DWC3_GCTL),
    (UINT32)((MmioRead32 (DWC3_BASE + DWC3_GCTL) >> 12) & 3u)
    ));
  return EFI_TIMEOUT;
}

//
// dwc3_otg_force_reset_core() (dwc3-otg_4_9.c:18): this core never self-clears
// GCTL.CORESOFTRESET.  The kernel sets the bit, reads it back and writes it
// clear again without ever waiting for it.  Driven by snps,gctl-reset-quirk.
//
STATIC
VOID
Dwc3GctlCoreSoftResetPulse (
  VOID
  )
{
  UINT32  Reg;

  Reg = MmioRead32 (DWC3_BASE + DWC3_GCTL);
  MmioWrite32 (DWC3_BASE + DWC3_GCTL, Reg | DWC3_GCTL_CORESOFTRESET);
  MicroSecondDelay (1000);

  Reg = MmioRead32 (DWC3_BASE + DWC3_GCTL);
  MmioWrite32 (DWC3_BASE + DWC3_GCTL, Reg & ~DWC3_GCTL_CORESOFTRESET);
  MicroSecondDelay (1000);
}

//
// dwc3_lscdtimer_set(), CONFIG_USB_DWC3_FEB branch (dwc3-usb-interface.c:248).
// Both dwc3_otg_start_host() and dwc3_otg_start_peripheral() call this before
// any transfer, so these HiSilicon link-layer timers are not host-only.
//
STATIC
VOID
Dwc3LscdTimerSet (
  VOID
  )
{
  UINT32  Reg;

  MmioWrite32 (DWC3_BASE + DWC3_LSCDTIM1, 0x47E1F4);
  MmioWrite32 (DWC3_BASE + DWC3_LSCDTIM2, 0x753546);

  Reg = MmioRead32 (DWC3_BASE + DWC3_LLUCTL);
  Reg &= ~DWC3_LLUCTL_PENDING_HP_TIMER_US_MASK;
  Reg |= DWC3_LLUCTL_PENDING_HP_TIMER_US (11);
  MmioWrite32 (DWC3_BASE + DWC3_LLUCTL, Reg);

  Reg = MmioRead32 (DWC3_BASE + DWC3_LLUCTL);
  Reg |= DWC3_LLUCTL_ENABLE_HP_TIMER;
  MmioWrite32 (DWC3_BASE + DWC3_LLUCTL, Reg);

  MmioWrite32 (DWC3_BASE + DWC3_LU1LFPSTXTIM, 0x940060);

  Reg = MmioRead32 (DWC3_BASE + DWC3_GUCTL);
  Reg &= ~DWC3_GUCTL_ENOVERLAPCHK;
  MmioWrite32 (DWC3_BASE + DWC3_GUCTL, Reg);

  Reg = MmioRead32 (DWC3_BASE + DWC3_LSKIPFREQ);
  Reg &= ~(0xFFu << 20);
  Reg |= (0xD9u << 20);
  MmioWrite32 (DWC3_BASE + DWC3_LSKIPFREQ, Reg);

  //
  // Chip bug 9001103026.
  //
  Reg = MmioRead32 (DWC3_BASE + DWC3_GUCTL1);
  Reg |= DWC3_GUCTL1_HW_LPM_CAP_DISABLE;
  MmioWrite32 (DWC3_BASE + DWC3_GUCTL1, Reg);
}

//
// drivers/usb/dwc3/core.c:641 dwc3_core_init(), in kernel order.  It runs
// while GCTL.PRTCAPDIR is still DEVICE - dwc3_otg_init() (dwc3-otg_4_9.c:265)
// already left it there - so both DCTL.CSFTRST writes are decoded by the
// device block, and it forces PRTCAP back to DEVICE when it is done
// (core.c:754).  The caller is what selects HOST afterwards.
//
STATIC
EFI_STATUS
Dwc3CoreInitHost (
  VOID
  )
{
  UINT32  Reg;
  UINT32  HwParams1;

  Reg = MmioRead32 (DWC3_BASE + DWC3_GSNPSID);
  if (((Reg & 0xFFFF0000) != 0x55330000) &&
      ((Reg & 0xFFFF0000) != 0x33310000))
  {
    DEBUG ((DEBUG_ERROR, "KirinDwc3: unexpected GSNPSID 0x%08x\n", Reg));
    return EFI_DEVICE_ERROR;
  }

  HwParams1 = MmioRead32 (DWC3_BASE + DWC3_GHWPARAMS1);
  DEBUG ((
    DEBUG_ERROR,
    "KirinDwc3: GSNPSID 0x%08x GHWPARAMS1 0x%08x\n",
    Reg,
    HwParams1
    ));

  Reg = MmioRead32 (DWC3_BASE + DWC3_GCTL);
  Reg &= ~DWC3_GCTL_PRTCAP_MASK;
  Reg |= DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_DEVICE);
  Reg &= ~DWC3_GCTL_CORESOFTRESET;
  MmioWrite32 (DWC3_BASE + DWC3_GCTL, Reg);
  MicroSecondDelay (100);

  //
  // dwc3_soft_reset() then dwc3_core_soft_reset().  The second one skips its
  // body for host-only cores; this DT is dr_mode = "otg", so it runs too.
  //
  if (EFI_ERROR (Dwc3DctlSoftReset ())) {
    return EFI_TIMEOUT;
  }

  if (EFI_ERROR (Dwc3DctlSoftReset ())) {
    return EFI_TIMEOUT;
  }

  //
  // dwc3_phy_setup(): UX_EXIT_PX is never valid in normal operation,
  // snps,tx_de_emphasis = 1 programs TX_DEEPH,
  // snps,dis-del-phy-power-chg-quirk clears DEPOCHANGE, and both
  // dis_u3/dis_u2_susphy quirks leave SUSPHY clear so endpoint and link
  // commands never race a PHY suspend.
  //
  Reg = MmioRead32 (DWC3_BASE + DWC3_GUSB3PIPECTL0);
  Reg &= ~DWC3_GUSB3PIPECTL_UX_EXIT_PX;
  Reg &= ~DWC3_GUSB3PIPECTL_TX_DEEPH (3u);
  Reg |= DWC3_GUSB3PIPECTL_TX_DEEPH (1u);
  Reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;
  MmioWrite32 (DWC3_BASE + DWC3_GUSB3PIPECTL0, Reg);

  //
  // phy_type = "utmi" with an 8 bit interface: PHYIF 0, USBTRDTIM 9,
  // ULPI_UTMI stays clear, SUSPHY cleared by snps,dis_u2_susphy_quirk.
  // Same treatment KirinUsbDeviceDxe already applies, so the register is
  // programmed identically whichever role is entered first.
  //
  Reg = MmioRead32 (DWC3_BASE + DWC3_GUSB2PHYCFG0);
  Reg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK | DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
  Reg |= DWC3_GUSB2PHYCFG_PHYIF (0)
         | DWC3_GUSB2PHYCFG_USBTRDTIM (USBTRDTIM_UTMI_8_BIT);
  Reg &= ~DWC3_GUSB2PHYCFG_ULPI_UTMI;
  Reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
  MmioWrite32 (DWC3_BASE + DWC3_GUSB2PHYCFG0, Reg);

  //
  // GCTL: scaledown off, clock gating per EN_PWROPT (CLK keeps gating on,
  // HIB enables hibernation exactly as the kernel does), scramble kept on,
  // then PRTCAP forced back to DEVICE as core.c:754 does.
  //
  Reg = MmioRead32 (DWC3_BASE + DWC3_GCTL);
  Reg &= ~DWC3_GCTL_SCALEDOWN_MASK;
  switch (DWC3_GHWPARAMS1_EN_PWROPT (HwParams1)) {
    case DWC3_GHWPARAMS1_EN_PWROPT_CLK:
      Reg &= ~DWC3_GCTL_DSBLCLKGTNG;
      break;
    case DWC3_GHWPARAMS1_EN_PWROPT_HIB:
      Reg |= DWC3_GCTL_GBLHIBERNATIONEN;
      break;
    default:
      break;
  }
  Reg &= ~DWC3_GCTL_DISSCRAMBLE;
  Reg &= ~DWC3_GCTL_PRTCAP_MASK;
  Reg |= DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_DEVICE);
  MmioWrite32 (DWC3_BASE + DWC3_GCTL, Reg);
  MicroSecondDelay (100);

  //
  // dwc3_event_buffers_setup(): the device event path is idle in host mode,
  // so drop anything a previous gadget session queued instead of replaying
  // it into the next one.
  //
  MmioWrite32 (DWC3_BASE + DWC3_GEVNTADRLO0, 0);
  MmioWrite32 (DWC3_BASE + DWC3_GEVNTADRHI0, 0);
  MmioWrite32 (DWC3_BASE + DWC3_GEVNTSIZ0, DWC3_GEVNTSIZ_INTMASK);
  MmioWrite32 (DWC3_BASE + DWC3_GEVNTCOUNT0, 0);
  MmioWrite32 (DWC3_BASE + DWC3_DEVTEN, 0);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
UsbSubsystemInit (
  VOID
  )
{
  UINT32  Reg;

  DEBUG ((DEBUG_INFO, "KirinDwc3: Enabling USB subsystem clocks\n"));

  //
  // Step 1: Enable hclk_usb3otg (needed for MISC register access)
  //
  MmioWrite32 (PERI_CRG_BASE + CRG_PEREN0, GT_HCLK_USB3OTG);

  //
  // Step 2: Enable controller clocks (aclk + ref)
  //
  MmioWrite32 (PERI_CRG_BASE + CRG_PEREN4, GT_CLK_USB3OTG_REF | GT_ACLK_USB3OTG);

  //
  // Step 3: Enable USB2 PHY reference clock
  //
  MmioWrite32 (PERI_CRG_BASE + CRG_PEREN16, GT_CLK_USB2PHY_REF);

  //
  // Step 4: Reset USB MISC controller
  //
  MmioWrite32 (PERI_CRG_BASE + CRG_PERRSTEN4, IP_RST_USB3OTG_MISC | IP_RST_USB3OTG_32K);
  MicroSecondDelay (100);

  //
  // Step 5: De-assert USB MISC controller reset
  //
  MmioWrite32 (PERI_CRG_BASE + CRG_PERRSTDIS4, IP_RST_USB3OTG_MISC | IP_RST_USB3OTG_32K);
  MicroSecondDelay (100);

  //
  // Step 5b: Release both USBs2/USB31 PHYs from reset (phy_release() in the
  // kernel sets CFGA0 bits 0-1 before starting the phy clock mux below).
  //
  MiscRegSetValue (BIT0 | BIT1, MISC_CFGA0);
  MicroSecondDelay (100);

  //
  // Step 6: Disable USB refclk isolation
  //
  MmioWrite32 (PERI_CRG_BASE + CRG_ISODIS, USB_REFCLK_ISO_EN);

  //
  // Step 6b: select the combophy reference clock.  set_combophy_clk()
  // (dwc3-feb.c:217) reads SCTRL+0x08 bit 20 through is_abb_clk_selected()
  // (dwc3-feb.c:166): bit clear means the ABB clock, bit set means the PAD
  // crystal.  The board selects the branch at boot, so it has to be read
  // rather than assumed.
  //
  Reg = MmioRead32 (SCTRL_BASE + SCTRL_SCDEEPSLEEPED);
  if ((Reg & BIT20) == 0) {
    //
    // ABB: enable the TCXO that feeds the combophy ABB reference clock, wait
    // for it to stabilize, then select it in the PCTRL mux.
    //
    MmioWrite32 (PCTRL_BASE + PCTRL_CLK_GATE, PCTRL_CLK_TCXO_EN);
    MicroSecondDelay (10000);

    Reg = MmioRead32 (PCTRL_BASE + PCTRL_PERI_CTRL24);
    Reg &= ~BIT25;
    MmioWrite32 (PCTRL_BASE + PCTRL_PERI_CTRL24, Reg);
  } else {
    //
    // PAD: gt_clk_usb2phy_ref (step 3) is the source instead.  Switch the
    // MISC clock mux over and pick divider 2 on MISC_CTRL7 bits 3:4.
    //
    MiscRegSetBit (12, MISC_CFG54);
    MiscRegSetBit (4, MISC_CFGA0);

    Reg = MmioRead32 (USB_MISC_BASE + MISC_CTRL7);
    Reg &= ~(0x3u << 3);
    Reg |= (0x2u << 3);
    MmioWrite32 (USB_MISC_BASE + MISC_CTRL7, Reg);

    DEBUG ((DEBUG_INFO, "KirinDwc3: PAD reference clock selected\n"));
  }

  //
  // Step 6c: Point the combophy through the "switched" PHY path (the
  // kernel's dwc3_open_controller_clk does this after enabling the
  // reference clocks).
  //
  MiscRegClrBit (4, MISC_CFGA0);
  MiscRegClrBit (5, MISC_CFGA0);
  MiscRegSetValue (BIT3 | BIT4, MISC_CTRL7);

  //
  // Step 7: Release USB20 PHY from IDDQ mode (MISC CTRL5 bit 0)
  //
  MiscRegClrBit (0, MISC_CTRL5);

  //
  // Step 8: Release USB31 PHY from TestPowerDown (MISC CFG50 bit 23)
  //
  MiscRegClrBit (23, MISC_CFG50);
  MicroSecondDelay (50);

  //
  // Step 9: PHY power stable signals (MISC CFG54 bits 1, 9, 11)
  //
  MiscRegSetValue (BIT1 | BIT9 | BIT11, MISC_CFG54);

  //
  // Step 10: TCA mux configuration for USB mode
  //
  MmioWrite32 (USB_MISC_BASE + MISC_TCA_208, 0xFFFF);
  MmioWrite32 (USB_MISC_BASE + MISC_TCA_204, 0x3);

  Reg = MmioRead32 (USB_MISC_BASE + MISC_TCA_200);
  Reg &= ~0x3u;
  MmioWrite32 (USB_MISC_BASE + MISC_TCA_200, Reg);

  MiscRegSetBit (4, MISC_TCA_210);

  Reg = MmioRead32 (USB_MISC_BASE + MISC_TCA_214);
  Reg &= ~(0x3u | (0x3u << 3));
  Reg |= (0x1u | (0x2u << 3));
  MmioWrite32 (USB_MISC_BASE + MISC_TCA_214, Reg);

  MiscRegSetValue (0x3u | (0x3u << 2), MISC_TCA_240);

  //
  // Step 11: Enable SSC (MISC CFG5C bit 1)
  //
  MiscRegSetBit (1, MISC_CFG5C);

  //
  // Step 12: Release USB2.0 PHY from reset (MISC CFGA0 bit 0)
  //
  MiscRegSetBit (0, MISC_CFGA0);
  MicroSecondDelay (100);

  //
  // Step 13: Unreset DWC3 controller (MISC CFGA0 bits 8-9)
  //
  MiscRegSetValue (BIT8 | BIT9, MISC_CFGA0);
  MicroSecondDelay (100);

  //
  // Step 13b: Cap host speed at SuperSpeed (kernel sets MISC 0x7C bit 30
  // when the DT maximum speed is below SuperSpeed+).
  //
  MiscRegSetBit (30, MISC_LINK_CTRL);

  //
  // Step 14: Set VBUS valid for host mode
  // CTRL0 bits 6-7: vbusvalid_sel + avalid
  //
  MiscRegSetValue (BIT6 | BIT7, MISC_CTRL0);

  //
  // CTRL3 bits 5-6: vbusvldextsel + vbusvldext
  //
  MiscRegSetValue (BIT5 | BIT6, MISC_CTRL3);
  MicroSecondDelay (100);

  //
  // Step 15: Set USB2 eye diagram parameter for host
  //
  MmioWrite32 (USB_MISC_BASE + MISC_CTRL4, EYE_DIAGRAM_HOST_PARAM);

  DEBUG ((DEBUG_INFO, "KirinDwc3: USB subsystem initialized\n"));
  return EFI_SUCCESS;
}

STATIC
VOID
Dwc3SetPrtCapHost (
  VOID
  )
{
  UINT32  Reg;

  Reg = MmioRead32 (DWC3_BASE + DWC3_GCTL);
  Reg &= ~DWC3_GCTL_PRTCAP_MASK;
  Reg |= DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_HOST);
  MmioWrite32 (DWC3_BASE + DWC3_GCTL, Reg);
}

STATIC
VOID
Dwc3SetHostMode (
  VOID
  )
{
  UINT32  Reg;

  Reg = MmioRead32 (DWC3_BASE + DWC3_GCTL);
  DEBUG ((DEBUG_INFO, "KirinDwc3: GCTL before = 0x%08x\n", Reg));

  Dwc3SetPrtCapHost ();

  Reg = MmioRead32 (DWC3_BASE + DWC3_GUSB2PHYCFG0);
  Reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
  MmioWrite32 (DWC3_BASE + DWC3_GUSB2PHYCFG0, Reg);

  Reg = MmioRead32 (DWC3_BASE + DWC3_GUSB3PIPECTL0);
  Reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;
  MmioWrite32 (DWC3_BASE + DWC3_GUSB3PIPECTL0, Reg);

  MicroSecondDelay (10000);

  Reg = MmioRead32 (DWC3_BASE + DWC3_GCTL);
  DEBUG ((DEBUG_INFO, "KirinDwc3: GCTL after  = 0x%08x\n", Reg));
}

//
// dwc3_otg_start_host() (dwc3-otg_4_9.c:46).  Runs after set_prtcap(HOST)
// and before xHCI is handed the controller.
//
STATIC
VOID
Dwc3StartHostSetup (
  VOID
  )
{
  UINT32  Reg;

  //
  // snps,gctl-reset-quirk: step 1 is the core-domain pulse.
  //
  Dwc3GctlCoreSoftResetPulse ();

  //
  // CORESOFTRESET resets the core, so put the role and the PHY timing back
  // afterwards.  The kernel leaves this implicit; making it explicit costs
  // two reads and cannot regress.
  //
  Dwc3SetPrtCapHost ();

  Reg = MmioRead32 (DWC3_BASE + DWC3_GUSB2PHYCFG0);
  Reg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK | DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
  Reg |= DWC3_GUSB2PHYCFG_PHYIF (0)
         | DWC3_GUSB2PHYCFG_USBTRDTIM (USBTRDTIM_UTMI_8_BIT);
  Reg &= ~(DWC3_GUSB2PHYCFG_ULPI_UTMI | DWC3_GUSB2PHYCFG_SUSPHY);
  MmioWrite32 (DWC3_BASE + DWC3_GUSB2PHYCFG0, Reg);

  Reg = MmioRead32 (DWC3_BASE + DWC3_GUSB3PIPECTL0);
  Reg &= ~(DWC3_GUSB3PIPECTL_UX_EXIT_PX | DWC3_GUSB3PIPECTL_SUSPHY);
  Reg &= ~DWC3_GUSB3PIPECTL_TX_DEEPH (3u);
  Reg |= DWC3_GUSB3PIPECTL_TX_DEEPH (1u);
  MmioWrite32 (DWC3_BASE + DWC3_GUSB3PIPECTL0, Reg);

  //
  // hisi_dwc3_cpmode_enable() is a no-op unless CP test mode was requested,
  // then the FEB link timers.
  //
  Dwc3LscdTimerSet ();

  //
  // snps,dis-split-quirk (dwc3_otg_split_disable, dwc3-otg_4_9.c:33).
  //
  Reg = MmioRead32 (DWC3_BASE + DWC3_GUCTL3);
  Reg |= DWC3_GUCTL3_SPLITDISABLE;
  MmioWrite32 (DWC3_BASE + DWC3_GUCTL3, Reg);

  //
  // hisi_dwc3_platform_host_quirks() (dwc3-hisi.c:1818): of the six quirk
  // callbacks there, only the two enabled by this DT are wired up -
  // quirk_clear_svc_opp_per_hs (bug 9001205968) and
  // quirk_set_svc_opp_per_hs_sep (bug 9001212079).
  //
  Reg = MmioRead32 (DWC3_BASE + DWC3_GUCTL2);
  Reg &= ~DWC31_GUCTL2_SVC_OPP_PER_HS_MASK;
  MmioWrite32 (DWC3_BASE + DWC3_GUCTL2, Reg);

  Reg = MmioRead32 (DWC3_BASE + DWC3_GUCTL3);
  Reg &= ~DWC3_GUCTL3_SVC_OPP_PER_HS_SEP_MASK;
  Reg |= DWC3_GUCTL3_SVC_OPP_PER_HS_SEP (3u);
  MmioWrite32 (DWC3_BASE + DWC3_GUCTL3, Reg);
}

EFI_STATUS
EFIAPI
KirinDwc3EntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  DEBUG ((DEBUG_INFO, "KirinDwc3: Initializing DWC3 USB at 0x%x\n", DWC3_BASE));

  Status = UsbSubsystemInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinDwc3: USB subsystem init failed: %r\n", Status));
    return Status;
  }

  //
  // dwc3_core_init() while PRTCAP is still DEVICE, then set_prtcap(HOST),
  // then set_vbus_power(1), then dwc3_otg_start_host() - the same order the
  // kernel's OTG state machine walks (dwc3-hisi.c:1453).
  //
  Status = Dwc3CoreInitHost ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinDwc3: core init failed: %r\n", Status));
    return Status;
  }

  Dwc3SetHostMode ();

  KirinHi6523EnableVbusBoost ();

  Dwc3StartHostSetup ();

  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             DWC3_BASE, DWC3_SIZE
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinDwc3: RegisterNonDiscoverable failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "KirinDwc3: xHCI registered as NonDiscoverable device\n"));
  return EFI_SUCCESS;
}
