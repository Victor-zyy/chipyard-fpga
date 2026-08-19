#include <stdint.h>
#include <stdio.h>

#define DAQ_BASE                0x64002000UL

#define DAQ_VERSION             0x00
#define DAQ_CAPABILITY          0x04
#define DAQ_CTRL                0x08
#define DAQ_STATUS              0x0c
#define DAQ_SAMPLE_PERIOD       0x10
#define DAQ_CHANNEL_ENABLE      0x14
#define DAQ_FIFO_LEVEL          0x18
#define DAQ_FIFO_WATERMARK      0x1c
#define DAQ_IRQ_ENABLE          0x20
#define DAQ_IRQ_STATUS          0x24
#define DAQ_DROP_COUNT          0x28
#define DAQ_SAMPLE_COUNT        0x2c
#define DAQ_FIFO_DATA0          0x30
#define DAQ_FIFO_DATA1          0x34
#define DAQ_FIFO_DATA2          0x38
#define DAQ_FIFO_DATA3          0x3c
#define DAQ_FIFO_POP            0x40
#define DAQ_TEST_PATTERN        0x44
#define DAQ_IRQ_CLEAR           0x48

#define CTRL_ENABLE             (1U << 0)
#define CTRL_FIFO_CLEAR         (1U << 1)
#define CTRL_COUNTER_CLEAR      (1U << 2)

#define STATUS_RUNNING          (1U << 0)
#define STATUS_FIFO_EMPTY       (1U << 1)
#define STATUS_FIFO_FULL        (1U << 2)
#define STATUS_OVERFLOW         (1U << 3)

#define IRQ_WATERMARK           (1U << 0)
#define IRQ_OVERFLOW            (1U << 1)

struct daq_sample {
	uint16_t ch0;
	uint16_t ch1;
	uint16_t ch2;
	uint16_t ch3;
	uint64_t timestamp;
};

static inline uint32_t mmio_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
	asm volatile("fence iorw, iorw" ::: "memory");
}

static uint32_t daq_read(uint32_t offset)
{
	return mmio_read32(DAQ_BASE + offset);
}

static void daq_write(uint32_t offset, uint32_t value)
{
	mmio_write32(DAQ_BASE + offset, value);
}

static int expect_u32(const char *name, uint32_t actual, uint32_t expected)
{
	printf("%-20s = 0x%08x", name, actual);
	if (actual != expected) {
		printf("  expected 0x%08x [FAIL]\n", expected);
		return -1;
	}
	printf("  [PASS]\n");
	return 0;
}

static struct daq_sample daq_peek(void)
{
	struct daq_sample s;
	uint32_t d0 = daq_read(DAQ_FIFO_DATA0);
	uint32_t d1 = daq_read(DAQ_FIFO_DATA1);
	uint32_t d2 = daq_read(DAQ_FIFO_DATA2);
	uint32_t d3 = daq_read(DAQ_FIFO_DATA3);

	s.ch0 = d0 & 0xffff;
	s.ch1 = d0 >> 16;
	s.ch2 = d1 & 0xffff;
	s.ch3 = d1 >> 16;
	s.timestamp = ((uint64_t)d3 << 32) | d2;
	return s;
}

static uint16_t lfsr_next(uint16_t x)
{
	uint16_t feedback;

	feedback = ((x >> 15) ^ (x >> 13) ^ (x >> 12) ^ (x >> 10)) & 1;
	return (uint16_t)((x << 1) | feedback);
}

