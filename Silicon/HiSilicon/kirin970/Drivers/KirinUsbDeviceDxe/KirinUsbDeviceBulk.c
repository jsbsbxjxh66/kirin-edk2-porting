#include "KirinUsbDeviceInternal.h"

//
// ---------------------------------------------------------------------------
// Bulk endpoints
// ---------------------------------------------------------------------------
//
VOID
ArmBulkOut (
  VOID
  )
{
  UINT8        *Slot;
  UINT8        SlotIndex;
  UINTN        ArmedLen;
  UINT32       Reg;
  EFI_STATUS   Status;

  if (!mConfigured) {
    return;
  }

  if (mBulkOutBusy) {
    return;
  }

  //
  // The final arm of a download has to be exactly what the host still owes.
  // A bulk OUT transfer terminates on a full buffer, a short packet or a
  // ZLP only - and a flash payload is always a whole number of 512-byte
  // maxpackets, so it supplies none of them.  With a fixed 64 KB arm the
  // last 36864 bytes of a 4624384-byte download would sit in the buffer with
  // no completion event, the app would stop one packet short of its total,
  // and the host would hang at 100% waiting for an OKAY that never goes out.
  //
  ArmedLen = RX_CHUNK_SIZE;
  if ((mBulkOutRemaining > 0) && (mBulkOutRemaining < ArmedLen)) {
    ArmedLen = mBulkOutRemaining;
  }

  //
  // dwc3_prepare_one_trb_linear() (gadget.c) rounds the length of every
  // non-EP0 OUT TRB up to a maxpacket multiple before the core sees it
  // ("add for ep out buffer aligned MaxPacketSize"), substituting one
  // maxpacket when the rounded value would be zero.  A length that is not a
  // whole number of maxpackets makes the core's OUT buffer accounting run
  // past the end of the buffer it was given.  The reservation recorded in
  // mBulkOutArmedLen has to move with it: the residual read back from the TRB
  // is measured against that number, so leaving it unrounded would clamp a
  // short final packet straight to zero and drop it.
  //
  ArmedLen = (ArmedLen + MAX_PACKET_BULK - 1) & ~((UINTN)MAX_PACKET_BULK - 1);
  if (ArmedLen == 0) {
    ArmedLen = MAX_PACKET_BULK;
  }

  //
  // One TRB per ring slot, cursor advanced before the command - the same
  // order dwc3_prepare_trbs()/dwc3_ep_inc_enq() use (gadget.c:1100-1120).
  // The core learns where to look from the cursor it already holds, so the
  // address only matters for the very first STARTTRANSFER.
  //
  SlotIndex = mBulkOutTrbEnq;
  Slot = TrbSlot (mBulkOutTrb, SlotIndex);
  PrepareTrb (
    (DWC3_TRB *)Slot,
    (UINT64)(UINTN)mRxDmaBuf,
    (UINT32)ArmedLen,
    DWC3_TRBCTL_NORMAL,
    TRUE
    );
  TrbInc (&mBulkOutTrbEnq);

  //
  // STARTTRANSFER vs UPDATETRANSFER is the kernel's own decision
  // (__dwc3_gadget_kick_transfer(), gadget.c:1129-1147): start when the
  // endpoint owns no resource, update otherwise.  ArmTransfer() also repairs
  // the bookkeeping in both directions so ENDTRANSFER never has to be reached
  // for - see why there: it is what wedged CMDACT and produced "bulk IN arm
  // failed" on ep3.
  //
  Status = ArmTransfer (
             EP_BULK_OUT,
             Slot,
             mBulkOutResource,
             &mBulkOutResource
             );

  if (EFI_ERROR (Status)) {
    //
    // The command never took, so the slot was not consumed: hand the cursor
    // back so enqueue and dequeue stay aligned with the core.  The resource
    // index is left alone - if the core still owns one, the next attempt
    // takes the UPDATE branch again instead of blind-STARTing on top of it.
    //
    mBulkOutTrbEnq = SlotIndex;
    Reg = DepRead (EP_BULK_OUT, DWC3_DEPCMD);
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: bulk OUT arm failed (%a rsc %u DEPCMD 0x%08x st %u act %u)\n",
      (mEpCmdFailReason != NULL) ? mEpCmdFailReason : "no-epcmd",
      mBulkOutResource,
      Reg,
      DWC3_DEPCMD_STATUS (Reg),
      ((Reg & DWC3_DEPCMD_CMDACT) != 0) ? 1 : 0
      ));
    return;
  }

  //
  // First arm, then one line every 16 slots, then always a short arm: a full
  // 4.5 MB download is 71 arms and printing all of them scrolls the screen
  // past the tail we need to read.  A climbing slot number with no "rx N pkt"
  // between the lines is still enough to spot an arm/complete loop, and the
  // short arm is the one the whole fix exists for - seeing it means the count
  // reached the driver, seeing it absent means SetRxLength never ran.
  //
  if ((SlotIndex == 0) || ((SlotIndex & 0x0F) == 0) || (ArmedLen != RX_CHUNK_SIZE)) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbDevice: bulk OUT armed rsc %u slot %u buf 0x%lx len %u\n",
      mBulkOutResource,
      (UINT32)SlotIndex,
      (UINT64)(UINTN)mRxDmaBuf,
      (UINT32)ArmedLen
      ));
  }

  mBulkOutBusy     = TRUE;
  mBulkOutArmedLen = ArmedLen;
}

