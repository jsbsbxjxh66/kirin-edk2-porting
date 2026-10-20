#include "KirinUsbDeviceInternal.h"

//
// ---------------------------------------------------------------------------
// Controller bring up
// ---------------------------------------------------------------------------
//

//
// Program the link-layer timers kernel's dwc3_lscdtimer_set() writes before
// start_peripheral() touches the gadget (FEB values from
// dwc3-usb-interface.c), including the GUCTL1 bit that fixes chip bug
// 9001103026 (disable hardware LPM capability).
//
STATIC
VOID
LscdTimerSet (
  VOID
  )
{
  UINT32  Reg;

  DwWrite (DWC3_LSCDTIM10, 0x47E1F4);
  DwWrite (DWC3_LSCDTIM20, 0x753546);

  Reg = DwRead (DWC3_LLUCTL0);
  Reg &= ~DWC3_LLUCTL_PENDING_HP_TIMER_US_MASK;
  Reg |= DWC3_LLUCTL_PENDING_HP_TIMER_US (11);
  DwWrite (DWC3_LLUCTL0, Reg);

  Reg = DwRead (DWC3_LLUCTL0);
  Reg |= DWC3_LLUCTL_ENABLE_HP_TIMER;
  DwWrite (DWC3_LLUCTL0, Reg);

  DwWrite (DWC3_LU1LFPSTXTIM0, 0x940060);

  Reg = DwRead (DWC3_GUCTL);
  Reg &= ~DWC3_GUCTL_ENOVERLAPCHK;
  DwWrite (DWC3_GUCTL, Reg);

  Reg = DwRead (DWC3_LSKIPFREQ0);
  Reg &= ~(0xFFu << 20);
  Reg |= (0xD9u << 20);
  DwWrite (DWC3_LSKIPFREQ0, Reg);

  Reg = DwRead (DWC3_GUCTL1);
  Reg |= (1u << 13);
  DwWrite (DWC3_GUCTL1, Reg);
}

//
// Port capability to DEVICE, exactly what dwc3_set_prtcap() does.  Must be in
// place before anything touches DCTL: the Device Global Register block is
// only decoded when GCTL.PRTCAPDIR selects device - while it still said HOST
// (KirinDwc3Dxe set that during DXE) a write to DCTL.CSFTRST is not
// acknowledged by the device block at all.
//
STATIC
VOID
SetPrtCapDevice (
  VOID
  )
{
  UINT32  Reg;

  Reg = DwRead (DWC3_GCTL);
  Reg &= ~DWC3_GCTL_SCALEDOWN_MASK;
  Reg &= ~DWC3_GCTL_PRTCAP_MASK;
  Reg |= DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_DEVICE);
  DwWrite (DWC3_GCTL, Reg);
  MicroSecondDelay (100);
}

//
// Device-side soft reset, as dwc3_soft_reset() issues it.
//
STATIC
EFI_STATUS
DctlSoftReset (
  VOID
  )
{
  UINT32  Reg;
  UINTN   Retry;

  Reg = DwRead (DWC3_DCTL);
  Reg &= ~DWC3_DCTL_RUN_STOP;
  Reg |= DWC3_DCTL_CSFTRST;
  DwWrite (DWC3_DCTL, Reg);

  for (Retry = 500000; Retry > 0; Retry--) {
    if ((DwRead (DWC3_DCTL) & DWC3_DCTL_CSFTRST) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }

  //
  // GCTL tells us whether PRTCAP actually landed as DEVICE - if it did not,
  // this reset was written to a block that is not decoding it.
  //
  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: dctl reset stuck, GCTL 0x%08x PRTCAP=%d\n",
    DwRead (DWC3_GCTL),
    (DwRead (DWC3_GCTL) >> 12) & 3u
    ));
  return EFI_TIMEOUT;
}

