/*************************************************************************
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * See LICENSE.txt for more license information
 *************************************************************************/

#include "ratls_tdx.h"
#include "core.h"
#include "debug.h"

#if defined(NCCL_OS_LINUX)

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// TDX guest UAPI definitions (mirrors <linux/tdx-guest.h> to avoid a build-time
// dependency on a recent kernel header).
#define NCCL_TDX_REPORTDATA_LEN 64
#define NCCL_TDX_REPORT_LEN     1024

struct ncclTdxReportReq {
  unsigned char reportdata[NCCL_TDX_REPORTDATA_LEN];
  unsigned char tdreport[NCCL_TDX_REPORT_LEN];
};

#define NCCL_TDX_CMD_GET_REPORT0 _IOWR('T', 1, struct ncclTdxReportReq)

// Offsets inside TDREPORT_STRUCT for the fields we care about.
//   reportmac: 256 bytes
//   tee_tcb_info: 239 bytes
//   reserved: 17 bytes
//   td_info.attr: 8 bytes
//   td_info.xfam: 8 bytes
//   td_info.mrtd: 48 bytes
#define NCCL_TDX_TDINFO_OFF      (256 + 239 + 17)
#define NCCL_TDX_MRTD_OFF        (NCCL_TDX_TDINFO_OFF + 8 + 8)

static constexpr const char* ncclTdxGuestDev = "/dev/tdx_guest";

ncclResult_t ncclRAtlsTdxGetReport(unsigned char* reportOut, size_t reportSize) {
  if (reportOut == nullptr || reportSize < NCCL_RATLS_TDX_REPORT_LEN) {
    WARN("ncclRAtlsTdxGetReport: output buffer must be at least %d bytes", NCCL_RATLS_TDX_REPORT_LEN);
    return ncclInvalidArgument;
  }

  int fd = open(ncclTdxGuestDev, O_RDWR | O_SYNC);
  if (fd < 0) {
    WARN("ncclRAtlsTdxGetReport: failed to open %s: %s", ncclTdxGuestDev, strerror(errno));
    return ncclSystemError;
  }

  struct ncclTdxReportReq req;
  memset(&req, 0, sizeof(req));
  // reportdata left zero: the quote is bound to the long-lived key in the
  // certificate, and a fresh key is generated each process lifetime.

  int rc = ioctl(fd, NCCL_TDX_CMD_GET_REPORT0, &req);
  if (rc != 0) {
    WARN("ncclRAtlsTdxGetReport: TDX_CMD_GET_REPORT0 failed: %s", strerror(errno));
    close(fd);
    return ncclSystemError;
  }

  memcpy(reportOut, req.tdreport, NCCL_RATLS_TDX_REPORT_LEN);
  close(fd);
  return ncclSuccess;
}

ncclResult_t ncclRAtlsTdxGetMrtd(const unsigned char* report, unsigned char mrtd[NCCL_RATLS_TDX_MRTD_BYTES]) {
  if (report == nullptr || mrtd == nullptr) return ncclInvalidArgument;
  memcpy(mrtd, report + NCCL_TDX_MRTD_OFF, NCCL_RATLS_TDX_MRTD_BYTES);
  return ncclSuccess;
}

#else // !NCCL_OS_LINUX

ncclResult_t ncclRAtlsTdxGetReport(unsigned char*, size_t) {
  WARN("ncclRAtlsTdxGetReport: TDX attestation is only supported on Linux");
  return ncclSystemError;
}

ncclResult_t ncclRAtlsTdxGetMrtd(const unsigned char*, unsigned char*) {
  return ncclSystemError;
}

#endif // NCCL_OS_LINUX
