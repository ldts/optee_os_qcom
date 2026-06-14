/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2026, Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _GPDSP1_H_
#define _GPDSP1_H_

#include "pas_data.h"

TEE_Result gpdsp1_get_resource_table(struct resource_table *rt, size_t *size);
struct qcom_pas_data *gpdsp1_get_pas_data(void);
TEE_Result gpdsp1_fw_shutdown(void);
TEE_Result gpdsp1_fw_start(void);

#endif /* _GPDSP1_H_ */
