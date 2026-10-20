/** @file
  Reboot-mode translation for SimpleInit's "reboot to recovery /
  bootloader / factory" menu entries.

  SimpleInit reboots the Qualcomm way (GPLDrivers/Library/SimpleInit/
  src/lib/reboot.c): known modes (recovery = 1, fastboot = 2) go as
  gRT->ResetSystem(EfiResetCold, ..., {L"RESET_PARAM", Bdata}); anything
  else ("factory", "kpoc", ...) goes as EfiResetPlatformSpecific with the
  plain ASCII argument. The stock ResetSystemRuntimeDxe ignores both
  payloads, so without this driver the menu items just do a plain reboot.

  What the Kirin970 boot chain actually reads, taken from the vendor
  kernel:

  PMIC HRST_REG13 (hi6421v600, SPMI sid 0 channel 2, register 0x25A)
    drivers/hisi/ap/platform/kirin970/mntn_public_interface.h
      #define PMU_RESET_REG_OFFSET (PMIC_HRST_REG13_ADDR(0))   -> 0x25A
      #define RST_FLAG_MASK 0xFF
    drivers/hisi/ap/platform/kirin970/pmic_interface.h
      #define PMIC_HRST_REG13_ADDR(base) ((base) + (0x25A))

    This is *the* reboot-reason register, and it is a byte-wide EXCH_SOURCE
    code (mntn_public_interface.h "typedef enum EXCH_SOURCE"):

      AP_S_COLDBOOT 0x00   bootloader     0x01   recovery       0x02
      resetfactory  0x03   resetuser      0x04   chargereboot   0x06
      COLDBOOT      0x10   AP_S_PANIC     0x60   ...

    It survives reset, and every restart funnels through it:
    drivers/hisi/mntn/hisi_poweroff.c -> hisi_pm_system_reset(), which is
    installed as arm_pm_restart(), maps the restart command string through
    reboot_reason_map[] and calls set_reboot_reason() (that writes 0x25A over
    SPMI). A plain reboot() has a NULL cmd and therefore maps to COLDBOOT -
    which is exactly why the stock kernel's bootargs carry
    "reboot_reason=COLDBOOT" on a normal boot. The boot chain reads the same
    byte back after reset (the sibling PMIC_HRST_REG14 at 0x25B shows up as
    "last_bootup_keypoint=" in the same bootargs).

    The kernel only ever gets to write this byte for a normal Android reboot;
    from UEFI the equivalent is doing the very same write here.

  misc partition BCB (Android bootloader_message, struct at offset 0)
    drivers/huawei_platform/recovery/recovery_misc.c writes "boot-recovery"
    into /dev/block/bootdevice/by-name/misc on a reboot-recovery, so the boot
    chain does consume the BCB. fastbootd is not a boot mode of its own: the
    device boots recovery and recovery's init checks the command field.

  This driver hooks ResetSystem through the EDKII reset notification
  protocol (invoked for every ResetSystem with the original parameters)
  and programs the markers before the reset happens.

  Copyright (c) 2026, Renegade Project. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/KirinPmicSpmiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/ResetNotification.h>

//
// PMIC HRST register holding the EXCH_SOURCE reboot reason (see header).
//
#define PMIC_REBOOT_REASON    0x25A

#define REASON_COLDBOOT       0x10
#define REASON_BOOTLOADER     0x01
#define REASON_RECOVERY       0x02
#define REASON_RESETFACTORY   0x03
#define REASON_CHARGEREBOOT   0x06

//
// SimpleInit's RESET_DATA_TYPE (src/lib/reboot.c): a CHAR16 marker
// followed by the one-byte reset reason.
//
#define RESET_PARAM_SIZE   26  // 12 * sizeof(CHAR16) + UINT8 + padding
#define RECOVERY_MODE      0x01
#define FASTBOOT_MODE      0x02

typedef enum {
  KirinRebootNone,
  KirinRebootRecovery,
  KirinRebootBootloader,
  KirinRebootFactory,
  KirinRebootKpoc,
  KirinRebootFastbootd,
} KIRIN_REBOOT_MODE;

STATIC CONST CHAR8  *mModeNames[] = {
  NULL, "recovery", "bootloader", "factory", "kpoc", "fastbootd",
};

/**
  List every GPT partition the platform exposes.

  A failed lookup of "misc" used to reduce to a single "not found" line,
  which cannot tell a genuinely absent partition from a differently named
  one or from PartitionDxe never producing the handle.  Reboot modes then
  silently degrade to a plain reboot.  This makes the next boot state
  exactly which names were seen.
**/
STATIC
VOID
DumpGptPartitionNames (
  VOID
  )
{
  EFI_STATUS                  Status;
  EFI_HANDLE                  *Handles;
  UINTN                       HandleCount;
  UINTN                       Index;
  EFI_BLOCK_IO_PROTOCOL       *BlockIo;
  EFI_PARTITION_INFO_PROTOCOL *PartInfo;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiBlockIoProtocolGuid,
                  NULL, &HandleCount, &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinRebootMode: LocateHandle(BlockIo) %r\n", Status));
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (
                      Handles[Index], &gEfiBlockIoProtocolGuid,
                      (VOID **)&BlockIo)) ||
        (BlockIo == NULL) || (BlockIo->Media == NULL) ||
        !BlockIo->Media->MediaPresent || !BlockIo->Media->LogicalPartition)
    {
      continue;
    }
    if (EFI_ERROR (gBS->HandleProtocol (
                      Handles[Index], &gEfiPartitionInfoProtocolGuid,
                      (VOID **)&PartInfo)))
    {
      DEBUG ((DEBUG_ERROR,
              "KirinRebootMode: [%u] logical block, no PartitionInfo\n",
              (UINT32)Index));
      continue;
    }
    if (PartInfo->Type != PARTITION_TYPE_GPT) {
      continue;
    }
    DEBUG ((
      DEBUG_ERROR,
      "KirinRebootMode: [%u] gpt '%s' lba 0x%lx..0x%lx\n",
      (UINT32)Index,
      PartInfo->Info.Gpt.PartitionName,
      (UINT64)PartInfo->Info.Gpt.StartingLBA,
      (UINT64)PartInfo->Info.Gpt.EndingLBA
      ));
  }

  FreePool (Handles);
}