//
// dwc3_otg_force_reset_core() pattern: this core never self-clears
// GCTL.CORESOFTRESET, kernel sets the bit and writes it back clear itself -
// never waits for it.  Hold it for a moment while set, then release.
//
STATIC
VOID
CoreSoftResetPulse (
  VOID
  )
{
  UINT32  Reg;

  Reg = DwRead (DWC3_GCTL);
  DwWrite (DWC3_GCTL, Reg | DWC3_GCTL_CORESOFTRESET);
  MicroSecondDelay (1000);

  Reg = DwRead (DWC3_GCTL);
  DwWrite (DWC3_GCTL, Reg & ~DWC3_GCTL_CORESOFTRESET);
  MicroSecondDelay (1000);
}

EFI_STATUS
Dwc3CoreInit (
  VOID
  )
{
  UINT32   Reg;
  UINT64   EventDma;
  UINTN    Attempt;
  BOOLEAN  Ep0Ok = FALSE;

  Reg = DwRead (DWC3_GSNPSID);
  if (((Reg & 0xFFFF0000) != 0x55330000) &&
      ((Reg & 0xFFFF0000) != 0x33310000))
  {
    DEBUG ((DEBUG_ERROR, "KirinUsbDevice: unexpected GSNPSID 0x%08x\n", Reg));
    mInitStage = "snpsid";
    return EFI_DEVICE_ERROR;
  }
  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: GSNPSID 0x%08x p1 0x%08x\n",
    Reg,
    DwRead (DWC3_GHWPARAMS1)
    ));

  //
  // dwc3_core_init() (core.c:786-797): "ENDXFER polling is available on
  // version 3.10a and later of the DWC_usb3 controller. It is NOT available
  // in the DWC_usb31 controller."  For DWC_usb3 the revision *is* GSNPSID
  // (core.c:651), so the kernel's condition
  //
  //     if (!dwc3_is_usb31(dwc) && dwc->revision >= DWC3_REVISION_310A)
  //
  // reduces to: high half 0x5533 (DWC_usb3, not 0x3331/DWC_usb31) and
  // GSNPSID >= 0x5533310a.  Without this bit, GUCTL2[14] never lets CMDACT
  // clear after an ENDTRANSFER, which is why ep3's teardown hung forever
  // (see EndTransfer()).  On a usb31 core the register lives at a different
  // offset (DWC31_GUCTL2, 0xc608) and the feature does not exist, so writing
  // 0xc19c there is skipped entirely.
  //
  if (((Reg & 0xFFFF0000) == 0x55330000) && (Reg >= DWC3_REVISION_310A)) {
    UINT32 Guctl2 = DwRead (DWC3_GUCTL2);
    if ((Guctl2 & DWC3_GUCTL2_RST_ACTBITLATER) == 0) {
      DwWrite (DWC3_GUCTL2, Guctl2 | DWC3_GUCTL2_RST_ACTBITLATER);
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: GUCTL2 0x%08x -> 0x%08x (ENDXFER poll)\n",
        Guctl2,
        Guctl2 | DWC3_GUCTL2_RST_ACTBITLATER
        ));
    }
  }

  //
  // dwc3_otg_init() (dwc3-otg_4_9.c:265) already leaves GCTL.PRTCAPDIR at
  // DEVICE before anything calls dwc3_core_init() - __dwc3_set_mode() does so
  // at dwc3-otg_4_9.c:158 and dwc3_gadget_restart() does so from pullup(1) -
  // so both DCTL.CSFTRST writes really do land.  dwc3_core_init() issues
  // dwc3_soft_reset() (RUNSTOP cleared, CSFTRST set, polled) followed by
  // dwc3_core_soft_reset() (CSFTRST set, polled) and then forces PRTCAP back
  // to DEVICE (core.c:754).
  //
  // The GCTL core pulse below is snps,gctl-reset-quirk (upstream RFC:
  // https://lkml.iu.edu/2010.2/04930.html#1).  That patch puts
  // dwc3_gctl_core_soft_reset() in the DWC3_GCTL_PRTCAP_DEVICE case so the
  // core is pulsed on every switch into device mode - the reported failure
  // mode is COREIDLE stuck clear in __dwc3_gadget_start(), which stops the
  // reset interrupt and leaves the gadget half-initialised.  This downstream
  // kernel never took that patch: its only GCTL pulse,
  // dwc3_otg_force_reset_core() (dwc3-otg_4_9.c:14), is called solely from
  // dwc3_otg_start_host() at line 54, i.e. host mode only.  Pulse it here so
  // the device path gets the same treatment, keeping the kernel's ordering:
  // GCTL pulse, then PRTCAP forced to DEVICE, then the DCTL soft reset that
  // dwc3_soft_reset() issues.
  //
  for (Attempt = 0; Attempt < 3; Attempt++) {
    if (Attempt > 0) {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: ep0 failed, retry %u\n",
        (UINT32)Attempt
        ));
    }

    mInitStage = "pulse";
    CoreSoftResetPulse ();

    mInitStage = "prtcap";
    SetPrtCapDevice ();

    //
    // dwc3_soft_reset(): clear RUNSTOP so the soft reset always lands even if
    // the port was left running by a previous function.  On attempt 0 a
    // timeout is reported but not fatal - the core can still answer EP0 - so
    // that we never lose a state that previously worked.
    //
    mInitStage = "dctl";
    if (EFI_ERROR (DctlSoftReset ()) && (Attempt > 0)) {
      return EFI_TIMEOUT;
    }

    //
    // USB 3 pipe control: nothing to do for a USB 2.0 only core beyond making
    // sure UX_EXIT_PX stays clear.
    //
    mInitStage = "phy";
    Reg = DwRead (DWC3_GUSB3PIPECTL0);
    Reg &= ~DWC3_GUSB3PIPECTL_UX_EXIT_PX;
    Reg &= ~DWC3_GUSB3PIPECTL_SUSPHY;
    DwWrite (DWC3_GUSB3PIPECTL0, Reg);

    //
    // USB 2.0 PHY interface: UTMI, 8 bit, 8 clocks required for transfer.
    // SUSPHY is deliberately left clear so that endpoint commands never have
    // to toggle it (dwc3_send_gadget_ep_cmd would otherwise race us).
    //
    Reg = DwRead (DWC3_GUSB2PHYCFG0);
    Reg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK | DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
    Reg |= DWC3_GUSB2PHYCFG_PHYIF (0) | DWC3_GUSB2PHYCFG_USBTRDTIM (USBTRDTIM_UTMI_8_BIT);
    Reg &= ~DWC3_GUSB2PHYCFG_ULPI_UTMI;
    Reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
    DwWrite (DWC3_GUSB2PHYCFG0, Reg);

    //
    // Same GCTL treatment dwc3_core_init() applies: scaledown cleared, PRTCAP
    // forced to DEVICE, and the EN_PWROPT power option honoured - CLK keeps
    // clock gating enabled (DSBLCLKGTNG clear, which is also the reset value),
    // HIB sets GBLHIBERNATIONEN exactly as the kernel does (device hibernation
    // is unimplemented but the kernel sets the bit anyway).
    //
    Reg = DwRead (DWC3_GCTL);
    Reg &= ~DWC3_GCTL_SCALEDOWN_MASK;
    switch (DWC3_GHWPARAMS1_EN_PWROPT (DwRead (DWC3_GHWPARAMS1))) {
      case DWC3_GHWPARAMS1_EN_PWROPT_CLK:
        Reg &= ~DWC3_GCTL_DSBLCLKGTNG;
        break;
      case DWC3_GHWPARAMS1_EN_PWROPT_HIB:
        Reg |= DWC3_GCTL_GBLHIBERNATIONEN;
        break;
      default:
        break;
    }
    Reg &= ~DWC3_GCTL_PRTCAP_MASK;
    Reg |= DWC3_GCTL_PRTCAPDIR (DWC3_GCTL_PRTCAP_DEVICE);
    DwWrite (DWC3_GCTL, Reg);
    MicroSecondDelay (100);

    //
    // The kernel programs these two before the first endpoint command:
    // start_peripheral() calls dwc3_lscdtimer_set(), and __dwc3_gadget_start()
    // clears GRXTHRCFG.PKTCNTSEL.
    //
    LscdTimerSet ();
    Reg = DwRead (DWC3_GRXTHRCFG);
    Reg &= ~DWC3_GRXTHRCFG_PKTCNTSEL;
    DwWrite (DWC3_GRXTHRCFG, Reg);

    //
    // High speed, address 0.
    //
    mInitStage = "dcfg";
    Reg = DwRead (DWC3_DCFG);
    Reg &= ~DWC3_DCFG_DEVADDR_MASK;
    Reg &= ~7u;
    Reg |= DWC3_DCFG_HIGHSPEED;
    //
    // dwc3_gadget_setup_nump(): GRXTHRCFG.PKTCNTSEL is cleared just above, so
    // the core takes the ACK packet's NUMP from DCFG instead of computing it.
    // Reset default is 0 and a 0-valued NUMP advertises no bulk OUT capacity.
    //
    {
      UINT32 Ram2Depth, MdWidth, RamBytes, Nump;

      Ram2Depth = DWC3_GHWPARAMS7_RAM2_DEPTH (DwRead (DWC3_GHWPARAMS7));
      MdWidth   = DWC3_GHWPARAMS0_MDWIDTH (DwRead (DWC3_GHWPARAMS0));
      Nump      = 0;
      RamBytes  = Ram2Depth * (MdWidth / 8);
      if ((Ram2Depth != 0) && (MdWidth >= 8) && (RamBytes > (24u + 16u))) {
        Nump = (RamBytes - 24u - 16u) / 1024u;
        if (Nump > 16) {
          Nump = 16;
        }
      }
      if (Nump == 0) {
        Nump = 1;
      }
      Reg = (Reg & ~DWC3_DCFG_NUMP_MASK) | DWC3_DCFG_NUMP (Nump);
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: DCFG nump %u (ram2 %u mdw %u)\n",
        Nump,
        Ram2Depth,
        MdWidth
        ));
    }
    DwWrite (DWC3_DCFG, Reg);

    //
    // Event buffer.
    //
    mInitStage = "events";
    mEventLpos = 0;
    //
    // The link starts up in U0 (ON in high speed); mLinkState only ever moves
    // on a link-status-change event after this.
    //
    mLinkState = DWC3_LINK_STATE_U0;
    ZeroMem (mEventBuffer, DWC3_EVENT_BUFFER_SIZE);
    WriteBackDataCacheRange (mEventBuffer, DWC3_EVENT_BUFFER_SIZE);

    EventDma = (UINT64)(UINTN)mEventBuffer;
    DwWrite (DWC3_GEVNTADRLO0, (UINT32)EventDma);
    DwWrite (DWC3_GEVNTADRHI0, (UINT32)RShiftU64 (EventDma, 32));
    //
    // Kernel mirror: dwc3_event_buffers_setup() (core.c:343-349) writes the
    // address and the size only - no INTMASK - and DEVTEN stays 0 while EP0
    // is being configured.  dwc3_gadget_enable_irq() raises DEVTEN only; it
    // never touches GEVNTSIZ.  We set GEVNTSIZ[31] afterwards (see below)
    // because nobody services this core's interrupt line in DXE and an
    // unmasked, unserviced line is the one part of the kernel's scheme that
    // does not translate - it is an interrupt mask, not a buffer enable, so
    // the polled path is unaffected.  INTMASK comes back only in teardown,
    // matching dwc3_event_buffers_cleanup() (core.c:364).
    //
    DwWrite (
      DWC3_GEVNTSIZ0,
      DWC3_GEVNTSIZ_SIZE (DWC3_EVENT_BUFFER_SIZE)
      );
    DwWrite (DWC3_GEVNTCOUNT0, 0);

    //
    // EP0 must be live before the pullup is turned on.  Per-step register dumps
    // are deliberately not printed here: the failure line below plus the
    // DEPSTARTCFG/xfer lines are what fits on screen.
    //
    mInitStage = "ep0";

    if (!EFI_ERROR (EnableEndpoint (EP0_OUT, 0, MAX_PACKET_EP0, FALSE)) &&
        !EFI_ERROR (EnableEndpoint (EP0_IN, 0, MAX_PACKET_EP0, TRUE)))
    {
      Ep0Ok = TRUE;
      break;
    }
  }

  if (!Ep0Ok) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Kernel equivalent of dwc3_gadget_enable_irq() (gadget.c:1873-1882),
  // word for word.  SOFEN is deliberately absent because the kernel leaves it
  // absent too; the interrupt output stays masked for our polled scheme via
  // GEVNTSIZ[31].
  //
  DwWrite (
    DWC3_DEVTEN,
    DWC3_DEVTEN_VNDRDEVTSTRCVEDEN | DWC3_DEVTEN_EVNTOVERFLOWEN
    | DWC3_DEVTEN_CMDCMPLTEN | DWC3_DEVTEN_ERRTICERREN
    | DWC3_DEVTEN_EOPFEN | DWC3_DEVTEN_WKUPEVTEN
    | DWC3_DEVTEN_ULSTCNGEN | DWC3_DEVTEN_CONNECTDONEEN
    | DWC3_DEVTEN_USBRSTEN | DWC3_DEVTEN_DISCONNEVTEN
    );
  DwWrite (
    DWC3_GEVNTSIZ0,
    DWC3_GEVNTSIZ_INTMASK | DWC3_GEVNTSIZ_SIZE (DWC3_EVENT_BUFFER_SIZE)
    );

  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: EP0 enabled, DALEPENA 0x%08x\n",
    DwRead (DWC3_DALEPENA)
    ));

  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: dep0 DEPCMD 0x%08x dep0 PAR0 0x%08x dep1 DEPCMD 0x%08x\n",
    DepRead (EP0_OUT, DWC3_DEPCMD),
    DepRead (EP0_OUT, DWC3_DEPCMDPAR0),
    DepRead (EP0_IN, DWC3_DEPCMD)
    ));

  Ep0ArmSetup ();

  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: ep0 armed rsc 0x%x v %d busy %d\n",
    mEp0Resource,
    mEp0ResourceValid ? 1 : 0,
    mEp0Busy ? 1 : 0
    ));

  return EFI_SUCCESS;
}

