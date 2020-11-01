/* Target-dependent code for QNX Neutrino ARM.

   Copyright (C) 2016 Free Software Foundation, Inc.

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


#ifndef ARM_NTO_TDEP_H
#define ARM_NTO_TDEP_H

#include <stdint.h>

#ifndef __QNXNTO__
typedef uint64_t _Uint64t;
typedef uint32_t _Uint32t;
#endif

/* from arm/context.h */
typedef struct arm_cpu_registers {
	_Uint32t	gpr[16];
	_Uint32t	spsr;
} ARM_CPU_REGISTERS;

union vfpv2 {
	_Uint32t	S[32];
	_Uint64t	D[16];
};

union vfpv3 {
	_Uint32t	S[32];
	_Uint64t	D[32];
};

typedef struct arm_fpu_registers {
	union {
		struct vfp {
			union {
				_Uint64t	X[32];
				union vfpv2	v2;
				union vfpv3	v3;
			} reg;
			_Uint32t	fpscr;
			_Uint32t	fpexc;
			_Uint32t	fpinst;
			_Uint32t	fpinst2;
		} vfp;

	} un;
} ARM_FPU_REGISTERS;

#endif /* arm-nto-tdep.h */