/**
  Write (or clear) the Android Bootloader Control Block in the "misc"
  GPT partition.

  The boot chain demonstrably consumes it: the vendor kernel's
  drivers/huawei_platform/recovery/recovery_misc.c registers a reboot
  notifier that writes "boot-recovery" into
  /dev/block/by-name/misc, which is how an Android reboot-recovery
  reaches recovery.  The layout is the standard bootloader_message with
  command[32] at offset 0.

  fastbootd is not a distinct boot mode of the chain: the device boots
  normally into recovery (same PMIC marker as a plain recovery reboot) and
  recovery's init checks the BCB command field - "boot-fastboot" makes it
  start the userspace fastbootd daemon instead of its UI. That is also what
  `adb reboot fastboot` programs from Android.

  Every write is read back.  A silent eMMC write failure here looks
  exactly like "the menu item does nothing", which is the failure being
  chased, so it is verified rather than assumed.

  @param[in] Command  NUL-terminated BCB command (e.g. "boot-recovery"),
                      or NULL to zero the BCB sector.

  @retval EFI_SUCCESS  BCB written (or cleared) and read back identical.
  @retval other        misc partition not found / write or verify failed.
**/
STATIC
EFI_STATUS
WriteMiscBcb (
  IN CONST CHAR8  *Command OPTIONAL
  )
{
  EFI_STATUS                  Status;
  EFI_HANDLE                  *Handles;
  UINTN                       HandleCount;
  UINTN                       Index;
  EFI_BLOCK_IO_PROTOCOL       *BlockIo;
  EFI_PARTITION_INFO_PROTOCOL *PartInfo;
  UINT8                       *Sector;
  UINT8                       *Verify;
  UINTN                       Size;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiBlockIoProtocolGuid,
                  NULL, &HandleCount, &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinRebootMode: misc lookup LocateHandle %r\n",
            Status));
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (
                      Handles[Index], &gEfiBlockIoProtocolGuid,
                      (VOID **)&BlockIo)))
    {
      continue;
    }
    if ((BlockIo->Media == NULL) || !BlockIo->Media->MediaPresent ||
        !BlockIo->Media->LogicalPartition)
    {
      continue;
    }
    if (EFI_ERROR (gBS->HandleProtocol (
                      Handles[Index], &gEfiPartitionInfoProtocolGuid,
                      (VOID **)&PartInfo)))
    {
      continue;
    }
    if ((PartInfo->Type != PARTITION_TYPE_GPT) ||
        (StrCmp (PartInfo->Info.Gpt.PartitionName, L"misc") != 0))
    {
      continue;
    }

    Size   = BlockIo->Media->BlockSize;
    Sector = AllocateZeroPool (Size);
    Verify = AllocateZeroPool (Size);
    if ((Sector == NULL) || (Verify == NULL)) {
      if (Sector != NULL) {
        FreePool (Sector);
      }
      if (Verify != NULL) {
        FreePool (Verify);
      }
      FreePool (Handles);
      return EFI_OUT_OF_RESOURCES;
    }

    //
    // struct bootloader_message: command[32] at offset 0. A full block
    // of the 832-byte BCB block gets written back with the rest kept
    // zero - recovery parses the command field and clears the block
    // after consuming it.
    //
    if (Command != NULL) {
      AsciiStrCpyS (
        (CHAR8 *)Sector, 32, Command
        );
    }

    Status = BlockIo->WriteBlocks (
                        BlockIo, BlockIo->Media->MediaId, 0, Size, Sector
                        );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "KirinRebootMode: misc WriteBlocks %r\n", Status));
      goto Done;
    }

    Status = BlockIo->ReadBlocks (
                        BlockIo, BlockIo->Media->MediaId, 0, Size, Verify
                        );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "KirinRebootMode: misc ReadBlocks %r\n", Status));
      goto Done;
    }

    if (CompareMem (Sector, Verify, Size) != 0) {
      DEBUG ((
        DEBUG_ERROR,
        "KirinRebootMode: misc VERIFY MISMATCH, read back '%a'\n",
        (CHAR8 *)Verify
        ));
      Status = EFI_DEVICE_ERROR;
      goto Done;
    }

    DEBUG ((
      DEBUG_ERROR,
      "KirinRebootMode: misc BCB ok, block=%u cmd='%a'\n",
      (UINT32)Size,
      (Command != NULL) ? Command : "(clear)"
      ));
    Status = EFI_SUCCESS;

