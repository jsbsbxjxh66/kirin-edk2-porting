/** @file
 *
 * Device Tree Parse Library
 *
 * Copyright (c) 2021, BigfootACA <bigfoot@classfun.cn>
 *
 **/

#ifndef _FDT_PARSER_LIB
#define _FDT_PARSER_LIB
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseMemoryLib.h>
#include <KernelFdt.h>
#include <fdtparser.h>
#include <param.h>
#include <keyval.h>

STATIC
inline
fdt*
EFIAPI
GetFdt(VOID)
{
  EFI_PHYSICAL_ADDRESS FdtAddress;
  #ifndef FDT_DIRECT
  EFI_STATUS          Status;
  KERNEL_FDT_PROTOCOL *Fdt;

  Status = gBS->LocateProtocol (
    &gKernelFdtProtocolGuid,
    NULL,
    (VOID**)&Fdt
    );
  if (EFI_ERROR (Status)) {
    DEBUG((EFI_D_ERROR, "Locate Kernel Fdt Protocol failed: %r\n", Status));
    return NULL;
  }
  if (Fdt == NULL || Fdt->Fdt == NULL) {
    DEBUG((EFI_D_ERROR, "Invalid Device Tree\n"));
    return NULL;
  }
  FdtAddress = (EFI_PHYSICAL_ADDRESS)Fdt->Fdt;
  #else
  FdtAddress = *(EFI_PHYSICAL_ADDRESS*)FixedPcdGet64 (PcdDeviceTreeStore);

  //
  // The old test was "FdtAddress < PcdSystemMemoryBase", which can never fire
  // here because that PCD is 0 -- so a stale or never-written slot went
  // straight into fdt_check_header(), which dereferences whatever it was
  // given.  Kirin970's bootloader hands the DTB up from below 4 GiB (the tags
  // window lives at BOOTIMG_TAGS_OFFSET 0x07a00000), so bound it to the low
  // 4 GiB before touching it.
  //
  if (FdtAddress == 0 ||
      FdtAddress < PcdGet64(PcdSystemMemoryBase) ||
      FdtAddress >= 0x100000000ULL) {
    DEBUG((EFI_D_ERROR, "Invalid Device Tree Address: 0x%016lx\n", FdtAddress));
    return NULL;
  }
  #endif

  DEBUG((EFI_D_ERROR, "Device Tree Address: 0x%016lx\n", FdtAddress));
  return get_fdt_from_pointer ((VOID*)FdtAddress);
}

STATIC
inline
VOID
EFIAPI
CmdlineDumpItemDebug(IN UINTN Level, IN CHAR8 *Prefix, IN keyval *Item)
{
	if (Item == NULL) return;
	DEBUG((
		Level,
		"%aKey = \"%a\", Value = \"%a\"\n",
		(Prefix != NULL ? Prefix : ""),
		Item->key, Item->value
	));
}

STATIC
inline
VOID
EFIAPI
CmdlineDumpItemsDebug(IN UINTN Level, IN CHAR8 *Prefix, IN keyval **Items)
{
	CHAR8 String[256];
	if (Items == NULL) return;
	KVARR_FOREACH (Items, Item, Index) {
		ZeroMem(String, sizeof (String));
		AsciiSPrint(
			String, sizeof (String), "%aItem %d: ",
			(Prefix != NULL ? Prefix : ""), Index + 1
		);
		CmdlineDumpItemDebug(Level, String, Item);
	}
}

#endif
