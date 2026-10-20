#include "KirinUsbDeviceInternal.h"

//
// ---------------------------------------------------------------------------
// Event processing
// ---------------------------------------------------------------------------
//
STATIC
VOID
HandleDeviceEvent (
  IN UINT32  Event
  )
{
  UINT32  SubEvent;
  UINT32  Reg;
  UINT32  Speed;
  UINT32  MaxPacket;

  //
  // dwc3_event_devt: one_bit(0), device_event(1..7) which the core expects
  // to read as zero, type(8..11) carries the event number, event_info(16..24).
  //
  SubEvent = (Event >> 8) & 0xF;

  switch (SubEvent) {
    case DWC3_DEVICE_EVENT_RESET:
      DEBUG ((DEBUG_ERROR, "KirinUsbDevice: bus reset\n"));

      mGadgetState = UsbStateDefault;
      mConfigured  = FALSE;
      mEp0ThreeStage = FALSE;
      mEp0ExpectIn   = FALSE;
      mEp0NextStatus = FALSE;
      //
      // A reset aborts whatever data stage was running, so the ZLP that
      // dwc3_ep0_complete_data() would have queued is no longer owed.
      //
      mEp0Zero     = FALSE;

      // Hardware clears the assigned address on its own in most revisions,
      // but the kernel driver does it explicitly, so mirror it.
      DwWrite (DWC3_DCFG, DwRead (DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK);

      //
      // dwc3_gadget_reset_interrupt() also drops DCTL.TSTCTRL.  A host that
      // ran TEST_PACKET or TEST_J before the reset leaves the link driving
      // test patterns, so every later SETUP is answered into a dead bus and
      // the host reports "device descriptor request failed" without ever
      // seeing a reply.  CONNDONE clears it again below, as the kernel does.
      //
      DwWrite (DWC3_DCTL, DwRead (DWC3_DCTL) & ~DWC3_DCTL_TSTCTRL_MASK);

      // Bulk transfers do not survive a bus reset.
      //
      // Deliberately NOT EndTransfer()-ing them here, exactly like the
      // DISCONNECT branch below.  The hardware drops the endpoint transfer
      // resources as part of the reset, and an ENDTRANSFER against an index
      // the core no longer owns leaves CMDACT stuck set - this code then
      // blocks inside PollEvents() for seconds, the GEVNT buffer stops being
      // drained, and Windows loses the enumeration ("unknown device" on
      // replug).  Zero the indices as well as the busy flags: the resources
      // are gone, so the next arm must take the STARTTRANSFER branch (which
      // EnableEndpoint() also enforces by re-initialising the TRB rings) and
      // must not UPDATE a resource the core dropped during the reset.
      //
      mBulkInBusy     = FALSE;
      mBulkOutBusy    = FALSE;
      mBulkInResource = 0;
      mBulkOutResource = 0;

      //
      // usb_gadget_udc_reset() -> composite_setup(NULL) -> reset_config()
      // calls usb_ep_disable() on every function endpoint, which clears its
      // DALEPENA bit.  We never do that, so the next SET_CONFIGURATION would
      // issue DEPCFG ACTION_INIT on an endpoint that is still enabled - the
      // one command the databook forbids - and SET_CONFIGURATION would stall
      // (Windows then shows "unknown device").  Clear the two bulk bits so
      // EnableEndpoint() starts from a clean, disabled endpoint again.
      //
      DwWrite (
        DWC3_DALEPENA,
        DwRead (DWC3_DALEPENA) &
          ~((1u << EP_BULK_OUT) | (1u << EP_BULK_IN))
        );
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: ep0 rsc valid %d busy %d phase %d dalep 0x%08x\n",
        mEp0ResourceValid,
        mEp0Busy,
        mEp0Phase,
        DwRead (DWC3_DALEPENA)
        ));

      // dwc3_stop_active_transfers() deliberately skips endpoints 0 and 1,
      // so the armed SETUP transfer stays valid across the reset as long as
      // no control transfer was in flight.  Otherwise re-arm it.  A function
      // switch drops the resource on purpose, so arm again when it is gone.
      if (!mEp0ResourceValid) {
        mEp0Busy = FALSE;
        Ep0ArmSetup ();
      } else if (mEp0Phase != Ep0PhaseSetup) {
        //
        // mEp0ResourceEp, not EP0_OUT: during a data or status stage the
        // resource may belong to EP0-IN, and ending it on the wrong
        // endpoint leaves the real one permanently allocated (every later
        // STARTTRANSFER on it then reports status 1).
        //
        EndTransfer (mEp0ResourceEp, mEp0Resource);
        mEp0Resource      = 0;
        mEp0ResourceValid = FALSE;
        mEp0Busy          = FALSE;
        Ep0ArmSetup ();
      } else {
        mEp0Busy = TRUE;
      }
      break;

    case DWC3_DEVICE_EVENT_CONNECT_DONE:
      Speed = DwRead (DWC3_DSTS) & DWC3_DSTS_CONNECTSPD;
      //
      // dwc3_gadget_conndone_interrupt() (gadget.c:2878-2924) derives the EP0
      // max packet size from the negotiated speed and re-issues SETEPCONFIG
      // with it.  It does NOT write DCFG.DSPD here: __dwc3_gadget_start()
      // programmed that once, from dwc->maximum_speed, and re-negotiating it
      // with the link already up is not part of the databook sequence.  Our
      // revision rewrote DSPD on every CONNDONE; that write is dropped.
      //
      switch (Speed) {
        case DWC3_DSTS_HIGHSPEED:
          DEBUG ((DEBUG_ERROR, "KirinUsbDevice: connected at HIGH speed\n"));
          MaxPacket = MAX_PACKET_EP0;
          break;
        case DWC3_DSTS_FULLSPEED:
          DEBUG ((DEBUG_ERROR, "KirinUsbDevice: connected at FULL speed\n"));
          MaxPacket = MAX_PACKET_EP0;
          break;
        case DWC3_DSTS_LOWSPEED:
          DEBUG ((DEBUG_ERROR, "KirinUsbDevice: connected at LOW speed\n"));
          MaxPacket = 8;
          break;
        default:
          DEBUG ((
            DEBUG_ERROR,
            "KirinUsbDevice: connected at speed %d\n",
            Speed
            ));
          MaxPacket = ((Speed == DWC3_DSTS_SUPERSPEED) ||
                       (Speed == DWC3_DSTS_SUPERSPEED_PLUS)) ?
                      512 : MAX_PACKET_EP0;
          break;
      }

      //
      // dwc3_gadget_conndone_interrupt() clears DCTL.TSTCTRL a second time:
      // the core can re-enter a test mode between reset and connect-done, and
      // the enumeration that follows is the last chance to undo it.
      //
      DwWrite (DWC3_DCTL, DwRead (DWC3_DCTL) & ~DWC3_DCTL_TSTCTRL_MASK);

      //
      // dwc3_gadget_conndone_interrupt() stores the negotiated max packet on
      // the endpoint before it re-issues SETEPCONFIG; keep ours so
      // Ep0StartData()/Ep0StartZlp() can test "is the length a whole number
      // of packets" the same way dwc3_ep0_complete_data() does.
      //
      mEp0MaxPacket = MaxPacket;

      //
      // dwc3_gadget_conndone_interrupt() re-issues SETEPCONFIG for EP0 with
      // the negotiated max packet size, but deliberately does NOT start a new
      // configuration round: __dwc3_gadget_ep_enable() only calls
      // dwc3_gadget_start_config() when the endpoint is not yet enabled, and
      // DEPSTARTCFG issued with an armed SETUP transfer would throw away the
      // transfer resources we already own.  It also passes modify=true, i.e.
      // DEPCFG_ACTION_MODIFY - ACTION_INIT on an endpoint that is already
      // enabled (and already holds the armed SETUP transfer) is what the
      // databook forbids.  Mirror both.
      //
      if (EFI_ERROR (SetEpConfig (EP0_OUT, 0, MaxPacket, FALSE, TRUE)) ||
          EFI_ERROR (SetEpConfig (EP0_IN, 0, MaxPacket, TRUE, TRUE)))
      {
        DEBUG ((DEBUG_ERROR, "KirinUsbDevice: conndone EP0 config failed\n"));
      }

      //
      // Kernel never re-arms EP0 here: the SETUP transfer armed at init is
      // still live across the reset.  Re-arming it double-starts the
      // endpoint, which the core rejects with status 1 ("no transfer
      // resource available") and which then loses the handle to the
      // transfer that is still running.
      //
      if (!mEp0ResourceValid) {
        mEp0Busy = FALSE;
        Ep0ArmSetup ();
      }
      break;

    case DWC3_DEVICE_EVENT_DISCONNECT:
      DEBUG ((DEBUG_ERROR, "KirinUsbDevice: disconnect\n"));
      mGadgetState = UsbStateDefault;
      mConfigured  = FALSE;
      mEp0ThreeStage = FALSE;
      mEp0ExpectIn   = FALSE;
      mEp0NextStatus = FALSE;
      mEp0Zero       = FALSE;
      mBulkInBusy    = FALSE;
      mBulkOutBusy   = FALSE;
      mBulkInResource  = 0;
      mBulkOutResource = 0;
      //
      // The cable went out: whatever the core thinks is armed on EP0 is no
      // longer reachable, and a stale mEp0ResourceValid would make the next
      // RESET skip Ep0ArmSetup() entirely - the device then never answers a
      // SETUP again and the host reports "unknown device".  Release it here
      // (a bus reset will re-arm it through the !mEp0ResourceValid path).
      //
      if (mEp0ResourceValid) {
        EndTransfer (mEp0ResourceEp, mEp0Resource);
        mEp0Resource      = 0;
        mEp0ResourceValid = FALSE;
      }
      mEp0Busy = FALSE;
      mEp0Phase = Ep0PhaseSetup;
      //
      // dwc3_gadget_disconnect_interrupt() drops DCTL.INITU1ENA and
      // INITU2ENA.  Leaving them set carries the previous session's low-power
      // intent into the next attach, where the host has not offered U1/U2 and
      // the link parks before the first SETUP.
      //
      DwWrite (
        DWC3_DCTL,
        DwRead (DWC3_DCTL) & ~(DWC3_DCTL_INITU1ENA | DWC3_DCTL_INITU2ENA)
        );
      DwWrite (
        DWC3_DALEPENA,
        DwRead (DWC3_DALEPENA) &
          ~((1u << EP_BULK_OUT) | (1u << EP_BULK_IN))
        );
      break;

    //
    // Everything below was falling into a silent default while DEVTEN only
    // carried the five bits we happened to have named.  With the full
    // dwc3_gadget_enable_irq() mask in place these are exactly the events
    // that answer "why did the host go quiet" - each one prints the state
    // read straight from DSTS rather than a decoded name, so nothing here
    // can be wrong about a link-state encoding we have not seen yet.
    //
    case DWC3_DEVICE_EVENT_OVERFLOW:
      //
      // The event buffer filled faster than PollEvents() drained it, so the
      // core stopped writing new events: from here on completions simply
      // never arrive, which is indistinguishable from a host that stopped
      // reading.  Loudest print in the file for that reason.
      //
      Reg = DwRead (DWC3_GEVNTCOUNT0) & DWC3_GEVNTCOUNT_MASK;
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: EVENT BUFFER OVERFLOW (gevntcount %u, lpos %u) - "
        "events lost from here\n",
        (UINT32)Reg,
        (UINT32)mEventLpos
        ));
      break;

    case DWC3_DEVICE_EVENT_LINK_STATUS_CHANGE:
      Reg = DwRead (DWC3_DSTS);
      //
      // Keep the state so dwc3_send_gadget_ep_cmd()'s "wakeup before
      // STARTTRANSFER while parked in U1/U2/U3" rule has the same input the
      // kernel's dwc->link_state does (gadget.c:3097).
      //
      mLinkState = (UINT8)(DWC3_DSTS_USBLNKST (Reg) & DWC3_LINK_STATE_MASK);
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: link state change -> %u (dsts 0x%08x)\n",
        (UINT32)DWC3_DSTS_USBLNKST (Reg),
        Reg
        ));
      break;

    case DWC3_DEVICE_EVENT_EOPF:
      //
      // On a >=2.30a core this is not end-of-frame, it is the SUSPEND event
      // (gadget.c:3052-3071).  A suspended link stops taking IN transfers
      // without raising any endpoint event at all - the other way to get
      // five seconds of silence out of PollEvents().
      //
      // Below 2.30a the same event is a genuine end-of-periodic-frame and
      // fires continuously, so the first eight go out verbatim (a real
      // suspend lands there) and everything after is sampled: an old core
      // must not scroll the line we are hunting off the screen.
      //
      mDeviceEventCount++;
      if ((mDeviceEventCount <= 8) || ((mDeviceEventCount & 0xFFF) == 0)) {
        Reg = DwRead (DWC3_DSTS);
        DEBUG ((
          DEBUG_ERROR,
          "KirinUsbDevice: suspend/EOPF #%u (dsts 0x%08x, configured %d)\n",
          (UINT32)mDeviceEventCount,
          Reg,
          mConfigured ? 1 : 0
          ));
      }
      break;

    case DWC3_DEVICE_EVENT_WAKEUP:
      Reg = DwRead (DWC3_DSTS);
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: wakeup (dsts 0x%08x)\n",
        Reg
        ));
      break;

    case DWC3_DEVICE_EVENT_HIBER_REQ:
      Reg = DwRead (DWC3_DSTS);
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: hibernation request (dsts 0x%08x)\n",
        Reg
        ));
      break;

    case DWC3_DEVICE_EVENT_ERRATIC_ERROR:
      DEBUG ((DEBUG_ERROR, "KirinUsbDevice: erratic error\n"));
      break;

    case DWC3_DEVICE_EVENT_CMD_CMPL:
      DEBUG ((DEBUG_ERROR, "KirinUsbDevice: device command complete\n"));
      break;

    case DWC3_DEVICE_EVENT_SOF:
      // SOFEN is never set, so this cannot arrive; count it anyway in case a
      // future mask edit turns it on - 125us intervals must not scroll the
      // screen away from the line we are hunting.
      mDeviceEventCount++;
      if ((mDeviceEventCount & 0x1FFF) == 1) {
        DEBUG ((
          DEBUG_ERROR,
          "KirinUsbDevice: unexpected SOF #%u\n",
          (UINT32)mDeviceEventCount
          ));
      }
      break;

    default:
      // dwc3_gadget_interrupt()'s default is dev_WARN("UNKNOWN IRQ %d")
      // (gadget.c:3189-3190).  Same idea, throttled for the same reason.
      mDeviceEventCount++;
      if (mDeviceEventCount <= 8) {
        DEBUG ((
          DEBUG_ERROR,
          "KirinUsbDevice: unknown device event %u (raw 0x%08x)\n",
          (UINT32)SubEvent,
          Event
          ));
      }
      break;
  }
}