VOID
Dwc3RunStop (
  IN BOOLEAN  Run
  )
{
  UINT32 Reg;
  UINTN  Retry;

  Reg = DwRead (DWC3_DCTL);
  if (Run) {
    //
    // dwc3_gadget_run_stop() (gadget.c): this core's revision is past 1.94a,
    // so KEEP_CONNECT is cleared before the pullup is asserted.  The kernel
    // then sets it again only when the core reports hibernation support, which
    // is the same GHWPARAMS1.EN_PWROPT field Dwc3CoreInit() reads for
    // GCTL.GBLHIBERNATIONEN.
    //
    Reg &= ~DWC3_DCTL_KEEP_CONNECT;
    Reg |= DWC3_DCTL_RUN_STOP;
    if (DWC3_GHWPARAMS1_EN_PWROPT (DwRead (DWC3_GHWPARAMS1)) ==
        DWC3_GHWPARAMS1_EN_PWROPT_HIB)
    {
      Reg |= DWC3_DCTL_KEEP_CONNECT;
    }
  } else {
    Reg &= ~DWC3_DCTL_RUN_STOP;
    Reg &= ~DWC3_DCTL_KEEP_CONNECT;
  }
  DwWrite (DWC3_DCTL, Reg);

  for (Retry = 500000; Retry > 0; Retry--) {
    Reg = DwRead (DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT;
    if ((!Run && (Reg != 0)) || (Run && (Reg == 0))) {
      return;
    }
    MicroSecondDelay (1);
  }

  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: run/stop %d did not settle\n",
    Run
    ));
}

