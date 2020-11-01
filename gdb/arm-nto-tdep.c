/* ARM specific functionality for QNX Neutrino.

   Copyright 2003, 2009 Free Software Foundation, Inc.

   Contributed by QNX Software Systems Ltd.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#include "defs.h"
#include "frame.h"
#include "target.h"
#include "regcache.h"
#include "solib-svr4.h"
#include "arch/arm-get-next-pcs.h"
#include "arm-tdep.h"
#include "nto-tdep.h"
#include "arm-nto-tdep.h"
#include "osabi.h"

#include "trad-frame.h"
#include "tramp-frame.h"
#include "gdbcore.h"

#include "frame-unwind.h"
#include "solib.h"

#include "elf-bfd.h"

/* 16 GP regs + spsr */
#define GP_REGSET_SIZE (17*4)
#define PS_OFF (16*4)
/* Our FP register size. See arm/context.h  */
#define NTO_FP_REGISTER_SIZE 8 /* 64-bit */

#define NTO_FP_REGISTER_NUM 32

#define NTO_STATUS_REGISTER_SIZE 4

/* FP registers - see context.h, Largest register file size + status regs. */
#define FP_REGSET_SIZE (NTO_FP_REGISTER_SIZE * NTO_FP_REGISTER_NUM + NTO_STATUS_REGISTER_SIZE * 4)

#define ARM_NTO_WMMX_WR_REGSIZE 8
#define ARM_NTO_WMMX_WC_REGSIZE 4
#define ARM_NTO_WMMX_WCSSF_OFFSET 0x80
#define ARM_NTO_WMMX_WCASF_OFFSET 0x84
#define ARM_NTO_WMMX_WCGR0_OFFSET 0x88

#define ALT_REGSET_SIZE (ARM_NTO_WMMX_WR_REGSIZE * 16 \
       + NTO_STATUS_REGISTER_SIZE * 6)

static CORE_ADDR
  nto_arm_get_next_pcs_syscall_next_pc (struct arm_get_next_pcs *self);

/* Operation function pointers for get_next_pcs.  */
static struct arm_get_next_pcs_ops nto_arm_get_next_pcs_ops = {
  arm_get_next_pcs_read_memory_unsigned_integer,
  nto_arm_get_next_pcs_syscall_next_pc,
  arm_get_next_pcs_addr_bits_remove,
  arm_get_next_pcs_is_thumb,
  NULL, /* no fixup */
};

static void
armnto_supply_reg_gregset (struct regcache *regcache, int regno,
         const gdb_byte *regs)
{
  int regi;

  for (regi = ARM_A1_REGNUM; regi < ARM_F0_REGNUM; regi++)
    {
      RAW_SUPPLY_IF_NEEDED (regcache, regi, regs);
      regs += 4;
    }
  RAW_SUPPLY_IF_NEEDED (regcache, ARM_PS_REGNUM, regs);
}

static void
armnto_supply_reg_fpregset (struct regcache *regcache, int regno,
          const gdb_byte *regs)
{
  int regi;
  const struct gdbarch_tdep *const tdep = gdbarch_tdep (target_gdbarch ());

  if (tdep->vfp_register_count)
    {
      for (regi = ARM_D0_REGNUM; regi != ARM_D31_REGNUM; ++regi)
  {
    const unsigned int offset = (regi - ARM_D0_REGNUM) * NTO_FP_REGISTER_SIZE;

    RAW_SUPPLY_IF_NEEDED (regcache, regi, &regs[offset]);
  }
      regs += NTO_FP_REGISTER_SIZE * NTO_FP_REGISTER_NUM;
      RAW_SUPPLY_IF_NEEDED (regcache, ARM_FPSCR_REGNUM, regs);
    }
  else
    {
      for (regi = ARM_F0_REGNUM; regi <= ARM_F7_REGNUM; regi++)
  {
    gdb_byte gdbbuf[FP_REGISTER_SIZE]; /* This is GDB's register size. */

    memset (gdbbuf, 0, 12);
    memcpy (gdbbuf + 4, regs, NTO_FP_REGISTER_SIZE);
    RAW_SUPPLY_IF_NEEDED (regcache, regi, gdbbuf);
    regs += NTO_FP_REGISTER_SIZE;
  }

      RAW_SUPPLY_IF_NEEDED (regcache, ARM_FPS_REGNUM, regs);
    }
}

