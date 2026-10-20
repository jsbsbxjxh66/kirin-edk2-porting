#include "KirinUsbDeviceInternal.h"

//
// ---------------------------------------------------------------------------
// EP0
// ---------------------------------------------------------------------------
//
/**
  (Re-)arm the controller so that the next SETUP packet is collected into
  mCtrlRequest.  Equivalent to dwc3_ep0_out_start().
**/
VOID
Ep0ArmSetup (
  VOID
  )
{
  UINT32   Previous;
  BOOLEAN  PreviousValid;

  //
  // dwc3_ep0_start_trans() (ep0.c:96) returns 0 without touching the
  // hardware while the endpoint still holds a live transfer: a second
  // STARTTRANSFER on an armed EP0 is rejected by the core with
  // DEPEVT_TRANSFER_NO_RESOURCE (status 1) and the retry path then loses
  // the handle to the transfer that is still running.  This is what happens
  // after every bus reset if we blindly re-arm.
  //
  if (mEp0ResourceValid && mEp0Busy) {
    return;
  }

  Previous          = mEp0Resource;
  PreviousValid     = mEp0ResourceValid;
  mEp0Resource      = 0;
  mEp0ResourceValid = FALSE;

  PrepareTrb (
    (DWC3_TRB *)mEp0Trb,
    (UINT64)(UINTN)mCtrlRequest,
    8,
    DWC3_TRBCTL_CONTROL_SETUP,
    TRUE
    );

  if (!EFI_ERROR (StartTransfer (EP0_OUT, mEp0Trb, &mEp0Resource))) {
    mEp0ResourceValid = TRUE;
    mEp0ResourceEp    = EP0_OUT;
    mEp0Busy    = TRUE;
    mEp0Phase   = Ep0PhaseSetup;
    mEp0Stalled = FALSE;
    return;
  }

  //
  // The previous control transfer may still hold the only transfer resource
  // (this happens when a bus reset aborts a data or status stage).  Free it
  // and try once more.  mEp0ResourceEp, not EP0_OUT: a two-stage status stage
  // runs on EP0-IN, so Previous may be EP0-IN's resource index.
  //
  if (PreviousValid) {
    EndTransfer (mEp0ResourceEp, Previous);
  }

  if (EFI_ERROR (StartTransfer (EP0_OUT, mEp0Trb, &mEp0Resource))) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: failed to arm SETUP (prev rsc 0x%x v %d DEPCMD 0x%08x "
      "par0 0x%08x)\n",
      Previous,
      PreviousValid ? 1 : 0,
      DepRead (EP0_OUT, DWC3_DEPCMD),
      DepRead (EP0_OUT, DWC3_DEPCMDPAR0)
      ));
    mEp0Resource      = 0;
    mEp0ResourceValid = FALSE;
    mEp0Busy          = FALSE;
    mEp0Phase         = Ep0PhaseSetup;
    return;
  }

  mEp0ResourceValid = TRUE;
  mEp0ResourceEp = EP0_OUT;
  mEp0Busy    = TRUE;
  mEp0Phase   = Ep0PhaseSetup;
  mEp0Stalled = FALSE;
}

VOID
Ep0Stall (
  VOID
  )
{
  DEBUG ((
    DEBUG_ERROR,
    "KirinUsbDevice: stall setup %02x %02x %04x %04x %04x\n",
    mSetup.RequestType,
    mSetup.Request,
    mSetup.Value,
    mSetup.Index,
    mSetup.Length
    ));

  EpCmd (EP0_OUT, DWC3_DEPCMD_SETSTALL, 0, 0, 0, NULL);
  mEp0Stalled = TRUE;
  mEp0Busy    = FALSE;
  mEp0Phase   = Ep0PhaseSetup;

  //
  // Ep0ArmSetup() below clears mEp0Stalled, so latch the stall decision
  // separately: the setup handler must not start a data stage for a
  // request that was just stalled.
  //
  mEp0StallPending = TRUE;

  Ep0ArmSetup ();
}

