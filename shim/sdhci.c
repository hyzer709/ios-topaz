/*
 * ginkgo (Redmi Note 8) probe: can we talk to the eMMC that ABL booted from?
 *
 * The Note 8 has no UFS; its storage is eMMC behind a Qualcomm SDHCI host
 * (sdhci@4744000, "sdhc_1"). This probe runs on the gold CPU with the MMU off,
 * dumps the SDHCI registers ABL left behind, and if the host looks alive it
 * issues READ-ONLY commands in PIO mode (no DMA at all):
 *   CMD13 SEND_STATUS   - find the card's RCA and check it is in TRAN state
 *   CMD17 READ_SINGLE   - LBA 1, must start with "EFI PART"
 *   CMD18 READ_MULTIPLE - the GPT entries, look for "userdata"
 * v2: ABL leaves the host with the SD clock off, bus power off and a 1-bit bus,
 * so the probe re-initializes the card (CMD0/1/2/3/7, then CMD6 to switch the
 * bus to 8-bit high speed). CMD6 only changes the card's volatile bus settings
 * (EXT_CSD BUS_WIDTH/HS_TIMING); no user data is ever written: there is no
 * CMD24/CMD25 in this file.
 *
 * Register layout: SD Host Controller Simplified Specification 3.00.
 * Every register access is 32 bits wide.
 */
#include <stdint.h>
#include "console.h"
#include "sdhci.h"

#define HC_BASE		0x04744000UL	/* sdhci@4744000 "hc_mem", ginkgo live FDT */
#define CQ_BASE		0x04745000UL	/* "cqhci_mem" (command queue engine) */

#define R_BLOCK		0x04	/* [15:0] block size, [31:16] block count */
#define R_ARG		0x08
#define R_XFER_CMD	0x0c	/* [15:0] transfer mode, [31:16] command */
#define R_RESP0		0x10
#define R_DATA		0x20
#define R_PRESENT	0x24
#define R_HOSTCTL	0x28
#define R_CLOCK		0x2c	/* [15:0] clock, [23:16] timeout, [31:24] soft reset */
#define R_INT_STAT	0x30	/* [15:0] normal, [31:16] error */
#define R_INT_EN	0x34
#define R_SIG_EN	0x38
#define R_CTRL2		0x3c	/* [31:16] host control 2 */
#define R_CAPS		0x40
#define R_CAPS1		0x44
#define R_VERSION	0xfc	/* [31:16] host version */

#define CQ_CFG		0x08

/* sdhci-msm v5 vendor power-control handshake (offsets from hc_mem) */
#define R_PWRCTL_STATUS	0x240
#define R_PWRCTL_CLEAR	0x248
#define R_PWRCTL_CTL	0x24c

#define PS_CMD_INHIBIT	(1u << 0)
#define PS_DAT_INHIBIT	(1u << 1)

#define INT_CMD_DONE	(1u << 0)
#define INT_XFER_DONE	(1u << 1)
#define INT_BUF_RD	(1u << 5)
#define INT_ERROR	(1u << 15)

/* Command register: index << 8 | flags */
#define C_RESP136	0x01
#define C_RESP48	0x02
#define C_RESP48B	0x03
#define C_CRC		0x08
#define C_INDEX		0x10
#define C_DATA		0x20

/* Transfer mode */
#define M_DMA		(1u << 0)
#define M_BLKCNT	(1u << 1)
#define M_ACMD12	(1u << 2)
#define M_READ		(1u << 4)
#define M_MULTI		(1u << 5)

#define TIMEOUT_TICKS	(19200000u / 2)		/* 0.5 s at 19.2 MHz */

static inline uint32_t hc_rd(uint32_t off) { return *(volatile uint32_t *)(HC_BASE + off); }
static inline void hc_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(HC_BASE + off) = v; }

static uint32_t buf[32 * 512 / 4];	/* up to 32 sectors */

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

/* Wait for any bit of mask (or an error) in the interrupt status. */
static int wait_int(uint32_t mask, uint32_t *st)
{
	uint64_t end = ticks() + TIMEOUT_TICKS;
	uint32_t s;
	while (!((s = hc_rd(R_INT_STAT)) & (mask | INT_ERROR))) {
		if (ticks() > end) {
			*st = s;
			return -1;
		}
	}
	*st = s;
	return (s & INT_ERROR) ? -2 : 0;
}

