// See LICENSE.Sifive for license details.
#include <stdint.h>

#include <platform.h>

#include "common.h"

#define DEBUG
#include "kprintf.h"

// A sector is 512 bytes, so (1 << 11) * 512B = 1 MiB
#define SECTOR_SIZE_B 512

#define BOOT_MAGIC 0x524F434B // "ROCK"

// Sector 34 contains this header; the payload image starts at sector 35.
#define IMAGE_HEADER_SECTOR 34
#define IMAGE_PAYLOAD_SECTOR (IMAGE_HEADER_SECTOR + 1)

typedef struct __attribute__((packed)) {
	uint32_t magic;         // BOOT_MAGIC
	uint32_t payload_size;  // payload size in bytes
	uint32_t load_addr;     // DDR load address, e.g. 0x80000000
	uint32_t entry_point;   // jump target after loading
	uint32_t crc32;         // payload CRC32; set to 0 to skip this check
	uint32_t version;       // optional image version
	uint8_t  reserved[488];
} image_header_t;

_Static_assert(sizeof(image_header_t) == SECTOR_SIZE_B,
	"image header must be exactly one sector");

#ifndef TL_CLK
#error Must define TL_CLK
#endif

#define F_CLK 		(TL_CLK)

// SPI SCLK frequency, in kHz
// We are using the 25MHz High Speed mode. If this speed is not supported by the
// SD card, consider changing to the Default Speed mode (12.5 MHz).
#define SPI_CLK 	5000 //25000


// SPI clock divisor value
// @see https://ucb-bar.gitbook.io/baremetal-ide/baremetal-ide/using-peripheral-devices/sifive-ips/serial-peripheral-interface-spi
#define SPI_DIV 	(((F_CLK * 1000) / SPI_CLK) / 2 - 1)

#define SPI_CLK_SLOW 400
#define SPI_DIV_SLOW (((F_CLK * 1000) / SPI_CLK_SLOW) / 2 - 1)

static volatile uint32_t * const spi = (void *)(SPI_CTRL_ADDR);

static inline uint8_t spi_xfer(uint8_t d)
{
	int32_t r;

	REG32(spi, SPI_REG_TXFIFO) = d;
	do {
		r = REG32(spi, SPI_REG_RXFIFO);
	} while (r < 0);
	return r;
}

static inline uint8_t sd_dummy(void)
{
	return spi_xfer(0xFF);
}

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
	unsigned long n;
	uint8_t r;

	REG32(spi, SPI_REG_CSMODE) = SPI_CSMODE_HOLD;
	sd_dummy();
	spi_xfer(cmd);
	spi_xfer(arg >> 24);
	spi_xfer(arg >> 16);
	spi_xfer(arg >> 8);
	spi_xfer(arg);
	spi_xfer(crc);

	n = 1000;
	do {
		r = sd_dummy();
		if (!(r & 0x80)) {
			dprintf("sd:cmd: %hx\r\n", r);
			goto done;
		}
	} while (--n > 0);
	kputs("sd_cmd: timeout");
done:
	return r;
}

static inline void sd_cmd_end(void)
{
	sd_dummy();
	REG32(spi, SPI_REG_CSMODE) = SPI_CSMODE_AUTO;
}


static void sd_poweron(void)
{
	long i;
	// HACK: frequency change

	REG32(spi, SPI_REG_SCKDIV) = SPI_DIV_SLOW;
	REG32(spi, SPI_REG_CSMODE) = SPI_CSMODE_OFF;
	for (i = 10; i > 0; i--) {
		sd_dummy();
	}
	REG32(spi, SPI_REG_CSMODE) = SPI_CSMODE_AUTO;
}

static int sd_cmd0(void)
{
	int rc;
	dputs("CMD0");
	rc = (sd_cmd(0x40, 0, 0x95) != 0x01);
	sd_cmd_end();
	return rc;
}