/**
  Start the data stage of a three stage control transfer.
**/
VOID
Ep0StartData (
  VOID
  )
{
  UINT8 EpNum;

  EpNum = mEp0ExpectIn ? EP0_IN : EP0_OUT;

  //
  // composite.c:1982 - req->zero = value < w_length.  Armed short of what the
  // host asked for means the short packet already ends the data stage; armed
  // to the full wLength means the only way the host can see the end is a ZLP,
  // which dwc3_ep0_complete_data() queues when the length is also an exact
  // multiple of the max packet size.  Decided here, at queue time, exactly
  // where the composite layer decides it.
  //
  mEp0Zero = (mEp0DataLen < (UINTN)mSetup.Length);

  PrepareTrb (
    (DWC3_TRB *)mEp0Trb,
    (UINT64)(UINTN)mEp0Data,
    (UINT32)mEp0DataLen,
    DWC3_TRBCTL_CONTROL_DATA,
    TRUE
    );

  if (EFI_ERROR (StartTransfer (EpNum, mEp0Trb, &mEp0Resource))) {
    DEBUG ((DEBUG_ERROR, "KirinUsbDevice: EP0 data start failed\n"));
    mEp0Resource      = 0;
    mEp0ResourceValid = FALSE;
    Ep0Stall ();
    return;
  }

  mEp0ResourceValid = TRUE;
  mEp0ResourceEp = EpNum;
  mEp0Busy  = TRUE;
  mEp0Phase = Ep0PhaseData;
}

/**
  Queue the ZLP that ends a control IN data stage whose length is an exact
  multiple of the EP0 max packet size.

  dwc3_ep0_complete_data() (ep0.c) does this after giving the data request
  back: a transfer of N * maxpacket bytes contains neither a short packet nor
  anything else that tells the host the data has ended, so without the ZLP the
  host keeps polling and finally reports "device descriptor request failed".
  The length goes into the TRB as zero and the TRB address is the shared EP0
  control buffer, exactly as the kernel does with dwc->ctrl_req_addr.
**/
VOID
Ep0StartZlp (
  VOID
  )
{
  UINT8 EpNum;

  EpNum = mEp0ExpectIn ? EP0_IN : EP0_OUT;

  //
  // The kernel reaches this only after dwc3_gadget_giveback(), so the data
  // request is already gone from the pending list and the ZLP's own completion
  // finds nothing to process.  Clearing mEp0Zero is that giveback: without it
  // the XFERCOMPLETE for this ZLP would take the same branch again and arm
  // another one, forever.
  //
  mEp0Zero = FALSE;

  PrepareTrb (
    (DWC3_TRB *)mEp0Trb,
    (UINT64)(UINTN)mCtrlRequest,
    0,
    DWC3_TRBCTL_CONTROL_DATA,
    TRUE
    );

  if (EFI_ERROR (StartTransfer (EpNum, mEp0Trb, &mEp0Resource))) {
    DEBUG ((DEBUG_ERROR, "KirinUsbDevice: EP0 ZLP start failed\n"));
    mEp0Resource      = 0;
    mEp0ResourceValid = FALSE;
    Ep0Stall ();
    return;
  }

  mEp0ResourceValid = TRUE;
  mEp0ResourceEp    = EpNum;
  mEp0Busy          = TRUE;
  mEp0Phase         = Ep0PhaseData;
}

/**
  Start the status stage.  EpNum is the endpoint the core reported the
  XferNotReady(CONTROL_STATUS) on, exactly as the kernel driver does.
**/
VOID
Ep0StartStatus (
  IN UINT8  EpNum
  )
{
  UINT32  TrbCtl;

  TrbCtl = mEp0ThreeStage ? DWC3_TRBCTL_CONTROL_STATUS3
                           : DWC3_TRBCTL_CONTROL_STATUS2;

  PrepareTrb (
    (DWC3_TRB *)mEp0Trb,
    (UINT64)(UINTN)mCtrlRequest,
    0,
    TrbCtl,
    TRUE
    );

  if (EFI_ERROR (StartTransfer (EpNum, mEp0Trb, &mEp0Resource))) {
    DEBUG ((DEBUG_ERROR, "KirinUsbDevice: EP0 status start failed\n"));
    mEp0Resource      = 0;
    mEp0ResourceValid = FALSE;
    Ep0Stall ();
    return;
  }

  mEp0ResourceValid = TRUE;
  mEp0ResourceEp = EpNum;
  mEp0Busy  = TRUE;
  mEp0Phase = Ep0PhaseStatus;
}

