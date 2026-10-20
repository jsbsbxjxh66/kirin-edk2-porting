/** @file

  USB mass-storage boot menu entry for the Kirin 970 DWC3 gadget.

  Exposes the UFS LUN0 whole-disk BlockIo (the GPT the fastboot entries
  flash) to the attached host as a single BOT (Bulk-Only Transport, class
  08/06/50) SCSI target, i.e. read/write access to the phone's internal
  storage from the PC.

  One interface, one IN and one OUT bulk endpoint - the same pair every other
  function on this port uses - so switching functions only needs a fresh
  descriptor set plus the disconnect/reconnect in KirinUsbDeviceStart().

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Protocol/UsbDevice.h>
#include <Protocol/KirinUsbFunction.h>
#include <Protocol/BlockIo.h>

#include <IndustryStandard/Usb.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

//
// ---------------------------------------------------------------------------
// Bulk-Only Transport
// ---------------------------------------------------------------------------
//
#define MS_CBW_SIGNATURE      0x43425355U   // "USBC"
#define MS_CSW_SIGNATURE      0x53425355U   // "USBS"
#define MS_CBW_SIZE           31
#define MS_CSW_SIZE           13
#define MS_FLAG_IN            0x80
#define MS_STATUS_PASS        0
#define MS_STATUS_FAIL        1
#define MS_STATUS_PHASE       2

#pragma pack(1)
typedef struct {
  UINT32  Signature;
  UINT32  Tag;
  UINT32  DataTransferLength;
  UINT8   Flags;
  UINT8   Lun;
  UINT8   CbLength;
  UINT8   Cb[16];
} MS_CBW;

typedef struct {
  UINT32  Signature;
  UINT32  Tag;
  UINT32  Residue;
  UINT8   Status;
} MS_CSW;
#pragma pack()

//
// ---------------------------------------------------------------------------
// SCSI opcodes (only the ones a mainstream host actually sends)
// ---------------------------------------------------------------------------
//
#define MS_OP_TEST_UNIT_READY   0x00
#define MS_OP_REQUEST_SENSE     0x03
#define MS_OP_INQUIRY           0x12
#define MS_OP_MODE_SEN6         0x1a
#define MS_OP_START_STOP_UNIT   0x1b
#define MS_OP_MEDIA_REMOVAL     0x1e
#define MS_OP_READ10            0x28
#define MS_OP_WRITE10           0x2a
#define MS_OP_VERIFY10          0x2f
#define MS_OP_SYNC_CACHE        0x35
#define MS_OP_READ_CAPACITY     0x25
#define MS_OP_READ_FMT_CAPAC    0x23
#define MS_OP_MODE_SEN10        0x5a
#define MS_OP_READ_CAP16        0x9e

#define MS_SENSE_NONE           0x00
#define MS_SENSE_NOT_READY      0x02
#define MS_SENSE_MEDIUM_ERROR   0x03
#define MS_SENSE_HARDWARE       0x04
#define MS_SENSE_ILLEGAL        0x05
#define MS_SENSE_DATA_PROTECT   0x07

//
// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
//
#define MS_TX_ENDPOINT         1     // matches FastbootTransportUsb
#define MS_IO_CHUNK            (1U * 1024U * 1024U)
#define MS_TX_CHUNK            (64U * 1024U)
#define MS_MAX_WRITE_BYTES     (64U * 1024U * 1024U)
#define MS_CBW_RING            32
#define MS_POLL_US             5000

//
// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
//
typedef enum {
  MsExpectCbw = 0,
  MsDataOut,
} MS_STATE;

typedef struct {
  VOID    *Buffer;
  UINTN   Size;
} MS_PACKET;

STATIC USB_DEVICE_PROTOCOL          *mUsbDevice;
STATIC KIRIN_USB_FUNCTION_PROTOCOL  *mFunction;
STATIC EFI_BLOCK_IO_PROTOCOL        *mBlockIo;
STATIC UINT64                        mLastLba;
STATIC UINT32                        mBlockSize;
STATIC BOOLEAN                       mReadOnly;

STATIC MS_STATE                      mState;
STATIC UINT32                        mTag;
STATIC UINTN                         mDataExpected;
STATIC UINTN                         mDataReceived;
STATIC UINT8                         *mDataBuf;
STATIC UINTN                         mDataBufPages;
STATIC BOOLEAN                       mDiscardOut;
STATIC volatile BOOLEAN              mWriteReady;
STATIC UINT64                        mWriteLba;
STATIC UINTN                         mWriteSize;
STATIC UINT8                         *mIoBuf;

STATIC UINT8                         mSenseKey;
STATIC UINT8                         mSenseAsc;
STATIC UINT8                         mSenseAscq;

//
// CBWs are small and rare (one per command), so they go through a queue that
// only the application pops.  Bulk OUT payload never does: it is copied
// straight into the target buffer from the callback, which runs at
// TPL_CALLBACK and must not allocate a queue entry per 64 KiB.
//
STATIC MS_PACKET                     *mCbwRing[MS_CBW_RING];
STATIC volatile UINTN                 mCbwHead;
STATIC volatile UINTN                 mCbwTail;

STATIC EFI_INPUT_KEY                  mKey;

//
// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------
//
#pragma pack(1)
typedef struct {
  USB_CONFIG_DESCRIPTOR       ConfigDescriptor;
  USB_INTERFACE_DESCRIPTOR    InterfaceDescriptor;
  USB_ENDPOINT_DESCRIPTOR     EndpointIn;
  USB_ENDPOINT_DESCRIPTOR     EndpointOut;
} MS_CONFIG_RESPONSE;
#pragma pack()

STATIC USB_DEVICE_DESCRIPTOR  mDeviceDescriptor = {
  sizeof (USB_DEVICE_DESCRIPTOR),   // Length
  USB_DESC_TYPE_DEVICE,             // DescriptorType
  0x0200,                           // BcdUSB
  0x00,                             // DeviceClass  (defined at interface)
  0x00,                             // DeviceSubClass
  0x00,                             // DeviceProtocol
  64,                               // MaxPacketSize0
  0x18d1,                           // IdVendor
  0x4ee8,                           // IdProduct
  0x0100,                           // BcdDevice
  0,                                // StrManufacturer
  0,                                // StrProduct
  0,                                // StrSerialNumber
  1                                 // NumConfigurations
};

STATIC MS_CONFIG_RESPONSE mConfigResponse = {
  {
    sizeof (USB_CONFIG_DESCRIPTOR),               // Length
    USB_DESC_TYPE_CONFIG,                         // DescriptorType
    sizeof (MS_CONFIG_RESPONSE),                  // TotalLength
    1,                                            // NumInterfaces
    1,                                            // ConfigurationValue
    0,                                            // Configuration
    BIT7,                                         // Attributes (bus powered)
    250                                           // MaxPower (500 mA)
  },
  {
    sizeof (USB_INTERFACE_DESCRIPTOR),            // Length
    USB_DESC_TYPE_INTERFACE,                      // DescriptorType
    0,                                            // InterfaceNumber
    0,                                            // AlternateSetting
    2,                                            // NumEndpoints
    0x08,                                         // InterfaceClass  - Mass Storage
    0x06,                                         // InterfaceSubClass - SCSI
    0x50,                                         // InterfaceProtocol - BBB
    0                                             // Interface
  },
  {
    sizeof (USB_ENDPOINT_DESCRIPTOR),             // Length
    USB_DESC_TYPE_ENDPOINT,                       // DescriptorType
    1 | BIT7,                                     // EndpointAddress (IN)
    0x2,                                          // Attributes (bulk)
    512,                                          // MaxPacketSize
    0                                             // Interval
  },
  {
    sizeof (USB_ENDPOINT_DESCRIPTOR),             // Length
    USB_DESC_TYPE_ENDPOINT,                       // DescriptorType
    1,                                            // EndpointAddress (OUT)
    0x2,                                          // Attributes (bulk)
    512,                                          // MaxPacketSize
    0                                             // Interval
  }
};

STATIC VOID  *mConfigDescriptors[] = {
  &mConfigResponse,
  NULL
};

//
// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
//
STATIC
UINT32
GetBe32 (
  IN CONST UINT8  *Buf
  )
{
  return ((UINT32)Buf[0] << 24) | ((UINT32)Buf[1] << 16) |
         ((UINT32)Buf[2] << 8)  | (UINT32)Buf[3];
}

STATIC
UINT16
GetBe16 (
  IN CONST UINT8  *Buf
  )
{
  return (UINT16)(((UINT16)Buf[0] << 8) | (UINT16)Buf[1]);
}

STATIC
VOID
SetBe32 (
  OUT UINT8   *Buf,
  IN  UINT32  Value
  )
{
  Buf[0] = (UINT8)(Value >> 24);
  Buf[1] = (UINT8)(Value >> 16);
  Buf[2] = (UINT8)(Value >> 8);
  Buf[3] = (UINT8)Value;
}

STATIC
VOID
SetSense (
  IN UINT8  Key,
  IN UINT8  Asc,
  IN UINT8  Ascq
  )
{
  mSenseKey  = Key;
  mSenseAsc  = Asc;
  mSenseAscq = Ascq;
}

//
// Queue a CBW for the application loop.  Only ever called with state ==
// MsExpectCbw, i.e. one entry deep in practice.
//
STATIC
VOID
PushCbw (
  IN VOID   *Buffer,
  IN UINTN  Size
  )
{
  MS_PACKET  *Packet;
  UINTN      Next;

  Packet = AllocatePool (sizeof (MS_PACKET));
  if (Packet == NULL) {
    FreePool (Buffer);
    return;
  }

  Packet->Buffer = Buffer;
  Packet->Size   = Size;

  MemoryFence ();
  Next = (mCbwHead + 1) % MS_CBW_RING;
  if (Next == mCbwTail) {
    //
    // Application fell behind; dropping a CBW desynchronises the BOT
    // sequence, so this really should not happen - the queue only ever
    // holds one entry per completed command.
    //
    DEBUG ((DEBUG_ERROR, "KirinUsbMassStorage: CBW queue full, dropped\n"));
    FreePool (Packet);
    FreePool (Buffer);
    return;
  }

  mCbwRing[mCbwHead] = Packet;
  MemoryFence ();
  mCbwHead = Next;
}

STATIC
BOOLEAN
PopCbw (
  OUT VOID   **Buffer,
  OUT UINTN  *Size
  )
{
  MS_PACKET  *Packet;

  if (mCbwTail == mCbwHead) {
    return FALSE;
  }

  Packet = mCbwRing[mCbwTail];
  MemoryFence ();
  mCbwTail = (mCbwTail + 1) % MS_CBW_RING;

  if (Packet == NULL) {
    return FALSE;
  }

  *Buffer = Packet->Buffer;
  *Size   = Packet->Size;
  FreePool (Packet);
  return TRUE;
}

//
// A CBW is exactly MS_CBW_SIZE bytes and starts with "USBC".  Recognising it
// by content rather than by state is what makes the BOT sequence recoverable:
// if the cable is pulled in the middle of a WRITE(10) the device is still in
// MsDataOut, and without this the host's first command after reconnecting
// would be swallowed as write payload, never queued, and never answered - the
// host then hangs and hot-plug looks broken.
//
STATIC
BOOLEAN
LooksLikeCbw (
  IN CONST VOID  *Buffer,
  IN UINTN       Size
  )
{
  if ((Buffer == NULL) || (Size != MS_CBW_SIZE)) {
    return FALSE;
  }
  return ReadUnaligned32 ((CONST UINT32 *)Buffer) == MS_CBW_SIGNATURE;
}

//
// Throw away an interrupted data-out phase: no CSW can reach the host that
// was aborted, so the partial buffer must not be written to disk either.
//
STATIC
VOID
AbandonDataOut (
  VOID
  )
{
  if (mDataBuf != NULL) {
    FreePages (mDataBuf, mDataBufPages);
    mDataBuf      = NULL;
    mDataBufPages = 0;
  }
  mDataReceived = 0;
  mDataExpected = 0;
  mDiscardOut   = FALSE;
  mWriteReady   = FALSE;
  mState        = MsExpectCbw;
}

//
// Called from the gadget's bulk OUT completion, i.e. at TPL_CALLBACK, so it
// only ever copies into an already allocated buffer and touches two
// single-word indices.  No allocation, no waiting, no list surgery.
//
STATIC
VOID
DataReceived (
  IN UINTN  Size,
  IN VOID   *Buffer
  )
{
  if (Buffer == NULL) {
    return;
  }

  if ((mState != MsDataOut) || LooksLikeCbw (Buffer, Size)) {
    PushCbw (Buffer, Size);
    return;
  }

  //
  // Data-out phase.  Anything that is not a whole number of blocks cannot
  // come from a legal WRITE(10); keep counting so the BOT sequence stays in
  // step, but do not hand it to BlockIo.
  //
  if ((mDataBuf != NULL) &&
      (mDataReceived < mDataExpected) &&
      (Size <= (mDataExpected - mDataReceived)) &&
      (mBlockSize != 0) &&
      ((Size % mBlockSize) == 0) &&
      ((mDataReceived % mBlockSize) == 0))
  {
    CopyMem (mDataBuf + mDataReceived, Buffer, Size);
  } else {
    if (mDataBuf != NULL) {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbMassStorage: %u unexpected bytes in data-out phase\n",
        (UINT32)Size
        ));
    }
    mDiscardOut = TRUE;
  }

  mDataReceived += Size;
  if (mDataReceived >= mDataExpected) {
    MemoryFence ();
    mWriteReady = TRUE;
  }

  FreePool (Buffer);
}

//
// ---------------------------------------------------------------------------
// BOT responses
// ---------------------------------------------------------------------------
//

//
// Send bytes to the host.  One DEPSTARTTRANSFER/TRB per call, so the payload
// is chunked: a single TRB is only guaranteed to be good for 64 KiB.
//
STATIC
UINTN
SendData (
  IN CONST VOID  *Buffer,
  IN UINTN       Size
  )
{
  UINTN  Offset;
  UINTN  Chunk;

  Offset = 0;
  while (Offset < Size) {
    Chunk = Size - Offset;
    if (Chunk > MS_TX_CHUNK) {
      Chunk = MS_TX_CHUNK;
    }
    if (EFI_ERROR (
          mUsbDevice->Send (MS_TX_ENDPOINT, Chunk, (CONST UINT8 *)Buffer + Offset)
          ))
    {
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbMassStorage: Send failed after %u bytes\n",
        (UINT32)Offset
        ));
      break;
    }
    Offset += Chunk;
  }

  return Offset;
}

STATIC
VOID
SendCsw (
  IN UINT8   Status,
  IN UINT32  Residue
  )
{
  MS_CSW  Csw;

  Csw.Signature = MS_CSW_SIGNATURE;
  Csw.Tag       = mTag;
  Csw.Residue   = Residue;
  Csw.Status    = Status;

  mState = MsExpectCbw;

  SendData (&Csw, MS_CSW_SIZE);
}

//
// ---------------------------------------------------------------------------
// SCSI
// ---------------------------------------------------------------------------
//
STATIC UINT8  mScsiBuf[36];

STATIC
VOID
BuildInquiry (
  IN UINT8   *Cb,
  IN UINTN   CbLength,
  OUT UINTN  *SendLen
  )
{
  *SendLen = 0;

  //
  // VPD (EVPD) requests: only the two pages a mainstream host probes for.
  //
  if ((CbLength > 1) && ((Cb[1] & 0x01) != 0)) {
    ZeroMem (mScsiBuf, sizeof (mScsiBuf));
    if (Cb[2] == 0x00) {
      mScsiBuf[0] = 0x00;
      mScsiBuf[1] = 0x00;
      mScsiBuf[2] = 0x00;
      mScsiBuf[3] = 0x02;    // two pages follow
      mScsiBuf[4] = 0x00;
      mScsiBuf[5] = 0x80;
      *SendLen = 6;
    } else if (Cb[2] == 0x80) {
      mScsiBuf[0] = 0x80;
      mScsiBuf[1] = 0x00;
      mScsiBuf[2] = 0x00;
      mScsiBuf[3] = 0x10;
      CopyMem (&mScsiBuf[4], "0000000000000000", 16);
      *SendLen = 20;
    } else {
      SetSense (MS_SENSE_ILLEGAL, 0x24, 0x00);
    }
    return;
  }

  ZeroMem (mScsiBuf, sizeof (mScsiBuf));
  mScsiBuf[0]  = 0x00;                        // PDT: direct access block
  mScsiBuf[1]  = 0x80;                        // RMB: removable
  mScsiBuf[2]  = 0x05;                        // SPC-3
  mScsiBuf[3]  = 0x02;                        // response format
  mScsiBuf[4]  = 31;                          // additional length
  CopyMem (&mScsiBuf[8],  "KIRIN   ", 8);
  CopyMem (&mScsiBuf[16], "UFS LUN0         ", 16);
  CopyMem (&mScsiBuf[32], "0001", 4);
  *SendLen = 36;
}

STATIC
VOID
BuildRequestSense (
  OUT UINTN  *SendLen
  )
{
  ZeroMem (mScsiBuf, sizeof (mScsiBuf));
  mScsiBuf[0]  = 0x70;                        // current, not deferred
  mScsiBuf[2]  = (UINT8)(mSenseKey & 0x0F);
  mScsiBuf[7]  = 10;                          // additional sense length
  mScsiBuf[12] = mSenseAsc;
  mScsiBuf[13] = mSenseAscq;
  *SendLen = 18;
}

STATIC
VOID
FinishWrite (
  VOID
  )
{
  UINT8   Status;
  BOOLEAN Discard;

  Discard   = mDiscardOut;
  mWriteReady = FALSE;
  mDiscardOut = FALSE;

  if (Discard) {
    Status = MS_STATUS_FAIL;
    SetSense (MS_SENSE_ILLEGAL, 0x24, 0x00);
  } else if (EFI_ERROR (
               mBlockIo->WriteBlocks (
                           mBlockIo,
                           mBlockIo->Media->MediaId,
                           mWriteLba,
                           mWriteSize,
                           mDataBuf
                           )
               ))
  {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbMassStorage: WriteBlocks(%lx, %u) failed\n",
      mWriteLba,
      (UINT32)mWriteSize
      ));
    SetSense (MS_SENSE_MEDIUM_ERROR, 0x0C, 0x00);
    Status = MS_STATUS_FAIL;
  } else {
    Status = MS_STATUS_PASS;
  }

  if (mDataBuf != NULL) {
    FreePages (mDataBuf, mDataBufPages);
    mDataBuf      = NULL;
    mDataBufPages = 0;
  }

  mDataReceived = 0;
  SendCsw (Status, 0);
}

STATIC
VOID
HandleCbw (
  IN CONST UINT8  *Buffer,
  IN UINTN        Size
  )
{
  MS_CBW   Cbw;
  UINT8    Op;
  BOOLEAN  HostIn;
  UINTN    SendLen;
  UINTN    Sent;
  UINT32   Residue;
  UINT8    Status;
  UINT32   Lba;
  UINT16   Blocks;
  UINTN    Total;
  UINTN    Chunk;
  UINTN    Offset;
  UINT64   CurLba;

  if (Size != MS_CBW_SIZE) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbMassStorage: %u byte packet in command phase, ignored\n",
      (UINT32)Size
      ));
    return;
  }

  CopyMem (&Cbw, Buffer, sizeof (Cbw));

  if (Cbw.Signature != MS_CBW_SIGNATURE) {
    DEBUG ((DEBUG_ERROR, "KirinUsbMassStorage: bad CBW signature\n"));
    return;
  }

  //
  // A well-formed CBW while a data-out phase is still outstanding means the
  // previous command was aborted (cable pull, host reset).  Drop it and start
  // the BOT sequence over instead of feeding this CBW to the SCSI layer.
  //
  if (mState == MsDataOut) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbMassStorage: new CBW during data-out, resynchronising\n"
      ));
    AbandonDataOut ();
  }

  mTag          = Cbw.Tag;
  mDataExpected = Cbw.DataTransferLength;
  mDataReceived = 0;
  mDiscardOut   = FALSE;

  if ((Cbw.CbLength < 1) || (Cbw.CbLength > 16)) {
    SetSense (MS_SENSE_ILLEGAL, 0x24, 0x00);
    SendCsw (MS_STATUS_FAIL, mDataExpected);
    return;
  }

  if (Cbw.Lun != 0) {
    SetSense (MS_SENSE_ILLEGAL, 0x25, 0x00);
    SendCsw (MS_STATUS_FAIL, mDataExpected);
    return;
  }

  HostIn = (Cbw.Flags & MS_FLAG_IN) != 0;
  Op     = Cbw.Cb[0];

  //
  // Non-IN command carrying payload: the host is about to push it at us.
  // Buffer it (WRITE(10)) or swallow it (anything else), then answer.
  //
  if (!HostIn && (mDataExpected != 0)) {
    if ((Op == MS_OP_WRITE10) &&
        !mReadOnly &&
        (mDataExpected <= MS_MAX_WRITE_BYTES) &&
        ((mDataExpected % mBlockSize) == 0))
    {
      Lba    = GetBe32 (&Cbw.Cb[2]);
      Blocks = GetBe16 (&Cbw.Cb[7]);
      Total  = (UINTN)Blocks * mBlockSize;

      if ((Total == mDataExpected) &&
          (((UINT64)Lba + (UINT64)Blocks - 1) <= mLastLba) &&
          (mDataExpected != 0))
      {
        mDataBufPages = EFI_SIZE_TO_PAGES (mDataExpected);
        mDataBuf      = AllocatePages (mDataBufPages);
        if (mDataBuf == NULL) {
          SetSense (MS_SENSE_HARDWARE, 0x44, 0x00);
          mState = MsDataOut;
          mDiscardOut = TRUE;
          return;
        }
        mWriteLba  = Lba;
        mWriteSize = Total;
        MemoryFence ();
        mState = MsDataOut;
        return;
      }
    }

    //
    // Rejected but the payload is already on its way: swallow it so the
    // sequence stays in step, then report the failure.
    //
    mState     = MsDataOut;
    mDiscardOut = TRUE;
    return;
  }

  //
  // No OUT payload: run the command and answer with any IN data plus CSW.
  //
  SendLen = 0;
  Sent    = 0;
  Status  = MS_STATUS_PASS;
  ZeroMem (mScsiBuf, sizeof (mScsiBuf));

  switch (Op) {
    case MS_OP_TEST_UNIT_READY:
      if (mBlockIo->Media->MediaPresent == FALSE) {
        SetSense (MS_SENSE_NOT_READY, 0x3A, 0x00);
        Status = MS_STATUS_FAIL;
      }
      break;

    case MS_OP_REQUEST_SENSE:
      BuildRequestSense (&SendLen);
      break;

    case MS_OP_INQUIRY:
      BuildInquiry (Cbw.Cb, Cbw.CbLength, &SendLen);
      if (SendLen == 0) {
        Status = MS_STATUS_FAIL;
      }
      break;

    case MS_OP_MODE_SEN6:
      mScsiBuf[0] = 3;
      SendLen     = 4;
      if (mReadOnly) {
        mScsiBuf[2] = 0x80;
      }
      break;

    case MS_OP_MODE_SEN10:
      mScsiBuf[1] = 6;
      SendLen     = 8;
      if (mReadOnly) {
        mScsiBuf[3] = 0x80;
      }
      break;

    case MS_OP_READ_CAPACITY:
      SetBe32 (&mScsiBuf[0], (UINT32)mLastLba);
      SetBe32 (&mScsiBuf[4], mBlockSize);
      SendLen = 8;
      break;

    case MS_OP_READ_FMT_CAPAC:
      SetBe32 (&mScsiBuf[0], 8);
      SetBe32 (&mScsiBuf[4], (UINT32)(mLastLba + 1));
      mScsiBuf[8] = 0x02;
      mScsiBuf[9] = (UINT8)(mBlockSize >> 16);
      mScsiBuf[10] = (UINT8)(mBlockSize >> 8);
      mScsiBuf[11] = (UINT8)mBlockSize;
      SendLen = 12;
      break;

    case MS_OP_START_STOP_UNIT:
    case MS_OP_MEDIA_REMOVAL:
    case MS_OP_VERIFY10:
      break;

    case MS_OP_SYNC_CACHE:
      if (EFI_ERROR (mBlockIo->FlushBlocks (mBlockIo))) {
        SetSense (MS_SENSE_MEDIUM_ERROR, 0x0C, 0x00);
        Status = MS_STATUS_FAIL;
      }
      break;

    case MS_OP_READ10:
      Lba    = GetBe32 (&Cbw.Cb[2]);
      Blocks = GetBe16 (&Cbw.Cb[7]);
      Total  = (UINTN)Blocks * mBlockSize;

      if ((Blocks == 0) ||
          (((UINT64)Lba + (UINT64)Blocks - 1) > mLastLba))
      {
        SetSense (MS_SENSE_ILLEGAL, 0x21, 0x00);
        Status = MS_STATUS_FAIL;
        break;
      }

      Offset = 0;
      while (Offset < Total) {
        Chunk = Total - Offset;
        if (Chunk > MS_IO_CHUNK) {
          Chunk = MS_IO_CHUNK;
        }
        CurLba = Lba + (Offset / mBlockSize);

        if (EFI_ERROR (
              mBlockIo->ReadBlocks (
                          mBlockIo,
                          mBlockIo->Media->MediaId,
                          CurLba,
                          Chunk,
                          mIoBuf
                          )
              ))
        {
          DEBUG ((
            DEBUG_ERROR,
            "KirinUsbMassStorage: ReadBlocks(%lx, %u) failed\n",
            CurLba,
            (UINT32)Chunk
            ));
          SetSense (MS_SENSE_MEDIUM_ERROR, 0x11, 0x00);
          Status = MS_STATUS_FAIL;
          break;
        }

        Sent = SendData (mIoBuf, Chunk);
        if (Sent != Chunk) {
          Status = MS_STATUS_FAIL;
          break;
        }
        Offset += Sent;
      }
      break;

    case MS_OP_WRITE10:
      // Only reachable with a zero length payload (or read-only media).
      if (mReadOnly) {
        SetSense (MS_SENSE_DATA_PROTECT, 0x27, 0x00);
      } else {
        SetSense (MS_SENSE_ILLEGAL, 0x24, 0x00);
      }
      Status = MS_STATUS_FAIL;
      break;

    default:
      DEBUG ((
        DEBUG_ERROR,
        "KirinUsbMassStorage: unsupported opcode 0x%02x\n",
        Op
        ));
      SetSense (MS_SENSE_ILLEGAL, 0x20, 0x00);
      Status = MS_STATUS_FAIL;
      break;
  }

  if (SendLen > 0) {
    if (SendLen > mDataExpected) {
      SendLen = mDataExpected;
    }
    Sent = SendData (mScsiBuf, SendLen);
  }

  Residue = 0;
  if (mDataExpected > Sent) {
    Residue = (UINT32)(mDataExpected - Sent);
  }

  SendCsw (Status, Residue);
}

//
// ---------------------------------------------------------------------------
// Block device discovery
// ---------------------------------------------------------------------------
//
STATIC
EFI_STATUS
LocateBackingDevice (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     Index;
  EFI_BLOCK_IO_PROTOCOL     *BlockIo;
  UINT64                    BestLastBlock;

  Handles       = NULL;
  Count         = 0;
  BestLastBlock = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    BlockIo = NULL;
    if (EFI_ERROR (
          gBS->HandleProtocol (
                Handles[Index],
                &gEfiBlockIoProtocolGuid,
                (VOID **)&BlockIo
                )
          ) || (BlockIo == NULL) || (BlockIo->Media == NULL))
    {
      continue;
    }

    //
    // Whole disk only: partitions are LogicalPartition == TRUE.  The largest
    // one is the UFS LUN0 GPT, which is what every other flash path uses.
    //
    if (BlockIo->Media->LogicalPartition ||
        BlockIo->Media->ReadOnly ||
        (BlockIo->Media->BlockSize == 0))
    {
      continue;
    }

    if (BlockIo->Media->LastBlock > BestLastBlock) {
      BestLastBlock = BlockIo->Media->LastBlock;
      mBlockIo      = BlockIo;
    }
  }

  gBS->FreePool (Handles);

  if (mBlockIo == NULL) {
    return EFI_NOT_FOUND;
  }

  mBlockSize = mBlockIo->Media->BlockSize;
  mLastLba   = mBlockIo->Media->LastBlock;
  mReadOnly  = mBlockIo->Media->ReadOnly;

  if (mBlockIo->Media->IoAlign > 4096) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinUsbMassStorage: IoAlign %u exceeds the page bounce buffer\n",
      (UINT32)mBlockIo->Media->IoAlign
      ));
    mBlockIo = NULL;
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------
//
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  VOID        *CbwBuffer;
  UINTN       CbwSize;

  gBS->SetWatchdogTimer (0, 0x10000, 0, NULL);

  Status = gBS->LocateProtocol (
                  &gUsbDeviceProtocolGuid,
                  NULL,
                  (VOID **)&mUsbDevice
                  );
  if (EFI_ERROR (Status)) {
    Print (L"USB device driver unavailable: %r\r\n", Status);
    return Status;
  }

  Status = gBS->LocateProtocol (
                  &gKirinUsbFunctionProtocolGuid,
                  NULL,
                  (VOID **)&mFunction
                  );
  if (EFI_ERROR (Status)) {
    Print (L"USB mode control unavailable: %r\r\n", Status);
    return Status;
  }

  Status = LocateBackingDevice ();
  if (EFI_ERROR (Status)) {
    Print (L"No whole-disk BlockIo to share: %r\r\n", Status);
    return Status;
  }

  mIoBuf = AllocatePages (EFI_SIZE_TO_PAGES (MS_IO_CHUNK));
  if (mIoBuf == NULL) {
    Print (L"Out of memory.\r\n");
    return EFI_OUT_OF_RESOURCES;
  }

  mCbwHead = 0;
  mCbwTail = 0;
  mState   = MsExpectCbw;
  ZeroMem (mScsiBuf, sizeof (mScsiBuf));

  Print (L"\r\n");
  Print (L"USB mass storage: sharing the UFS disk with the host.\r\n");
  Print (L"  %lx blocks of %u bytes (LBA 0 .. %lx)\r\n",
         mLastLba + 1, (UINT32)mBlockSize, mLastLba);
  if (mReadOnly) {
    Print (L"  media is READ ONLY\r\n");
  }
  Print (L"\r\nThe disk now belongs to the attached PC - do not boot the OS\r\n");
  Print (L"while it is shared. Press the POWER key to stop sharing.\r\n\r\n");

  Status = mUsbDevice->Start (
                         &mDeviceDescriptor,
                         mConfigDescriptors,
                         DataReceived,
                         NULL
                         );
  if (EFI_ERROR (Status)) {
    Print (L"Could not start the gadget: %r\r\n", Status);
    goto Done;
  }

  while (TRUE) {
    if (mWriteReady) {
      FinishWrite ();
    }

    while (PopCbw (&CbwBuffer, &CbwSize)) {
      //
      // Deliberately not gated on mState: HandleCbw() validates the packet
      // and resynchronises itself when a CBW turns up in the middle of an
      // aborted data-out phase (the cable-pull case).
      //
      HandleCbw (CbwBuffer, CbwSize);
      FreePool (CbwBuffer);
    }

    if ((gST->ConIn != NULL) && (gST->ConIn->WaitForKey != NULL)) {
      if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &mKey))) {
        if ((mKey.ScanCode == SCAN_NULL) &&
            ((mKey.UnicodeChar == CHAR_CARRIAGE_RETURN) ||
             (mKey.UnicodeChar == L' ')))
        {
          break;
        }
      }
    }

    gBS->Stall (MS_POLL_US);
  }

  Print (L"\r\nStopped sharing the UFS disk.\r\n");

Done:
  if (mIoBuf != NULL) {
    FreePages (mIoBuf, EFI_SIZE_TO_PAGES (MS_IO_CHUNK));
    mIoBuf = NULL;
  }
  if (mDataBuf != NULL) {
    FreePages (mDataBuf, mDataBufPages);
    mDataBuf      = NULL;
    mDataBufPages = 0;
  }
  while (PopCbw (&CbwBuffer, &CbwSize)) {
    FreePool (CbwBuffer);
  }

  mState = MsExpectCbw;

  //
  // Back to the platform's default gadget so the port is not left exposing
  // the internal disk.
  //
  if (mFunction != NULL) {
    mFunction->Idle ();
  }

  return Status;
}