STATIC
VOID
HandleEp0Event (
  IN UINT32  EpEvent,
  IN UINT32  Status,
  IN UINT8   EpNum
  )
{
  switch (EpEvent) {
    case DWC3_DEPEVT_XFERCOMPLETE:
      //
      // dwc3_ep0_xfer_complete() drops dep->resource_index to 0 here: a
      // completed transfer gives its resource back to the core, so keeping
      // the old index would make the next Ep0ArmSetup() issue a pointless
      // (and rejected) ENDTRANSFER against a resource that no longer exists.
      //
      mEp0Resource      = 0;
      mEp0ResourceValid = FALSE;
      mEp0Busy          = FALSE;

      switch (mEp0Phase) {
        case Ep0PhaseSetup:
          InvalidateDataCacheRange (mCtrlRequest, 8);
          CopyMem (&mSetup, mCtrlRequest, sizeof (mSetup));

          mEp0ThreeStage = (mSetup.Length != 0);
          mEp0ExpectIn   = (mSetup.RequestType & 0x80) != 0;
          mEp0NextStatus = !mEp0ThreeStage;

          DEBUG ((DEBUG_ERROR,
            "KirinUsbDevice: setup %02x %02x val %04x idx %04x len %04x\n",
            mSetup.RequestType,
            mSetup.Request,
            mSetup.Value,
            mSetup.Index,
            mSetup.Length
            ));

          mEp0StallPending = FALSE;

          if ((mSetup.RequestType & 0x60) == USB_REQ_TYPE_STANDARD) {
            HandleStandardSetup ();
          } else if ((mSetup.RequestType == 0x21) && (mSetup.Request == 0xFF)) {
            //
            // Bulk-Only Mass Storage Reset (class, interface, 0xFF): the
            // host's escape hatch after an aborted command or a cable pull.
            // Accept it with no data stage; the application resynchronises on
            // the next well-formed CBW.  GetMaxLUN (0xA1 / 0xFE) is left to
            // STALL on purpose - the BOT spec defines a STALL there as
            // "device supports a single LUN".
            //
            mEp0Data    = NULL;
            mEp0DataLen = 0;
            DEBUG ((DEBUG_ERROR, "KirinUsbDevice: BOT reset\n"));
          } else {
            Ep0Stall ();
            return;
          }

          if (mEp0StallPending) {
            return;
          }

          if (mEp0ThreeStage) {
            Ep0StartData ();
          }
          // Two stage transfers wait for XferNotReady(CONTROL_STATUS).
          break;

        case Ep0PhaseData:
          //
          // dwc3_ep0_complete_data() (ep0.c): once the data request is given
          // back, a control IN whose length is an exact multiple of the max
          // packet size still needs a ZLP - the transfer then contains
          // neither a short packet nor a ZLP, so nothing tells the host the
          // data has ended and it polls until it reports "device descriptor
          // request failed".  The condition is
          //   IS_ALIGNED(ur->length, maxpacket) && ur->length && ur->zero
          // with ur->zero being composite.c:1982 (armed < wLength), set by
          // Ep0StartData().  A ZLP's own completion re-enters this case with
          // mEp0Zero already cleared, so it falls through to the status stage.
          //
          if (mEp0Zero && (mEp0DataLen != 0) &&
              (mEp0MaxPacket != 0) &&
              ((mEp0DataLen % mEp0MaxPacket) == 0))
          {
            Ep0StartZlp ();
          } else {
            mEp0NextStatus = TRUE;
          }
          break;

        case Ep0PhaseStatus:
          Ep0ArmSetup ();
          break;

        default:
          break;
      }
      break;

    case DWC3_DEPEVT_XFERNOTREADY:
      if (Status == DEPEVT_STATUS_CONTROL_DATA) {
        if ((mEp0Phase == Ep0PhaseData) && !mEp0Busy && !mEp0Stalled) {
          Ep0StartData ();
        }
      } else if (Status == DEPEVT_STATUS_CONTROL_STATUS) {
        if (mEp0NextStatus) {
          mEp0NextStatus = FALSE;
          Ep0StartStatus (EpNum);
        }
      }
      break;

    case DWC3_DEPEVT_EPCMDCMPLT:
    case DWC3_DEPEVT_XFERINPROGRESS:
    default:
      break;
  }
}

