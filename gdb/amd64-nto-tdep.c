/* Target-dependent code for QNX Neutrino x86_64.

   Copyright (C) 2014 Free Software Foundation, Inc.

   Contributed by QNX Software Systems Ltd.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "defs.h"
#include "frame.h"
#include "osabi.h"
#include "regset.h"
#include "regcache.h"
#include "target.h"

#include "nto-tdep.h"
#include "amd64-tdep.h"

#include "gdb_assert.h"

#include "nto-tdep.h"
#include "solib.h"
#include "solib-svr4.h"
#include "amd64-nto-tdep.h"
#include "i387-tdep.h"
#include "x86-xstate.h"

/* CPU capability/state flags from x86_64/syspage.h */
#define X86_64_CPU_FXSR        (1UL <<  12)  /* CPU supports FXSAVE/FXRSTOR  */
#define X86_64_CPU_XSAVE       (1UL <<  17)  /* CPU supports XSAVE/XRSTOR */

/* Registers.  */
#define AMD64_GREGSZ 8U

static int
x86_64nto_gregset_reg_offset[] =
{
  X86_64_RAX * AMD64_GREGSZ,
  X86_64_RBX * AMD64_GREGSZ,
  X86_64_RCX * AMD64_GREGSZ,
  X86_64_RDX * AMD64_GREGSZ,
  X86_64_RSI * AMD64_GREGSZ,
  X86_64_RDI * AMD64_GREGSZ,
  X86_64_RBP * AMD64_GREGSZ,
  X86_64_RSP * AMD64_GREGSZ,
  X86_64_R8  * AMD64_GREGSZ,
  X86_64_R9  * AMD64_GREGSZ,
  X86_64_R10 * AMD64_GREGSZ,
  X86_64_R11 * AMD64_GREGSZ,
  X86_64_R12 * AMD64_GREGSZ,
  X86_64_R13 * AMD64_GREGSZ,
  X86_64_R14 * AMD64_GREGSZ,
  X86_64_R15 * AMD64_GREGSZ,
  X86_64_RIP * AMD64_GREGSZ,
  X86_64_RFLAGS * AMD64_GREGSZ,
  -1, /* CS */
  -1, /* SS */
  -1, /* DS */
  -1, /* ES */  /* Our gregset context ends here. */
  -1, /* FS */
  -1 /* GS */
};

#define X86_64NTO_NUM_GREGS  20

/* TODO: Sigtramp stuff... see aarch64-nto-tdep.c */

/* Given a GDB register number REGNUM, return the offset into
   Neutrino's register structure or -1 if the register is unknown.  */

static int
nto_reg_offset (int regnum)
{
  if (regnum >= 0 && regnum < ARRAY_SIZE (x86_64nto_gregset_reg_offset))
    return x86_64nto_gregset_reg_offset[regnum];

  return -1;
}

static void
amd64_nto_supply_fpregset (const struct regset *regset,
                       struct regcache *regcache,
           int regnum, const void *fpregs, size_t len)
{
  if (len > I387_SIZEOF_FXSAVE)
    amd64_supply_xsave (regcache, regnum, fpregs);
  else
    amd64_supply_fxsave (regcache, regnum, fpregs);
}

static void
amd64_nto_collect_fpregset (const struct regset *regset,
      const struct regcache *regcache,
      int regnum, void *fpregs, size_t len)
{
  if (len > I387_SIZEOF_FXSAVE)
    amd64_collect_xsave (regcache, regnum, fpregs, 0);
  else
    amd64_collect_fxsave (regcache, regnum, fpregs);
}

static const struct regset amd64_nto_fpregset =
  {
    NULL, amd64_nto_supply_fpregset, amd64_nto_collect_fpregset, REGSET_VARIABLE_SIZE
  };

static void
amd64_nto_iterate_over_regset_sections (struct gdbarch *gdbarch,
            iterate_over_regset_sections_cb *cb,
            void *cb_data,
            const struct regcache *regcache)
{
  const struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  cb (".reg", tdep->sizeof_gregset, tdep->sizeof_gregset, &i386_gregset, NULL, cb_data);
  cb (".reg2", tdep->sizeof_fpregset, tdep->sizeof_fpregset, &amd64_nto_fpregset, NULL, cb_data);
}

static void
x86_64nto_supply_gregset (struct regcache *regcache, const gdb_byte *gregs, size_t len)
{
  i386_gregset.supply_regset (&i386_gregset, regcache, -1, gregs, len);
}