STATIC
VOID
HandleGetDescriptor (
  VOID
  )
{
  UINT16  DescType;
  UINT16  DescIndex;
  UINT8   *Desc;
  UINTN   DescLen;

  DescType  = (UINT16)(mSetup.Value >> 8);
  DescIndex = (UINT16)(mSetup.Value & 0xFF);
  Desc      = NULL;
  DescLen   = 0;

  switch (DescType) {
    case USB_DESC_TYPE_DEVICE:
      Desc = (UINT8 *)mDeviceDescriptor;
      if (Desc != NULL) {
        DescLen = sizeof (USB_DEVICE_DESCRIPTOR);
      }
      break;

    case USB_DESC_TYPE_CONFIG:
      if ((mConfigDescriptors != NULL) && (DescIndex < mConfigDescriptorCount)) {
        Desc = (UINT8 *)mConfigDescriptors[DescIndex];
        if (Desc != NULL) {
          DescLen = ((USB_CONFIG_DESCRIPTOR *)Desc)->TotalLength;
        }
      }
      break;

    case USB_DESC_TYPE_STRING:
      if (DescIndex == 0) {
        mStringZero[0] = 4;
        mStringZero[1] = USB_DESC_TYPE_STRING;
        mStringZero[2] = 0x09;   // en-US
        mStringZero[3] = 0x04;
        Desc           = mStringZero;
        DescLen        = sizeof (mStringZero);
      } else if (DescIndex == KIRIN_SERIAL_STRING_INDEX) {
        //
        // EP0 is the only path this gadget has proven works, so every
        // fastboot command - which always starts by reading the serial -
        // gets one line describing the bulk side:
        //   dalep  0x0000000f = EP0 halves plus both bulk halves enabled
        //   dsts   bits 2:0 = link speed (0 high, 1 full, 2 low), bits 21:18
        //          link state, bit 22 controller halted, bit 23 core idle
        //   np     DCFG.NUMP, the ACK packet count used for bulk OUT
        //   rx     the 64 KB OUT buffer is currently armed
        //   pkt/b  packets and bytes actually received since boot - the
        //          running total, because the one-off "rx N pkt" line scrolls
        //          off a screen this small before anyone can read it
        //
        DEBUG ((
          DEBUG_ERROR,
          "KirinUsbDevice: state dalep 0x%08x dsts 0x%08x np %u rx %u pkt %u b %u\n",
          DwRead (DWC3_DALEPENA),
          DwRead (DWC3_DSTS),
          DWC3_DCFG_NUMP_GET (DwRead (DWC3_DCFG)),
          mBulkOutBusy,
          mBulkOutPackets,
          mBulkOutBytes
          ));
        //
        // The only string this gadget actually has.  Everything else is
        // answered with a 2-byte empty descriptor so an index the caller
        // left at 0 does not stall the bus.
        //
        Desc    = mSerialString;
        DescLen = mSerialString[0];
      } else {
        mStringEmpty[0] = 2;
        mStringEmpty[1] = USB_DESC_TYPE_STRING;
        Desc            = mStringEmpty;
        DescLen         = sizeof (mStringEmpty);
      }
      break;

    case 6:  // DEVICE QUALIFIER
      if (mDeviceDescriptor == NULL) {
        break;
      }
      ZeroMem (mQualifierBuf, sizeof (mQualifierBuf));
      mQualifierBuf[0] = 10;
      mQualifierBuf[1] = 6;
      mQualifierBuf[2] = (UINT8)(mDeviceDescriptor->BcdUSB & 0xFF);
      mQualifierBuf[3] = (UINT8)(mDeviceDescriptor->BcdUSB >> 8);
      mQualifierBuf[4] = mDeviceDescriptor->DeviceClass;
      mQualifierBuf[5] = mDeviceDescriptor->DeviceSubClass;
      mQualifierBuf[6] = mDeviceDescriptor->DeviceProtocol;
      mQualifierBuf[7] = mDeviceDescriptor->MaxPacketSize0;
      mQualifierBuf[8] = mDeviceDescriptor->NumConfigurations;
      Desc             = mQualifierBuf;
      DescLen          = sizeof (mQualifierBuf);
      break;

    case 7:  // OTHER SPEED CONFIGURATION
      if ((mConfigDescriptors != NULL) && (DescIndex < mConfigDescriptorCount)) {
        DescLen = 0;
        Desc    = (UINT8 *)mConfigDescriptors[DescIndex];
        if (Desc != NULL) {
          DescLen = ((USB_CONFIG_DESCRIPTOR *)Desc)->TotalLength;
        }
        if ((Desc == NULL) || (DescLen > EP0_DATA_MAX)) {
          Desc = NULL;
          break;
        }
        CopyMem (mEp0DataBuf, Desc, DescLen);
        mEp0DataBuf[1] = 7;
        Desc            = mEp0DataBuf;
      }
      break;

    default:
      break;
  }

  if (Desc == NULL) {
    //
    // This is the "configuration descriptor invalid" failure: if the type is
    // 2 the host sees a stall right where Windows reports an unknown device,
    // and the bus gets reset.  It used to be a DEBUG_WARN, i.e. invisible on
    // a screen that only prints DEBUG_ERROR.
    //
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: no descriptor type %u idx %u (cfgs %u)\n",
      (UINT32)DescType,
      (UINT32)DescIndex,
      (UINT32)mConfigDescriptorCount
      ));
    Ep0Stall ();
    return;
  }

  if (DescLen > mSetup.Length) {
    DescLen = mSetup.Length;
  }

  if (Desc != mEp0DataBuf) {
    if (DescLen > EP0_DATA_MAX) {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: descriptor type %u len %u exceeds EP0 buffer\n",
        (UINT32)DescType,
        (UINT32)DescLen
        ));
      Ep0Stall ();
      return;
    }
    CopyMem (mEp0DataBuf, Desc, DescLen);
  }

  mEp0Data    = mEp0DataBuf;
  mEp0DataLen = DescLen;
  WriteBackDataCacheRange (mEp0DataBuf, DescLen);
}

