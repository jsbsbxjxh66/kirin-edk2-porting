#include "KirinUsbDeviceInternal.h"

//
// ---------------------------------------------------------------------------
// Register access helpers
// ---------------------------------------------------------------------------
//
UINT32
DwRead (
  IN UINT32  Offset
  )
{
  return MmioRead32 ((UINTN)(DWC3_BASE + Offset));
}

VOID
DwWrite (
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 ((UINTN)(DWC3_BASE + Offset), Value);
}

UINT32
DepRead (
  IN UINT8   EpNum,
  IN UINT32  Offset
  )
{
  return MmioRead32 ((UINTN)(DWC3_BASE + DWC3_DEP_BASE (EpNum) + Offset));
}

VOID
DepWrite (
  IN UINT8   EpNum,
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 ((UINTN)(DWC3_BASE + DWC3_DEP_BASE (EpNum) + Offset), Value);
}

UINT64
TrbDma (
  IN UINT8   *Trb
  )
{
  return (UINT64)(UINTN)Trb;
}

//
// Pointer to TRB slot Index inside a ring allocated as one page.
//
UINT8 *
TrbSlot (
  IN UINT8   *Ring,
  IN UINT8   Index
  )
{
  return Ring + ((UINTN)Index * DWC3_TRB_SIZE);
}

//
// dwc3_ep_inc_trb() (gadget.c:165-173): the index may never point at the
// Link TRB, so after incrementing, 255 wraps straight back to 0.  Slot 255 is
// therefore never used for data.
//
VOID
TrbInc (
  IN OUT UINT8  *Index
  )
{
  (*Index)++;
  if (*Index == (TRB_RING_NUM - 1)) {
    *Index = 0;
  }
}

//
// __dwc3_gadget_ep_enable() (gadget.c:689-706): zero the ring, then install a
// Link TRB in the last slot pointing back at slot 0 with HWO set (the comment
// there notes the HWO bit on a Link TRB is never reset).  Called whenever a
// bulk endpoint is (re)configured, so a bus reset starts from a clean ring
// with the cursors back at 0 - which is also what makes STARTTRANSFER the
// first command again after SET_CONFIGURATION.
//
VOID
InitTrbRing (
  IN UINT8   *Ring,
  OUT UINT8  *Enq,
  OUT UINT8  *Deq
  )
{
  DWC3_TRB  *Link;
  UINT64    Base;

  ZeroMem (Ring, (TRB_RING_NUM * DWC3_TRB_SIZE));

  Base = TrbDma (Ring);
  Link = (DWC3_TRB *)TrbSlot (Ring, (UINT8)(TRB_RING_NUM - 1));
  Link->Bpl = (UINT32)Base;
  Link->Bph = (UINT32)RShiftU64 (Base, 32);
  Link->Size = 0;
  Link->Ctl  = DWC3_TRBCTL_LINK_TRB | DWC3_TRB_CTRL_HWO;
  WriteBackDataCacheRange (Ring, TRB_RING_NUM * DWC3_TRB_SIZE);

  *Enq = 0;
  *Deq = 0;
}