Done:
    FreePool (Sector);
    FreePool (Verify);
    FreePool (Handles);
    return Status;
  }

  FreePool (Handles);
  DEBUG ((DEBUG_ERROR, "KirinRebootMode: misc partition not found\n"));
  DumpGptPartitionNames ();
  return EFI_NOT_FOUND;
}

/**
  The BCB `command` string that carries one reboot mode.

  This is the explicit, per-mode specification of what the next boot
  should do.  Only commands the chain is known to consume are written;
  every other mode returns NULL, which clears the block - leaving a
  stale "boot-recovery" behind would hijack a factory / charger / plain
  reboot, and nothing else clears it for us.

  The chain has no AOSP-standard BCB command for a plain hand-off to the
  bootloader or for factory mode, so those ride on the PMIC reason alone
  and must still drop any inherited request.

  @param[in] Mode  Requested reboot mode.

  @retval CONST CHAR8*  NUL-terminated BCB command, or NULL to clear.
**/
STATIC
CONST CHAR8 *
BcbCommandForMode (
  IN KIRIN_REBOOT_MODE  Mode
  )
{
  switch (Mode) {
    case KirinRebootRecovery:
      return "boot-recovery";

    case KirinRebootFastbootd:
      //
      // fastbootd runs inside recovery: recovery's init reads this and
      // starts the userspace daemon instead of the recovery UI.
      //
      return "boot-fastboot";

    default:
      return NULL;
  }
}

