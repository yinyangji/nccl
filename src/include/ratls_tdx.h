/*************************************************************************
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * See LICENSE.txt for more license information
 *************************************************************************/

#ifndef NCCL_RATLS_TDX_H_
#define NCCL_RATLS_TDX_H_

#include "nccl.h"
#include <stddef.h>

#define NCCL_RATLS_TDX_REPORT_LEN 1024
#define NCCL_RATLS_TDX_MRTD_BYTES 48

// Open /dev/tdx_guest and retrieve a TDREPORT0. reportOut must point to at
// least NCCL_RATLS_TDX_REPORT_LEN bytes.
ncclResult_t ncclRAtlsTdxGetReport(unsigned char* reportOut, size_t reportSize);

// Extract the build-time measurement (MRTD) from a TDREPORT.
ncclResult_t ncclRAtlsTdxGetMrtd(const unsigned char* report, unsigned char mrtd[NCCL_RATLS_TDX_MRTD_BYTES]);

#endif
