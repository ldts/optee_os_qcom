// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * TPM2 physical transport backend implementing the TCG PC Client Platform
 * TPM Profile (PTP) SPI protocol (a.k.a. SPI TIS) on top of the Qualcomm
 * GENI SPI driver. It exposes the tpm2_ptp_phy_ops register interface the
 * FIFO/TIS state machine (tpm2_ptp_fifo.c) expects, translating each
 * register access into a CS-held SPI transaction.
 */

#include <assert.h>
#include <drivers/qcom_geni_spi.h>
#include <drivers/tpm2_chip.h>
#include <drivers/tpm2_ptp_fifo.h>
#include <drivers/tpm2_spi.h>
#include <io.h>
#include <kernel/delay.h>
#include <spi.h>
#include <string.h>
#include <trace.h>
#include <util.h>

/* Max payload of a single SPI TIS transaction, as encoded in the header */
#define TPM2_SPI_MAX_XFER	64

/*
 * The TPM register window lives at 0x00D4_xxxx in the SPI TIS address
 * space. The FIFO layer passes TIS register offsets below 0x10000
 * (locality is encoded in bits [15:12]).
 */
#define TPM2_SPI_TIS_BASE	0x00d40000

/* Header byte 0: read/write direction bit and (len - 1) size field */
#define TPM2_SPI_READ		0x80
#define TPM2_SPI_HDR_LEN	4

/* Wait-state flow control: MISO LSB set means the TPM is ready */
#define TPM2_SPI_WAIT_STATE_RDY	BIT(0)

/* Number of wait-state polling bytes before giving up */
#define TPM2_SPI_WAIT_RETRIES	50

static struct qup_spi_data *tpm2_spi_qs;

static const struct tpm2_ptp_ops tpm2_fifo_ops = {
	.init = tpm2_fifo_init,
	.end = tpm2_fifo_end,
	.send = tpm2_fifo_send,
	.recv = tpm2_fifo_recv,
};

/*
 * Run one SPI TIS transaction for up to TPM2_SPI_MAX_XFER bytes at TIS
 * register offset adr. CS is asserted for the whole header + wait-state +
 * data sequence and deasserted at the end.
 */
static enum tpm2_result tpm2_spi_xfer_one(struct qup_spi_data *qs, bool read,
					  uint32_t adr, uint16_t len,
					  uint8_t *buf)
{
	uint8_t hdr[TPM2_SPI_HDR_LEN] = { };
	uint8_t rsp[TPM2_SPI_HDR_LEN] = { };
	uint8_t zero[TPM2_SPI_MAX_XFER] = { };
	uint32_t addr = TPM2_SPI_TIS_BASE | (adr & 0xffff);
	struct spi_chip *chip = &qs->chip;
	enum tpm2_result ret = TPM2_ERR_IO;
	unsigned int retries = 0;
	bool ready = false;

	assert(len >= 1 && len <= TPM2_SPI_MAX_XFER);

	hdr[0] = (read ? TPM2_SPI_READ : 0) | (len - 1);
	hdr[1] = (addr >> 16) & 0xff;
	hdr[2] = (addr >> 8) & 0xff;
	hdr[3] = addr & 0xff;

	chip->ops->start(chip);

	/* Send the 4-byte header; the TPM answers with wait-state bytes. */
	if (chip->ops->txrx8(chip, hdr, rsp, TPM2_SPI_HDR_LEN) != SPI_OK)
		goto out;

	/*
	 * Per the PTP spec the LSB of the last header response byte signals
	 * whether the TPM is ready. If not, keep clocking single bytes until
	 * it is (bounded), inserting wait states.
	 */
	if (rsp[TPM2_SPI_HDR_LEN - 1] & TPM2_SPI_WAIT_STATE_RDY) {
		ready = true;
	} else {
		for (retries = 0; retries < TPM2_SPI_WAIT_RETRIES; retries++) {
			uint8_t tx = 0;
			uint8_t rx = 0;

			if (chip->ops->txrx8(chip, &tx, &rx, 1) != SPI_OK)
				goto out;

			if (rx & TPM2_SPI_WAIT_STATE_RDY) {
				ready = true;
				break;
			}

			mdelay(TPM2_TIMEOUT_RETRY_MS);
		}
	}

	if (!ready) {
		ret = TPM2_ERR_TIMEOUT;
		goto out;
	}

	/* Data phase; drive the unused direction with zeros (full-duplex). */
	if (read) {
		if (chip->ops->txrx8(chip, zero, buf, len) != SPI_OK)
			goto out;
	} else {
		if (chip->ops->txrx8(chip, buf, zero, len) != SPI_OK)
			goto out;
	}

	ret = TPM2_OK;
out:
	chip->ops->end(chip);

	return ret;
}