/**
  Program every boot-mode marker for one reboot mode: the PMIC
  HRST_REG13 reason byte the boot chain reads, plus the BCB that
  carries the same intent in the misc partition.

  A plain reboot (KirinRebootNone) writes COLDBOOT, which is what
  hisi_pm_system_reset() does for a NULL restart command - leaving a
  stale recovery/bootloader code behind would send the next boot to the
  wrong place, because nothing clears the byte for us.
**/
STATIC
VOID
MarkRebootMode (
  IN KIRIN_REBOOT_MODE  Mode
  )
{
  UINT8       Reason = REASON_COLDBOOT;
  EFI_STATUS  BcbStatus;

  switch (Mode) {
    case KirinRebootRecovery:
      Reason = REASON_RECOVERY;
      break;

    case KirinRebootFastbootd:
      //
      // Same PMIC markers as a plain recovery reboot; the BCB command
      // is what distinguishes fastbootd from the recovery UI.
      //
      Reason = REASON_RECOVERY;
      break;

    case KirinRebootBootloader:
      Reason = REASON_BOOTLOADER;
      break;

    case KirinRebootFactory:
      Reason = REASON_RESETFACTORY;
      break;

    case KirinRebootKpoc:
      Reason = REASON_CHARGEREBOOT;
      break;

    default:
      break;
  }

  if (RETURN_ERROR (KirinPmicWrite8 (PMIC_REBOOT_REASON, Reason))) {
    DEBUG ((DEBUG_ERROR, "KirinRebootMode: PMIC 0x25A write FAILED\n"));
  }

  //
  // The BCB is rewritten for *every* mode, not only the ones that name a
  // command: a mode that returns NULL must still clear an inherited
  // boot-recovery, otherwise it silently loses to a stale request.
  //
  BcbStatus = WriteMiscBcb (BcbCommandForMode (Mode));
  if (EFI_ERROR (BcbStatus)) {
    DEBUG ((
      DEBUG_ERROR,
      "KirinRebootMode: BCB update failed (%r) - mode '%a' carries no\n"
      "                misc marker and relies on the PMIC byte alone\n",
      BcbStatus,
      (Mode == KirinRebootNone) ? "normal" : mModeNames[Mode]
      ));
  }
}

/**
  Render an unrecognised ResetData payload for the log.

  The payload is not guaranteed to be a NUL-terminated ASCII string -
  SimpleInit's EDL path sends L"EDL" as raw CHAR16 bytes - so bytes are
  sanitised to printable ASCII rather than printed with %a directly.

  @param[in] Data  Reset payload bytes.
  @param[in] Size  Payload length in bytes.
**/
STATIC
VOID
LogResetArg (
  IN CONST VOID  *Data,
  IN UINTN       Size
  )
{
  CONST UINT8  *Bytes = (CONST UINT8 *)Data;
  CHAR8        Buf[41];
  UINTN        Index;
  UINTN        Len;
  UINT8        Raw;

  if (Data == NULL) {
    return;
  }

  Len = (Size > 32) ? 32 : Size;
  for (Index = 0; Index < Len; Index++) {
    Raw      = Bytes[Index];
    Buf[Index] = ((Raw >= 0x20) && (Raw < 0x7F)) ? (CHAR8)Raw : '.';
  }
  Buf[Len] = '\0';

  DEBUG ((
    DEBUG_ERROR,
    "KirinRebootMode: no Kirin970 mode matches reboot arg '%a'"
    " (raw %u bytes) - rebooting normally\n",
    Buf,
    (UINT32)Size
    ));
}