static void
armnto_supply_reg_altregset (struct regcache *regcache, int regno,
           const gdb_byte *regs)
{
    int regi;

    /* WMMX data registers: */
    for (regi = ARM_WR0_REGNUM; regi <= ARM_WR15_REGNUM; ++regi)
      RAW_SUPPLY_IF_NEEDED (regcache, regi, &regs[(regi - ARM_WR0_REGNUM)
              * ARM_NTO_WMMX_WR_REGSIZE]);
    /* WMMX control registers: */
    RAW_SUPPLY_IF_NEEDED (regcache, ARM_WCSSF_REGNUM,
        &regs[ARM_NTO_WMMX_WCSSF_OFFSET]);
    RAW_SUPPLY_IF_NEEDED (regcache, ARM_WCASF_REGNUM,
        &regs[ARM_NTO_WMMX_WCASF_OFFSET]);
    for (regi = ARM_WCGR0_REGNUM; regi <= ARM_WCGR3_REGNUM; ++regi)
      RAW_SUPPLY_IF_NEEDED (regcache, regi, &regs[(regi - ARM_WCGR0_REGNUM)
              * ARM_NTO_WMMX_WC_REGSIZE]);
}

static void
armnto_supply_gregset (struct regcache *regcache, const gdb_byte *regs,
           size_t len)
{
  armnto_supply_reg_gregset (regcache, NTO_ALL_REGS, regs);
}

static void
armnto_supply_fpregset (struct regcache *regcache, const gdb_byte *regs,
      size_t len)
{
  armnto_supply_reg_fpregset (regcache, NTO_ALL_REGS, regs);
}

static void
armnto_supply_altregset (struct regcache *regcache, const gdb_byte *regs,
       size_t len)
{
  armnto_supply_reg_altregset (regcache, NTO_ALL_REGS, regs);
}

static void
armnto_supply_regset (struct regcache *regcache, int regset,
          const gdb_byte *data, size_t len)
{
  switch (regset)
    {
    case NTO_REG_GENERAL:
      armnto_supply_gregset (regcache, data, len);
      break;
    case NTO_REG_FLOAT:
      armnto_supply_fpregset (regcache, data, len);
      break;
    case NTO_REG_ALT:
      armnto_supply_altregset (regcache, data, len);
      break;
    default:
      gdb_assert (0);
    }
}

static int
armnto_regset_id (int regno)
{
  if (regno == -1)
    return NTO_REG_END;
  else if (regno < ARM_F0_REGNUM || regno == ARM_FPS_REGNUM
     || regno == ARM_PS_REGNUM)
    return NTO_REG_GENERAL;
  else if (regno >= ARM_F0_REGNUM && regno <= ARM_F7_REGNUM)
    return NTO_REG_FLOAT;
  /* VFP registers are mapped into FPU registers. */
  else if ((regno >= ARM_D0_REGNUM && regno <= ARM_D31_REGNUM)
     || regno == ARM_FPSCR_REGNUM)
    return NTO_REG_FLOAT;
  else if (regno >= ARM_WR0_REGNUM && regno < ARM_NUM_REGS)
    return NTO_REG_ALT;
  return -1;
}

static int
armnto_register_area (int regset, unsigned cpuflags)
{
  switch (regset)
    {
    case NTO_REG_GENERAL:
  return GP_REGSET_SIZE;
    case NTO_REG_FLOAT:
  return FP_REGSET_SIZE;
    case NTO_REG_ALT:
  return ALT_REGSET_SIZE;
    }
  return -1;
}