/*
 * Byte-stream access at TIS offset adr. The FIFO layer may request bursts
 * larger than a single SPI TIS transaction can carry (e.g. DATA_FIFO
 * reads/writes), so split into <= TPM2_SPI_MAX_XFER chunks. The same
 * register offset is reused for every chunk: the TPM's DATA_FIFO auto-
 * advances internally and status/access registers are single bytes.
 */
static enum tpm2_result tpm2_spi_rw8(struct qup_spi_data *qs, bool read,
				     uint32_t adr, uint16_t len, uint8_t *buf)
{
	enum tpm2_result ret = TPM2_OK;
	uint16_t done = 0;

	if (!len)
		return TPM2_ERR_INVALID_ARG;

	while (done < len) {
		uint16_t chunk = MIN((uint16_t)(len - done),
				     (uint16_t)TPM2_SPI_MAX_XFER);

		ret = tpm2_spi_xfer_one(qs, read, adr, chunk, buf + done);
		if (ret)
			return ret;

		done += chunk;
	}

	return TPM2_OK;
}

static enum tpm2_result tpm2_spi_rx8(struct tpm2_chip *chip __unused,
				     uint32_t adr, uint16_t len, uint8_t *buf)
{
	return tpm2_spi_rw8(tpm2_spi_qs, true, adr, len, buf);
}

static enum tpm2_result tpm2_spi_tx8(struct tpm2_chip *chip __unused,
				     uint32_t adr, uint16_t len, uint8_t *buf)
{
	return tpm2_spi_rw8(tpm2_spi_qs, false, adr, len, buf);
}

static enum tpm2_result tpm2_spi_rx32(struct tpm2_chip *chip __unused,
				      uint32_t adr, uint32_t *buf)
{
	uint8_t b[4] = { };
	enum tpm2_result ret = TPM2_OK;

	ret = tpm2_spi_rw8(tpm2_spi_qs, true, adr, sizeof(b), b);
	if (ret)
		return ret;

	*buf = get_le32(b);

	return TPM2_OK;
}

static enum tpm2_result tpm2_spi_tx32(struct tpm2_chip *chip __unused,
				      uint32_t adr, uint32_t val)
{
	uint8_t b[4] = { };

	put_le32(b, val);

	return tpm2_spi_rw8(tpm2_spi_qs, false, adr, sizeof(b), b);
}

static const struct tpm2_ptp_phy_ops tpm2_spi_ops = {
	.rx32 = tpm2_spi_rx32,
	.tx32 = tpm2_spi_tx32,
	.rx8 = tpm2_spi_rx8,
	.tx8 = tpm2_spi_tx8,
};

static struct tpm2_chip tpm2_spi_chip = {
	.phy_ops = &tpm2_spi_ops,
	.ops = &tpm2_fifo_ops,
};

enum tpm2_result tpm2_spi_init(struct qup_spi_data *qs)
{
	enum tpm2_result ret = TPM2_OK;

	assert(qs);
	tpm2_spi_qs = qs;

	DMSG("TPM2 SPI backend on GENI SE id %u cs %u", qs->id, qs->cs);

	ret = tpm2_chip_register(&tpm2_spi_chip);
	if (ret) {
		EMSG("TPM2 SPI chip register failed: %d", ret);
		return ret;
	}

	return TPM2_OK;
}
