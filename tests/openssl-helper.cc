// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include <sys/types.h>
#include <sys/socket.h>

#include "openssl/bio.h"
#include "openssl/ssl.h"
#include "openssl/err.h"

static const char kKeyFile[] = "testdata/openssl.key";
static const char kCertFile[] = "testdata/openssl.crt";

static int sni_cb(SSL *s, int *ad, void *arg) {
  const char* servername = SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);
  if (servername && strcmp(servername, "test.example.com") == 0)
    *reinterpret_cast<bool*>(arg) = true;

  return SSL_TLSEXT_ERR_OK;
}

static const char kTestSrcDirAdditional[] =
#if defined(TEST_SRCDIR_ADDITIONAL)
#define xstr(x) str(x)
#define str(x) #x
 "" xstr(TEST_SRCDIR_ADDITIONAL) "";
#undef xstr
#undef str
#else
  "";
#endif

static std::string
TestData(const char *filename) {
  const char* testSrcDir = getenv("TEST_SRCDIR");
  if (testSrcDir) {
    return std::string(testSrcDir) + "/" + kTestSrcDirAdditional + "/" + filename;
  } else {
    return filename;
  }
}

int
main(int argc, char **argv) {
  SSL_library_init();
  ERR_load_crypto_strings();
  OpenSSL_add_all_algorithms();
  SSL_load_error_strings();

  bool sni = false, sni_good = false, snap_start = false, snap_start_recovery = false, sslv3 = false, session_tickets = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "sni") == 0) {
      sni = true;
    } else if (strcmp(argv[i], "snap-start") == 0) {
      snap_start = true;
    } else if (strcmp(argv[i], "snap-start-recovery") == 0) {
      snap_start = true;
      snap_start_recovery = true;
    } else if (strcmp(argv[i], "sslv3") == 0) {
      sslv3 = true;
    } else if (strcmp(argv[i], "session-tickets") == 0) {
      session_tickets = true;
    } else {
      fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      return 1;
    }
  }

  SSL_CTX* ctx;

  if (sslv3) {
    ctx = SSL_CTX_new(SSLv3_server_method());
  } else {
    ctx = SSL_CTX_new(TLSv1_server_method());
  }

  if (sni) {
    SSL_CTX_set_tlsext_servername_callback(ctx, sni_cb);
    SSL_CTX_set_tlsext_servername_arg(ctx, &sni_good);
  }

  BIO* key = BIO_new(BIO_s_file());
  if (BIO_read_filename(key, TestData(kKeyFile).c_str()) <= 0) {
    fprintf(stderr, "Failed to read %s\n", TestData(kKeyFile).c_str());
    return 1;
  }

  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(key, NULL, NULL, NULL);
  if (!pkey) {
    fprintf(stderr, "Failed to parse %s\n", kKeyFile);
    return 1;
  }
  BIO_free(key);


  BIO* cert = BIO_new(BIO_s_file());
  if (BIO_read_filename(cert, TestData(kCertFile).c_str()) <= 0) {
    fprintf(stderr, "Failed to read %s\n", kCertFile);
    return 1;
  }

  X509 *pcert = PEM_read_bio_X509_AUX(cert, NULL, NULL, NULL);
  if (!pcert) {
    fprintf(stderr, "Failed to parse %s\n", kCertFile);
    return 1;
  }
  BIO_free(cert);

  if (SSL_CTX_use_certificate(ctx, pcert) <= 0) {
    fprintf(stderr, "Failed to load %s\n", kCertFile);
    return 1;
  }

  if (SSL_CTX_use_PrivateKey(ctx, pkey) <= 0) {
    fprintf(stderr, "Failed to load %s\n", kKeyFile);
    return 1;
  }

  if (!SSL_CTX_check_private_key(ctx)) {
    fprintf(stderr, "Public and private keys don't match\n");
    return 1;
  }

  if (session_tickets)
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_BOTH);

  if (snap_start) {
    unsigned char orbit[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    SSL_CTX_set_snap_start_orbit(ctx, orbit);
  }

  unsigned connection_limit = 1;
  if (snap_start || session_tickets)
    connection_limit = 2;

  for (unsigned connections = 0; connections < connection_limit; connections++) {
    SSL* server = SSL_new(ctx);
    BIO* bio = BIO_new_socket(3, 0 /* don't take ownership of fd */);
    SSL_set_bio(server, bio, bio);

    int err;
    for (;;) {
      const int ret = SSL_accept(server);
      if (ret != 1) {
        err = SSL_get_error(server, ret);
        if (err == SSL_ERROR_WANT_READ)
          continue;
        if (err == SSL_ERROR_SERVER_RANDOM_VALIDATION_PENDING && snap_start) {
          SSL_set_suggested_server_random_validity(server, !snap_start_recovery);
          continue;
        }
        ERR_print_errors_fp(stderr);
        fprintf(stderr, "SSL_accept failed: %d\n", err);
        return 1;
      } else {
        break;
      }
    }

    if (sni && !sni_good) {
      fprintf(stderr, "SNI failed\n");
      return 1;
    }

    unsigned char buffer[6];
    int ret = SSL_read(server, buffer, sizeof(buffer));
    if (ret == -1) {
      err = SSL_get_error(server, ret);
      ERR_print_errors_fp(stderr);
      fprintf(stderr, "SSL_read failed: %d\n", err);
    }
    if (memcmp(buffer, "hello!", sizeof(buffer)) == 0) {
      SSL_write(server, "goodbye!", 8);
    }

    SSL_shutdown(server);
    SSL_free(server);
  }

  SSL_CTX_free(ctx);

  return 0;
}
