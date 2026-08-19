#include <stdint.h>
#include <stdio.h>

#define DAQ_BASE		0x64002000UL

#define DAQ_CTRL		0x08
#define DAQ_STATUS		0x0c
#define DAQ_SAMPLE_PERIOD	0x10
#define DAQ_CHANNEL_ENABLE	0x14
#define DAQ_FIFO_LEVEL		0x18
#define DAQ_FIFO_WATERMARK	0x1c
#define DAQ_IRQ_ENABLE		0x20
#define DAQ_IRQ_STATUS		0x24
#define DAQ_DROP_COUNT		0x28
#define DAQ_SAMPLE_COUNT	0x2c
#define DAQ_FIFO_DATA0		0x30
#define DAQ_FIFO_DATA1		0x34
#define DAQ_FIFO_DATA2		0x38
#define DAQ_FIFO_DATA3		0x3c
#define DAQ_FIFO_POP		0x40
#define DAQ_TEST_PATTERN	0x44
#define DAQ_IRQ_CLEAR		0x48

#define DAQ_CTRL_ENABLE		(1U << 0)
#define DAQ_CTRL_FIFO_CLEAR	(1U << 1)
#define DAQ_CTRL_COUNTER_CLEAR	(1U << 2)

#define DAQ_IRQ_WATERMARK	(1U << 0)
#define DAQ_IRQ_OVERFLOW	(1U << 1)

#define PLIC_BASE		0x0c000000UL

#define PLIC_PRIORITY(id)	(PLIC_BASE + ((uintptr_t)(id) * 4))
#define PLIC_ENABLE_M0		(PLIC_BASE + 0x2000)
#define PLIC_THRESHOLD_M0	(PLIC_BASE + 0x200000)
#define PLIC_CLAIM_M0		(PLIC_BASE + 0x200004)

/*
 * Verify this with the generated "Interrupt map".
 */
#define DAQ_IRQ_ID		2U

#define MSTATUS_MIE		(1UL << 3)
#define MIE_MEIE		(1UL << 11)

#define MCAUSE_INTERRUPT	(1UL << (sizeof(uintptr_t) * 8 - 1))
#define MCAUSE_CODE_MASK	(~MCAUSE_INTERRUPT)
#define MCAUSE_MEI		11UL

static volatile uint32_t irq_seen;
static volatile uint32_t irq_claim;
static volatile uint32_t irq_status;
static volatile uint32_t irq_fifo_level;
static volatile uint32_t trap_error;

static inline uint32_t mmio_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
	asm volatile("fence iorw, iorw" ::: "memory");
}

static inline void csr_set_mie(uintptr_t mask)
{
	asm volatile("csrs mie, %0" :: "r"(mask) : "memory");
}

static inline void csr_clear_mie(uintptr_t mask)
{
	asm volatile("csrc mie, %0" :: "r"(mask) : "memory");
}

static inline void csr_set_mstatus(uintptr_t mask)
{
	asm volatile("csrs mstatus, %0" :: "r"(mask) : "memory");
}

static inline uint64_t read_mcycle(void)
{
	uint64_t val;

	asm volatile("csrr %0, mcycle" : "=r"(val));
	return val;
}

static void plic_enable_irq(unsigned int irq)
{
	uintptr_t addr;
	uint32_t val;

	addr = PLIC_ENABLE_M0 + ((irq / 32) * 4);

	val = mmio_read32(addr);
	val |= 1U << (irq % 32);
	mmio_write32(addr, val);
}

static void plic_disable_irq(unsigned int irq)
{
	uintptr_t addr;
	uint32_t val;

	addr = PLIC_ENABLE_M0 + ((irq / 32) * 4);

	val = mmio_read32(addr);
	val &= ~(1U << (irq % 32));
	mmio_write32(addr, val);
}

static void plic_init(void)
{
	/*
	 * DAQ interrupt priority > threshold.
	 */
	mmio_write32(PLIC_PRIORITY(DAQ_IRQ_ID), 1);
	mmio_write32(PLIC_THRESHOLD_M0, 0);

	plic_enable_irq(DAQ_IRQ_ID);
}

/*
 * libgloss-htif trap_entry calls this hook.
 *
 * Verify the exact prototype against your local libgloss source before
 * compiling:
 *
 * grep -R "handle_trap" ~/grad/chipyard/toolchains/libgloss -n | head
 */
static volatile uintptr_t trap_epc;
static volatile uintptr_t trap_cause;
static volatile uintptr_t trap_tval;

