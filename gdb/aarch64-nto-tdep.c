/* Target-dependent code for QNX Neutrino aarch64.

   Copyright (C) 2014-2019 Free Software Foundation, Inc.

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
#include "tramp-frame.h"
#include "trad-frame.h"

#include "nto-tdep.h"
#include "aarch64-tdep.h"

#include "gdb_assert.h"

#include "nto-tdep.h"
#include "solib.h"
#include "solib-svr4.h"
#include "aarch64-nto-tdep.h"

#define AARCH64_GREGSZ 8U
#define AARCH64_FPREGSZ 16U
#define AARCH64_FPSRSZ 4U
#define AARCH64_FPCRSZ 4U


static void
aarch64_nto_supply_gregset (struct regcache *regcache, const gdb_byte *gregs,
         size_t len)
{
  int regno;

  for (regno = AARCH64_X0_REGNUM; regno <= AARCH64_CPSR_REGNUM; regno++)
    regcache->raw_supply (regno, gregs + AARCH64_GREGSZ * regno);
}

static void
aarch64_nto_supply_fpregset (struct regcache *regcache, const gdb_byte *fpregs,
          size_t len)
{
  int regno;

  for (regno = AARCH64_V0_REGNUM; regno <= AARCH64_V31_REGNUM; regno++)
    regcache->raw_supply (regno, fpregs + AARCH64_FPREGSZ
             * (regno - AARCH64_V0_REGNUM));

  regcache->raw_supply (AARCH64_FPSR_REGNUM,
           fpregs + AARCH64_FPREGSZ * 32);
  regcache->raw_supply (AARCH64_FPCR_REGNUM,
           fpregs + AARCH64_FPREGSZ * 32 + AARCH64_FPSRSZ);
}

static void
aarch64_nto_supply_regset (struct regcache *regcache, int regset,
        const gdb_byte *data, size_t len)
{
  switch (regset)
    {
    case NTO_REG_GENERAL:
      aarch64_nto_supply_gregset (regcache, data, len);
      break;
    case NTO_REG_FLOAT:
      aarch64_nto_supply_fpregset (regcache, data, len);
      break;
    default:
      gdb_assert (!"Unknown regset");
    }
}

static int
aarch64_nto_regset_id (int regno)
{
  if (regno == -1)
    return NTO_REG_END;
  else if (regno < AARCH64_V0_REGNUM)
    return NTO_REG_GENERAL;
  else if (regno <= AARCH64_FPCR_REGNUM )
    return NTO_REG_FLOAT;

  return -1;
}

static int
aarch64_nto_register_area (int regset, unsigned cpuflags)
{
  if (regset == NTO_REG_GENERAL)
      return AARCH64_GREGSZ * AARCH64_V0_REGNUM;
  else if (regset == NTO_REG_FLOAT)
      return sizeof (AARCH64_FPU_REGISTERS);
  else
      warning(_("Only general and floatpoint registers supported on aarch64 for now\n"));
  return -1;
}

static int
aarch64_nto_regset_fill (const struct regcache *const regcache,
      const int regset, gdb_byte *const data,
      size_t len)
{
  if (regset == NTO_REG_GENERAL)
    {
      int regno;

      for (regno = 0; regno <= AARCH64_CPSR_REGNUM; regno++)
          regcache->raw_collect (regno, data + AARCH64_GREGSZ * regno);
      return 0;
    }
  else if (regset == NTO_REG_FLOAT)
    {
      int regno;

      for (regno = AARCH64_V0_REGNUM; regno <= AARCH64_V31_REGNUM; regno++)
          regcache->raw_collect (regno,
              data + (regno - AARCH64_V0_REGNUM) * AARCH64_FPREGSZ);
      regcache->raw_collect (AARCH64_FPSR_REGNUM,
          data + 32 * AARCH64_FPREGSZ);
      regcache->raw_collect (AARCH64_FPCR_REGNUM,
          data + 32 * AARCH64_FPREGSZ + AARCH64_FPSRSZ);
      return 0;
    }
  return -1;
}

/* Implement the "init" method of struct tramp_frame.  */
/* todo: these are the BSD offsets, make sure these are correct */
#define AARCH64_SIGFRAME_UCONTEXT_OFFSET  80
#define AARCH64_UCONTEXT_MCONTEXT_OFFSET  16
#define AARCH64_MCONTEXT_FPREGS_OFFSET    272
#define AARCH64_MCONTEXT_FLAGS_OFFSET     800