/**
  The remote wakeup dwc3_send_gadget_ep_cmd() (gadget.c:337-347) issues
  before a STARTTRANSFER while the link is parked.

  A STARTTRANSFER accepted on a parked link only completes once the link has
  returned to U0, so the kernel drives that transition itself rather than
  waiting for the host.  The state test is the one __dwc3_gadget_wakeup()
  (gadget.c:1630-1678) makes - only U1/U2/U3, and on a SuperSpeed link never -
  and this core is UTMI high speed only, so the SuperSpeed refusal has nothing
  to match here.  The recovery request itself is dwc3_gadget_set_link_state()
  with DWC3_LINK_STATE_RECOV: wait for DSTS.DCNRD to fall, clear the request
  field in DCTL, ask for recovery, then poll DSTS.USBLNKST back to U0.
**/
VOID
Dwc3WakeupIfNeeded (
  VOID
  )
{
  UINT32  Reg;
  UINTN   Retry;

  if ((mLinkState != DWC3_LINK_STATE_U1) &&
      (mLinkState != DWC3_LINK_STATE_U2) &&
      (mLinkState != DWC3_LINK_STATE_U3))
  {
    return;
  }

  for (Retry = 10000; Retry > 0; Retry--) {
    if ((DwRead (DWC3_DSTS) & DWC3_DSTS_DCNRD) == 0) {
      break;
    }
    MicroSecondDelay (5);
  }

  Reg  = DwRead (DWC3_DCTL);
  Reg &= ~DWC3_DCTL_ULSTCHNGREQ_MASK;
  Reg |= DWC3_DCTL_ULSTCHNGREQ (DWC3_LINK_STATE_RECOV);
  DwWrite (DWC3_DCTL, Reg);
  MicroSecondDelay (10);

  for (Retry = 0; Retry < 300; Retry++) {
    if ((DWC3_DSTS_USBLNKST (DwRead (DWC3_DSTS)) & DWC3_LINK_STATE_MASK) ==
        DWC3_LINK_STATE_U0)
    {
      mLinkState = DWC3_LINK_STATE_U0;
      return;
    }
  }

  //
  // Give up silently under DEBUG_ERROR's usual throttle: the STARTTRANSFER
  // that follows reports its own failure through the command status, and a
  // line per parked arm would bury that.
  //
  mDeviceEventCount++;
  if (mDeviceEventCount <= 8) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: remote wakeup did not reach U0 (dsts 0x%08x)\n",
      DwRead (DWC3_DSTS)
      ));
  }
}