static int
armnto_regset_fill (const struct regcache *regcache, int regset,
        gdb_byte *data, size_t len)
{
  int regi;

  if (regset == NTO_REG_GENERAL)
    {
      for (regi = ARM_A1_REGNUM; regi < ARM_F0_REGNUM; regi++)
  {
    regcache->raw_collect (regi, data);
    data += 4;
  }
      regcache->raw_collect (ARM_PS_REGNUM, data);
    }
  else
    return -1;
  return 0;
}

static const char *
armnto_variant_directory_suffix (void)
{
  const struct gdbarch_tdep *const tdep = gdbarch_tdep (target_gdbarch ());

  /* On QNX, it is always v7 variant if abi is AAPCS. */
  if (tdep->arm_abi == ARM_ABI_AAPCS)
    {
      nto_trace(1) ("Selecting -v7 variant\n");
      return "-v7";
    }

  return "";
}


/* From sys/syspage.h: */
#define CPU_FLAG_FPU  (1UL <<  31)  /* CPU has floating point support */

/* From arm/syspage.h: */
/*
 * CPU capability/state flags
 */
#define ARM_CPU_FLAG_XSCALE_CP0      0x0001    /* Xscale CP0 MAC unit */
#define ARM_CPU_FLAG_V6        0x0002    /* ARMv6 architecture */
#define ARM_CPU_FLAG_V6_ASID      0x0004    /* use ARMv6 MMU ASID */
#define ARM_CPU_FLAG_SMP      0x0008  /* multiprocessor system */
#define ARM_CPU_FLAG_V7_MP      0x0010    /* ARMv7 multiprocessor extenstions */
#define ARM_CPU_FLAG_V7        0x0020    /* ARMv7 architecture */
#define ARM_CPU_FLAG_NEON      0x0040    /* Neon Media Engine */
#define ARM_CPU_FLAG_WMMX2      0x0080    /* iWMMX2 coprocessor */

/* we want to see the proper register sets */
static const struct target_desc *
armnto_read_description (unsigned cpuflags)
{
  if (cpuflags & ARM_CPU_FLAG_NEON)
    {
      nto_trace(0)("armnto_read_description(): NEON\n");
      return tdesc_arm_with_neon;
    }
  if (cpuflags & ARM_CPU_FLAG_WMMX2)
    {
      nto_trace(0)("armnto_read_description(): WMMX2\n");
      return tdesc_arm_with_iwmmxt;
    }
  if (cpuflags & CPU_FLAG_FPU)
    {
      nto_trace(0)("armnto_read_description(): VFPV3\n");
      return tdesc_arm_with_vfpv3;
    }
  nto_trace(0)("armnto_read_description(): plain\n");
  return NULL;
}

static int
armnto_breakpoint_size (const CORE_ADDR addr)
{
  int size;
  if (arm_pc_is_thumb (target_gdbarch (), addr))
    size = 2;
  else
    size = 4;
  nto_trace (0) ("%08lx is in %s mode\n", addr, (size==4)?"ARM":"Thumb");
  return size;
}

static struct nto_target_ops arm_nto_ops;

static void
init_armnto_ops (void)
{
  arm_nto_ops.regset_id = armnto_regset_id;
  arm_nto_ops.supply_gregset = armnto_supply_gregset;
  arm_nto_ops.supply_fpregset = armnto_supply_fpregset;
  arm_nto_ops.supply_altregset = armnto_supply_altregset;
  arm_nto_ops.supply_regset = armnto_supply_regset;
  arm_nto_ops.register_area = armnto_register_area;
  arm_nto_ops.regset_fill = armnto_regset_fill;
  arm_nto_ops.variant_directory_suffix = armnto_variant_directory_suffix;
  arm_nto_ops.read_description = armnto_read_description;
  arm_nto_ops.fetch_link_map_offsets =
    nto_generic_svr4_fetch_link_map_offsets;
  arm_nto_ops.breakpoint_size = armnto_breakpoint_size;
}

