/*************************************************************************
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * See LICENSE.txt for more license information
 *************************************************************************/

#ifndef NCCL_RATLS_CERT_H_
#define NCCL_RATLS_CERT_H_

#include "nccl.h"
#include <stddef.h>

// OID for the NCCL RA-TLS TDX quote extension.  This is a private-use OID;
// it identifies the DER-encoded TDX TDREPORT carried inside the certificate.
#define NCCL_RATLS_TDX_QUOTE_OID "1.3.6.1.4.1.8072.666.1"

// Generate a fresh self-signed X.509 certificate that embeds the local TDX
// quote in a custom extension.  The certificate and its private key are kept
// only in memory for the lifetime of the process.
// On success *certDer points to a DER-encoded certificate of *certLen bytes,
// *certX509 and *pkey hold the in-memory OpenSSL objects, and the caller owns
// all three.  Any output pointer may be nullptr if that representation is not
// needed.
ncclResult_t ncclRAtlsGenerateCert(unsigned char** certDer, size_t* certLen,
                                   void** certX509, void** pkey);

// Verify a peer certificate presented during the TLS handshake.  expectedMrtd
// is the allowed MRTD measurement (48 bytes); if nullptr, any valid TDX quote
// is accepted (useful for debugging, not for production).
// This function is intended to be called from an OpenSSL verify callback.
int ncclRAtlsVerifyPeerCert(void* peerCertVoid, const unsigned char* expectedMrtd);

#endif
