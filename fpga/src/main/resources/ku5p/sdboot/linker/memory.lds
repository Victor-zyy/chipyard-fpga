MEMORY
{
  bootrom_mem (rx) : ORIGIN = 0x10000, LENGTH = 0x2000
  memory_mem (rwx) : ORIGIN = 0x80000000, LENGTH = 0x40000000
}
/* Two 256M x16-bit DDR4 devices provide a 32-bit, 1 GiB memory. */