VOID
HandleStandardSetup (
  VOID
  )
{
  UINT8  Recipient;

  mEp0Data    = NULL;
  mEp0DataLen = 0;
  Recipient   = mSetup.RequestType & 0x1F;

  switch (mSetup.Request) {
    case USB_REQ_GET_STATUS:
      mStatusBuf[0] = 0;
      mStatusBuf[1] = 0;
      if (Recipient == 0) {
        // SELF POWERED
        mStatusBuf[0] = 1;
      }
      mEp0Data    = mStatusBuf;
      mEp0DataLen = sizeof (mStatusBuf);
      break;

    case USB_REQ_CLEAR_FEATURE:
    case USB_REQ_SET_FEATURE:
      // Remote wakeup / endpoint halt are not implemented; accept and ignore.
      mEp0Data    = NULL;
      mEp0DataLen = 0;
      break;

    case USB_REQ_SET_ADDRESS:
      if (mSetup.Value > 127) {
        Ep0Stall ();
        return;
      }
      if (mGadgetState == UsbStateConfigured) {
        Ep0Stall ();
        return;
      }
      DwWrite (
        DWC3_DCFG,
        (DwRead (DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK)
        | DWC3_DCFG_DEVADDR (mSetup.Value)
        );
      mGadgetState = (mSetup.Value != 0) ? UsbStateAddress : UsbStateDefault;
      DEBUG ((DEBUG_ERROR,
        "KirinUsbDevice: SET_ADDRESS %d\n",
        mSetup.Value
        ));
      break;

    case USB_REQ_GET_DESCRIPTOR:
      HandleGetDescriptor ();
      break;

    case USB_REQ_SET_DESCRIPTOR:
      Ep0Stall ();
      return;

    case USB_REQ_GET_CONFIG:
      mMiscBuf[0] = (mGadgetState == UsbStateConfigured) ? 1 : 0;
      mEp0Data    = mMiscBuf;
      mEp0DataLen = sizeof (mMiscBuf);
      break;

    case USB_REQ_SET_CONFIG:
      if (mGadgetState == UsbStateDefault) {
        Ep0Stall ();
        return;
      }
      if ((mSetup.Value != 0) && (mSetup.Value != 1)) {
        Ep0Stall ();
        return;
      }

      if (mSetup.Value != 0) {
        if (!mConfigured) {
          if (EFI_ERROR (EnableEndpoint (
                           EP_BULK_OUT,
                           2,
                           MAX_PACKET_BULK,
                           FALSE
                           ))
              || EFI_ERROR (EnableEndpoint (
                              EP_BULK_IN,
                              2,
                              MAX_PACKET_BULK,
                              TRUE
                              )))
          {
            //
            // Deliberately NOT stalling.  The realistic failure here is a
            // bulk endpoint whose DEPCMD engine is wedged from the previous
            // session ("bulk IN arm failed"): EpCmd() now returns instantly
            // instead of blocking, but EnableEndpoint() still reports an
            // error.  Stalling SET_CONFIGURATION makes Windows declare the
            // whole device unknown on the next replug - i.e. hot-plug looks
            // broken when only bulk is.  Let EP0 finish the enumeration;
            // ArmBulkOut()/KirinUsbDeviceSend() already print their own
            // failures.
            //
            DEBUG ((
              DEBUG_ERROR,
              "KirinUsbDevice: bulk enable failed, continuing\n"
              ));
          }
          mConfigured = TRUE;
        }
        mGadgetState = UsbStateConfigured;
        ArmBulkOut ();
      } else {
        //
        // reset_config() -> usb_ep_disable() (composite.c:735) clears the
        // DALEPENA bit of every function endpoint.  Clearing only
        // mConfigured leaves ep2/ep3 enabled, so a later SET_CONFIGURATION 1
        // would re-run DEPSETEPCONFIG ACTION_INIT on an enabled endpoint -
        // the command the databook forbids - wedging CMDACT and killing every
        // bulk arm from the first command onward.  Same bookkeeping and same
        // no-ENDTRANSFER rule as the bus reset handler above: an ENDTRANSFER
        // against a transfer the core already dropped is what wedges CMDACT.
        //
        mConfigured      = FALSE;
        mGadgetState     = UsbStateAddress;
        mBulkInBusy      = FALSE;
        mBulkOutBusy     = FALSE;
        mBulkInResource  = 0;
        mBulkOutResource = 0;
        DwWrite (
          DWC3_DALEPENA,
          DwRead (DWC3_DALEPENA) &
            ~((1u << EP_BULK_OUT) | (1u << EP_BULK_IN))
          );
      }

      //
      // DALEPENA in the same line so it survives on a 6-line screen: 0x0000000f
      // means EP0 (bits 0,1) plus both bulk halves (bits 2,3) really made it
      // into the register, 0x00000003 means the bulk endpoints never enabled
      // and every host bulk transfer will NAK forever.
      //
      DEBUG ((DEBUG_ERROR,
        "KirinUsbDevice: SET_CONFIGURATION %d DALEPENA 0x%08x\n",
        mSetup.Value,
        DwRead (DWC3_DALEPENA)
        ));
      break;

    case USB_REQ_GET_INTERFACE:
      mMiscBuf[0] = 0;
      mEp0Data    = mMiscBuf;
      mEp0DataLen = sizeof (mMiscBuf);
      break;

    case USB_REQ_SET_INTERFACE:
      // Single interface, no alternate settings.
      mEp0Data    = NULL;
      mEp0DataLen = 0;
      break;

    case USB_REQ_SYNCH_FRAME:
      Ep0Stall ();
      return;

    default:
      Ep0Stall ();
      return;
  }
}
