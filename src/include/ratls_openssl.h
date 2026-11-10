/*************************************************************************
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * See LICENSE.txt for more license information
 *************************************************************************/

#ifndef NCCL_RATLS_OPENSSL_H_
#define NCCL_RATLS_OPENSSL_H_

#include "core.h"

#if defined(NCCL_TLS_BACKEND_OPENSSL3)

#include <openssl/asn1.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/kdf.h>
#include <openssl/opensslv.h>
#include <openssl/params.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#if OPENSSL_VERSION_MAJOR != 3
#error "NCCL TLS support requires OpenSSL 3"
#endif

#define NCCL_CRYPT_CRYPTO_SYMBOLS(SYMBOL) \
  SYMBOL(BIO_clear_flags) \
  SYMBOL(BIO_get_data) \
  SYMBOL(BIO_get_new_index) \
  SYMBOL(BIO_meth_free) \
  SYMBOL(BIO_meth_new) \
  SYMBOL(BIO_meth_set_create) \
  SYMBOL(BIO_meth_set_ctrl) \
  SYMBOL(BIO_meth_set_destroy) \
  SYMBOL(BIO_meth_set_read) \
  SYMBOL(BIO_meth_set_write) \
  SYMBOL(BIO_new) \
  SYMBOL(BIO_set_data) \
  SYMBOL(BIO_set_flags) \
  SYMBOL(BIO_set_init) \
  SYMBOL(CRYPTO_memcmp) \
  SYMBOL(ERR_clear_error) \
  SYMBOL(ERR_error_string_n) \
  SYMBOL(ERR_get_error) \
  SYMBOL(EVP_KDF_CTX_free) \
  SYMBOL(EVP_KDF_CTX_new) \
  SYMBOL(EVP_KDF_derive) \
  SYMBOL(EVP_KDF_fetch) \
  SYMBOL(EVP_KDF_free) \
  SYMBOL(EVP_sha256) \
  SYMBOL(OPENSSL_cleanse) \
  SYMBOL(OSSL_PARAM_construct_end) \
  SYMBOL(OSSL_PARAM_construct_octet_string) \
  SYMBOL(OSSL_PARAM_construct_utf8_string)

// Minimal SSL symbols required by the PSK and RA-TLS paths.
#define NCCL_CRYPT_SSL_SYMBOLS(SYMBOL) \
  SYMBOL(OPENSSL_init_ssl) \
  SYMBOL(SSL_CIPHER_find) \
  SYMBOL(SSL_CTX_clear_options) \
  SYMBOL(SSL_CTX_ctrl) \
  SYMBOL(SSL_CTX_free) \
  SYMBOL(SSL_CTX_new) \
  SYMBOL(SSL_CTX_set_ciphersuites) \
  SYMBOL(SSL_CTX_set_num_tickets) \
  SYMBOL(SSL_CTX_set_psk_find_session_callback) \
  SYMBOL(SSL_CTX_set_psk_use_session_callback) \
  SYMBOL(SSL_CTX_set_verify) \
  SYMBOL(SSL_do_handshake) \
  SYMBOL(SSL_free) \
  SYMBOL(SSL_get_error) \
  SYMBOL(SSL_new) \
  SYMBOL(SSL_read_ex) \
  SYMBOL(SSL_SESSION_free) \
  SYMBOL(SSL_SESSION_new) \
  SYMBOL(SSL_SESSION_set1_master_key) \
  SYMBOL(SSL_SESSION_set_cipher) \
  SYMBOL(SSL_SESSION_set_protocol_version) \
  SYMBOL(SSL_session_reused) \
  SYMBOL(SSL_set_accept_state) \
  SYMBOL(SSL_set_bio) \
  SYMBOL(SSL_set_connect_state) \
  SYMBOL(SSL_write_ex) \
  SYMBOL(TLS_method)

// Additional OpenSSL crypto symbols used only by the RA-TLS certificate path.
#define NCCL_RATLS_CRYPTO_SYMBOLS(SYMBOL) \
  SYMBOL(ASN1_INTEGER_set) \
  SYMBOL(ASN1_OBJECT_free) \
  SYMBOL(ASN1_OCTET_STRING_free) \
  SYMBOL(ASN1_OCTET_STRING_new) \
  SYMBOL(ASN1_OCTET_STRING_set) \
  SYMBOL(ASN1_STRING_get0_data) \
  SYMBOL(ASN1_STRING_length) \
  SYMBOL(ASN1_TIME_diff) \
  SYMBOL(d2i_X509) \
  SYMBOL(ERR_peek_last_error) \
  SYMBOL(EVP_PKEY_free) \
  SYMBOL(EVP_PKEY_get1_RSA) \
  SYMBOL(EVP_PKEY_new) \
  SYMBOL(EVP_PKEY_set1_RSA) \
  SYMBOL(i2d_X509) \
  SYMBOL(OBJ_cmp) \
  SYMBOL(OBJ_txt2obj) \
  SYMBOL(RSA_free) \
  SYMBOL(RSA_generate_key_ex) \
  SYMBOL(RSA_new) \
  SYMBOL(X509_add_ext) \
  SYMBOL(X509_add1_notAfter) \
  SYMBOL(X509_add1_notBefore) \
  SYMBOL(X509_EXTENSION_create_by_OBJ) \
  SYMBOL(X509_EXTENSION_free) \
  SYMBOL(X509_EXTENSION_get_data) \
  SYMBOL(X509_EXTENSION_get_object) \
  SYMBOL(X509_get_ext) \
  SYMBOL(X509_get_ext_count) \
  SYMBOL(X509_getm_notAfter) \
  SYMBOL(X509_getm_notBefore) \
  SYMBOL(X509_gmtime_adj) \
  SYMBOL(X509_NAME_add_entry_by_txt) \
  SYMBOL(X509_NAME_free) \
  SYMBOL(X509_NAME_new) \
  SYMBOL(X509_new) \
  SYMBOL(X509_set_issuer_name) \
  SYMBOL(X509_set_pubkey) \
  SYMBOL(X509_set_serialNumber) \
  SYMBOL(X509_set_subject_name) \
  SYMBOL(X509_set_version) \
  SYMBOL(X509_sign) \
  SYMBOL(X509_verify_cert_error_string)

// Additional OpenSSL SSL symbols used only by the RA-TLS certificate path.
#define NCCL_RATLS_SSL_SYMBOLS(SYMBOL) \
  SYMBOL(SSL_CTX_use_certificate) \
  SYMBOL(SSL_CTX_use_PrivateKey) \
  SYMBOL(SSL_get_peer_certificate) \
  SYMBOL(X509_STORE_CTX_get_current_cert) \
  SYMBOL(X509_STORE_CTX_get_error)

#define NCCL_CRYPT_DECLARE_SYMBOL(name) decltype(&::name) pfn_##name;

struct ncclCryptOpenSslSymbols {
  NCCL_CRYPT_CRYPTO_SYMBOLS(NCCL_CRYPT_DECLARE_SYMBOL)
  NCCL_CRYPT_SSL_SYMBOLS(NCCL_CRYPT_DECLARE_SYMBOL)
  NCCL_RATLS_CRYPTO_SYMBOLS(NCCL_CRYPT_DECLARE_SYMBOL)
  NCCL_RATLS_SSL_SYMBOLS(NCCL_CRYPT_DECLARE_SYMBOL)
};

#undef NCCL_CRYPT_DECLARE_SYMBOL

extern struct ncclCryptOpenSslSymbols ncclCryptOpenSsl;

#endif // NCCL_TLS_BACKEND_OPENSSL3
#endif // NCCL_RATLS_OPENSSL_H_
