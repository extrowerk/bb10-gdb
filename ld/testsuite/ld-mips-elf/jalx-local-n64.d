#name: MIPS JALX to local symbol (n64)
#source: ../../../gas/testsuite/gas/mips/jalx-local.s
#as: -EB -64 -march=from-abi
#ld: -EB -Ttext 0x1c000000 -e 0x1c000000
#objdump: -dr --prefix-addresses --show-raw-insn
#dump: jalx-local.d