static void
aarch64_nto_sigframe_init (const struct tramp_frame *self,
           struct frame_info *this_frame,
           struct trad_frame_cache *this_cache,
           CORE_ADDR func)
{
  struct gdbarch *gdbarch = get_frame_arch (this_frame);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);

  CORE_ADDR sp = get_frame_register_unsigned (this_frame, AARCH64_SP_REGNUM);
  CORE_ADDR mcontext_addr = get_frame_register_unsigned (this_frame, AARCH64_PC_REGNUM );
  gdb_byte buf[4];
  int i;

  nto_trace(0)("Initializing sigframe at %lx\n", func);

  /* fill in the gregs */
  for (i = 0; i < AARCH64_X_REGS_NUM; i++)
    {
      trad_frame_set_reg_addr (this_cache, AARCH64_X0_REGNUM+i,
          mcontext_addr + i * AARCH64_GREGSZ);
    }

  /* fill in status registers */
  trad_frame_set_reg_addr (this_cache, AARCH64_SP_REGNUM,
     mcontext_addr + AARCH64_SP_REGNUM * AARCH64_GREGSZ);
  trad_frame_set_reg_addr (this_cache, AARCH64_PC_REGNUM,
     mcontext_addr + AARCH64_PC_REGNUM * AARCH64_GREGSZ);
  trad_frame_set_reg_addr (this_cache, AARCH64_CPSR_REGNUM,
     mcontext_addr + AARCH64_CPSR_REGNUM * AARCH64_GREGSZ);

  /* fill in FP Registers */
  if (target_read_memory (mcontext_addr + AARCH64_MCONTEXT_FLAGS_OFFSET, buf,
        4) == 0
        && (extract_unsigned_integer (buf, 4, byte_order) & AARCH64_FPVALID ) )
    {
      for (i = 0; i < AARCH64_V_REGS_NUM; i++)
        {
          trad_frame_set_reg_addr (this_cache, AARCH64_V0_REGNUM + i,
               mcontext_addr
               + AARCH64_MCONTEXT_FPREGS_OFFSET
               + i * AARCH64_FPREGSZ);
        }
      trad_frame_set_reg_addr (this_cache, AARCH64_FPSR_REGNUM,
             mcontext_addr + AARCH64_MCONTEXT_FPREGS_OFFSET
             + 32 * AARCH64_FPREGSZ);
      trad_frame_set_reg_addr (this_cache, AARCH64_FPCR_REGNUM,
             mcontext_addr + AARCH64_MCONTEXT_FPREGS_OFFSET
             + 32 * AARCH64_FPREGSZ + 4);
    }

  /* add frame to the list */
  trad_frame_set_id (this_cache, frame_id_build (sp, func));
}

#if 0
/* todo this is never called */
/* Assuming THIS_FRAME is a QNX Neutrino sigtramp routine, return the
   address of the associated sigcontext structure.  */
static CORE_ADDR
aarch64_nto_sigcontext_addr (struct frame_info *this_frame)
{
  struct gdbarch *gdbarch = get_frame_arch (this_frame);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  gdb_byte buf[AARCH64_GREGSZ];
  CORE_ADDR ptrctx;

  get_frame_register (this_frame, AARCH64_PC_REGNUM, buf);
  ptrctx = extract_unsigned_integer (buf, AARCH64_GREGSZ, byte_order);

  return ptrctx;
}
#endif


#ifdef __NTO_SIGTRAMP_VALIDATE__
/* Return whether THIS_FRAME corresponds to a QNX Neutrino sigtramp
   routine.  */
static int
aarch64_nto_sigframe_validate (const struct tramp_frame *self,
       struct frame_info *this_frame,
       CORE_ADDR *pc)
{
  const char *name;
  find_pc_partial_function (*pc, &name, NULL, NULL);
  nto_trace(0)("Check sigframe %lx - %s\n", *pc, name);

  return name && strcmp ("__signalstub", name) == 0;
}
#endif

static const struct tramp_frame aarch64_nto_sigframe =
{
  /* The trampoline's type, some are signal trampolines, some are normal
     call-frame trampolines (aka thunks).  */
  SIGTRAMP_FRAME,
  /* The trampoline's entire instruction sequence.  It consists of a
     bytes/mask pair.  Search for this in the inferior at or around
     the frame's PC.  It is assumed that the PC is INSN_SIZE aligned,
     and that each element of TRAMP contains one INSN_SIZE
     instruction.  It is also assumed that INSN[0] contains the first
     instruction of the trampoline and hence the address of the
     instruction matching INSN[0] is the trampoline's "func" address.
     The instruction sequence is terminated by
     TRAMP_SENTINEL_INSN.  */
  8,
  {
    {0xaa0003f3,-1},       /* mov     x19, x0 */
    {0xf9401a63,-1},       /* ldr     x3, [x19,#SIGSTACK_HANDLER] */
    {0xf9401e62,-1},       /* ldr     x2, [x19,#SIGSTACK_CONTEXT] */
    {0x91000261,-1},       /* add     x1, x19, #SIGSTACK_SIGINFO  */
    {0xf9400260,-1},       /* ldr     x0, [x19, SIGSTACK_SIGNO]   */
    {0xd63f0060,-1},       /* blr     x3 */
    {0xaa1303e0,-1},       /* mov     x0, x19 */
    {0x14000000,14000000}, /* b       SignalReturn */
    {TRAMP_SENTINEL_INSN, -1}
  },
  /* Initialize a trad-frame cache corresponding to the tramp-frame.
     FUNC is the address of the instruction TRAMP[0] in memory.  */
  aarch64_nto_sigframe_init,
#ifdef __NTO_SIGTRAMP_VALIDATE__
  /* Return non-zero if the tramp-frame is valid for the PC requested.
     Adjust the PC to point to the address to check the instruction
     sequence against if required.  If this is NULL, then the tramp-frame
     is valid for any PC.  */
  aarch64_nto_sigframe_validate
#endif
};

