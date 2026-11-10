/*************************************************************************
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * See LICENSE.txt for more license information
 *************************************************************************/

#include "ratls_cert.h"
#include "core.h"
#include "debug.h"
#include "ratls_openssl.h"
#include "ratls_tdx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NCCL_TLS_BACKEND_OPENSSL3)

#define NCCL_RATLS_CERT_DAYS_VALID 1
#define NCCL_RATLS_RSA_BITS 2048

// Helper: wipe and free a heap buffer.
static void ratlsClearAndFree(unsigned char* buf, size_t len) {
  if (buf == nullptr) return;
  volatile unsigned char* p = buf;
  for (size_t i = 0; i < len; i++) p[i] = 0;
  free(buf);
}

static bool ratlsAddNamePart(X509_NAME* name, const char* field, const char* value) {
  return ncclCryptOpenSsl.pfn_X509_NAME_add_entry_by_txt(
    name, field, MBSTRING_ASC, (const unsigned char*)value, -1, -1, 0) == 1;
}

static X509_NAME* ratlsCreateName(void) {
  X509_NAME* name = ncclCryptOpenSsl.pfn_X509_NAME_new();
  if (name == nullptr) return nullptr;
  if (!ratlsAddNamePart(name, "CN", "nccl-ratls")) {
    ncclCryptOpenSsl.pfn_X509_NAME_free(name);
    return nullptr;
  }
  return name;
}

static bool ratlsFindQuoteExtension(X509* cert, const unsigned char** quoteOut, size_t* quoteLenOut) {
  ASN1_OBJECT* targetObj = ncclCryptOpenSsl.pfn_OBJ_txt2obj(NCCL_RATLS_TDX_QUOTE_OID, 1);
  if (targetObj == nullptr) return false;

  bool found = false;
  int extCount = ncclCryptOpenSsl.pfn_X509_get_ext_count(cert);
  for (int i = 0; i < extCount && !found; i++) {
    X509_EXTENSION* ext = ncclCryptOpenSsl.pfn_X509_get_ext(cert, i);
    if (ext == nullptr) continue;
    ASN1_OBJECT* obj = ncclCryptOpenSsl.pfn_X509_EXTENSION_get_object(ext);
    if (obj == nullptr) continue;
    if (ncclCryptOpenSsl.pfn_OBJ_cmp(obj, targetObj) != 0) continue;
    ASN1_OCTET_STRING* data = ncclCryptOpenSsl.pfn_X509_EXTENSION_get_data(ext);
    if (data == nullptr) continue;
    *quoteOut = ncclCryptOpenSsl.pfn_ASN1_STRING_get0_data(data);
    *quoteLenOut = (size_t)ncclCryptOpenSsl.pfn_ASN1_STRING_length(data);
    found = true;
  }

  ncclCryptOpenSsl.pfn_ASN1_OBJECT_free(targetObj);
  return found;
}