/* */
static int
arm_nto_in_dynsym_resolve_code (CORE_ADDR pc)
{
  gdb_byte buff[24];
  gdb_byte *p = buff + 8;
  ULONGEST instr[] = { 0xe59fc004, 0xe08fc00c, 0xe59cf000 };
  ULONGEST instrmask[] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
  enum bfd_endian byte_order = gdbarch_byte_order (target_gdbarch ());

  nto_trace (0) ("%s (pc=%s)\n", __func__, paddress (target_gdbarch (), pc));

  read_memory (pc - 8, buff, 24);

  while (p >= buff)
    {
      if ((extract_unsigned_integer (p, 4, byte_order) & instrmask[0])
    == instr[0])
        break;

      p -= 4;
    }

  if (p >= buff)
    {
      int i;
      int match = 1;

      for (i = 1; i != sizeof(instr)/sizeof(instr[0]); ++i)
        {
    ULONGEST inst;

    p += 4;

    inst = extract_unsigned_integer (p, 4, byte_order) & instrmask[i];

    if (inst != instr[i])
      {
        match = 0;
        break; // did not match
      }
        }

  if (match)
    {
      nto_trace (0) ("Looks like plt code\n");
      return 1;
    }
    }

  nto_trace (0) ("%s: could not recognize plt code\n", __func__);
  return nto_in_dynsym_resolve_code (pc);
}

/* Core file support */
static void
armnto_core_supply_gregset (const struct regset *regset,
                             struct regcache *regcache,
           int regnum, const void *preg,
           size_t len)
{
  nto_trace (0) ("%s () regnum=%d\n", __func__, regnum);

  armnto_supply_reg_gregset (regcache, regnum, (const gdb_byte *)preg);
}

static void
armnto_core_supply_fpregset (const struct regset *regset,
           struct regcache *regcache,
           int regnum, const void *preg,
           size_t len)
{
  nto_trace (0) ("%s () regnum=%d\n", __func__, regnum);

  armnto_supply_reg_fpregset (regcache, regnum, (const gdb_byte *)preg);
}

static void
armnto_core_supply_altregset (const struct regset *regset,
            struct regcache *regcache,
            int regnum, const void *preg,
            size_t len)
{
  nto_trace (0) ("%s () regnum=%d\n", __func__, regnum);
   /* NYI */
}

static struct regset armnto_gregset =
{
  NULL,
  armnto_core_supply_gregset,
  NULL,
  0
};

static struct regset armnto_fpregset =
{
  NULL,
  armnto_core_supply_fpregset,
  NULL,
  0
};

static struct regset armnto_altregset =
{
  NULL,
  armnto_core_supply_altregset,
  NULL,
  0
};

#if 0
/* todo not used?! */
/* Return the appropriate register set for the core section identified
   by SECT_NAME and SECT_SIZE.  */

static const struct regset *
armnto_regset_from_core_section (struct gdbarch *gdbarch,
           const char *sect_name, size_t sect_size)
{
  nto_trace (0) ("%s () sect_name:%s\n", __func__, sect_name);

  if (strcmp (sect_name, ".reg") == 0)
    {
      if (sect_size < GP_REGSET_SIZE)
  warning (_("Section '%s' has invalid size (%zu)\n"), sect_name,
     sect_size);
      return &armnto_gregset;
    }

  if (strcmp (sect_name, ".reg2") == 0)
    {
      if (sect_size < FP_REGSET_SIZE)
  warning (_("Section '%s' has invalid size (%zu)\n"), sect_name,
     sect_size);
      return &armnto_fpregset;
    }

  return NULL;
}
#endif

/* Signal trampolines. */

/* Signal trampoline sniffer.  */