STATIC
VOID
ProcessEndpointEvent (
  IN UINT32  Event
  )
{
  UINT8   EpNum;
  UINT32  EpEvent;
  UINT32  Status;
  UINT8   *Slot;
  UINT32  TrbSize;
  UINTN   Residual;
  UINTN   Transferred;

  EpNum   = (UINT8)((Event >> 1) & 0x1F);
  EpEvent = (Event >> 6) & 0x0F;
  Status  = (Event >> 12) & 0x0F;

  if ((EpNum == EP0_OUT) || (EpNum == EP0_IN)) {
    HandleEp0Event (EpEvent, Status, EpNum);
    return;
  }

  //
  // dwc3_endpoint_interrupt() (gadget.c:2569-2571) drops every event for an
  // endpoint that is not enabled - its software DWC3_EP_ENABLED flag, whose
  // exact hardware counterpart is the DALEPENA bit - unless an ENDTRANSFER
  // completion is still outstanding.  We never wait on ENDTRANSFER
  // completions (EpCmd() polls CMDACT), so the DALEPENA bit alone is the
  // faithful test.  Without it a late completion for an endpoint that a bus
  // reset or a new SET_CONFIGURATION has just taken down is still handed to
  // HandleBulkOutComplete()/the bulk IN completion: the transport then sees a
  // packet from a session that no longer exists, and the TRB cursor it
  // advances has already been reset to 0 by the next EnableEndpoint().
  //
  if ((DwRead (DWC3_DALEPENA) & (1u << EpNum)) == 0) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: ep%u ev 0x%x st 0x%x dropped, ep disabled\n",
      EpNum,
      EpEvent,
      Status
      ));
    return;
  }

  //
  // A non-EP0 event is the only way to see which event type the core actually
  // raised and with what status.  XFERINPROGRESS is by far the commonest - it
  // arrives once per 64 KB chunk, 71 times for a 4.5 MB download - so it is
  // counted instead of printed in full: #1 proves the events really are
  // arriving, and the periodic sample says how far the download got.  Every
  // other event type (an unexpected EPCMDCMPLT, a stall, an event on the wrong
  // endpoint) prints unconditionally, because those are the ones we are hunting.
  //
  if (EpEvent == DWC3_DEPEVT_XFERINPROGRESS) {
    mBulkXferEventCount++;
    if ((mBulkXferEventCount == 1) || ((mBulkXferEventCount & 0x3F) == 0)) {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: ep%u ev 0x%x st 0x%x #%u\n",
        EpNum,
        EpEvent,
        Status,
        mBulkXferEventCount
        ));
    }
  } else {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: ep%u ev 0x%x st 0x%x\n",
      EpNum,
      EpEvent,
      Status
      ));
  }

  //
  // Bulk endpoints are configured with XFER_IN_PROGRESS_EN only -
  // XFER_COMPLETE_EN is control-only, matching dwc3_gadget_set_ep_config()
  // (gadget.c:599-603) - so a bulk completion arrives as XFERINPROGRESS and
  // never as XFERCOMPLETE.  The two enables are mutually exclusive in
  // practice: with bit 8 clear the core cannot raise ev 0x1 for a bulk
  // endpoint, so no transfer can be reported twice here.  The TRB size
  // write-back happens either way, so the residual is read identically.
  //
  if (EpNum == EP_BULK_OUT) {
    if (EpEvent == DWC3_DEPEVT_XFERCOMPLETE) {
      //
      // gadget.c:2523-2525: the core hands the transfer resource back on
      // XFERCOMPLETE.  Unreachable for bulk while XFER_COMPLETE_EN is clear;
      // kept so this file stays line-for-line with the kernel in case the
      // enable set is ever revisited.
      //
      mBulkOutResource = 0;
    }
    if ((EpEvent == DWC3_DEPEVT_XFERCOMPLETE) ||
        (EpEvent == DWC3_DEPEVT_XFERINPROGRESS))
    {
      HandleBulkOutComplete ();
    }
    //
    // EPCMDCMPLT is the CMDIOC completion of our own ENDTRANSFER, which since
    // the UPDATETRANSFER change is only issued by EP0 and by the TX timeout
    // path.  It must not touch mBulkOutResource/mBulkOutBusy: by the time it
    // is dequeued, ArmBulkOut() may already have started a new transfer on
    // that same index.
    //
    return;
  }

  if (EpNum == EP_BULK_IN) {
    if (EpEvent == DWC3_DEPEVT_XFERCOMPLETE) {
      // See the EP_BULK_OUT branch above: gadget.c:2523-2525.
      mBulkInResource = 0;
    }
    if ((EpEvent == DWC3_DEPEVT_XFERCOMPLETE) ||
        (EpEvent == DWC3_DEPEVT_XFERINPROGRESS))
    {
      //
      // How many of the bytes we armed did the host actually take?  Printed
      // only when it is not all of them, so a healthy session costs no screen
      // space - and "device -> host 7 bytes" is the *requested* size, which
      // says nothing about what landed.  This is the datum that separates a
      // device that sent nothing from a host that read nothing.
      //
      Slot = TrbSlot (mBulkInTrb, mBulkInTrbDeq);
      InvalidateDataCacheRange (Slot, DWC3_TRB_SIZE);
      TrbSize   = ((DWC3_TRB *)Slot)->Size;
      Residual  = TrbSize & 0x00FFFFFFu;
      if (Residual > mBulkInArmedLen) {
        Residual = mBulkInArmedLen;
      }
      Transferred = mBulkInArmedLen - Residual;
      //
      // Recorded rather than printed here: the "bulk IN done" line below is
      // the one every log actually contains, and it used to echo the
      // *requested* size.  A partial completion therefore produced two lines
      // that contradicted each other ("ep3 done 42/46" next to "done 46").
      // One truthful line instead.  Nonzero residual on IN is what the kernel
      // calls "incomplete IN transfer" (gadget.c:2307) - it never means the
      // host took less than it should have by accident.
      //
      mBulkInTaken = Transferred;
      TrbInc (&mBulkInTrbDeq);

      mBulkInBusy = FALSE;
      if (mTxCallback != NULL) {
        mTxCallback (1);
      }
    }
    //
    // mBulkInResource is deliberately kept: an XFERINPROGRESS completion
    // releases neither the resource nor DWC3_EP_BUSY for a bulk endpoint
    // (gadget.c:2523 only clears it on XFERCOMPLETE, and gadget.c:2458-2461
    // only clears BUSY for control/isoc), so the next KirinUsbDeviceSend()
    // issues UPDATETRANSFER against it.  See UpdateTransfer().
    //
  }
}

