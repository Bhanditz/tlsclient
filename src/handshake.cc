// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tlsclient/src/handshake.h"

#include <vector>

#include "tlsclient/public/context.h"
#include "tlsclient/public/error.h"
#include "tlsclient/src/base-internal.h"
#include "tlsclient/src/buffer.h"
#include "tlsclient/src/crypto/prf/prf.h"
#include "tlsclient/src/connection_private.h"
#include "tlsclient/src/error-internal.h"
#include "tlsclient/src/extension.h"
#include "tlsclient/src/sink.h"
#include "tlsclient/src/crypto/cipher_suites.h"

namespace tlsclient {

// RFC 5746, section 3.3
static const uint16_t kSignalingCipherSuiteValue = 0xff00;

bool IsValidHandshakeType(uint8_t type) {
  HandshakeMessage m(static_cast<HandshakeMessage>(type));

  switch (m) {
    case HELLO_REQUEST:
    case CLIENT_HELLO:
    case SERVER_HELLO:
    case CERTIFICATE:
    case SERVER_KEY_EXCHANGE:
    case CERTIFICATE_REQUEST:
    case SERVER_HELLO_DONE:
    case CERTIFICATE_VERIFY:
    case CLIENT_KEY_EXCHANGE:
    case FINISHED:
      return true;
    default:
      return false;
  }
}

bool IsValidRecordType(uint8_t wire_value) {
  RecordType t = static_cast<RecordType>(wire_value);

  switch (t) {
    case RECORD_CHANGE_CIPHER_SPEC:
    case RECORD_ALERT:
    case RECORD_HANDSHAKE:
    case RECORD_APPLICATION_DATA:
      return true;
    default:
      return false;
  }
}

bool IsValidAlertLevel(uint8_t wire_level) {
  AlertLevel level = static_cast<AlertLevel>(wire_level);

  switch (level) {
    case ALERT_LEVEL_WARNING:
    case ALERT_LEVEL_ERROR:
      return true;
    default:
      return false;
  }
}

bool IsValidVersion(uint16_t wire_version) {
  TLSVersion v = static_cast<TLSVersion>(wire_version);

  switch (v) {
    case SSLv3:
    case TLSv10:
    case TLSv11:
    case TLSv12:
      return true;
    default:
      return false;
  }
}

Result AlertTypeToResult(AlertType type) {
  switch (type) {
    case ALERT_CLOSE_NOTIFY:
      return ERROR_RESULT(ERR_ALERT_CLOSE_NOTIFY);
    case ALERT_UNEXPECTED_MESSAGE:
      return ERROR_RESULT(ERR_ALERT_UNEXPECTED_MESSAGE);
    case ALERT_BAD_RECORD_MAC:
      return ERROR_RESULT(ERR_ALERT_BAD_RECORD_MAC);
    case ALERT_DECRYPTION_FAILED:
      return ERROR_RESULT(ERR_ALERT_DECRYPTION_FAILED);
    case ALERT_HANDSHAKE_FAILURE:
      return ERROR_RESULT(ERR_ALERT_HANDSHAKE_FAILURE);
    case ALERT_NO_CERTIFICATE:
      return ERROR_RESULT(ERR_ALERT_NO_CERTIFICATE);
    case ALERT_BAD_CERTIFICATE:
      return ERROR_RESULT(ERR_ALERT_BAD_CERTIFICATE);
    case ALERT_UNSUPPORTED_CERTIFICATE:
      return ERROR_RESULT(ERR_ALERT_UNSUPPORTED_CERTIFICATE);
    case ALERT_CERTIFICATE_REVOKED:
      return ERROR_RESULT(ERR_ALERT_CERTIFICATE_REVOKED);
    case ALERT_CERTIFICATE_EXPIRED:
      return ERROR_RESULT(ERR_ALERT_CERTIFICATE_EXPIRED);
    case ALERT_CERTIFICATE_UNKNOWN:
      return ERROR_RESULT(ERR_ALERT_CERTIFICATE_UNKNOWN);
    case ALERT_ILLEGAL_PARAMETER:
      return ERROR_RESULT(ERR_ALERT_ILLEGAL_PARAMETER);
    case ALERT_UNKNOWN_CA:
      return ERROR_RESULT(ERR_ALERT_UNKNOWN_CA);
    case ALERT_ACCESS_DENIED:
      return ERROR_RESULT(ERR_ALERT_ACCESS_DENIED);
    case ALERT_DECODE_ERROR:
      return ERROR_RESULT(ERR_ALERT_DECODE_ERROR);
    case ALERT_DECRYPT_ERROR:
      return ERROR_RESULT(ERR_ALERT_DECRYPT_ERROR);
    case ALERT_EXPORT_RESTRICTION:
      return ERROR_RESULT(ERR_ALERT_EXPORT_RESTRICTION);
    case ALERT_PROTOCOL_VERSION:
      return ERROR_RESULT(ERR_ALERT_PROTOCOL_VERSION);
    case ALERT_INSUFFICIENT_SECURITY:
      return ERROR_RESULT(ERR_ALERT_INSUFFICIENT_SECURITY);
    case ALERT_INTERNAL_ERROR:
      return ERROR_RESULT(ERR_ALERT_INTERNAL_ERROR);
    case ALERT_USER_CANCELED:
      return ERROR_RESULT(ERR_ALERT_USER_CANCELED);
    case ALERT_NO_RENEGOTIATION:
      return ERROR_RESULT(ERR_ALERT_NO_RENEGOTIATION);
    case ALERT_UNSUPPORTED_EXTENSION:
      return ERROR_RESULT(ERR_ALERT_UNSUPPORTED_EXTENSION);
    default:
      return ERROR_RESULT(ERR_UNKNOWN_FATAL_ALERT);
  }
}

// FIXME: I just made this up --agl
static const unsigned kMaxHandshakeLength = 65536;

Result GetHandshakeMessage(bool* found, HandshakeMessage* htype, std::vector<struct iovec>* out, Buffer* in) {
  uint8_t header[4];
  *found = false;

  if (!in->Read(header, sizeof(header)))
    return 0;

  if (!IsValidHandshakeType(header[0]))
    return ERROR_RESULT(ERR_UNKNOWN_HANDSHAKE_MESSAGE_TYPE);
  *htype = static_cast<HandshakeMessage>(header[0]);

  const uint32_t length = static_cast<uint32_t>(header[1]) << 16 |
                          static_cast<uint32_t>(header[2]) << 8 |
                          header[3];
  if (length > kMaxHandshakeLength)
    return ERROR_RESULT(ERR_HANDSHAKE_MESSAGE_TOO_LONG);
  if (in->remaining() < length)
    return 0;

  in->PeekV(out, length);
  in->Advance(length);
  *found = true;
  return 0;
}

Result GetRecordOrHandshake(bool* found, RecordType* type, HandshakeMessage* htype, std::vector<struct iovec>* out, Buffer* in, ConnectionPrivate* priv) {
  uint8_t header[5];
  *found = false;
  std::vector<struct iovec> handshake_vectors;

  for (unsigned n = 0; ; n++) {
    const bool first_record = n == 0;
    uint16_t length;

    if (priv->partial_record_remaining && first_record) {
      length = priv->partial_record_remaining;
      // We only ever half-process a record if it's a handshake record.
      *type = RECORD_HANDSHAKE;
    } else {
      if (!in->Read(header, sizeof(header)))
        return 0;
      if (!IsValidRecordType(header[0]))
        return ERROR_RESULT(ERR_INVALID_RECORD_TYPE);
      *type = static_cast<RecordType>(header[0]);

      const uint16_t version = static_cast<uint16_t>(header[1]) << 8 | header[2];
      if (priv->version_established) {
        if (priv->version != static_cast<TLSVersion>(version))
          return ERROR_RESULT(ERR_BAD_RECORD_VERSION);
      } else {
        if (!IsValidVersion(version))
          return ERROR_RESULT(ERR_INVALID_RECORD_VERSION);
        priv->version_established = true;
        priv->version = static_cast<TLSVersion>(version);
      }

      length = static_cast<uint16_t>(header[3]) << 8 | header[4];
      if (in->remaining() < length)
        return 0;

      if (*type != RECORD_HANDSHAKE) {
        if (!first_record)
          return ERROR_RESULT(ERR_TRUNCATED_HANDSHAKE_MESSAGE);
        // Records other than handshake records are processed one at a time and
        // we can store the vectors directly into |out|.
        const size_t orig = out->size();
        in->PeekV(out, length);
        if (priv->read_cipher_spec) {
          unsigned iov_len = out->size() - orig;
          unsigned bytes_stripped;
          if (!priv->read_cipher_spec->Decrypt(&bytes_stripped, &(*out)[orig], &iov_len, header, priv->read_seq_num))
            return ERROR_RESULT(ERR_BAD_MAC);
          out->resize(orig + iov_len);
          priv->read_seq_num++;
        }

        in->Advance(length);
        *found = true;
        return 0;
      }
    }

    // Otherwise we append the vectors of the handshake message into
    // |handshake_vectors|
    const size_t orig = handshake_vectors.size();
    in->PeekV(&handshake_vectors, length);
    // This is the number of bytes of padding and MAC removed from the end.
    unsigned bytes_stripped = 0;
    if (priv->read_cipher_spec) {
      unsigned iov_len = handshake_vectors.size() - orig;
      if (n < priv->pending_records_decrypted) {
        bytes_stripped = priv->read_cipher_spec->StripMACAndPadding(&handshake_vectors[orig], &iov_len);
      } else {
        if (!priv->read_cipher_spec->Decrypt(&bytes_stripped, &handshake_vectors[orig], &iov_len, header, priv->read_seq_num))
            return ERROR_RESULT(ERR_BAD_MAC);
        priv->read_seq_num++;
        priv->pending_records_decrypted++;
      }
      handshake_vectors.resize(orig + iov_len);
    }
    Buffer buf(&handshake_vectors[0], handshake_vectors.size());

    const Result r = GetHandshakeMessage(found, htype, out, &buf);
    if (r)
      return r;
    if (*found == false) {
      // If we didn't find a complete handshake message then we consumed the
      // whole record.
      in->Advance(length);
    } else {
      // If we did find a complete handshake message then it might not have
      // taken up the whole record. In this case, we advance the record, less
      // the amount of data left over in the handshake message buffer.
      priv->partial_record_remaining = buf.remaining() + bytes_stripped;
      in->Advance(length - priv->partial_record_remaining);
      // If we have a partial record remaining, then it has been decrypted.
      // Otherwise, we can consumed all the decrypted records.
      priv->pending_records_decrypted = priv->partial_record_remaining > 0;
      return 0;
    }
  }
}

uint16_t TLSVersionToOffer(ConnectionPrivate* priv) {
  if (priv->sslv3)
    return static_cast<uint16_t>(SSLv3);

  return static_cast<uint16_t>(TLSv12);
}

Result MarshalClientHello(Sink* sink, ConnectionPrivate* priv) {
  const uint64_t now = priv->ctx->EpochSeconds();
  if (!now)
    return ERROR_RESULT(ERR_EPOCH_SECONDS_FAILED);

  priv->client_random[0] = now >> 24;
  priv->client_random[1] = now >> 16;
  priv->client_random[2] = now >> 8;
  priv->client_random[3] = now;
  if (!priv->ctx->RandomBytes(priv->client_random + 4, sizeof(priv->client_random) - 4))
    return ERROR_RESULT(ERR_RANDOM_BYTES_FAILED);

  sink->U16(TLSVersionToOffer(priv));
  sink->Append(priv->client_random, sizeof(priv->client_random));

  sink->U8(0);  // no session resumption for the moment.

  {
    Sink s(sink->VariableLengthBlock(2));

    // For SSLv3 we'll include the SCSV. See RFC 5746.
    if (priv->sslv3)
      sink->U16(kSignalingCipherSuiteValue);

    unsigned written = 0;
    const CipherSuite* suites = AllCipherSuites();
    for (unsigned i = 0; suites[i].flags; i++) {
      if ((suites[i].flags & priv->cipher_suite_flags_enabled) == suites[i].flags) {
        s.U16(suites[i].value);
        written++;
      }
    }

    if (!written)
      return ERROR_RESULT(ERR_NO_POSSIBLE_CIPHERSUITES);
  }

  sink->U8(1);  // number of compression methods
  sink->U8(0);  // no compression.

  if (priv->sslv3) // no extensions in SSLv3
    return 0;

  {
    Sink s(sink->VariableLengthBlock(2));
    const Result r = MarshalClientHelloExtensions(&s, priv);
    if (r)
      return r;
  }

  return 0;
}

Result MarshalClientKeyExchange(Sink* sink, ConnectionPrivate* priv) {
  assert(priv->cipher_suite);
  assert(priv->cipher_suite->flags & CIPHERSUITE_RSA);

  uint8_t premaster_secret[48];
  const uint16_t offered_version = TLSVersionToOffer(priv);
  premaster_secret[0] = offered_version >> 8;
  premaster_secret[1] = offered_version;
  const bool is_sslv3 = priv->version == SSLv3;

  if (!priv->ctx->RandomBytes(&premaster_secret[2], sizeof(premaster_secret) - 2))
    return ERROR_RESULT(ERR_RANDOM_BYTES_FAILED);

  const size_t encrypted_premaster_size = priv->server_cert->SizeEncryptPKCS1();
  if (!encrypted_premaster_size)
    return ERROR_RESULT(ERR_SIZE_ENCRYPT_PKCS1_FAILED);

  // SSLv3 doesn't prefix the encrypted premaster secret with length bytes.
  Sink s(sink->VariableLengthBlock(is_sslv3 ? 0 : 2));
  uint8_t* encrypted_premaster_secret = s.Block(encrypted_premaster_size);
  if (!priv->server_cert->EncryptPKCS1(encrypted_premaster_secret, premaster_secret, sizeof(premaster_secret)))
    return ERROR_RESULT(ERR_ENCRYPT_PKCS1_FAILED);

  KeyBlock kb;
  kb.key_len = priv->cipher_suite->key_len;
  kb.mac_len = priv->cipher_suite->mac_len;
  kb.iv_len = priv->cipher_suite->iv_len;

  if (!KeysFromPreMasterSecret(priv->version, &kb, premaster_secret, sizeof(premaster_secret), priv->client_random, priv->server_random))
    return ERROR_RESULT(ERR_INTERNAL_ERROR);

  memcpy(priv->master_secret, kb.master_secret, sizeof(priv->master_secret));
  if (priv->pending_read_cipher_spec)
    priv->pending_read_cipher_spec->DecRef();
  if (priv->pending_write_cipher_spec)
    priv->pending_write_cipher_spec->DecRef();
  priv->pending_read_cipher_spec = priv->pending_write_cipher_spec = priv->cipher_suite->create(kb);
  priv->pending_write_cipher_spec->AddRef();

  return 0;
}

Result MarshalFinished(Sink* sink, ConnectionPrivate* priv) {
  unsigned verify_data_size;
  const uint8_t* const verify_data = priv->handshake_hash->ClientVerifyData(&verify_data_size, priv->master_secret, sizeof(priv->master_secret));
  uint8_t* b = sink->Block(verify_data_size);
  memcpy(b, verify_data, verify_data_size);

  return 0;
}

static const HandshakeMessage kPermittedHandshakeMessagesPerState[][2] = {
  /* AWAIT_HELLO_REQUEST */ { HELLO_REQUEST, INVALID_MESSAGE },
  /* SEND_PHASE_ONE */ { INVALID_MESSAGE },
  /* RECV_SERVER_HELLO */ { SERVER_HELLO, INVALID_MESSAGE },
  /* RECV_SERVER_CERTIFICATE */ { CERTIFICATE, INVALID_MESSAGE },
  /* RECV_SERVER_HELLO_DONE */ { SERVER_HELLO_DONE, INVALID_MESSAGE },
  /* SEND_PHASE_TWO */ { INVALID_MESSAGE },
  /* RECV_CHANGE_CIPHER_SPEC */ { CHANGE_CIPHER_SPEC, INVALID_MESSAGE },
  /* RECV_FINISHED */ { FINISHED, INVALID_MESSAGE },
};

static void AddHandshakeMessageToVerifyHash(ConnectionPrivate* priv, HandshakeMessage type, Buffer* in) {
  uint8_t header[4];
  header[0] = static_cast<uint8_t>(type);
  header[1] = in->size() >> 16;
  header[2] = in->size() >> 8;
  header[3] = in->size();
  priv->handshake_hash->Update(header, sizeof(header));
  for (unsigned i = 0; i < in->iovec_len(); i++) {
    const struct iovec& iov = in->iovec()[i];
    priv->handshake_hash->Update(iov.iov_base, iov.iov_len);
  }
}

Result ProcessHandshakeMessage(ConnectionPrivate* priv, HandshakeMessage type, Buffer* in) {
  for (size_t i = 0; i < arraysize(kPermittedHandshakeMessagesPerState[0]); i++) {
    const HandshakeMessage permitted = kPermittedHandshakeMessagesPerState[priv->state][i];
    if (permitted == INVALID_MESSAGE)
      return ERROR_RESULT(ERR_UNEXPECTED_HANDSHAKE_MESSAGE);
    if (permitted == type)
      break;
  }

  if (priv->handshake_hash &&
      type != FINISHED && type != SERVER_HELLO && type != CHANGE_CIPHER_SPEC)
    AddHandshakeMessageToVerifyHash(priv, type, in);

  switch (type) {
    case SERVER_HELLO:
      return ProcessServerHello(priv, in);
    case CERTIFICATE:
      return ProcessServerCertificate(priv, in);
    case SERVER_HELLO_DONE:
      return ProcessServerHelloDone(priv, in);
    case CHANGE_CIPHER_SPEC:
      uint8_t b;
      if (!in->Read(&b, 1) || b != 1 || in->remaining() != 0)
        return ERROR_RESULT(ERR_UNEXPECTED_HANDSHAKE_MESSAGE);
      if (priv->read_cipher_spec)
        priv->read_cipher_spec->DecRef();
      priv->read_cipher_spec = priv->pending_read_cipher_spec;
      priv->pending_read_cipher_spec = NULL;
      priv->state = RECV_FINISHED;
      return 0;
    case FINISHED:
      return ProcessServerFinished(priv, in);
    default:
      return ERROR_RESULT(ERR_INTERNAL_ERROR);
  }
}

Result ProcessServerHello(ConnectionPrivate* priv, Buffer* in) {
  bool ok;

  uint16_t server_wire_version;
  if (!in->U16(&server_wire_version))
    return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);
  if (!IsValidVersion(server_wire_version))
    return ERROR_RESULT(ERR_UNSUPPORTED_SERVER_VERSION);
  const TLSVersion version = static_cast<TLSVersion>(server_wire_version);
  if (priv->version_established && priv->version != version)
    return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);

  if (!in->Read(&priv->server_random, sizeof(priv->server_random)))
    return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);

  // No session id support yet.
  Buffer session_id(in->VariableLength(&ok, 1));
  if (!ok)
    return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);

  uint16_t cipher_suite;
  if (!in->U16(&cipher_suite))
    return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);

  const CipherSuite* suites = AllCipherSuites();
  for (size_t i = 0; suites[i].flags; i++) {
    const CipherSuite* suite = &suites[i];
    if (suite->value == cipher_suite) {
      // Check that the ciphersuite was one that we offered.
      if ((suite->flags & priv->cipher_suite_flags_enabled) == suite->flags)
        priv->cipher_suite = suite;
      break;
    }
  }

  if (!priv->cipher_suite)
    return ERROR_RESULT(ERR_UNSUPPORTED_CIPHER_SUITE);

  uint8_t compression_method;
  if (!in->U8(&compression_method))
    return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);

  // We don't support compression yet.
  if (compression_method)
    return ERROR_RESULT(ERR_UNSUPPORTED_COMPRESSION_METHOD);

  priv->handshake_hash = HandshakeHashForVersion(version);
  if (!priv->handshake_hash)
    return ERROR_RESULT(ERR_INTERNAL_ERROR);

  // We didn't know, until now, which TLS version to use. That meant that we
  // didn't know which hash to use for the ClientHello. However, the
  // ClientHello is still hanging around in priv->last_buffer so we can add it,
  // and this message, now.

  if (priv->last_buffer) {
    // The handshake hash doesn't include the record header, so we skip the first
    // five bytes.
    priv->handshake_hash->Update(priv->last_buffer + 5, priv->last_buffer_len - 5);
  }
  AddHandshakeMessageToVerifyHash(priv, SERVER_HELLO, in);

  if (in->remaining() == 0)
    return 0;

  Buffer extensions(in->VariableLength(&ok, 2));
  if (!ok)
    return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);

  Result r = ProcessServerHelloExtensions(&extensions, priv);
  if (r)
    return r;

  if (in->remaining())
    return ERROR_RESULT(ERR_HANDSHAKE_TRAILING_DATA);

  priv->state = RECV_SERVER_CERTIFICATE;

  return 0;
}