static CORE_ADDR
armnto_sigcontext_addr (struct frame_info *this_frame)
{
  struct gdbarch *gdbarch = get_frame_arch (this_frame);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  gdb_byte buf[4];
  CORE_ADDR ptrctx;

  nto_trace (0) ("%s ()\n", __func__);

  /*  read base from r5 of the sigtramp frame:
   we store base + 4 (addr of r1, not r0) in r5.
   stmia  lr, {r1-r12}
   mov  r5, lr
  */
  get_frame_register (this_frame, ARM_A1_REGNUM + 5, buf);
  ptrctx = extract_unsigned_integer (buf, 4, byte_order);
  ptrctx -= 4;

  nto_trace (0) ("context addr: 0x%s\n", paddress (gdbarch, ptrctx));

  return ptrctx;
}

struct arm_nto_sigtramp_cache
{
  CORE_ADDR base;
  struct trad_frame_saved_reg *saved_regs;
};

static struct arm_nto_sigtramp_cache *
armnto_sigtramp_cache (struct frame_info *this_frame, void **this_cache)
{
  CORE_ADDR ptrctx;
  int i;
  struct arm_nto_sigtramp_cache *cache;
  const int REGSIZE = 4;

  nto_trace (0) ("%s ()\n", __func__);

  if ((*this_cache) != NULL)
    return (struct arm_nto_sigtramp_cache *)(*this_cache);
  cache = FRAME_OBSTACK_ZALLOC (struct arm_nto_sigtramp_cache);
  (*this_cache) = cache;
  cache->saved_regs
    = (struct trad_frame_saved_reg *) trad_frame_alloc_saved_regs (this_frame);
  cache->base = get_frame_pc (this_frame);
  ptrctx = armnto_sigcontext_addr (this_frame);

  /* Registers from before signal. */
  for (i = ARM_A1_REGNUM; i != ARM_F0_REGNUM; ++i)
    cache->saved_regs[i].addr = ptrctx + i * REGSIZE;

  /* PS register. We store it as 16-th register in ucontext_t structure. */
  cache->saved_regs[ARM_PS_REGNUM].addr = ptrctx + 16 * REGSIZE;

  return cache;
}

static void
armnto_sigtramp_this_id (struct frame_info *this_frame,
       void **this_prologue_cache, struct frame_id *this_id)
{
  struct arm_nto_sigtramp_cache *info =
  armnto_sigtramp_cache (this_frame, this_prologue_cache);
  nto_trace (0) ("%s ()\n", __func__);
  (*this_id) = frame_id_build (info->base, get_frame_pc (this_frame));
}

static struct value *
armnto_sigtramp_prev_register (struct frame_info *this_frame,
             void **this_prologue_cache,
             int regnum)
{
  struct arm_nto_sigtramp_cache *info =
      armnto_sigtramp_cache (this_frame, this_prologue_cache);
  nto_trace (0) ("%s ()\n", __func__);
  return trad_frame_get_prev_register (this_frame, info->saved_regs, regnum);
}

static int
armnto_sigtramp_sniffer (const struct frame_unwind *self,
       struct frame_info *this_frame,
       void **this_prolobue_cache)
{
  CORE_ADDR pc = get_frame_pc (this_frame);
  const char *name;

  nto_trace (0) ("%s ()\n", __func__);

  find_pc_partial_function (pc, &name, NULL, NULL);
  if (name
      && (strcmp ("__signalstub", name) == 0
    || strcmp ("SignalReturn", name) == 0))
     return 1;

  return 0;
}

static const struct frame_unwind arm_nto_sigtramp_unwind =
{
  SIGTRAMP_FRAME,
  default_frame_unwind_stop_reason,
  armnto_sigtramp_this_id,
  armnto_sigtramp_prev_register,
  NULL,
  armnto_sigtramp_sniffer,
  NULL
};