int main(void)
{
	const uint32_t period = 16;
	const uint16_t pattern = 0x1234;
	struct daq_sample prev = { 0 };
	uint16_t expected_lfsr = 0xace1;
	uint32_t level;
	uint32_t count;
	uint32_t status;
	uint32_t irq_status;
	uint32_t drop_count;
	int failed = 0;
	int i;

	printf("FPGA DAQ Engine full functional test\n\n");

	failed |= expect_u32("VERSION", daq_read(DAQ_VERSION), 0x00010000);
	failed |= expect_u32("CAPABILITY", daq_read(DAQ_CAPABILITY), 0x10010004);

	daq_write(DAQ_CTRL, CTRL_FIFO_CLEAR | CTRL_COUNTER_CLEAR);
	daq_write(DAQ_IRQ_CLEAR, IRQ_OVERFLOW);
	daq_write(DAQ_SAMPLE_PERIOD, period);
	daq_write(DAQ_CHANNEL_ENABLE, 0xf);
	daq_write(DAQ_FIFO_WATERMARK, 4);
	daq_write(DAQ_TEST_PATTERN, pattern);

	daq_write(DAQ_CTRL, CTRL_ENABLE);
	while (daq_read(DAQ_SAMPLE_COUNT) < 8)
		;
	daq_write(DAQ_CTRL, 0);

	count = daq_read(DAQ_SAMPLE_COUNT);
	level = daq_read(DAQ_FIFO_LEVEL);
	irq_status = daq_read(DAQ_IRQ_STATUS);

	printf("\nSampler/FIFO\n");
	printf("SAMPLE_COUNT         = %u\n", count);
	printf("FIFO_LEVEL           = %u\n", level);
	printf("IRQ_STATUS           = 0x%08x\n", irq_status);

	if (count < 8 || level < 8 || !(irq_status & IRQ_WATERMARK)) {
		printf("Sampler/FIFO state [FAIL]\n");
		failed = -1;
	} else {
		printf("Sampler/FIFO state [PASS]\n");
	}

	printf("\nFIFO sample contents\n");
	for (i = 0; i < 8; i++) {
		struct daq_sample s = daq_peek();

		printf("[%d] ts=%llu ch0=%u ch1=0x%04x ch2=%u ch3=0x%04x",
		       i, (unsigned long long)s.timestamp, s.ch0, s.ch1, s.ch2, s.ch3);

		if (s.ch0 != (uint16_t)i || s.ch1 != expected_lfsr || s.ch3 != pattern) {
			printf(" [FAIL]\n");
			failed = -1;
		} else if (i > 0 &&
			   (s.timestamp - prev.timestamp != period ||
			    (uint16_t)(s.ch2 - prev.ch2) != period)) {
			printf(" [FAIL]\n");
			failed = -1;
		} else {
			printf(" [PASS]\n");
		}

		prev = s;
		expected_lfsr = lfsr_next(expected_lfsr);
		daq_write(DAQ_FIFO_POP, 1);
	}

	printf("\nOverflow test\n");
	daq_write(DAQ_CTRL, CTRL_FIFO_CLEAR | CTRL_COUNTER_CLEAR);
	daq_write(DAQ_IRQ_CLEAR, IRQ_OVERFLOW);
	daq_write(DAQ_SAMPLE_PERIOD, 1);
	daq_write(DAQ_FIFO_WATERMARK, 32);
	daq_write(DAQ_CTRL, CTRL_ENABLE);

	while (daq_read(DAQ_DROP_COUNT) == 0)
		;
	daq_write(DAQ_CTRL, 0);

	level = daq_read(DAQ_FIFO_LEVEL);
	drop_count = daq_read(DAQ_DROP_COUNT);
	status = daq_read(DAQ_STATUS);
	irq_status = daq_read(DAQ_IRQ_STATUS);

	printf("FIFO_LEVEL           = %u\n", level);
	printf("DROP_COUNT           = %u\n", drop_count);
	printf("STATUS               = 0x%08x\n", status);
	printf("IRQ_STATUS           = 0x%08x\n", irq_status);

	if (level != 256 || drop_count == 0 ||
	    !(status & STATUS_FIFO_FULL) || !(status & STATUS_OVERFLOW) ||
	    !(irq_status & IRQ_OVERFLOW)) {
		printf("Overflow behavior [FAIL]\n");
		failed = -1;
	} else {
		printf("Overflow behavior [PASS]\n");
	}

	daq_write(DAQ_IRQ_CLEAR, IRQ_OVERFLOW);
	if (daq_read(DAQ_STATUS) & STATUS_OVERFLOW) {
		printf("Overflow clear [FAIL]\n");
		failed = -1;
	} else {
		printf("Overflow clear [PASS]\n");
	}

	daq_write(DAQ_CTRL, CTRL_FIFO_CLEAR);
	failed |= expect_u32("FIFO_LEVEL after clear", daq_read(DAQ_FIFO_LEVEL), 0);

	printf("\nDAQ full functional test: %s\n", failed ? "FAIL" : "PASS");
	return failed ? 1 : 0;
}