static void soft_reset_cmd_dat(void)
{
	hc_wr(R_CLOCK, hc_rd(R_CLOCK) | (6u << 24));	/* reset CMD and DAT lines */
	uint64_t end = ticks() + TIMEOUT_TICKS;
	while ((hc_rd(R_CLOCK) >> 24) & 6)
		if (ticks() > end)
			break;
	hc_wr(R_INT_STAT, 0xffffffff);
}

static int wait_idle(uint32_t bits)
{
	uint64_t end = ticks() + TIMEOUT_TICKS;
	while (hc_rd(R_PRESENT) & bits)
		if (ticks() > end)
			return -1;
	return 0;
}

/* Issue one command. For data commands, reads nblk sectors into buf (PIO). */
static int cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t mode, uint32_t nblk,
	       uint32_t *resp, uint32_t *st)
{
	*st = 0;
	if (wait_idle(PS_CMD_INHIBIT | (nblk ? PS_DAT_INHIBIT : 0)))
		return -3;
	hc_wr(R_INT_STAT, 0xffffffff);
	if (nblk)
		hc_wr(R_BLOCK, (nblk << 16) | 512);
	hc_wr(R_ARG, arg);
	hc_wr(R_XFER_CMD, ((idx << 8 | flags) << 16) | mode);
	int r = wait_int(INT_CMD_DONE, st);
	if (r)
		return r;
	hc_wr(R_INT_STAT, INT_CMD_DONE);
	*resp = hc_rd(R_RESP0);
	for (uint32_t b = 0; b < nblk; b++) {
		r = wait_int(INT_BUF_RD, st);
		if (r)
			return r;
		hc_wr(R_INT_STAT, INT_BUF_RD);
		for (int w = 0; w < 128; w++)
			buf[b * 128 + w] = hc_rd(R_DATA);
	}
	if (nblk) {
		r = wait_int(INT_XFER_DONE, st);
		if (r)
			return r;
		hc_wr(R_INT_STAT, INT_XFER_DONE);
	}
	return 0;
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static void delay_us(uint32_t us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (ticks() < end)
		;
}

/*
 * The msm host raises a power-control request whenever bus power or a full
 * reset is written and waits for software to acknowledge it (Linux:
 * sdhci_msm_handle_pwr_irq). The regulators are already on here, so just
 * report success for whatever was requested.
 */
static uint32_t pwrctl_ack(void)
{
	uint64_t end = ticks() + TIMEOUT_TICKS / 5;
	uint32_t st;
	while (!(st = hc_rd(R_PWRCTL_STATUS)))
		if (ticks() > end)
			return 0;
	hc_wr(R_PWRCTL_CLEAR, st);
	uint32_t ok = 0;
	if (st & 3)		/* BUS_OFF / BUS_ON */
		ok |= 1;
	if (st & 0xc)		/* IO_LOW / IO_HIGH */
		ok |= 4;
	hc_wr(R_PWRCTL_CTL, ok);
	return st;
}

/* SD clock = base / (2 * div); div 0 = base. */
static void set_clock(uint32_t div)
{
	uint32_t c = hc_rd(R_CLOCK) & ~0xffffu;
	hc_wr(R_CLOCK, c | (div & 0xff) << 8 | ((div >> 8) & 3) << 6 | 1);
	uint64_t end = ticks() + TIMEOUT_TICKS;
	while (!(hc_rd(R_CLOCK) & 2))
		if (ticks() > end)
			break;
	hc_wr(R_CLOCK, hc_rd(R_CLOCK) | 4);
	delay_us(1000);
}

/* Wait for the end of a busy signal on DAT0 (R1b commands). */
static void wait_busy(void)
{
	uint32_t st;
	(void)wait_int(INT_XFER_DONE, &st);
	hc_wr(R_INT_STAT, 0xffffffff);
	uint64_t end = ticks() + TIMEOUT_TICKS;
	while (!(hc_rd(R_PRESENT) & (1u << 20)))
		if (ticks() > end)
			break;
}

static int read_lba1(const char *label)
{
	uint32_t resp, st;
	int e = cmd(17, 1, C_RESP48 | C_CRC | C_INDEX | C_DATA, M_BLKCNT | M_READ, 1, &resp, &st);
	if (e) {
		con_hex2(label, "CMD17 e=", (uint32_t)e, "st=", st);
		soft_reset_cmd_dat();
		return -1;
	}
	const uint8_t *b = (const uint8_t *)buf;
	int ok = b[0] == 'E' && b[1] == 'F' && b[2] == 'I' && b[3] == ' ' &&
		 b[4] == 'P' && b[5] == 'A' && b[6] == 'R' && b[7] == 'T';
	con_hex2(label, ok ? "EFI PART w0=" : "no GPT w0=", buf[0], "w1=", buf[1]);
	return ok ? 0 : -1;
}

/*
 * v3: 32-bit ADMA2 reads (the PIO path above costs ~1.5 us per 32-bit word,
 * far too slow for an OS). MMU off, so this memory is Device and coherent.
 * Descriptor: u16 attr (valid|end|act=tran), u16 length, u32 address.
 */
#define R_ADMA_ADDR	0x58
#define R_ADMA_ADDR_HI	0x5c
#define INT_ADMA_ERR	(1u << 25)
#define ADMA_CHUNK	0x8000u
#define BIG_SECTORS	2048u			/* 1 MiB */

struct adma_desc { uint16_t attr, len; uint32_t addr; };
static struct adma_desc adma_tbl[BIG_SECTORS * 512 / ADMA_CHUNK] __attribute__((aligned(8)));
/* 1 MiB scratch in free DDR (System RAM 0x80000000+; nothing else runs here). */
#define big ((uint8_t *)0x90000000UL)

static int adma_read(uint32_t lba, uint32_t nblk, uint8_t *dst, uint32_t *st)
{
	uint32_t bytes = nblk * 512, n = 0;
	for (uint32_t off = 0; off < bytes; off += ADMA_CHUNK, n++) {
		uint32_t len = bytes - off < ADMA_CHUNK ? bytes - off : ADMA_CHUNK;
		adma_tbl[n].attr = 0x21 | (off + len >= bytes ? 2 : 0);
		adma_tbl[n].len = (uint16_t)len;
		adma_tbl[n].addr = (uint32_t)(uintptr_t)(dst + off);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*st = 0;
	if (wait_idle(PS_CMD_INHIBIT | PS_DAT_INHIBIT))
		return -3;
	hc_wr(R_HOSTCTL, (hc_rd(R_HOSTCTL) & ~(3u << 3)) | (2u << 3));	/* 32-bit ADMA2 */
	hc_wr(R_ADMA_ADDR, (uint32_t)(uintptr_t)adma_tbl);
	hc_wr(R_ADMA_ADDR_HI, 0);
	hc_wr(R_INT_STAT, 0xffffffff);
	hc_wr(R_BLOCK, (nblk << 16) | 512);
	hc_wr(R_ARG, lba);
	hc_wr(R_XFER_CMD, ((18u << 8 | C_RESP48 | C_CRC | C_INDEX | C_DATA) << 16) |
	      M_DMA | M_BLKCNT | M_ACMD12 | M_READ | M_MULTI);
	int r = wait_int(INT_XFER_DONE, st);
	hc_wr(R_INT_STAT, 0xffffffff);
	hc_wr(R_HOSTCTL, hc_rd(R_HOSTCTL) & ~(3u << 3));
	return r;
}

static uint32_t sum32(const void *p, uint32_t bytes)
{
	const uint32_t *w = p;
	uint32_t s = 0;
	for (uint32_t i = 0; i < bytes / 4; i++)
		s = s * 31 + w[i];
	return s;
}

static void test_adma(uint32_t entry_lba, uint32_t userdata_first)
{
	uint32_t st;
	/* Same 32 GPT-entry sectors as the PIO read: the checksums must match. */
	uint32_t pio_sum = sum32(buf, 32 * 512);
	int e = adma_read(entry_lba, 32, big, &st);
	if (e) {
		con_hex2("ADMA", "failed e=", (uint32_t)e, "st=", st);
		soft_reset_cmd_dat();
		return;
	}
	uint32_t dma_sum = sum32(big, 32 * 512);
	con_hex2("ADMA", "pio sum=", pio_sum, "dma sum=", dma_sum);
	if (pio_sum != dma_sum) {
		con_str("ADMA", "MISMATCH: DMA data differs from PIO");
		return;
	}
	/* 1 MiB from the start of userdata, timed. */
	uint64_t t0 = ticks();
	e = adma_read(userdata_first, BIG_SECTORS, big, &st);
	uint32_t us = (uint32_t)((ticks() - t0) * 10 / 192);
	if (e) {
		con_hex2("ADMA 1M", "failed e=", (uint32_t)e, "st=", st);
		soft_reset_cmd_dat();
		return;
	}
	con_hex2("ADMA 1M", "KiB/s=", us ? BIG_SECTORS * 512u / us * 1000u : 0, "us=", us);
	con_str("ADMA", "OK: DMA reads work");
}

void test_sdhci(void)
{
	con_str("EMMC", "ginkgo sdhci probe v2 (no data writes)");
	uint32_t ver = hc_rd(R_VERSION);
	con_hex2("HC", "ver=", ver, "present=", hc_rd(R_PRESENT));
	con_hex2("HC", "hostctl=", hc_rd(R_HOSTCTL), "clock=", hc_rd(R_CLOCK));
	con_hex2("HC", "pwrctl=", hc_rd(R_PWRCTL_STATUS), "caps=", hc_rd(R_CAPS));
	if (ver == 0 || ver == 0xffffffff) {
		con_str("EMMC", "registers read as 0/ff: host clocks off?");
		return;
	}
	if (*(volatile uint32_t *)(CQ_BASE + CQ_CFG) & 1) {
		con_str("EMMC", "command queue engine is ON: stopping");
		return;
	}
	uint32_t base_mhz = (hc_rd(R_CAPS) >> 8) & 0xff;
	if (!base_mhz)
		base_mhz = 200;

	hc_wr(R_INT_EN, 0xffff00ff & ~0x100u);
	hc_wr(R_SIG_EN, 0);
	hc_wr(R_INT_STAT, 0xffffffff);

	/* Bus power on (1.8 V signalling setting), acknowledged to the msm core. */
	uint32_t hc = hc_rd(R_HOSTCTL);
	if (!(hc & (1u << 8))) {
		hc_wr(R_HOSTCTL, (hc & ~0xff00u) | (0x0bu << 8));
		con_hex2("POWER", "on, pwrctl=", pwrctl_ack(), "hostctl=", hc_rd(R_HOSTCTL));
	}
	/* Data timeout: max. Identification clock: 400 kHz. */
	hc_wr(R_CLOCK, (hc_rd(R_CLOCK) & ~(0xffu << 16)) | (0x0eu << 16));
	set_clock(base_mhz * 1000 / 800);		/* base / (2 * div) = 400 kHz */
	con_hex2("CLOCK", "400k reg=", hc_rd(R_CLOCK), "present=", hc_rd(R_PRESENT));

	/* Is the card still selected from ABL? (diagnostic only) */
	uint32_t resp = 0, st = 0, rca = 0;
	for (uint32_t r = 1; r <= 4 && !rca; r++) {
		if (cmd(13, r << 16, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st) == 0)
			rca = r;
		else
			soft_reset_cmd_dat();
	}
	if (rca)
		con_hex2("CMD13", "rca=", rca, "state=", (resp >> 9) & 0xf);
	else
		con_hex2("CMD13", "no answer st=", st, "rca=", 0);

	/* Full identification: CMD0, CMD1 until ready, CMD2, CMD3 (RCA 1), CMD7. */
	(void)cmd(0, 0, 0, 0, 0, &resp, &st);
	delay_us(2000);
	uint32_t ocr = 0;
	for (int i = 0; i < 1000; i++) {
		if (cmd(1, 0x40ff8080, C_RESP48, 0, 0, &ocr, &st) != 0) {
			soft_reset_cmd_dat();
			ocr = 0;
		}
		if (ocr & (1u << 31))
			break;
		delay_us(1000);
	}
	con_hex2("CMD1", "ocr=", ocr, "st=", st);
	if (!(ocr & (1u << 31))) {
		con_str("EMMC", "card did not power up (no OCR ready)");
		return;
	}
	if (cmd(2, 0, C_RESP136 | C_CRC, 0, 0, &resp, &st)) {
		con_hex2("CMD2", "failed st=", st, "", 0);
		return;
	}
	con_hex2("CID", "w0=", resp, "w1=", hc_rd(R_RESP0 + 4));
	if (cmd(3, 1u << 16, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st) ||
	    cmd(7, 1u << 16, C_RESP48B | C_CRC | C_INDEX, 0, 0, &resp, &st)) {
		con_hex2("CMD3/7", "failed st=", st, "resp=", resp);
		return;
	}
	wait_busy();
	(void)cmd(13, 1u << 16, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st);
	con_hex2("SELECTED", "rca=", 1, "state=", (resp >> 9) & 0xf);

	if (read_lba1("LBA1 slow"))
		return;

	/* 8-bit bus, high speed (52 MHz SDR): EXT_CSD BUS_WIDTH=2, HS_TIMING=1. */
	if (cmd(6, 3u << 24 | 183u << 16 | 2u << 8, C_RESP48B | C_CRC | C_INDEX, 0, 0, &resp, &st) == 0) {
		wait_busy();
		hc_wr(R_HOSTCTL, hc_rd(R_HOSTCTL) | (1u << 5));		/* 8-bit */
	} else {
		soft_reset_cmd_dat();
	}
	if (cmd(6, 3u << 24 | 185u << 16 | 1u << 8, C_RESP48B | C_CRC | C_INDEX, 0, 0, &resp, &st) == 0) {
		wait_busy();
		hc_wr(R_HOSTCTL, hc_rd(R_HOSTCTL) | (1u << 2));		/* high speed */
		set_clock(base_mhz / 100);			/* base / (2 * div) = 50 MHz */
	} else {
		soft_reset_cmd_dat();
	}
	con_hex2("FAST", "hostctl=", hc_rd(R_HOSTCTL), "clock=", hc_rd(R_CLOCK));
	if (read_lba1("LBA1 fast"))
		return;

	const uint8_t *b = (const uint8_t *)buf;
	uint64_t entry_lba = le64(b + 72);
	uint32_t nent = b[80] | b[81] << 8 | b[82] << 16 | (uint32_t)b[83] << 24;
	con_hex2("GPT", "entries@", (uint32_t)entry_lba, "n=", nent);

	/* CMD18 + auto CMD12: the GPT entries, 4 per sector, until "userdata". */
	uint64_t t0 = ticks();
	uint32_t sectors = 0;
	for (uint32_t chunk = 0; chunk * 128 < nent && chunk < 8; chunk++) {
		int e = cmd(18, (uint32_t)entry_lba + chunk * 32, C_RESP48 | C_CRC | C_INDEX | C_DATA,
			M_BLKCNT | M_ACMD12 | M_READ | M_MULTI, 32, &resp, &st);
		if (e) {
			con_hex2("CMD18", "failed e=", (uint32_t)e, "st=", st);
			soft_reset_cmd_dat();
			return;
		}
		sectors += 32;
		for (int i = 0; i < 128; i++) {
			const uint8_t *ent = b + i * 128;
			const char *want = "userdata";
			int c = 0;
			while (want[c] && ent[56 + 2 * c] == (uint8_t)want[c] && ent[57 + 2 * c] == 0)
				c++;
			if (want[c] || ent[56 + 2 * c] != 0)
				continue;
			uint32_t us = (uint32_t)((ticks() - t0) * 10 / 192);
			uint64_t first = le64(ent + 32), end = le64(ent + 40);
			con_hex2("USERDATA", "first=", (uint32_t)first, "last=", (uint32_t)end);
			con_hex2("SPEED", "KiB/s=", us ? sectors * 512u * 1000u / us : 0, "us=", us);
			con_str("EMMC", "OK: eMMC readable from our code");
			test_adma((uint32_t)entry_lba + chunk * 32, (uint32_t)first);
			return;
		}
	}
	con_str("GPT", "no partition named userdata");
}