#if 0
static void
armnto_sigtramp_cache_init (const struct tramp_frame *self,
                            struct frame_info *this_frame,
          struct trad_frame_cache *this_cache,
          CORE_ADDR func)
{
  struct gdbarch *gdbarch = get_frame_arch (this_frame);
  CORE_ADDR ptrctx, sp;
  int regi;
  const int REGSIZE = 4;
  //struct frame_info *this_frame = get_prev_frame(next_frame);

  nto_trace (0) ("%s () funcaddr=0x%s\n", __func__, paddress (gdbarch, func));

  /* stack pointer for __signal_stub frame */
  sp = get_frame_sp (this_frame);

  nto_trace (0) ("sp: 0x%s\n", paddress (gdbarch, sp));

  gdb_assert (gdbarch_sp_regnum (gdbarch) == ARM_SP_REGNUM);

  /* Construct the frame ID using the function start. */
  trad_frame_set_id (this_cache, frame_id_build (sp, func));
  ptrctx = armnto_sigcontext_addr (this_frame);
  nto_trace (0) ("context addr: 0x%s\n", paddress (gdbarch, ptrctx));

  /* retrieve registers */
  for (regi = ARM_A1_REGNUM; regi < ARM_F0_REGNUM; regi++)
    {
      const CORE_ADDR addr = ptrctx + (regi - ARM_A1_REGNUM) * REGSIZE;
      trad_frame_set_reg_addr (this_cache, regi, addr);
    }

  trad_frame_set_reg_addr (this_cache, ARM_PS_REGNUM, ptrctx + 16 * REGSIZE);
}
static struct tramp_frame arm_nto_sighandler_tramp_frame = {
  SIGTRAMP_FRAME,
  4,
  {
    { 0xe1a00004, 0xFFFFFFFF }, /* mov r0, r4 */
    { 0xe8950ffe, 0xFFFFFFFF }, /* ldmia r5, {r1, r2, r3, r4,.... */
    { TRAMP_SENTINEL_INSN, -1 },
  },
  armnto_sigtramp_cache_init
};
#endif
#if 0
static struct tramp_frame armbe_nto_sighandler_tramp_frame = {
  SIGTRAMP_FRAME,
  4,
  {
    { 0xebfffff0, 0xFFFFFFFF },
    { 0xe91ba800, 0xFFFFFFFF },
    { TRAMP_SENTINEL_INSN, -1 },
  },
  armnto_sigtramp_cache_init
};
#endif

static CORE_ADDR
nto_arm_get_next_pcs_syscall_next_pc (struct arm_get_next_pcs *self)
{
  CORE_ADDR pc = regcache_read_pc (self->regcache);
  int is_thumb = arm_is_thumb (self->regcache);

  /* todo caveat! There are 4Byte Thumb instructions now! */
  if (is_thumb)
    {
      return pc + 2;
    }
  else
    {
      return pc + 4;
    }
}

#if 1
// countdown for if-then-else constructs, needs to be global
static int itsteps=0;

/* todo the logic has changed a lot here
 * The only real difference we make is that we take special care of SVCs
 * alas, this may also confuse breakpoints with SVCs
 * nto_arm_get_next_pcs_ops does already have a hook for SVC handling,
 * not sure what was done before ?!
 */