static void
x86_64nto_supply_fpregset (struct regcache *regcache, const gdb_byte *fpregs, size_t len)
{
  amd64_nto_fpregset.supply_regset (&amd64_nto_fpregset, regcache, -1, fpregs, len);
}

static void
x86_64nto_supply_regset (struct regcache *regcache, int regset, const gdb_byte *data, size_t len)
{
  switch (regset)
    {
    case NTO_REG_GENERAL:
      x86_64nto_supply_gregset (regcache, data, len);
      break;
    case NTO_REG_FLOAT:
      x86_64nto_supply_fpregset (regcache, data, len);
      break;
    default:
      gdb_assert (!"Unknown regset");
    }
}

static int
x86_64nto_regset_id (int regno)
{
  if (regno == -1)
    return NTO_REG_END;
  else if (regno < AMD64_ST0_REGNUM)
    return NTO_REG_GENERAL;
  else if (regno < AMD64_NUM_REGS)
    return NTO_REG_FLOAT;

  return -1;      /* Error.  */
}

static int
x86_64nto_register_area (int regset, unsigned cpuflags)
{
  if (regset == NTO_REG_GENERAL)
  return X86_64NTO_NUM_GREGS * AMD64_GREGSZ;
  else if (regset == NTO_REG_FLOAT)
    {
      if (cpuflags & X86_64_CPU_XSAVE)
  {
    /* At most DS_DATA_MAX_SIZE: */
    return 1024;
  }
      else if (cpuflags & X86_64_CPU_FXSR)
    return 512;
      else
    return 108;
    }
  else
    {
      warning(_("Only general and floatpoint registers supported on x86_64 for now\n"));
    }
  return -1;
}

static int
x86_64nto_regset_fill (const struct regcache *const regcache,
           const int regset, gdb_byte *const data,
           size_t len)
{
  if (regset == NTO_REG_GENERAL)
    {
      int regno;

      for (regno = 0; regno < AMD64_ST0_REGNUM; regno++)
  {
    const int offset = nto_reg_offset (regno);
    if (offset != -1)
      regcache->raw_collect (regno, data + offset);
  }
      return 0;
    }
  else if (regset == NTO_REG_FLOAT)
    {
      if (len > I387_SIZEOF_FXSAVE)
        amd64_collect_xsave (regcache, -1, data, 0);
      else
        amd64_collect_fxsave (regcache, -1, data);
      return 0;
    }
  return -1; // Error
}

/* Return whether THIS_FRAME corresponds to a QNX Neutrino sigtramp
   routine.  */

static int
amd64nto_sigtramp_p (struct frame_info *this_frame)
{
  CORE_ADDR pc = get_frame_pc (this_frame);
  const char *name;

  find_pc_partial_function (pc, &name, NULL, NULL);

  return name && strcmp ("__signalstub", name) == 0;
}

/* Assuming THIS_FRAME is a QNX Neutrino sigtramp routine, return the
   address of the associated sigcontext structure.  */

static CORE_ADDR
amd64nto_sigcontext_addr (struct frame_info *this_frame)
{
  struct gdbarch *gdbarch = get_frame_arch (this_frame);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  gdb_byte buf[AMD64_GREGSZ];
  CORE_ADDR ptrctx;

  get_frame_register (this_frame, AMD64_R12_REGNUM, buf);
  ptrctx = extract_unsigned_integer (buf, AMD64_GREGSZ, byte_order);
  ptrctx += 0x28 /* Context pointer is at this offset.  */;

  return ptrctx;
}

static int
amd64nto_breakpoint_size (CORE_ADDR addr)
{
  return 0;
}

static const struct target_desc *
amd64nto_read_description(unsigned cpuflags)
{
  /* With a lazy allocation of the fpu context we cannot easily tell
     up-front what the target supports, so set an upper bound on the
     features. */
  return amd64_target_description(X86_XSTATE_AVX_MASK,1);
}

static const struct target_desc *
amd64nto_core_read_description (struct gdbarch *gdbarch,
          struct target_ops *target,
          bfd *abfd)
{
  /* We could pull xcr0 from the corefile, but just keep things
     consistent with amd64nto_read_description() */
  return amd64_target_description(X86_XSTATE_AVX_MASK,1);
}


static struct nto_target_ops amd64_nto_ops;