VOID
PollEvents (
  VOID
  )
{
  UINT32 Count;
  UINT32 Event;
  UINTN  Guard;

  if (mPollBusy) {
    return;
  }
  mPollBusy = TRUE;

  for (Guard = 0; Guard < 4096; Guard++) {
    Count = DwRead (DWC3_GEVNTCOUNT0) & DWC3_GEVNTCOUNT_MASK;
    if (Count < 4) {
      break;
    }

    InvalidateDataCacheRange (
      (VOID *)((UINTN)mEventBuffer + mEventLpos),
      4
      );
    Event = *(UINT32 *)((UINTN)mEventBuffer + mEventLpos);

    mEventLpos = (mEventLpos + 4) % DWC3_EVENT_BUFFER_SIZE;
    DwWrite (DWC3_GEVNTCOUNT0, 4);

    if ((Event & 1) == 0) {
      ProcessEndpointEvent (Event);
    } else if (((Event >> 1) & 0x7F) == DWC3_EVENT_TYPE_DEV) {
      HandleDeviceEvent (Event);
    } else {
      //
      // dwc3_process_event_entry() takes the same two branches and then
      // prints "UNKNOWN IRQ type %d" for anything else (the Carkit/I2C gevt
      // types).  Falling through silently is the one shape that cannot be
      // told apart from "the event never arrived", which is exactly the
      // question every one of these logs is asking.  Throttled like the
      // other uncommon prints: first 8, then every 4096th.
      //
      mUnknownIrqCount++;
      if ((mUnknownIrqCount <= 8) || ((mUnknownIrqCount & 0xFFF) == 0)) {
        DEBUG ((
          DEBUG_ERROR,
          "KirinUsbDevice: UNKNOWN IRQ type %u raw 0x%08x #%u\n",
          (Event >> 1) & 0x7F,
          Event,
          mUnknownIrqCount
          ));
      }
    }
  }

  mPollBusy = FALSE;
}

VOID
EFIAPI
PollTimerNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mInitialized) {
    PollEvents ();
  }
}