std::vector<CORE_ADDR>
nto_arm_software_single_step (struct regcache *regcache)
{
#define THUMB_MASK_ITOP 0xff00
#define THUMB_ITOP 0xbf00
  struct gdbarch *gdbarch = regcache->arch ();
  struct arm_get_next_pcs next_pcs_ctx;
  CORE_ADDR pc=0;
  LONGEST insn=0;
  int mc=0;

  /* take care of IT opcodes */
  if (arm_is_thumb (regcache)) {
      pc = regcache_read_pc (regcache);
      if (safe_read_memory_integer (pc, 4, gdbarch_byte_order_for_code (gdbarch), &insn) )
	{
	  if ( ( insn & THUMB_MASK_ITOP ) == THUMB_ITOP )
	    {
	      nto_trace(0)("Found an If-Then (0x%8lx) at 0x%08lx\n", insn, pc );
	      itsteps=4;
	    }
	}
      pc += 2;
   }

  arm_get_next_pcs_ctor (&next_pcs_ctx,
       &nto_arm_get_next_pcs_ops,
       gdbarch_byte_order (gdbarch),
       gdbarch_byte_order_for_code (gdbarch),
       0,
       regcache);

  std::vector<CORE_ADDR> next_pcs = arm_get_next_pcs (&next_pcs_ctx);

  if ( itsteps )
    {
      nto_trace(0)("  setting %i explicit breakpoints\n", itsteps );
      // if pc is 0 then we found an ARM instruction inside an IT block!
      gdb_assert(pc != 0);
      for( mc=0; mc<itsteps; mc++ )
	{
	  next_pcs.push_back(std::move(pc+(2*mc)));
	}
      itsteps--;
    }

  for (CORE_ADDR &pc_ref : next_pcs)
    {
      pc_ref = gdbarch_addr_bits_remove (gdbarch, pc_ref);
    }

  return next_pcs;
}
#else
static int
nto_arm_software_single_step (struct frame_info *frame)
{
  struct regcache *regcache = get_current_regcache ();
  struct gdbarch *gdbarch = regcache->arch();
  const struct address_space *aspace = regcache->aspace();
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  struct arm_get_next_pcs next_pcs_ctx;
  CORE_ADDR pc;
  int i;
  int at_syscall;
  std::vector<CORE_ADDR> next_pcs;
  struct cleanup *old_chain;
  LONGEST insn, mask, match;

  if (arm_is_thumb (regcache)) {
    mask = 0xff000000;
    match = 0xdf000000; /* svc */
  }
  else {
    mask = 0x0f000000;
    match = 0x0f000000; /* svc */
  }

  pc = regcache_read_pc (regcache);
  if (safe_read_memory_integer (pc, 4, byte_order_for_code, &insn)
    && (insn & mask) == match)
    {
      at_syscall = 1;
    }

// todo this should not be necessary with a vector
//  old_chain = make_cleanup (std::vector<CORE_ADDR>, &next_pcs);

  arm_get_next_pcs_ctor (&next_pcs_ctx,
       &nto_arm_get_next_pcs_ops,
       gdbarch_byte_order (gdbarch),
       gdbarch_byte_order_for_code (gdbarch),
       1,
       regcache);

  next_pcs = arm_get_next_pcs (&next_pcs_ctx);

  for (CORE_ADDR &pc_ref : next_pcs) {
//    pc_ref = gdbarch_addr_bits_remove (gdbarch, pc_ref);
    arm_insert_single_step_breakpoint (gdbarch, aspace, pc_ref);

    if (at_syscall)
  {
    /* We are currently at a syscall which makes this the pc when
       successful, so we need to add another bp for the failure case. */
    arm_insert_single_step_breakpoint (gdbarch, aspace, pc_ref + 4);
  }
  }

#if 0
  for (i = 0; VEC_iterate (CORE_ADDR, next_pcs, i, pc); i++)
    {
      arm_insert_single_step_breakpoint (gdbarch, aspace, pc);

      if (at_syscall)
  {
    /* We are currently at a syscall which makes this the pc when
       successful, so we need to add another bp for the failure case. */
    arm_insert_single_step_breakpoint (gdbarch, aspace, pc + 4);
  }
    }
#endif

//  do_cleanups (old_chain);

  return 1;
}
#endif

static const gdb_byte arm_nto_thumb2_le_breakpoint[] = { 0xfe, 0xde, 0xff, 0xe7 };
static const gdb_byte arm_nto_thumb_le_breakpoint[] = { 0xfe, 0xde };

/* Register maps.  */

static const struct regcache_map_entry arm_nto_gregmap[] =
  {
    { 16, ARM_A1_REGNUM, 4 }, /* r0 ... r15 */
    { 1, ARM_PS_REGNUM, 4 },
    { 0 }
  };