EFI_STATUS
AllocateDmaBuffers (
  VOID
  )
{
  if (mDmaAllocated) {
    return EFI_SUCCESS;
  }

  mEventBuffer = AllocateAlignedPages (
                   EFI_SIZE_TO_PAGES (DWC3_EVENT_BUFFER_SIZE),
                   EFI_SIZE_TO_PAGES (DWC3_EVENT_BUFFER_SIZE)
                   );
  mCtrlRequest = AllocateAlignedPages (1, 1);
  mEp0Trb      = AllocateAlignedPages (1, 1);
  mBulkInTrb   = AllocateAlignedPages (1, 1);
  mBulkOutTrb  = AllocateAlignedPages (1, 1);
  mTxDmaBuf    = AllocateAlignedPages (
                   EFI_SIZE_TO_PAGES (TX_STATIC_BUF_SIZE),
                   EFI_SIZE_TO_PAGES (TX_STATIC_BUF_SIZE)
                   );
  mRxDmaBuf    = AllocateAlignedPages (
                   EFI_SIZE_TO_PAGES (RX_CHUNK_SIZE),
                   EFI_SIZE_TO_PAGES (RX_CHUNK_SIZE)
                   );
  mEp0DataBuf  = AllocateAlignedPages (
                   EFI_SIZE_TO_PAGES (EP0_DATA_MAX),
                   EFI_SIZE_TO_PAGES (EP0_DATA_MAX)
                   );

  if ((mEventBuffer == NULL) || (mCtrlRequest == NULL) ||
      (mEp0Trb == NULL) || (mBulkInTrb == NULL) ||
      (mBulkOutTrb == NULL) || (mTxDmaBuf == NULL) ||
      (mRxDmaBuf == NULL) || (mEp0DataBuf == NULL))
  {
    return EFI_OUT_OF_RESOURCES;
  }

  mDmaAllocated = TRUE;

  return EFI_SUCCESS;
}