/**
  Flatten a ResetData payload into a printable, NUL-terminated string.

  SimpleInit does not send one byte pattern (adv_reboot,
  GPLDrivers/Library/SimpleInit/src/lib/reboot.c):

    - a RESET_PARAM packet for recovery / bootloader (handled separately);
    - L"EDL" as six raw CHAR16 bytes for the edl entry;
    - the plain ASCII argument for everything else ("meta", "factory",
      "fastbootd", ...).

  The CHAR16 form never reached the alias table: it compares as
  "E\0D\0L\0" against "edl" and length 6 != 3, so it fell straight through
  to the generic "no mode matches" path.  Normalising here makes the
  matching layer payload-agnostic, which is exactly what allows
  SimpleInit's reboot arguments to be left untouched - this driver takes
  the hijack from there.

  @param[in]  Data      Reset payload bytes.
  @param[in]  DataSize  Payload length in bytes.
  @param[out] Out       Flattened string.
  @param[in]  OutSize   Size of Out including the terminator.

  @retval != 0   Length of the flattened string.
  @retval 0      Nothing printable was in the payload.
**/
STATIC
UINTN
NormaliseResetArg (
  IN  CONST VOID  *Data,
  IN  UINTN       DataSize,
  OUT CHAR8       *Out,
  IN  UINTN       OutSize
  )
{
  CONST UINT8  *Bytes = (CONST UINT8 *)Data;
  UINTN        Index;
  UINTN        Length = 0;
  UINTN        Step   = 1;
  UINT8        Raw;

  if ((Data == NULL) || (Out == NULL) || (OutSize == 0)) {
    return 0;
  }

  //
  // UTF-16LE is only unambiguous when the length is even and every high
  // byte is zero.  No ASCII argument satisfies that, so the two forms can
  // be told apart without guessing.
  //
  if ((DataSize >= 2) && ((DataSize % 2) == 0)) {
    Step = 2;
    for (Index = 1; Index < DataSize; Index += 2) {
      if (Bytes[Index] != 0) {
        Step = 1;
        break;
      }
    }
  }

  for (Index = 0; (Index < DataSize) && (Length + 1 < OutSize); Index += Step) {
    Raw = Bytes[Index];
    if ((Raw >= 0x20) && (Raw < 0x7F)) {
      Out[Length++] = (CHAR8)Raw;
    }
  }

  Out[Length] = '\0';
  return Length;
}

