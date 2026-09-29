// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2025, Linaro Limited
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <console.h>
#include <drivers/gic.h>
#include <drivers/qcom_geni_uart.h>
#include <kernel/boot.h>
#include <mm/core_mmu.h>
#include <platform_config.h>

#if defined(CFG_DRIVERS_TPM2_SPI)
#include <drivers/qcom_geni_spi.h>
#include <drivers/tpm2_spi.h>
#include <initcall.h>
#include <inttypes.h>
#include <spi.h>
#endif

#include "diag_log.h"

/*
 * Register the physical memory area for peripherals etc. Here we are
 * registering the UART console.
 */
register_phys_mem_pgdir(MEM_AREA_IO_NSEC, GENI_UART_REG_BASE,
			GENI_UART_REG_SIZE);

register_phys_mem_pgdir(MEM_AREA_IO_SEC, GICD_BASE, GIC_DIST_REG_SIZE);
#ifdef _CFG_ARM_GIC_V3_OR_V4
register_phys_mem_pgdir(MEM_AREA_IO_SEC, GICR_BASE,
			GIC_REDIST_REG_SIZE * CFG_TEE_CORE_NB_CORE);
#else
register_phys_mem_pgdir(MEM_AREA_IO_SEC, GICC_BASE, GIC_CPU_REG_SIZE);
#endif

register_ddr(DRAM0_BASE, DRAM0_SIZE);
#ifdef DRAM1_BASE
register_ddr(DRAM1_BASE, DRAM1_SIZE);
#endif
#ifdef DRAM2_BASE
register_ddr(DRAM2_BASE, DRAM2_SIZE);
#endif

static struct qcom_geni_uart_data console_data;

void plat_trace_ext_puts(const char *str)
{
	qcom_diag_log_puts(str);
}

void plat_trace_init(void)
{
	qcom_diag_log_init();
}

void plat_console_init(void)
{
	qcom_geni_uart_init(&console_data, GENI_UART_REG_BASE);
	register_serial_console(&console_data.chip);
}

static TEE_Result platform_banner(void)
{
	IMSG("Platform Qualcomm: Flavor %s", TO_STR(PLATFORM_FLAVOR));

	return TEE_SUCCESS;
}

boot_final(platform_banner);

void boot_primary_init_intc(void)
{
#ifdef _CFG_ARM_GIC_V3_OR_V4
	gic_init_v3(0, GICD_BASE, GICR_BASE);
#else
	gic_init(GICC_BASE, GICD_BASE);
#endif
}

void boot_secondary_init_intc(void)
{
	gic_init_per_cpu();
}

#if defined(CFG_DRIVERS_TPM2_SPI)
static struct qup_spi_data tpm2_spi_qs;

static TEE_Result init_tpm2_spi(void)
{
	TEE_Result res = TEE_SUCCESS;

	res = qup_spi_init(&tpm2_spi_qs, CFG_TPM2_SPI_SE_ID);
	if (res) {
		EMSG("TPM2: qup_spi_init(%u) failed: %#" PRIx32,
		     (unsigned int)CFG_TPM2_SPI_SE_ID, res);
		return res;
	}

	tpm2_spi_qs.speed_hz = CFG_TPM2_SPI_SPEED_HZ;
	tpm2_spi_qs.bits_per_word = 8;
	tpm2_spi_qs.mode = SPI_MODE0;
	tpm2_spi_qs.cs = CFG_TPM2_SPI_CS;
	tpm2_spi_qs.cs_high = false;
	tpm2_spi_qs.loopback = false;
	tpm2_spi_qs.chip.ops->configure(&tpm2_spi_qs.chip);

	if (tpm2_spi_init(&tpm2_spi_qs)) {
		EMSG("TPM2: chip registration failed");
		return TEE_ERROR_GENERIC;
	}

	DMSG("TPM2 SPI chip initialized");

	return TEE_SUCCESS;
}
driver_init(init_tpm2_spi);
#endif /* defined(CFG_DRIVERS_TPM2_SPI) */
