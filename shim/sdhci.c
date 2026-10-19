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
 * Nothing is ever written to the card: there is no CMD24/CMD25/CMD6 here.
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

#define PS_CMD_INHIBIT	(1u << 0)
#define PS_DAT_INHIBIT	(1u << 1)

#define INT_CMD_DONE	(1u << 0)
#define INT_XFER_DONE	(1u << 1)
#define INT_BUF_RD	(1u << 5)
#define INT_ERROR	(1u << 15)

/* Command register: index << 8 | flags */
#define C_RESP48	0x02
#define C_CRC		0x08
#define C_INDEX		0x10
#define C_DATA		0x20

/* Transfer mode */
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

void test_sdhci(void)
{
	con_str("EMMC", "ginkgo sdhci@4744000 probe (read-only, PIO)");
	uint32_t ver = hc_rd(R_VERSION);
	uint32_t present = hc_rd(R_PRESENT), clock = hc_rd(R_CLOCK);
	con_hex2("HC", "ver=", ver, "present=", present);
	con_hex2("HC", "hostctl=", hc_rd(R_HOSTCTL), "clock=", clock);
	con_hex2("HC", "ctrl2=", hc_rd(R_CTRL2), "caps=", hc_rd(R_CAPS));
	con_hex2("HC", "int_en=", hc_rd(R_INT_EN), "sig_en=", hc_rd(R_SIG_EN));
	con_hex2("CQ", "cfg=", *(volatile uint32_t *)(CQ_BASE + CQ_CFG), "ver=",
		 *(volatile uint32_t *)(CQ_BASE + 0));
	if (ver == 0 || ver == 0xffffffff) {
		con_str("EMMC", "registers read as 0/ff: host clocks off?");
		return;
	}
	if (!(clock & (1u << 2))) {
		con_str("EMMC", "SD clock is off: ABL turned the card clock down");
		return;
	}
	if (*(volatile uint32_t *)(CQ_BASE + CQ_CFG) & 1) {
		con_str("EMMC", "command queue engine is ON: stopping");
		return;
	}

	/* Latch every status bit, signal none (polled). */
	hc_wr(R_INT_EN, 0xffff00ff & ~0x100u);	/* not card interrupt */
	hc_wr(R_SIG_EN, 0);
	hc_wr(R_INT_STAT, 0xffffffff);

	/* CMD13 with the RCAs a bootloader is likely to have assigned. */
	uint32_t resp = 0, st = 0, rca = 0;
	for (uint32_t r = 1; r <= 4 && !rca; r++) {
		int e = cmd(13, r << 16, C_RESP48 | C_CRC | C_INDEX, 0, 0, &resp, &st);
		if (e == 0) {
			rca = r;
		} else {
			soft_reset_cmd_dat();
		}
	}
	if (!rca) {
		con_hex2("CMD13", "no answer st=", st, "resp=", resp);
		return;
	}
	con_hex2("CMD13", "rca=", rca, "status=", resp);
	con_hex2("CARD", "state=", (resp >> 9) & 0xf, "(4=tran) err=", resp & 0xfff80000u);

	/* CMD17: LBA 1 = GPT header (byte addressing would show garbage). */
	int e = cmd(17, 1, C_RESP48 | C_CRC | C_INDEX | C_DATA, M_BLKCNT | M_READ, 1, &resp, &st);
	if (e) {
		con_hex2("CMD17", "failed e=", (uint32_t)e, "st=", st);
		soft_reset_cmd_dat();
		return;
	}
	const uint8_t *b = (const uint8_t *)buf;
	con_hex2("LBA1", "w0=", buf[0], "w1=", buf[1]);
	if (!(b[0] == 'E' && b[1] == 'F' && b[2] == 'I' && b[3] == ' ' &&
	      b[4] == 'P' && b[5] == 'A' && b[6] == 'R' && b[7] == 'T')) {
		con_str("GPT", "no EFI PART at LBA 1");
		return;
	}
	uint64_t entry_lba = le64(b + 72), last = le64(b + 32);
	uint32_t nent = b[80] | b[81] << 8 | b[82] << 16 | (uint32_t)b[83] << 24;
	con_str("GPT", "EFI PART found");
	con_hex2("GPT", "entries@", (uint32_t)entry_lba, "n=", nent);
	con_hex("GPT backup", last);

	/* CMD18 + auto CMD12: the GPT entries, 4 per sector, until "userdata". */
	uint64_t t0 = ticks();
	for (uint32_t chunk = 0; chunk * 128 < nent && chunk < 8; chunk++) {
		e = cmd(18, (uint32_t)entry_lba + chunk * 32, C_RESP48 | C_CRC | C_INDEX | C_DATA,
			M_BLKCNT | M_ACMD12 | M_READ | M_MULTI, 32, &resp, &st);
		if (e) {
			con_hex2("CMD18", "failed e=", (uint32_t)e, "st=", st);
			soft_reset_cmd_dat();
			return;
		}
		for (int i = 0; i < 128; i++) {
			const uint8_t *ent = b + i * 128;
			const char *want = "userdata";
			int c = 0;
			while (want[c] && ent[56 + 2 * c] == (uint8_t)want[c] && ent[57 + 2 * c] == 0)
				c++;
			if (want[c] || ent[56 + 2 * c] != 0)
				continue;
			uint64_t first = le64(ent + 32), end = le64(ent + 40);
			con_hex2("USERDATA", "first=", (uint32_t)first, "last=", (uint32_t)end);
			con_hex2("USERDATA", "MiB=", (uint32_t)((end - first + 1) >> 11),
				 "read us=", (uint32_t)((ticks() - t0) * 10 / 192));
			con_str("EMMC", "OK: eMMC readable from our code");
			return;
		}
	}
	con_str("GPT", "no partition named userdata");
}