static int sd_cmd8(void)
{
	int rc;
	dputs("CMD8");
	rc = (sd_cmd(0x48, 0x000001AA, 0x87) != 0x01);
	sd_dummy(); /* command version; reserved */
	sd_dummy(); /* reserved */
	rc |= ((sd_dummy() & 0xF) != 0x1); /* voltage */
	rc |= (sd_dummy() != 0xAA); /* check pattern */
	sd_cmd_end();
	return rc;
}

static void sd_cmd55(void)
{
	sd_cmd(0x77, 0, 0x65);
	sd_cmd_end();
}

static int sd_acmd41(void)
{
	uint8_t r;
	dputs("ACMD41");
	do {
		sd_cmd55();
		r = sd_cmd(0x69, 0x40000000, 0x77); /* HCS = 1 */
	} while (r == 0x01);
	return (r != 0x00);
}

static int sd_cmd58(void)
{
	int rc;
	dputs("CMD58");
	rc = (sd_cmd(0x7A, 0, 0xFD) != 0x00);
	rc |= ((sd_dummy() & 0x80) != 0x80); /* Power up status */
	sd_dummy();
	sd_dummy();
	sd_dummy();
	sd_cmd_end();
	return rc;
}

static int sd_cmd16(void)
{
	int rc;
	dputs("CMD16");
	rc = (sd_cmd(0x50, 0x200, 0x15) != 0x00);
	sd_cmd_end();
	return rc;
}

static uint16_t crc16_round(uint16_t crc, uint8_t data) {
	crc = (uint8_t)(crc >> 8) | (crc << 8);
	crc ^= data;
	crc ^= (uint8_t)(crc >> 4) & 0xf;
	crc ^= crc << 12;
	crc ^= (crc & 0xff) << 5;
	return crc;
}

static uint32_t crc32_round(uint32_t crc, uint8_t data)
{
	int i;

	crc ^= data;
	for (i = 0; i < 8; i++) {
		crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
	}
	return crc;
}

#define SPIN_SHIFT	6
#define SPIN_UPDATE(i)	(!((i) & ((1 << SPIN_SHIFT)-1)))
#define SPIN_INDEX(i)	(((i) >> SPIN_SHIFT) & 0x3)

static const char spinner[] = { '-', '/', '|', '\\' };

static int read_sector_from_stream(volatile uint8_t *dst, uint32_t bytes_to_copy,
	uint32_t *crc32)
{
	uint16_t crc, crc_exp;
	long timeout;
	uint32_t n;

	crc = 0;
	timeout = 1000000;
	while (sd_dummy() != 0xFE) {
		if (--timeout == 0) {
			kputs("sd: data token timeout");
			return 1;
		}
	}

	n = SECTOR_SIZE_B;
	do {
		uint8_t x = sd_dummy();
		if (bytes_to_copy > 0) {
			*dst++ = x;
			if (crc32 != 0) {
				*crc32 = crc32_round(*crc32, x);
			}
			bytes_to_copy--;
		}
		crc = crc16_round(crc, x);
	} while (--n > 0);

	crc_exp = ((uint16_t)sd_dummy() << 8);
	crc_exp |= sd_dummy();

	if (crc != crc_exp) {
		kputs("\b- CRC16 mismatch");
		return 1;
	}

	return 0;
}

static void sd_stop_transmission(void)
{
	sd_cmd_end();
	sd_cmd(0x4C, 0, 0x01);
	sd_cmd_end();
}

static int image_range_ok(uintptr_t addr, uint32_t size)
{
	uintptr_t mem_start = (uintptr_t)MEMORY_MEM_ADDR;
	uintptr_t mem_end = mem_start + (uintptr_t)MEMORY_MEM_SIZE;

	if (size == 0) {
		return 0;
	}
	if (addr < mem_start || addr >= mem_end) {
		return 0;
	}
	if ((uintptr_t)size > mem_end - addr) {
		return 0;
	}
	return 1;
}