static void
init_x86_64nto_ops (void)
{
  amd64_nto_ops.regset_id = x86_64nto_regset_id;
  amd64_nto_ops.supply_gregset = x86_64nto_supply_gregset;
  amd64_nto_ops.supply_fpregset = x86_64nto_supply_fpregset;
  amd64_nto_ops.supply_altregset = nto_dummy_supply_regset;
  amd64_nto_ops.supply_regset = x86_64nto_supply_regset;
  amd64_nto_ops.register_area = x86_64nto_register_area;
  amd64_nto_ops.regset_fill = x86_64nto_regset_fill;
  amd64_nto_ops.fetch_link_map_offsets =
    nto_generic_svr4_fetch_link_map_offsets;
  amd64_nto_ops.breakpoint_size = amd64nto_breakpoint_size;
  amd64_nto_ops.read_description = amd64nto_read_description;
}

static void
amd64_nto_init_abi (struct gdbarch_info info, struct gdbarch *gdbarch)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  static struct target_so_ops nto_svr4_so_ops;
  struct nto_target_ops *nto_ops;

  init_x86_64nto_ops ();
  /* do generic initialization */
  nto_init_abi( info, gdbarch );

  tdep->gregset_reg_offset = x86_64nto_gregset_reg_offset;
  tdep->gregset_num_regs = ARRAY_SIZE (x86_64nto_gregset_reg_offset);
  tdep->sizeof_gregset = X86_64NTO_NUM_GREGS * AMD64_GREGSZ;

//  gdb_assert (tdep->gregset_num_regs == AMD64_NUM_GREGS);

  tdep->sigtramp_p = amd64nto_sigtramp_p;
  tdep->sigcontext_addr = amd64nto_sigcontext_addr;
  tdep->sc_reg_offset = x86_64nto_gregset_reg_offset;
  tdep->sc_num_regs = ARRAY_SIZE (x86_64nto_gregset_reg_offset);

  /* Deal with our strange signals.  */
  nto_initialize_signals ();

  /* NTO uses ELF.  */
  amd64_init_abi (info, gdbarch, amd64_target_description(X86_XSTATE_AVX_MASK, 1) );

  nto_ops = (struct nto_target_ops *) gdbarch_data (gdbarch, nto_gdbarch_ops);
  *nto_ops = amd64_nto_ops;

#if 0
  /* Neutrino rewinds to look more normal.  Need to override the i386
     default which is [unfortunately] to decrement the PC.  */
  //set_gdbarch_decr_pc_after_break (gdbarch, 0);

  tdep->gregset_reg_offset = i386nto_gregset_reg_offset;
  tdep->gregset_num_regs = ARRAY_SIZE (i386nto_gregset_reg_offset);
  tdep->sizeof_gregset = NUM_GPREGS * 4;

  tdep->sigtramp_p = i386nto_sigtramp_p;
  tdep->sigcontext_addr = i386nto_sigcontext_addr;
  tdep->sc_reg_offset = i386nto_gregset_reg_offset;
  tdep->sc_num_regs = ARRAY_SIZE (i386nto_gregset_reg_offset);

  /* Setjmp()'s return PC saved in EDX (5).  */
  tdep->jb_pc_offset = 20;  /* 5x32 bit ints in.  */

#endif

  set_solib_svr4_fetch_link_map_offsets
    (gdbarch, nto_generic_svr4_fetch_link_map_offsets);

  /* Initialize this lazily, to avoid an initialization order
     dependency on solib-svr4.c's _initialize routine.  */
  if (nto_svr4_so_ops.in_dynsym_resolve_code == NULL)
    {
      nto_svr4_so_ops = svr4_so_ops;

      /* Our loader handles solib relocations differently than svr4.  */
      nto_svr4_so_ops.relocate_section_addresses
        = nto_relocate_section_addresses;

      /* Supply a nice function to find our solibs.  */
      nto_svr4_so_ops.find_and_open_solib
        = nto_find_and_open_solib;

      /* Our linker code is in libc.  */
      nto_svr4_so_ops.in_dynsym_resolve_code
        = nto_in_dynsym_resolve_code;
    }
  set_solib_ops (gdbarch, &nto_svr4_so_ops);

  set_gdbarch_get_siginfo_type (gdbarch, nto_get_siginfo_type);

  set_gdbarch_gdb_signal_to_target (gdbarch, nto_gdb_signal_to_target);
  set_gdbarch_gdb_signal_from_target (gdbarch, nto_gdb_signal_from_target);

  set_gdbarch_iterate_over_regset_sections
    (gdbarch, amd64_nto_iterate_over_regset_sections);
  set_gdbarch_core_read_description (gdbarch,
             amd64nto_core_read_description);
}


extern initialize_file_ftype _initialize_x86_64nto_tdep;

void
_initialize_x86_64nto_tdep (void)
{
  gdbarch_register_osabi (bfd_arch_i386, bfd_mach_x86_64,
        GDB_OSABI_QNXNTO, amd64_nto_init_abi);
}