static int
aarch64_nto_breakpoint_size (CORE_ADDR addr)
{
  return 0;
}

/* Register maps.  */

static const struct regcache_map_entry aarch64_nto_gregmap[] =
  {
    { 31, AARCH64_X0_REGNUM, 8 }, /* x0 ... x30 */
    { 1, AARCH64_SP_REGNUM, 8 },
    { 1, AARCH64_PC_REGNUM, 8 },
    { 1, AARCH64_CPSR_REGNUM, 8 },
    { 0 }
  };

static const struct regcache_map_entry aarch64_nto_fpregmap[] =
  {
    { 32, AARCH64_V0_REGNUM, 16 }, /* v0 ... v31 */
    { 1, AARCH64_FPSR_REGNUM, 4 },
    { 1, AARCH64_FPCR_REGNUM, 4 },
    { 0 }
  };

/* Register set definitions.  */

const struct regset aarch64_nto_gregset =
  {
    aarch64_nto_gregmap,
    regcache_supply_regset, regcache_collect_regset
  };

const struct regset aarch64_nto_fpregset =
  {
    aarch64_nto_fpregmap,
    regcache_supply_regset, regcache_collect_regset
  };

/* Implement the "regset_from_core_section" gdbarch method.  */

static void
aarch64_nto_iterate_over_regset_sections (struct gdbarch *gdbarch,
            iterate_over_regset_sections_cb *cb,
            void *cb_data,
            const struct regcache *regcache)
{
  cb (".reg", sizeof (AARCH64_CPU_REGISTERS), sizeof (AARCH64_CPU_REGISTERS), &aarch64_nto_gregset,
      NULL, cb_data);
  cb (".reg2", sizeof (AARCH64_FPU_REGISTERS), sizeof (AARCH64_FPU_REGISTERS), &aarch64_nto_fpregset,
      NULL, cb_data);
}

static struct nto_target_ops aarch64_nto_ops;

static void
init_aarch64_nto_ops (void)
{
  aarch64_nto_ops.regset_id = aarch64_nto_regset_id;
  aarch64_nto_ops.supply_gregset = aarch64_nto_supply_gregset;
  aarch64_nto_ops.supply_fpregset = aarch64_nto_supply_fpregset;
  aarch64_nto_ops.supply_altregset = nto_dummy_supply_regset;
  aarch64_nto_ops.supply_regset = aarch64_nto_supply_regset;
  aarch64_nto_ops.register_area = aarch64_nto_register_area;
  aarch64_nto_ops.regset_fill = aarch64_nto_regset_fill;
  aarch64_nto_ops.fetch_link_map_offsets =
      nto_generic_svr4_fetch_link_map_offsets;
  aarch64_nto_ops.breakpoint_size = aarch64_nto_breakpoint_size;
}

static void
aarch64_nto_init_abi (struct gdbarch_info info, struct gdbarch *gdbarch)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  static struct target_so_ops nto_svr4_so_ops;
  struct nto_target_ops *nto_ops;

  tdep->lowest_pc = 0x1000;

  init_aarch64_nto_ops ();

  /* Deal with our strange signals.  */
  nto_initialize_signals ();

  set_solib_svr4_fetch_link_map_offsets
    (gdbarch, nto_generic_svr4_fetch_link_map_offsets);

  nto_ops = (struct nto_target_ops*) gdbarch_data (gdbarch, nto_gdbarch_ops);
  *nto_ops = aarch64_nto_ops;

  nto_init_abi( info, gdbarch );

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

  tramp_frame_prepend_unwinder (gdbarch, &aarch64_nto_sigframe);

  set_gdbarch_iterate_over_regset_sections
    (gdbarch, aarch64_nto_iterate_over_regset_sections);
}

extern initialize_file_ftype _initialize_aarch64_nto_tdep;

void
_initialize_aarch64_nto_tdep (void)
{
  gdbarch_register_osabi (bfd_arch_aarch64, 0,
        GDB_OSABI_QNXNTO, aarch64_nto_init_abi);
}
