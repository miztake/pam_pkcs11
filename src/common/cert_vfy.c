/*
 * PKCS #11 PAM Login Module
 * Copyright (C) 2003 Mario Strasser <mast@gmx.net>,
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * $Id$
 */

#include <string.h>
#include <openssl/objects.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include "debug.h"
#include "error.h"
#include "uri.h"
#include "cert_vfy.h"

static int base64_to_bin(const char *str, unsigned char *buf, size_t buf_len)
{
  int i;
  size_t str_len = strlen(str);
  size_t pad_len = 0;
  size_t length = 0;
  unsigned char val[4], *b = buf;

  while (str_len > 3) {
    /* char -> int */
    for (i = 0; i < 4;) {
      if ('A' <= *str && *str <= 'Z')
        val[i++] = *str - 'A';
      else if ('a' <= *str && *str <= 'z')
        val[i++] = (*str - 'a') + 26;
      else if ('0' <= *str && *str <= '9')
        val[i++] = (*str - '0') + 52;
      else if (*str == '+')
        val[i++] = 62;
      else if (*str == '/')
        val[i++] = 63;
      else if (*str == '=')
        val[i++] = 0, pad_len++;
      str++;
      if (--str_len < 0)
        return -1;
    }
    /* 32bit -> 24bit */
    if (++length > buf_len)
      return -1;
    *b++ = (val[0] << 2) + (val[1] >> 4);
    if (pad_len == 2)
      break;
    if (++length > buf_len)
      return -1;
    *b++ = (val[1] << 4) + (val[2] >> 2);
    if (pad_len == 1)
      break;
    if (++length > buf_len)
      return -1;
    *b++ = (val[2] << 6) + val[3];
  }
  return length;
}

static X509_CRL *download_crl(const char *uri)
{
  int rv;
  unsigned int i, j;
  unsigned char *data, *der, *p;
  size_t data_len, der_len;
  X509_CRL *crl;

  rv = get_from_uri(uri, &data, &data_len);
  if (rv != 0) {
    set_error("get_from_uri() failed: %s", get_error());
    return NULL;
  }
  /* convert base64 to der if needed */
  for (i = 0; i <= data_len - 24; i++) {
    if (!strncmp((const char *)&data[i], "-----BEGIN X509 CRL-----", 24))
      break;
  }
  for (j = 0; j <= data_len - 22; j++) {
    if (!strncmp((const char *)&data[j], "-----END X509 CRL-----", 22))
      break;
  }
  if (i <= data_len - 24 && j <= data_len - 22 && i < j) {
    /* base64 format */
    DBG("crl is base64 encoded");
    der_len = (j - i + 1);      /* roughly */
    der = malloc(der_len);
    if (der == NULL) {
      free(data);
      set_error("not enough free memory available");
      return NULL;
    }
    data[j] = 0;
    der_len = base64_to_bin((const char *)&data[i + 24], der, der_len);
    free(data);
    if (der_len <= 0) {
      set_error("invalid base64 (pem) format");
      return NULL;
    }
    p = der;
    crl = d2i_X509_CRL(NULL, &p, der_len);
    free(der);
  } else {
    /* der format */
    DBG("crl is der encoded");
    p = data;
    crl = d2i_X509_CRL(NULL, &p, data_len);
    free(data);
  }
  if (crl == NULL)
    set_error("d2i_X509_CRL() failed");
  return crl;
}