Result ProcessServerCertificate(ConnectionPrivate* priv, Buffer* in) {
  bool ok;

  Buffer certificates(in->VariableLength(&ok, 3));
  if (!ok)
    return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);

  while (certificates.remaining()) {
    Buffer certificate(certificates.VariableLength(&ok, 3));
    if (!ok)
      return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);
    const size_t size = certificate.size();
    if (!size)
      return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);
    void* certbytes = priv->arena.Allocate(size);
    if (!certificate.Read(certbytes, size))
      assert(false);
    struct iovec iov = {certbytes, size};
    priv->server_certificates.push_back(iov);
  }

  if (!priv->server_certificates.size())
    return ERROR_RESULT(ERR_INVALID_HANDSHAKE_MESSAGE);

  if (in->remaining())
    return ERROR_RESULT(ERR_HANDSHAKE_TRAILING_DATA);

  priv->server_cert = priv->ctx->ParseCertificate(static_cast<uint8_t*>(priv->server_certificates[0].iov_base), priv->server_certificates[0].iov_len);
  if (!priv->server_cert)
    return ERROR_RESULT(ERR_CANNOT_PARSE_CERTIFICATE);

  priv->state = RECV_SERVER_HELLO_DONE;

  return 0;
}

Result ProcessServerHelloDone(ConnectionPrivate* priv, Buffer* in) {
  if (in->remaining())
    return ERROR_RESULT(ERR_HANDSHAKE_TRAILING_DATA);

  priv->state = SEND_PHASE_TWO;

  return 0;
}

Result ProcessServerFinished(ConnectionPrivate* priv, Buffer* in) {
  unsigned server_verify_len;
  const uint8_t* const server_verify = priv->handshake_hash->ServerVerifyData(&server_verify_len, priv->master_secret, sizeof(priv->master_secret));
  uint8_t verify_data[32];

  if (in->remaining() != server_verify_len)
    return ERROR_RESULT(ERR_BAD_VERIFY);
  if (server_verify_len > sizeof(verify_data))
    return ERROR_RESULT(ERR_INTERNAL_ERROR);
  if (!in->Read(verify_data, server_verify_len))
    return ERROR_RESULT(ERR_INTERNAL_ERROR);

  if (!CompareBytes(server_verify, verify_data, server_verify_len))
    return ERROR_RESULT(ERR_BAD_VERIFY);

  priv->state = AWAIT_HELLO_REQUEST;
  priv->application_data_allowed = true;

  return 0;
}

}  // namespace tlsclient
