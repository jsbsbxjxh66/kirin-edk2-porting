/** @file

  Copyright (c) 2011-2015, ARM Limited. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#define FDT_DIRECT
#include <PiPei.h>

#include <Library/ArmMmuLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/FdtParserLib.h>
#include <Library/PlatformMemoryMapLib.h>

#define SIZE_KB ((UINTN)(1024))
#define SIZE_MB ((UINTN)(SIZE_KB * 1024))
#define SIZE_GB ((UINTN)(SIZE_MB * 1024))
#define SIZE_MB_BIG(_Size,_Value) ((_Size) > ((_Value) * SIZE_MB))
#define SIZE_MB_SMALL(_Size,_Value) ((_Size) < ((_Value) * SIZE_MB))
#define SIZE_MB_IN(_Min,_Max,_Size) \
  if (SIZE_MB_BIG((MemoryTotal), (_Min)) && SIZE_MB_SMALL((MemoryTotal), (_Max)))\
    Mem = Mem##_Size##G, MemGB = _Size

//
// (base, length) pairs of every Conventional region we handed to DXE.  Used
// afterwards to carve FDT no-map firmware back out of them.
//
#define MAX_CONV_SPAN 16
#define MAX_FDT_MEM   8

extern UINT64 mSystemMemoryEnd;

VOID BuildMemoryTypeInformationHob(VOID);


STATIC
VOID InitMmu(IN ARM_MEMORY_REGION_DESCRIPTOR *MemoryTable)
{

  VOID *        TranslationTableBase;
  UINTN         TranslationTableSize;
  RETURN_STATUS Status;

  // Note: Because we called PeiServicesInstallPeiMemory() before
  // to call InitMmu() the MMU Page Table resides in
  // RAM (even at the top of DRAM as it is the first permanent memory
  // allocation)
  Status = ArmConfigureMmu(
      MemoryTable, &TranslationTableBase, &TranslationTableSize);

  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "Error: Failed to enable MMU: %r\n", Status));
  }
}

STATIC
VOID AddHob(PARM_MEMORY_REGION_DESCRIPTOR_EX Desc)
{
  BuildResourceDescriptorHob(
      Desc->ResourceType, Desc->ResourceAttribute, Desc->Address, Desc->Length);

  //
  // A Conventional row must NOT get a memory-allocation HOB.  Such a HOB makes
  // CoreInitializeGcdServices() claim the whole row in the GCD map (it stamps
  // gDxeCoreImageHandle onto every entry) while it walks the HOB list, and our
  // FDT carve HOBs are built after the table so they are seen later.
  // CoreAllocateSpaceCheckEntry() rejects any entry whose ImageHandle is
  // already set, so CoreAllocateMemorySpace() fails, the guard around
  // CoreAddMemoryDescriptor(EfiReservedMemoryType, ...) is skipped, and the
  // carve silently does nothing while still printing its count.  Without the
  // allocation HOB the row stays unallocated in GCD until the closing "add the
  // remaining unallocated system memory" loop -- which runs after our Reserved
  // HOBs have taken the firmware out -- so the firmware really does end up
  // EfiReservedMemoryType and the rest really does end up EfiConventionalMemory.
  // Rows typed anything else (RtData, BsData, Reserv, ...) still need their
  // allocation HOB and are left alone.
  //
  if (Desc->MemoryType == EfiConventionalMemory) {
    return;
  }

  if (Desc->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY ||
      Desc->MemoryType == EfiRuntimeServicesData)
  {
    BuildMemoryAllocationHob(Desc->Address, Desc->Length, Desc->MemoryType);
  }
}

//
// Reserve [From, To) as EfiReservedMemoryType, rounded outward to 4K so no
// fragment of it is ever handed to DXE.  Returns 1 if a HOB was built.
//
STATIC
UINTN ReservePages(IN UINT64 From, IN UINT64 To)
{
  UINT64 PageStart;
  UINT64 PageEnd;

  if (To <= From) {
    return 0;
  }
  PageStart = From & ~(UINT64)0xFFF;
  if (To > MAX_UINT64 - 0xFFF) {
    PageEnd = MAX_UINT64 & ~(UINT64)0xFFF;
  } else {
    PageEnd = (To + 0xFFF) & ~(UINT64)0xFFF;
  }
  if (PageEnd <= PageStart) {
    return 0;
  }
  BuildMemoryAllocationHob(
      PageStart, PageEnd - PageStart, EfiReservedMemoryType);
  return 1;
}

//
// Reserve every part of a Conventional span that the FDT does not report in
// /memory.  The static table's low window is fixed by the SoC (RAM top
// 0xE0000000 on COR-AL00, 0xC0000000 on BLA-AL00) and is declared
// unconditionally, and the FDT filler above can only ever add memory -- it can
// never take any away.  So if a SKU's FDT declares a smaller low bank than the
// table assumes, the difference would be handed to DXE as free memory sitting
// on top of a physical hole and the first allocation there would fault.  With
// the tables and the bootloader as shipped every span is fully covered by
// /memory, so this normally reserves nothing; it only bites when the two
// disagree.
//
STATIC
UINTN ReserveFdtMemoryOvermap(
    IN fdt *Fdt, IN CONST EFI_PHYSICAL_ADDRESS *Span, IN UINTN SpanCount)
{
  struct {
    UINT64 Base;
    UINT64 End;
  } Merged[MAX_FDT_MEM];
  UINTN Count = 0;
  UINTN Used = 0;
  UINTN Index;
  UINTN I;
  UINTN Reserved = 0;
  uint64_t Base;
  uint64_t Size;
  int Node = 0;

  if (Fdt == NULL || SpanCount < 2) {
    return 0;
  }

  while (Count < MAX_FDT_MEM &&
         fdt_get_memory(Fdt, Node, &Base, &Size)) {
    if (Size != 0 && Base <= MAX_UINT64 - Size) {
      Merged[Count].Base = Base;
      Merged[Count].End = Base + Size;
      Count++;
    }
    Node++;
  }
  if (Count == 0) {
    return 0;
  }

  // Sort ascending by base; Count is at most MAX_FDT_MEM.
  for (I = 1; I < Count; I++) {
    UINT64 B = Merged[I].Base;
    UINT64 E = Merged[I].End;
    UINTN J = I;

    while (J > 0 && Merged[J - 1].Base > B) {
      Merged[J].Base = Merged[J - 1].Base;
      Merged[J].End = Merged[J - 1].End;
      J--;
    }
    Merged[J].Base = B;
    Merged[J].End = E;
  }
  // Fold overlapping / abutting regions so the walk below is monotonic.
  Used = 1;
  for (I = 1; I < Count; I++) {
    if (Merged[I].Base <= Merged[Used - 1].End) {
      if (Merged[I].End > Merged[Used - 1].End) {
        Merged[Used - 1].End = Merged[I].End;
      }
    } else {
      Merged[Used++] = Merged[I];
    }
  }

  for (Index = 0; Index + 1 < SpanCount; Index += 2) {
    UINT64 Lo;
    UINT64 Hi;
    UINT64 Cur;

    if (Span[Index] > MAX_UINT64 - Span[Index + 1]) {
      continue;
    }
    Lo = Span[Index];
    Hi = Lo + Span[Index + 1];
    Cur = Lo;

    for (I = 0; I < Used && Cur < Hi; I++) {
      UINT64 From;
      UINT64 To;

      if (Merged[I].End <= Cur) {
        continue;
      }
      if (Merged[I].Base >= Hi) {
        // Runs past the span; whatever is left of the span is one hole.
        break;
      }
      From = (Merged[I].Base > Cur) ? Merged[I].Base : Cur;
      To = (Merged[I].End < Hi) ? Merged[I].End : Hi;
      if (From > Cur) {
        Reserved += ReservePages(Cur, From);
      }
      if (To > Cur) {
        Cur = To;
      }
    }
    if (Cur < Hi) {
      Reserved += ReservePages(Cur, Hi);
    }
  }

  return Reserved;
}

//
// Pull the no-map firmware out of the Conventional regions we just declared.
//
// CoreInitializeGcdServices() turns every SystemMemory GCD range that no
// memory-allocation HOB claimed into EfiConventionalMemory, i.e. free DXE
// memory.  On Kirin970 the static table declares HLOS 2 / HLOS 3 /
// Remaining RAM as one big Conv blob, so without this the DXE page allocator
// happily hands out the hypervisor (hhee), the secure OS (secos), the modem,
// the LPMCU and the blackbox -- all of which are no-map, stay resident while
// we run, and take the whole machine down when stomped.  Intersecting with
// the Conv spans we actually created keeps the carve strictly inside RAM we
// own, so MMIO / Reserv / BsData rows are never touched.
//
STATIC
UINTN CarveFdtNoMapReserved(
    IN fdt *Fdt, IN CONST EFI_PHYSICAL_ADDRESS *Span, IN UINTN SpanCount)
{
  int Parent;
  int Node;
  int Ac;
  int Sc;
  int Pos;
  int Len;
  int EntryBytes;
  UINTN Index;
  UINTN Carved = 0;
  CONST fdt32_t *Reg;

  if (Fdt == NULL || SpanCount < 2) {
    return 0;
  }

  Parent = fdt_path_offset(Fdt, "/reserved-memory");
  if (Parent >= 0) {
    // Cells describe the bus, i.e. the /reserved-memory node itself.
    Ac = fdt_address_cells(Fdt, Parent);
    Sc = fdt_size_cells(Fdt, Parent);
    if (Ac >= 1 && Ac <= 2 && Sc >= 1 && Sc <= 2) {
      EntryBytes = (Ac + Sc) * (int)sizeof(fdt32_t);

      for (Node = fdt_first_subnode(Fdt, Parent); Node >= 0;
           Node = fdt_next_subnode(Fdt, Node)) {
        // status is deliberately NOT consulted here.  Linux would skip a
        // "disabled"/"disable" child, but these regions are physical facts
        // about the SoC -- badpage-one/two are dead DRAM, dmss_pt is the
        // modem's page-table scratch, supersonic/modem-socp/modem-sa are the
        // modem's carve-outs -- and handing any of them to the DXE page
        // allocator produces the classic intermittent crash.  Everything is
        // intersected with the Conv spans below, so a node that does not fall
        // in RAM we own (dmss_pt at 0x1_4000_0000 on a 4 GiB SKU) simply
        // carves nothing.
        //
        // reusable/CMA pools are legal for others to use, only no-map is off
        // limits.
        if (fdt_getprop(Fdt, Node, "no-map", NULL) == NULL) {
          continue;
        }
        Reg = fdt_getprop(Fdt, Node, "reg", &Len);
        if (Reg == NULL || Len < EntryBytes) {
          continue;
        }

        for (Pos = 0; Pos + EntryBytes <= Len; Pos += EntryBytes) {
          UINT64 Start = 0;
          UINT64 Size = 0;
          INTN Cell;

          for (Cell = 0; Cell < Ac; Cell++) {
            Start = (Start << 32) | (UINT64)fdt32_to_cpu(
                        Reg[(Pos / 4) + (int)Cell]);
          }
          for (Cell = 0; Cell < Sc; Cell++) {
            Size = (Size << 32) | (UINT64)fdt32_to_cpu(
                       Reg[(Pos / 4) + Ac + (int)Cell]);
          }
          if (Size == 0 || Start > MAX_UINT64 - Size) {
            continue;
          }

          for (Index = 0; Index + 1 < SpanCount; Index += 2) {
            UINT64 Base = Span[Index];
            UINT64 Limit = Base + Span[Index + 1];
            UINT64 From = (Start > Base) ? Start : Base;
            UINT64 To = (Start + Size < Limit) ? (Start + Size) : Limit;

            if (From >= To) {
              continue;
            }
            Carved += ReservePages(From, To);
          }
        }
      }
    }
  }

  return Carved;
}

/*++

Routine Description:



Arguments:

  FileHandle  - Handle of the file being invoked.
  PeiServices - Describes the list of possible PEI Services.

Returns:

  Status -  EFI_SUCCESS if the boot mode could be set

--*/
EFI_STATUS
EFIAPI
MemoryPeim(IN EFI_PHYSICAL_ADDRESS UefiMemoryBase, IN UINT64 UefiMemorySize)
{

  PARM_MEMORY_REGION_DESCRIPTOR_EX MemoryDescriptorEx =
      GetPlatformMemoryMap();
  ARM_MEMORY_REGION_DESCRIPTOR
        MemoryDescriptor[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];
  UINTN Index = 0;
  UINTN Node = 0;
  UINTN MemoryBase = 0;
  UINTN MemorySize = 0;
  UINTN MemoryTotal = 0;
  DeviceMemoryAddHob Mem = Mem4G;
  UINT8 MemGB = 4;
  fdt *Fdt;
  PARM_MEMORY_REGION_DESCRIPTOR_EX Probe;
  UINT64 MapLowTop = 0;
  UINT64 MapMmioTop = 0;
  EFI_PHYSICAL_ADDRESS ConvSpan[MAX_CONV_SPAN];
  UINTN ConvCount = 0;
  BOOLEAN FdtMemory;

  //
  // PcdFdtMemoryEnable is the master switch for everything the bootloader's
  // device tree tells us about memory.  Off = legacy path: the static platform
  // table above is the only source, no filler, no carve, no over-map reserve.
  //
  FdtMemory = PcdGetBool(PcdFdtMemoryEnable);
  Fdt = NULL;
  if (FdtMemory) {
    Fdt = GetFdt();
    if (Fdt == NULL) {
      DEBUG((EFI_D_ERROR, "KirinMem: no FDT, static map only\n"));
    }
  } else {
    DEBUG((EFI_D_ERROR, "KirinMem: FDT memory off, static map only\n"));
  }

  while (Fdt != NULL &&
         fdt_get_memory(Fdt, (int)Node, (uint64_t*)&MemoryBase, (uint64_t*)&MemorySize)) {
    MemoryTotal += MemorySize;
    DEBUG((
      EFI_D_ERROR,
      "KirinMem: bank %-2d 0x%016llx - 0x%016llx (%lld MB)\n",
      Node, MemoryBase, (MemoryBase + MemorySize), MemorySize / SIZE_MB
    ));
    Node++;
  }

  // Memory   Min    Max   Config
  SIZE_MB_IN (3072,  4608, 4);
  SIZE_MB_IN (5120,  6656, 6);
  SIZE_MB_IN (7168,  8704, 8);
  SIZE_MB_IN (9216, 10752, 10);
  SIZE_MB_IN (11520, 12488, 12);

  if (FdtMemory) {
    DEBUG((EFI_D_ERROR,
           "KirinMem: FDT @0x%lx sz 0x%x, memory 0x%lx (%d GiB) -> cfg %dG\n",
           (UINT64)(UINTN)Fdt, (Fdt != NULL) ? (UINT32)fdt_totalsize(Fdt) : 0,
           (UINT64)MemoryTotal, (INT32)(MemoryTotal / SIZE_GB), (INT32)MemGB));
  }

  // Run through each memory descriptor
  while (MemoryDescriptorEx->Length != 0) {
    switch (MemoryDescriptorEx->HobOption) {
    case Mem4G:
    case Mem6G:
    case Mem8G:
    case Mem10G:
    case Mem12G:
      if (MemoryDescriptorEx->HobOption != Mem) {
        MemoryDescriptorEx++;
        continue;
      }
      // fallthrough
    case AddMem:
    case AddDev:
    case HobOnlyNoCacheSetting:
      AddHob(MemoryDescriptorEx);
      if (MemoryDescriptorEx->ResourceType == SYS_MEM &&
          MemoryDescriptorEx->MemoryType == Conv &&
          ConvCount + 1 < MAX_CONV_SPAN)
      {
        ConvSpan[ConvCount++] = MemoryDescriptorEx->Address;
        ConvSpan[ConvCount++] = MemoryDescriptorEx->Length;
      }
      break;
    case NoHob:
    default:
      goto update;
    }

    if (MemoryDescriptorEx->HobOption == HobOnlyNoCacheSetting) {
      MemoryDescriptorEx++;
      continue;
    }

  update:
    ASSERT(Index < MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT);

    MemoryDescriptor[Index].PhysicalBase = MemoryDescriptorEx->Address;
    MemoryDescriptor[Index].VirtualBase  = MemoryDescriptorEx->Address;
    MemoryDescriptor[Index].Length       = MemoryDescriptorEx->Length;
    MemoryDescriptor[Index].Attributes   = MemoryDescriptorEx->ArmAttributes;

    Index++;
    MemoryDescriptorEx++;
  }

  //
  // Kirin970 ships as 4G/6G/8G.  The DDR window below 4G is fixed by the SoC
  // (MMIO starts at 0xE8100000) and is fully described by the static table
  // above, but the remaining bank sits at a SKU dependent address above 4G --
  // a 4GB COR-AL00 declares 0x2_0000_0000 + 0x20000000 in its FDT.  SIZE_MB_IN
  // already picked the total from the FDT, so now map whatever the bootloader
  // declares that the static table does not already cover.  That is the
  // Kirin970 equivalent of mt6853's per-size rows: one build serves 4G, 6G
  // and 8G without hardcoding bank addresses.  The low half is clamped to the
  // table's first MMIO window so RAM can never be mapped over a peripheral.
  //
  Probe = GetPlatformMemoryMap();
  while (Probe->Length != 0) {
    switch (Probe->HobOption) {
    case Mem4G:
    case Mem6G:
    case Mem8G:
    case Mem10G:
    case Mem12G:
      if (Probe->HobOption != Mem) {
        break;
      }
      // fallthrough
    case AddMem:
    case AddDev:
    case HobOnlyNoCacheSetting:
      if (Probe->ResourceType == SYS_MEM) {
        if (Probe->Address + Probe->Length > MapLowTop)
          MapLowTop = Probe->Address + Probe->Length;
      } else if (Probe->ResourceType == MMAP_IO) {
        if (MapMmioTop == 0 || Probe->Address < MapMmioTop)
          MapMmioTop = Probe->Address;
      }
      break;
    default:
      break;
    }
    Probe++;
  }
  if (MapMmioTop == 0)
    MapMmioTop = 0x100000000ULL;

  if (Fdt != NULL) {
    Node = 0;
    while (fdt_get_memory(Fdt, (int)Node, (uint64_t*)&MemoryBase, (uint64_t*)&MemorySize)) {
      UINT64 RegionStart = (UINT64)MemoryBase;
      UINT64 RegionEnd = RegionStart + (UINT64)MemorySize;
      UINT64 Window[2][2];
      UINT32 W;

      // Window 0: everything below 4G, cut off at the static MMIO base.
      Window[0][0] = RegionStart;
      Window[0][1] = RegionEnd < 0x100000000ULL ? RegionEnd : 0x100000000ULL;
      // Window 1: everything at or above 4G, no MMIO lives up there.
      Window[1][0] = RegionStart > 0x100000000ULL ? RegionStart : 0x100000000ULL;
      Window[1][1] = RegionEnd;

      for (W = 0; W < 2; W++) {
        ARM_MEMORY_REGION_DESCRIPTOR_EX Extra;
        UINT64 Start = Window[W][0];
        UINT64 End = Window[W][1];

        if (Start < MapLowTop)
          Start = MapLowTop;
        if (W == 0 && End > MapMmioTop)
          End = MapMmioTop;
        if (End <= Start)
          continue;
        if (Index >= MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT - 1)
          break;

        Extra.Name[0] = '\0';
        Extra.Address = Start;
        Extra.Length = End - Start;
        Extra.HobOption = AddMem;
        Extra.ResourceType = SYS_MEM;
        Extra.ResourceAttribute = SYS_MEM_CAP;
        Extra.MemoryType = Conv;
        Extra.ArmAttributes = WRITE_BACK_XN;
        AddHob(&Extra);

        MemoryDescriptor[Index].PhysicalBase = Start;
        MemoryDescriptor[Index].VirtualBase  = Start;
        MemoryDescriptor[Index].Length       = End - Start;
        MemoryDescriptor[Index].Attributes   = WRITE_BACK_XN;
        Index++;

        if (ConvCount + 1 < MAX_CONV_SPAN) {
          ConvSpan[ConvCount++] = Start;
          ConvSpan[ConvCount++] = End - Start;
        }

        DEBUG((EFI_D_ERROR, "KirinMem: +RAM 0x%016lx + 0x%016lx\n",
               Start, End - Start));
      }
      Node++;
    }
  }

  //
  // Reconcile with the FDT.  Must run last so the FDT-derived spans are
  // included too.  First take back whatever the static table calls
  // Conventional but the bootloader does not call memory at all, then pull the
  // no-map firmware back out of what is left.  Both only work because the
  // Conventional rows above no longer carry a memory-allocation HOB (see
  // AddHob): a later EfiReservedMemoryType HOB can only claim GCD space that
  // is still unallocated.
  //
  if (Fdt != NULL) {
    UINTN Overmap = ReserveFdtMemoryOvermap(Fdt, ConvSpan, ConvCount);
    UINTN Carved = CarveFdtNoMapReserved(Fdt, ConvSpan, ConvCount);

    DEBUG((
        EFI_D_ERROR,
        "KirinMem: %u FDT no-map carve(s), %u over-map reserve(s)\n",
        (UINT32)Carved, (UINT32)Overmap));
  }

  // Last one (terminator)
  ASSERT(Index < MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT);
  MemoryDescriptor[Index].PhysicalBase = 0;
  MemoryDescriptor[Index].VirtualBase  = 0;
  MemoryDescriptor[Index].Length       = 0;
  MemoryDescriptor[Index].Attributes   = 0;

  // Build Memory Allocation Hob
  DEBUG((EFI_D_INFO, "\nConfigure MMU In \n"));
  InitMmu(MemoryDescriptor);
  DEBUG((EFI_D_INFO, "Configure MMU Out \n"));

  if (FeaturePcdGet(PcdPrePiProduceMemoryTypeInformationHob)) {
    // Optional feature that helps prevent EFI memory map fragmentation.
    BuildMemoryTypeInformationHob();
  }

  return EFI_SUCCESS;
}