void handle_trap(uintptr_t epc, uintptr_t cause,
		 uintptr_t tval, uintptr_t regs[32])
{
	uint32_t claim;

	(void)regs;

	/*
	 * Save trap state for main() to inspect.
	 */
	trap_epc = epc;
	trap_cause = cause;
	trap_tval = tval;

	/*
	 * One-shot debug mode.
	 *
	 * Disable Machine External Interrupt immediately so an unexpected
	 * level IRQ cannot cause an interrupt storm after mret.
	 */
	csr_clear_mie(MIE_MEIE);

	if (!(cause & MCAUSE_INTERRUPT)) {
		trap_error = 1;
		return;
	}

	if ((cause & MCAUSE_CODE_MASK) != MCAUSE_MEI) {
		trap_error = 2;
		return;
	}

	/*
	 * Claim the highest-priority pending interrupt for M context 0.
	 */
	claim = mmio_read32(PLIC_CLAIM_M0);
	irq_claim = claim;

	if (claim != DAQ_IRQ_ID) {
		trap_error = 3;

		if (claim != 0)
			mmio_write32(PLIC_CLAIM_M0, claim);

		return;
	}

	/*
	 * Snapshot DAQ state before removing the level interrupt source.
	 */
	irq_status = mmio_read32(DAQ_BASE + DAQ_IRQ_STATUS);
	irq_fifo_level = mmio_read32(DAQ_BASE + DAQ_FIFO_LEVEL);
	irq_seen++;

	/*
	 * Watermark IRQ is level-triggered.
	 *
	 * Remove the source before completing the PLIC interrupt.
	 */
	mmio_write32(DAQ_BASE + DAQ_IRQ_ENABLE, 0);
	mmio_write32(DAQ_BASE + DAQ_CTRL, 0);

	/*
	 * Complete IRQ source 2.
	 */
	mmio_write32(PLIC_CLAIM_M0, claim);
}
int main(void)
{
	uint64_t start;
	uint32_t level;
	uint32_t status;
	uint32_t samples;
	int failed = 0;

	printf("FPGA DAQ Engine IRQ test\n\n");

	/*
	 * Make sure CPU interrupt delivery is disabled while configuring.
	 */
	csr_clear_mie(MIE_MEIE);

	/*
	 * Reset DAQ runtime state.
	 *
	 * bit1 FIFO_CLEAR
	 * bit2 COUNTER_CLEAR
	 */
	mmio_write32(DAQ_BASE + DAQ_CTRL,
		     DAQ_CTRL_FIFO_CLEAR | DAQ_CTRL_COUNTER_CLEAR);

	/*
	 * Slow enough to observe clearly but still fast in Verilator.
	 */
	mmio_write32(DAQ_BASE + DAQ_SAMPLE_PERIOD, 10000);
	mmio_write32(DAQ_BASE + DAQ_CHANNEL_ENABLE, 0xf);
	mmio_write32(DAQ_BASE + DAQ_TEST_PATTERN, 0x1234);

	/*
	 * IRQ should assert when four samples are in FIFO.
	 */
	mmio_write32(DAQ_BASE + DAQ_FIFO_WATERMARK, 4);

	/*
	 * Configure PLIC before allowing the DAQ to interrupt.
	 */
	plic_init();

	/*
	 * Enable machine external interrupt delivery.
	 */
	csr_set_mie(MIE_MEIE);
	csr_set_mstatus(MSTATUS_MIE);

	/*
	 * Enable only the DAQ watermark interrupt.
	 */
	mmio_write32(DAQ_BASE + DAQ_IRQ_ENABLE,
		     DAQ_IRQ_WATERMARK);

	/*
	 * Start DAQ.
	 */
	mmio_write32(DAQ_BASE + DAQ_CTRL,
		     DAQ_CTRL_ENABLE);

	start = read_mcycle();

	while (!irq_seen && !trap_error) {
		if (read_mcycle() - start > 1000000ULL)
			break;
	}

	/*
	 * Stop accepting external interrupts while checking results.
	 */
	csr_clear_mie(MIE_MEIE);

	printf("irq_seen       = %u\n", irq_seen);
	printf("irq_claim      = %u\n", irq_claim);
	printf("irq_status     = 0x%08x\n", irq_status);
	printf("irq_fifo_level = %u\n", irq_fifo_level);
	printf("trap_error     = %u\n", trap_error);

	if (!irq_seen) {
		printf("IRQ delivery          [FAIL]\n");
		failed = 1;
	} else {
		printf("IRQ delivery          [PASS]\n");
	}

	if (irq_claim != DAQ_IRQ_ID) {
		printf("PLIC claim            [FAIL]\n");
		failed = 1;
	} else {
		printf("PLIC claim            [PASS]\n");
	}

	if (!(irq_status & DAQ_IRQ_WATERMARK)) {
		printf("Watermark status      [FAIL]\n");
		failed = 1;
	} else {
		printf("Watermark status      [PASS]\n");
	}

	if (irq_status & DAQ_IRQ_OVERFLOW) {
		printf("Unexpected overflow   [FAIL]\n");
		failed = 1;
	} else {
		printf("No overflow           [PASS]\n");
	}

	if (irq_fifo_level < 4) {
		printf("FIFO watermark level  [FAIL]\n");
		failed = 1;
	} else {
		printf("FIFO watermark level  [PASS]\n");
	}

	if (trap_error) {
		printf("Trap handling         [FAIL]\n");
		failed = 1;
	} else {
		printf("Trap handling         [PASS]\n");
	}

	/*
	 * Check the post-IRQ state as seen from normal program context.
	 */
	level = mmio_read32(DAQ_BASE + DAQ_FIFO_LEVEL);
	status = mmio_read32(DAQ_BASE + DAQ_IRQ_STATUS);
	samples = mmio_read32(DAQ_BASE + DAQ_SAMPLE_COUNT);

	printf("\nPost IRQ state\n");
	printf("FIFO_LEVEL   = %u\n", level);
	printf("IRQ_STATUS   = 0x%08x\n", status);
	printf("SAMPLE_COUNT = %u\n", samples);

	/*
	 * Cleanup.
	 */
	mmio_write32(DAQ_BASE + DAQ_IRQ_ENABLE, 0);
	mmio_write32(DAQ_BASE + DAQ_CTRL,
		     DAQ_CTRL_FIFO_CLEAR | DAQ_CTRL_COUNTER_CLEAR);

	plic_disable_irq(DAQ_IRQ_ID);

	printf("\nDAQ watermark IRQ test: %s\n",
	       failed ? "FAIL" : "PASS");

	return failed ? 1 : 0;
}
