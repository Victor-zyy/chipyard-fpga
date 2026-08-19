// See LICENSE.Sifive for license details.
#include <platform.h>
#include <smp.h>
#include "common.h"

  .section .text.init
  .option norvc
  .globl _prog_start
_prog_start:
  smp_pause(s1, s2)
  li sp, (MEMORY_MEM_ADDR + MEMORY_MEM_SIZE - 0x1000)
  call sdboot_main
  mv s3, a0
  beqz s3, 1f
  smp_resume(s1, s2)
  csrr a0, mhartid // hartid for next level bootloader
  la a1, dtb // dtb address for next level bootloader
  jr s3
1:
  wfi
  j 1b

  .section .dtb
  .align 3
dtb:
