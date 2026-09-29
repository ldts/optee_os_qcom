/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __DRIVERS_TPM2_SPI_H__
#define __DRIVERS_TPM2_SPI_H__

#include <drivers/qcom_geni_spi.h>
#include <drivers/tpm2_chip.h>

/*
 * Register a TPM2 chip reachable over SPI using the TCG PC Client PTP
 * SPI (TIS) protocol. The caller owns *qs and must have completed
 * qup_spi_init() plus configure() on it before calling this; the handle
 * must remain valid for the lifetime of the registered chip.
 */
enum tpm2_result tpm2_spi_init(struct qup_spi_data *qs);

#endif	/* __DRIVERS_TPM2_SPI_H__ */