ncclResult_t ncclRAtlsGenerateCert(unsigned char** certDer, size_t* certLen,
                                   void** certX509, void** pkeyOut) {
  if (certDer != nullptr) {
    *certDer = nullptr;
    *certLen = 0;
  }
  if (certX509 != nullptr) *certX509 = nullptr;
  if (pkeyOut != nullptr) *pkeyOut = nullptr;

  ncclResult_t ret = ncclSuccess;
  RSA* rsa = nullptr;
  EVP_PKEY* pkey = nullptr;
  X509* cert = nullptr;
  X509_NAME* name = nullptr;
  ASN1_OBJECT* obj = nullptr;
  ASN1_OCTET_STRING* octet = nullptr;
  X509_EXTENSION* ext = nullptr;
  unsigned char* quote = nullptr;
  unsigned char* der = nullptr;
  int derLen = 0;

  // 1. Ephemeral RSA key.
  rsa = ncclCryptOpenSsl.pfn_RSA_new();
  pkey = ncclCryptOpenSsl.pfn_EVP_PKEY_new();
  if (rsa == nullptr || pkey == nullptr) {
    WARN("ncclRAtlsGenerateCert: key allocation failed");
    ret = ncclSystemError;
    goto fail;
  }
  if (ncclCryptOpenSsl.pfn_RSA_generate_key_ex(rsa, NCCL_RATLS_RSA_BITS, nullptr, nullptr) != 1 ||
      ncclCryptOpenSsl.pfn_EVP_PKEY_set1_RSA(pkey, rsa) != 1) {
    WARN("ncclRAtlsGenerateCert: RSA key generation failed");
    ret = ncclSystemError;
    goto fail;
  }

  // 2. X.509 v3 certificate.
  cert = ncclCryptOpenSsl.pfn_X509_new();
  if (cert == nullptr) {
    ret = ncclSystemError;
    goto fail;
  }
  if (ncclCryptOpenSsl.pfn_X509_set_version(cert, 2) != 1 || // 2 == v3
      ncclCryptOpenSsl.pfn_ASN1_INTEGER_set(ncclCryptOpenSsl.pfn_X509_get_serialNumber(cert), 1) != 1) {
    WARN("ncclRAtlsGenerateCert: certificate version/serial failed");
    ret = ncclSystemError;
    goto fail;
  }

  // Validity: notBefore = now, notAfter = now + 1 day.
  if (ncclCryptOpenSsl.pfn_X509_gmtime_adj(ncclCryptOpenSsl.pfn_X509_getm_notBefore(cert), 0) == nullptr ||
      ncclCryptOpenSsl.pfn_X509_gmtime_adj(ncclCryptOpenSsl.pfn_X509_getm_notAfter(cert),
                                           60LL * 60 * 24 * NCCL_RATLS_CERT_DAYS_VALID) == nullptr) {
    WARN("ncclRAtlsGenerateCert: certificate validity failed");
    ret = ncclSystemError;
    goto fail;
  }

  name = ratlsCreateName();
  if (name == nullptr ||
      ncclCryptOpenSsl.pfn_X509_set_subject_name(cert, name) != 1 ||
      ncclCryptOpenSsl.pfn_X509_set_issuer_name(cert, name) != 1 ||
      ncclCryptOpenSsl.pfn_X509_set_pubkey(cert, pkey) != 1) {
    WARN("ncclRAtlsGenerateCert: certificate name/key setup failed");
    ret = ncclSystemError;
    goto fail;
  }

  // 3. Embed the TDX quote as a custom extension.
  NCCLCHECKGOTO(ncclCalloc(&quote, NCCL_RATLS_TDX_REPORT_LEN), ret, fail);
  NCCLCHECKGOTO(ncclRAtlsTdxGetReport(quote, NCCL_RATLS_TDX_REPORT_LEN), ret, fail);

  obj = ncclCryptOpenSsl.pfn_OBJ_txt2obj(NCCL_RATLS_TDX_QUOTE_OID, 1);
  octet = ncclCryptOpenSsl.pfn_ASN1_OCTET_STRING_new();
  if (obj == nullptr || octet == nullptr ||
      ncclCryptOpenSsl.pfn_ASN1_OCTET_STRING_set(octet, quote, NCCL_RATLS_TDX_REPORT_LEN) != 1) {
    WARN("ncclRAtlsGenerateCert: quote extension allocation failed");
    ret = ncclSystemError;
    goto fail;
  }
  ext = ncclCryptOpenSsl.pfn_X509_EXTENSION_create_by_OBJ(nullptr, obj, /*critical=*/0, octet);
  if (ext == nullptr || ncclCryptOpenSsl.pfn_X509_add_ext(cert, ext, -1) != 1) {
    WARN("ncclRAtlsGenerateCert: quote extension add failed");
    ret = ncclSystemError;
    goto fail;
  }

  // 4. Self-sign and export to DER.
  if (ncclCryptOpenSsl.pfn_X509_sign(cert, pkey, ncclCryptOpenSsl.pfn_EVP_sha256()) <= 0) {
    WARN("ncclRAtlsGenerateCert: certificate signing failed");
    ret = ncclSystemError;
    goto fail;
  }
  derLen = ncclCryptOpenSsl.pfn_i2d_X509(cert, &der);
  if (derLen <= 0 || der == nullptr) {
    WARN("ncclRAtlsGenerateCert: certificate DER encoding failed");
    ret = ncclSystemError;
    goto fail;
  }

  // Hand ownership to the caller for the requested representations.
  if (certDer != nullptr) {
    *certDer = der;
    *certLen = (size_t)derLen;
    der = nullptr;
  }
  if (certX509 != nullptr) {
    *certX509 = cert;
    cert = nullptr;
  }
  if (pkeyOut != nullptr) {
    *pkeyOut = pkey;
    pkey = nullptr;
  }

fail:
  ratlsClearAndFree(quote, NCCL_RATLS_TDX_REPORT_LEN);
  ncclCryptOpenSsl.pfn_X509_EXTENSION_free(ext);
  ncclCryptOpenSsl.pfn_ASN1_OCTET_STRING_free(octet);
  ncclCryptOpenSsl.pfn_ASN1_OBJECT_free(obj);
  ncclCryptOpenSsl.pfn_X509_NAME_free(name);
  ncclCryptOpenSsl.pfn_X509_free(cert);
  ncclCryptOpenSsl.pfn_EVP_PKEY_free(pkey);
  ncclCryptOpenSsl.pfn_RSA_free(rsa);
  if (der != nullptr) ncclCryptOpenSsl.pfn_OPENSSL_cleanse(der, derLen);
  return ret;
}

int ncclRAtlsVerifyPeerCert(void* peerCertVoid, const unsigned char* expectedMrtd) {
  X509* cert = (X509*)peerCertVoid;
  if (cert == nullptr) {
    WARN("ncclRAtlsVerifyPeerCert: no peer certificate");
    return 0;
  }

  const unsigned char* quote = nullptr;
  size_t quoteLen = 0;
  if (!ratlsFindQuoteExtension(cert, &quote, &quoteLen)) {
    WARN("ncclRAtlsVerifyPeerCert: peer certificate does not contain a TDX quote extension");
    return 0;
  }
  if (quoteLen != NCCL_RATLS_TDX_REPORT_LEN) {
    WARN("ncclRAtlsVerifyPeerCert: peer quote length %zu != expected %d", quoteLen, NCCL_RATLS_TDX_REPORT_LEN);
    return 0;
  }

  unsigned char mrtd[NCCL_RATLS_TDX_MRTD_BYTES];
  if (ncclRAtlsTdxGetMrtd(quote, mrtd) != ncclSuccess) {
    WARN("ncclRAtlsVerifyPeerCert: failed to extract MRTD from peer quote");
    return 0;
  }

  if (expectedMrtd != nullptr) {
    if (memcmp(mrtd, expectedMrtd, NCCL_RATLS_TDX_MRTD_BYTES) != 0) {
      WARN("ncclRAtlsVerifyPeerCert: peer MRTD does not match expected measurement");
      return 0;
    }
  }

  return 1;
}

#endif // NCCL_TLS_BACKEND_OPENSSL3