/**
  Reset notification handler (see file header): translate both the
  Qualcomm RESET_PARAM payload and the ASCII reboot arguments into
  Kirin boot-mode markers.
**/
STATIC
VOID
EFIAPI
KirinResetNotify (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  KIRIN_REBOOT_MODE  Mode = KirinRebootNone;
  UINTN              Index;
  UINT8              Reason = 0;

  //
  // Shutdown is not implemented here: hisi_pm_system_off() marks that one
  // AP_S_COLDBOOT (0x00) itself and no boot mode is involved.
  //
  if (ResetType == EfiResetShutdown) {
    return;
  }

  if (ResetData != NULL) {
    //
    // Qualcomm-style packet: L"RESET_PARAM" + mode byte (SimpleInit
    // recovery / bootloader menu items).
    //
    if ((DataSize >= RESET_PARAM_SIZE) &&
        (CompareMem (ResetData, L"RESET_PARAM",
                     sizeof (L"RESET_PARAM") - sizeof (CHAR16)) == 0))
    {
      switch (*((UINT8 *)ResetData + 12 * sizeof (CHAR16))) {
        case RECOVERY_MODE:
          Mode = KirinRebootRecovery;
          break;
        case FASTBOOT_MODE:
          Mode = KirinRebootBootloader;
          break;
        default:
          DEBUG ((DEBUG_WARN, "KirinRebootMode: unknown RESET_PARAM mode %u\n",
                  (UINT32) * ((UINT8 *)ResetData + 12 * sizeof (CHAR16))));
          break;
      }
    } else {
      //
      // Plain reboot argument (SimpleInit generic reboot configs: factory /
      // kpoc / meta / edl, also "recovery"/"bootloader" from the boot item
      // editor).  Normalise first so the CHAR16 EDL payload and the ASCII
      // payloads take one path, then match the whole argument against the
      // Kirin970 mode names plus the aliases vendors use for them.
      //
      STATIC CONST struct {
        CONST CHAR8      *Name;
        KIRIN_REBOOT_MODE Mode;
      }  Aliases[] = {
        { "recovery",      KirinRebootRecovery },
        { "bootloader",    KirinRebootBootloader },
        { "fastboot",      KirinRebootBootloader },
        { "factory",       KirinRebootFactory },
        { "ftm",           KirinRebootFactory },
        { "resetfactory",  KirinRebootFactory },
        { "kpoc",          KirinRebootKpoc },
        { "charger",       KirinRebootKpoc },
        { "fastbootd",     KirinRebootFastbootd },
      };
      //
      // Recognised by SimpleInit, but no Kirin970 register carries them.
      // Worth matching anyway: the point of this layer is to name which
      // entry was used rather than let it look like a dead click.
      //
      STATIC CONST CHAR8  *ForeignOnly[] = {
        "edl",   // Qualcomm 9008 emergency download
        "meta",  // MediaTek preloader META mode
      };
      CHAR8   Norm[64];
      BOOLEAN Foreign = FALSE;

      NormaliseResetArg (ResetData, DataSize, Norm, sizeof (Norm));

      for (Index = 0; Index < ARRAY_SIZE (Aliases); Index++) {
        if (AsciiStriCmp (Norm, Aliases[Index].Name) == 0) {
          Mode = Aliases[Index].Mode;
          break;
        }
      }

      //
      // Anything that got this far has no Kirin970 counterpart.  Split it
      // into "recognised but other-vendor" and "unrecognised" so the log
      // says which entry was involved instead of only that a reboot
      // happened - which is exactly what made edl/meta look like no-ops.
      //
      if (Mode == KirinRebootNone) {
        for (Index = 0; Index < ARRAY_SIZE (ForeignOnly); Index++) {
          if (AsciiStriCmp (Norm, ForeignOnly[Index]) == 0) {
            Foreign = TRUE;
            break;
          }
        }
        if (Foreign) {
          DEBUG ((
            DEBUG_ERROR,
            "KirinRebootMode: '%a' belongs to another SoC vendor and has no"
            " Kirin970 marker - rebooting normally\n",
            Norm
            ));
        } else {
          LogResetArg (ResetData, DataSize);
        }
      }
    }
  }

  MarkRebootMode (Mode);

  if (RETURN_ERROR (KirinPmicRead8 (PMIC_REBOOT_REASON, &Reason))) {
    Reason = 0xFF;
  }

  //
  // Read-back verification + a short on-screen window: the flags live in
  // registers that survive the reset, so pausing here only delays the
  // reboot. This lets a screen photo prove whether the marker actually
  // landed (separating a write failure from the boot chain ignoring it).
  //
  DEBUG ((
    DEBUG_ERROR,
    "KirinRebootMode: '%a' marked, PMIC 0x25A reads 0x%02x\n",
    (Mode == KirinRebootNone) ? "normal" : mModeNames[Mode],
    Reason
    ));
  MicroSecondDelay (3 * 1000 * 1000);  // 3s: photo window

  DEBUG ((DEBUG_INFO, "KirinRebootMode: resetting now\n"));
}

EFI_STATUS
EFIAPI
KirinRebootModeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                       Status;
  EFI_RESET_NOTIFICATION_PROTOCOL  *ResetNotify;

  Status = gBS->LocateProtocol (
                  &gEfiResetNotificationProtocolGuid,
                  NULL,
                  (VOID **)&ResetNotify
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinRebootMode: reset notification not found: %r\n", Status));
    return Status;
  }

  Status = ResetNotify->RegisterResetNotify (ResetNotify, KirinResetNotify);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KirinRebootMode: register handler failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "KirinRebootMode: reboot mode hook ready\n"));
  return EFI_SUCCESS;
}