static int verify_crl(X509_CRL * crl, X509_STORE_CTX * ctx)
{
  int rv;
  X509_OBJECT obj;
  EVP_PKEY *pkey;

  /* get issuer certificate */
  rv = X509_STORE_get_by_subject(ctx, X509_LU_X509, X509_CRL_get_issuer(crl), &obj);
  if (rv <= 0) {
    set_error("getting the certificate of the crl-issuer failed");
    return -1;
  }
  /* extract public key and verify signature */
  pkey = X509_get_pubkey(obj.data.x509);
  X509_OBJECT_free_contents(&obj);
  if (pkey == NULL) {
    set_error("getting the issuer's public key failed");
    return -1;
  }
  rv = X509_CRL_verify(crl, pkey);
  EVP_PKEY_free(pkey);
  if (rv < 0) {
    set_error("X509_CRL_verify() failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  } else if (rv == 0) {
    DBG("crl is invalid");
    return 0;
  }
  /* compare update times */
  rv = X509_cmp_current_time(X509_CRL_get_lastUpdate(crl));
  if (rv == 0) {
    set_error("crl has an invalid last update field");
    return -1;
  }
  if (rv > 0) {
    DBG("crl is not yet valid");
    return 0;
  }
  rv = X509_cmp_current_time(X509_CRL_get_nextUpdate(crl));
  if (rv == 0) {
    set_error("crl has an invalid next update field");
    return -1;
  }
  if (rv < 0) {
    DBG("crl has expired");
    return 0;
  }
  return 1;
}

/* the structure DIST_POINT_NAME_st has been changed from 0.9.6 to 0.9.7 */
#if OPENSSL_VERSION_NUMBER >= 0x00907000L
#define GET_FULLNAME(a) a->name.fullname
#else
#define GET_FULLNAME(a) a->fullname
#endif

static int check_for_revocation(X509 * x509, X509_STORE_CTX * ctx, crl_policy_t policy)
{
  int rv, i, j;
  X509_OBJECT obj;
  X509_REVOKED rev;
  STACK_OF(DIST_POINT) * dist_points;
  DIST_POINT *point;
  GENERAL_NAME *name;
  X509_CRL *crl;

  DBG1("crl policy: %d", policy);
  if (policy == CRLP_NONE) {
    /* NONE */
    DBG("no revocation-check performed");
    return 1;
  } else if (policy == CRLP_AUTO) {
    /* AUTO -> first try it ONLINE then OFFLINE */
    rv = check_for_revocation(x509, ctx, CRLP_ONLINE);
    if (rv < 0) {
      DBG1("check_for_revocation() failed: %s", get_error());
      rv = check_for_revocation(x509, ctx, CRLP_OFFLINE);
    }
    return rv;
  } else if (policy == CRLP_OFFLINE) {
    /* OFFLINE */
    DBG("looking for an dedicated local crl");
    rv = X509_STORE_get_by_subject(ctx, X509_LU_CRL, X509_get_issuer_name(x509), &obj);
    if (rv <= 0) {
      set_error("no dedicated crl available");
      return -1;
    }
    crl = X509_CRL_dup(obj.data.crl);
    X509_OBJECT_free_contents(&obj);
  } else if (policy == CRLP_ONLINE) {
    /* ONLINE */
    DBG("extracting crl distribution points");
    dist_points = X509_get_ext_d2i(x509, NID_crl_distribution_points, NULL, NULL);
    if (dist_points == NULL) {
      /* if there is not crl distribution point in the certificate hava a look at the ca certificate */
      rv = X509_STORE_get_by_subject(ctx, X509_LU_X509, X509_get_issuer_name(x509), &obj);
      if (rv <= 0) {
        set_error("no dedicated ca certificate available");
        return -1;
      }
      dist_points = X509_get_ext_d2i(obj.data.x509, NID_crl_distribution_points, NULL, NULL);
      X509_OBJECT_free_contents(&obj);
      if (dist_points == NULL) {
        set_error("neither the user nor the ca certificate does contain a crl distribution point");
        return -1;
      }
    }
    crl = NULL;
    for (i = 0; i < sk_DIST_POINT_num(dist_points) && crl == NULL; i++) {
      point = sk_DIST_POINT_value(dist_points, i);
      /* until now, only fullName is supported */
      if (point->distpoint != NULL && GET_FULLNAME(point->distpoint) != NULL) {
        for (j = 0; j < sk_GENERAL_NAME_num(GET_FULLNAME(point->distpoint)); j++) {
          name = sk_GENERAL_NAME_value(GET_FULLNAME(point->distpoint), j);
          if (name != NULL && name->type == GEN_URI) {
            DBG1("downloading crl from %s", name->d.ia5->data);
            crl = download_crl((const char *)name->d.ia5->data);
	    
            /*crl = download_crl("file:///home/mario/projects/pkcs11_login/tests/ca_crl_0.pem"); */
            /*crl = download_crl("http://www-t.zhwin.ch/ca/root_ca.crl"); */
            /*crl = download_crl("http://www.zhwin.ch/~sri/"); */
            /*crl = download_crl("ldap://directory.verisign.com:389/CN=VeriSign IECA, OU=IECA-3, OU=Contractor, OU=PKI, OU=DOD, O=U.S. Government, C=US?certificateRevocationList;binary"); */
            if (crl != NULL)
              break;
            else
              DBG1("download_crl() failed: %s", get_error());
          }
        }
      }
    }
    sk_DIST_POINT_pop_free(dist_points, DIST_POINT_free);
    if (crl == NULL) {
      set_error("downloading the crl failed for all distribution points");
      return -1;
    }
  } else {
    set_error("policy %d is not supported", policy);
    return -1;
  }
  /* verify the crl and check whether the certificate is revoked or not */
  DBG("verifying crl");
  rv = verify_crl(crl, ctx);
  if (rv < 0) {
    X509_CRL_free(crl);
    set_error("verify_crl() failed: %s", get_error());
    return -1;
  } else if (rv == 0) {
    return 0;
  }
  rev.serialNumber = X509_get_serialNumber(x509);
  rv = sk_X509_REVOKED_find(crl->crl->revoked, &rev);
  X509_CRL_free(crl);
  return (rv == -1);
}

int verify_certificate(X509 * x509, char *ca_dir, char *crl_dir, crl_policy_t policy)
{
  int rv;
  X509_STORE *store;
  X509_STORE_CTX *ctx;
  X509_LOOKUP *lookup;

  /* setup the x509 store to verify the certificate */
  store = X509_STORE_new();
  if (store == NULL) {
    set_error("X509_STORE_new() failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  }
  lookup = X509_STORE_add_lookup(store, X509_LOOKUP_hash_dir());
  if (lookup == NULL) {
    X509_STORE_free(store);
    set_error("X509_STORE_add_lookup() failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  }
  DBG1("adding ca certificate lookup dir %s", ca_dir);
  rv = X509_LOOKUP_add_dir(lookup, ca_dir, X509_FILETYPE_PEM);
  if (rv != 1) {
    X509_LOOKUP_free(lookup);
    X509_STORE_free(store);
    set_error("X509_LOOKUP_add_dir(PEM) failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  }
  rv = X509_LOOKUP_add_dir(lookup, ca_dir, X509_FILETYPE_ASN1);
  if (rv != 1) {
    X509_LOOKUP_free(lookup);
    X509_STORE_free(store);
    set_error("X509_LOOKUP_add_dir(ASN1) failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  }
  DBG1("adding crl lookup dir %s", crl_dir);
  rv = X509_LOOKUP_add_dir(lookup, crl_dir, X509_FILETYPE_PEM);
  if (rv != 1) {
    X509_LOOKUP_free(lookup);
    X509_STORE_free(store);
    set_error("X509_LOOKUP_add_dir(PEM) failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  }
  rv = X509_LOOKUP_add_dir(lookup, crl_dir, X509_FILETYPE_ASN1);
  if (rv != 1) {
    X509_LOOKUP_free(lookup);
    X509_STORE_free(store);
    set_error("X509_LOOKUP_add_dir(ASN1) failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  }
  ctx = X509_STORE_CTX_new();
  if (ctx == NULL) {
    X509_STORE_free(store);
    set_error("X509_STORE_CTX_new() failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  }
  X509_STORE_CTX_init(ctx, store, x509, NULL);
#if 0
  X509_STORE_CTX_set_purpose(ctx, purpose);
#endif
  rv = X509_verify_cert(ctx);
  if (rv != 1) {
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store); 
    set_error("certificate is invalid: %s", X509_verify_cert_error_string(ctx->error));
    return 0;  
  } else {
    DBG("certificate is valid");
  }
  /* verify whether the certificate was revoked or not */
  rv = check_for_revocation(x509, ctx, policy);
  X509_STORE_CTX_free(ctx);
  X509_STORE_free(store);
  if (rv < 0) {
    set_error("check_for_revocation() failed: %s", get_error());
    return -1;
  } else if (rv == 0) {
    DBG("certificate has been revoked");
  } else {
    DBG("certificate has not been revoked");
  }
  return rv;
}

int verify_signature(X509 * x509, unsigned char *data, int data_length,
                     unsigned char *signature, int signature_length)
{
  int rv;
  EVP_PKEY *pubkey;
  EVP_MD_CTX md_ctx;

  /* get the public-key */
  pubkey = X509_get_pubkey(x509);
  if (pubkey == NULL) {
    set_error("X509_get_pubkey() failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  }
  /* verify the signature */
  EVP_VerifyInit(&md_ctx, EVP_sha1());
  EVP_VerifyUpdate(&md_ctx, data, data_length);
  rv = EVP_VerifyFinal(&md_ctx, signature, signature_length, pubkey);
  EVP_PKEY_free(pubkey);
  if (rv != 1) {
    set_error("EVP_VerifyFinal() failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return -1;
  }
  DBG("signature is valid");
  return 0;
}