static const struct regcache_map_entry arm_nto_fpregmap[] =
  {
    { 32, ARM_D0_REGNUM, 8 }, /* d0 ... d31 */
    { 1, ARM_FPSCR_REGNUM, 4 },
    { 0 }
  };

/* Register set definitions.  */

const struct regset arm_nto_gregset =
  {
    arm_nto_gregmap,
    regcache_supply_regset, regcache_collect_regset
  };

const struct regset arm_nto_fpregset =
  {
    arm_nto_fpregmap,
    regcache_supply_regset, regcache_collect_regset
  };

/* Implement the "regset_from_core_section" gdbarch method.  */

static void
arm_nto_iterate_over_regset_sections (struct gdbarch *gdbarch,
            iterate_over_regset_sections_cb *cb,
            void *cb_data,
            const struct regcache *regcache)
{
  cb (".reg", sizeof (ARM_CPU_REGISTERS), sizeof (ARM_CPU_REGISTERS),
      &arm_nto_gregset, NULL, cb_data);
  cb (".reg2", sizeof (ARM_FPU_REGISTERS), sizeof (ARM_FPU_REGISTERS),
      &arm_nto_fpregset, NULL, cb_data);
}

static void
armnto_init_abi (struct gdbarch_info info, struct gdbarch *gdbarch)
{
  struct gdbarch_tdep *const tdep = gdbarch_tdep (gdbarch);
  struct nto_target_ops *nto_ops;

  /* do generic initialization */
  nto_init_abi( info, gdbarch );

  /* Deal with our strange signals.  */
  nto_initialize_signals ();

  set_solib_svr4_fetch_link_map_offsets
    (gdbarch, nto_generic_svr4_fetch_link_map_offsets);

  /* Our loader handles solib relocations slightly differently than svr4.  */
  svr4_so_ops.relocate_section_addresses = nto_relocate_section_addresses;

  /* Supply a nice function to find our solibs.  */
  svr4_so_ops.find_and_open_solib = nto_find_and_open_solib;

  /* Our linker code is in libc.  */
  svr4_so_ops.in_dynsym_resolve_code = arm_nto_in_dynsym_resolve_code;

  set_solib_ops (gdbarch, &svr4_so_ops);

  set_gdbarch_get_siginfo_type (gdbarch, nto_get_siginfo_type);

  /* Signal trampoline */
  frame_unwind_append_unwinder (gdbarch, &arm_nto_sigtramp_unwind);

  /* Our single step is broken. Use software. */
  set_gdbarch_software_single_step (gdbarch, nto_arm_software_single_step);

  set_gdbarch_iterate_over_regset_sections
    (gdbarch, arm_nto_iterate_over_regset_sections);

  if (tdep->arm_abi == ARM_ABI_AAPCS)
    {
      tdep->arm_breakpoint = arm_nto_thumb2_le_breakpoint;
      tdep->thumb2_breakpoint = arm_nto_thumb2_le_breakpoint;
      tdep->thumb_breakpoint = arm_nto_thumb_le_breakpoint;
      tdep->arm_breakpoint_size = 4;
      tdep->thumb2_breakpoint_size = 4;
      tdep->thumb_breakpoint_size = 2;

      tdep->lowest_pc = 0x1000;
    }

  nto_ops = (struct nto_target_ops *) gdbarch_data (gdbarch, nto_gdbarch_ops);
  *nto_ops = arm_nto_ops;

  set_gdbarch_gdb_signal_to_target (gdbarch, nto_gdb_signal_to_target);
  set_gdbarch_gdb_signal_from_target (gdbarch, nto_gdb_signal_from_target);
}

extern initialize_file_ftype _initialize_armnto_tdep;

void
_initialize_armnto_tdep (void)
{
  init_armnto_ops ();
  gdbarch_register_osabi (bfd_arch_arm, 0, GDB_OSABI_QNXNTO, armnto_init_abi);
  gdbarch_register_osabi_sniffer (bfd_arch_arm, bfd_target_elf_flavour,
          nto_elf_osabi_sniffer);
}