static int entry_range_ok(uintptr_t entry)
{
	uintptr_t mem_start = (uintptr_t)MEMORY_MEM_ADDR;
	uintptr_t mem_end = mem_start + (uintptr_t)MEMORY_MEM_SIZE;

	return entry >= mem_start && entry < mem_end;
}

static uintptr_t copy(void)
{
	image_header_t header;
	volatile uint8_t *p;
	uintptr_t load_addr, entry;
	uint32_t payload_sectors, copied, payload_crc;
	uint32_t i;
	uint32_t *payload_crc_ptr;
	int rc = 0;

	dputs("CMD18");

	REG32(spi, SPI_REG_SCKDIV) = SPI_DIV;

	REG32(spi, SPI_REG_CSMODE) = SPI_CSMODE_OFF;
	sd_dummy();
	sd_dummy();
	REG32(spi, SPI_REG_CSMODE) = SPI_CSMODE_AUTO;

	if (sd_cmd(0x52, IMAGE_HEADER_SECTOR, 0xE1) != 0x00) {
		sd_cmd_end();
		return 0;
	}

	if (read_sector_from_stream((volatile uint8_t *)&header, SECTOR_SIZE_B, 0)) {
		rc = 1;
		goto done;
	}

	if (header.magic != BOOT_MAGIC) {
		kprintf("BAD MAGIC 0x%x\r\n", header.magic);
		rc = 1;
		goto done;
	}

	load_addr = (uintptr_t)header.load_addr;
	entry = (uintptr_t)header.entry_point;
	if (!image_range_ok(load_addr, header.payload_size) ||
	    !entry_range_ok(entry)) {
		kputs("BAD IMAGE RANGE");
		rc = 1;
		goto done;
	}

	payload_sectors = (header.payload_size + SECTOR_SIZE_B - 1) / SECTOR_SIZE_B;
	payload_crc = 0xffffffffU;
	payload_crc_ptr = header.crc32 != 0 ? &payload_crc : 0;
	copied = 0;
	p = (volatile uint8_t *)load_addr;

	kprintf("HEADER SECTOR 0x%x\r\n", IMAGE_HEADER_SECTOR);
	kprintf("IMAGE SECTOR  0x%x\r\n", IMAGE_PAYLOAD_SECTOR);
	kprintf("LOAD  0x%lx\r\n", (unsigned long)load_addr);
	kprintf("ENTRY 0x%lx\r\n", (unsigned long)entry);
	kprintf("SIZE  0x%x B\r\n", header.payload_size);
	kprintf("VER   0x%x\r\n", header.version);
	kprintf("LOADING  ");

	for (i = 0; i < payload_sectors; i++) {
		uint32_t remaining = header.payload_size - copied;
		uint32_t bytes = remaining > SECTOR_SIZE_B ? SECTOR_SIZE_B : remaining;

		if (read_sector_from_stream(p, bytes, payload_crc_ptr)) {
			rc = 1;
			break;
		}

		p += bytes;
		copied += bytes;

		if (SPIN_UPDATE(i)) {
			kputc('\b');
			kputc(spinner[SPIN_INDEX(i)]);
		}
	}

done:
	sd_stop_transmission();
	kputs("\b ");

	if (rc) {
		return 0;
	}

	if (header.crc32 != 0) {
		payload_crc ^= 0xffffffffU;
		if (payload_crc != header.crc32) {
			kprintf("BAD CRC32 0x%x\r\n", payload_crc);
			return 0;
		}
	}

	return entry;
}

uintptr_t sdboot_main(void)
{
	uintptr_t entry;

	REG32(uart, UART_REG_TXCTRL) = UART_TXEN;

	kputs("INIT");
	sd_poweron();
	if (sd_cmd0() ||
	    sd_cmd8() ||
	    sd_acmd41() ||
	    sd_cmd58() ||
	    sd_cmd16()) {
		kputs("ERROR");
		return 0;
	}

	entry = copy();
	if (entry == 0) {
		kputs("ERROR");
		return 0;
	}

	kputs("BOOT");

	__asm__ __volatile__ ("fence.i" : : : "memory");

	return entry;
}