VOID
HandleBulkOutComplete (
  VOID
  )
{
  UINT32  TrbSize;
  UINTN   Residual;
  UINTN   Actual;
  UINT8   *Packet;
  UINT8   *Slot;

  //
  // mBulkOutResource is deliberately NOT cleared here: an XFERINPROGRESS
  // completion gives neither the resource back (gadget.c:2523 clears it only
  // on XFERCOMPLETE, which bulk never receives) nor DWC3_EP_BUSY
  // (gadget.c:2458-2461), so the next arm is an UPDATETRANSFER on the same
  // resource - see UpdateTransfer().
  //
  mBulkOutBusy     = FALSE;

  //
  // The write-back belongs to the slot the arm used, i.e. the dequeue cursor.
  // __dwc3_cleanup_done_trbs() captures trb = &trb_pool[trb_dequeue] first and
  // only then calls dwc3_ep_inc_deq() (gadget.c:2258, 2366), so read here and
  // advance afterwards.
  //
  Slot = TrbSlot (mBulkOutTrb, mBulkOutTrbDeq);
  InvalidateDataCacheRange (Slot, DWC3_TRB_SIZE);
  TrbSize = ((DWC3_TRB *)Slot)->Size;
  TrbInc (&mBulkOutTrbDeq);

  Residual = TrbSize & 0x00FFFFFFu;
  if (Residual > mBulkOutArmedLen) {
    Residual = mBulkOutArmedLen;
  }
  Actual = mBulkOutArmedLen - Residual;

  if (Actual > 0) {
    InvalidateDataCacheRange (mRxDmaBuf, Actual);
    mBulkOutPackets++;
    mBulkOutBytes += (UINT32)Actual;

    //
    // What the host still owes after this packet.  Once it reaches zero the
    // next arm goes back to RX_CHUNK_SIZE, which is what a <= 64 byte
    // command needs - the short packet ends those on its own.
    //
    if (mBulkOutRemaining >= Actual) {
      mBulkOutRemaining -= Actual;
    } else {
      mBulkOutRemaining = 0;
    }

    //
    // First packet always, then one line every 16 packets, then *every*
    // packet from #64 on.  A 4.5 MB download is 78 packets: under the plain
    // every-16 rule packets 65..78 print nothing at all, so a device that
    // stalls at 80% and a device that finishes both leave "slot 64" as the
    // last line on screen.  From #64 on the tail is only ~15 lines, which is
    // exactly the part we need to read, and "rx 78 pkt 4624384 bytes" with no
    // "device -> host" after it is a byte-count mismatch rather than a stall.
    //
    if ((mBulkOutPackets == 1) || ((mBulkOutPackets & 0x0F) == 0) ||
        (mBulkOutPackets >= 64))
    {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: rx %u pkt %u bytes\n",
        mBulkOutPackets,
        mBulkOutBytes
        ));
    }

    Packet = AllocateCopyPool (Actual, mRxDmaBuf);
    if (Packet != NULL) {
      if (mRxCallback != NULL) {
        mRxCallback (Actual, Packet);
      } else {
        FreePool (Packet);
      }
    } else {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: rx alloc failed, dropped %u bytes\n",
        (UINT32)Actual
        ));
    }
  } else {
    //
    // The event fired but the TRB still owns every byte: distinguishable
    // from "no event at all", which is the case we are hunting.  Throttled
    // like the event line - a stream of these is itself the failure, and a
    // flood would hide the count that says how many there were.
    //
    mBulkNoDataCount++;
    if ((mBulkNoDataCount == 1) || ((mBulkNoDataCount & 0x3F) == 0)) {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbDevice: rx event, no data #%u (residual %u of %u)\n",
        mBulkNoDataCount,
        (UINT32)Residual,
        (UINT32)mBulkOutArmedLen
        ));
    }
  }

  ArmBulkOut ();
}
