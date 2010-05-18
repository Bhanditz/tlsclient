// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>

#include "tlsclient/public/connection.h"
#include "tlsclient/public/context.h"
#include "tlsclient/public/error.h"
#include "tlsclient/tests/openssl-context.h"

#include "tlsclient/src/buffer.h"

#include <openssl/err.h>
#include <openssl/evp.h>

#if 0
static void hexdump(const struct iovec* iovs, unsigned len) {
  for (unsigned i = 0; i < len; i++) {
    size_t iov_len = iovs[i].iov_len;
    size_t done = 0;

    while (iov_len) {
      size_t todo = iov_len;
      if (todo > 16)
        todo = 16;
      const uint8_t* data = static_cast<uint8_t*>(iovs[i].iov_base);
      data += done;
      for (size_t j = 0; j < todo; j++)
        printf("%x%x ", data[j] >> 4, data[j] & 15);
      printf("\n");
      done += todo;
      iov_len -= todo;
    }
  }
}
#endif

static int usage(const char* argv0) {
  fprintf(stderr, "Usage: %s <hostname> [<port number>]\n", argv0);
  return 1;
}

static int fatal_result(tlsclient::Result r) {
  char filename[8];
  tlsclient::FilenameFromResult(filename, r);
  fprintf(stderr, "libtlsclient error: %s:%d %s\n", filename, tlsclient::LineNumberFromResult(r), tlsclient::StringFromResult(r));
  return 1;
}

static int fatal_error(const char* err) {
  fprintf(stderr, "fatal error: %s\n", err);
  return 1;
}

static bool writeU16(int fd, uint16_t v) {
  uint8_t out[2];
  out[0] = v >> 8;
  out[1] = v;
  return write(fd, out, sizeof(out)) == sizeof(out);
}

static bool writeU24(int fd, uint32_t v) {
  uint8_t out[3];
  out[0] = v >> 16;
  out[1] = v >> 8;
  out[2] = v;
  return write(fd, out, sizeof(out)) == sizeof(out);
}

struct Buffer {
  Buffer()
    : offset_(0) {
    }

  bool Write(int fd) {
    ssize_t n;
    do {
      n = writev(fd, &iovs_[0], iovs_.size());
      if (n == -1) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return true;
        perror("writev");
        return false;
      }
    } while (n == -1);

    Consume(n);
    return true;
  }

  void Consume(size_t n) {
    unsigned i = 0;
    unsigned offset = 0;

    while (n) {
      size_t len = iovs_[i].iov_len;
      size_t todo = n;
      if (todo > len)
        todo = len;
      offset += todo;
      n -= todo;
      if (offset == len) {
        offset = 0;
        i++;
      }
    }

    if (n)
      abort();

    for (unsigned j = 0; j < i; j++) {
      if (j == 0) {
        uint8_t* b = static_cast<uint8_t*>(iovs_[0].iov_base) - offset_;
        free(b);
        offset_ = 0;
      } else {
        free(iovs_[j].iov_base);
      }
    }

    size_t remaining = iovs_.size() - i;
    memmove(&iovs_[0], &iovs_[i], sizeof(struct iovec) * remaining);
    iovs_.resize(remaining);
    offset_ += offset;
    if (iovs_.size()) {
      iovs_[0].iov_base = static_cast<uint8_t*>(iovs_[0].iov_base) + offset;
      iovs_[0].iov_len -= offset;
    }
  }

  void Enqueue(const uint8_t* data, size_t length) {
    uint8_t* copy = static_cast<uint8_t*>(malloc(length));
    memcpy(copy, data, length);
    struct iovec iov = {copy, length};
    iovs_.push_back(iov);
  }

  void EnqueueUnused(const struct iovec* iovs, unsigned iov_len, size_t used) {
    unsigned i = 0;
    size_t offset = 0;
    while (used) {
      size_t todo = used;
      if (todo > iovs[i].iov_len)
        todo = iovs[i].iov_len;
      offset += todo;
      used -= todo;
      if (offset == iovs[i].iov_len) {
        i++;
        offset = 0;
      }
    }
    for ( ; i < iov_len; i++, offset = 0) {
      Enqueue(static_cast<uint8_t*>(iovs[i].iov_base) + offset, iovs[i].iov_len - offset);
    }
  }

  unsigned size() const {
    return iovs_.size();
  }

  const struct iovec* iovs() const {
    return &iovs_[0];
  }

 private:
  std::vector<struct iovec> iovs_;
  size_t offset_;
};

int
main(int argc, char **argv) {
  ERR_load_crypto_strings();
  OpenSSL_add_all_algorithms();

  const char *hostname_str = NULL;
  const char *port_str = NULL;
  const char *resume_state = NULL;
  const char *snap_start_state = NULL;
  bool false_start = false;

  for (unsigned i = 1; argv[i]; i++) {
    if (strcmp(argv[i], "--false-start") == 0) {
      false_start = true;
    } else if (strcmp(argv[i], "--resume-state") == 0) {
      i++;
      resume_state = argv[i];
      if (!resume_state)
        return usage(argv[0]);
    } else if (strcmp(argv[i], "--snap-start-state") == 0) {
      i++;
      snap_start_state = argv[i];
      if (!snap_start_state)
        return usage(argv[0]);
    } else if (!hostname_str) {
      hostname_str = argv[i];
    } else if (!port_str) {
      port_str = argv[i];
    } else {
      fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      return usage(argv[0]);
    }
  }

  if (!hostname_str)
    return usage(argv[0]);

  if (!port_str)
    port_str = "443";

  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res;
  fprintf(stderr, " - resolving\n");
  int r = getaddrinfo(hostname_str, port_str, &hints, &res);
  if (r) {
    fprintf(stderr, " - getaddrinfo: %s\n", gai_strerror(r));
    return 1;
  }

  int sock = -1;

  struct addrinfo *cur = res;
  while (cur) {
    sock = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
    if (sock < 0) {
      perror(" - socket");
      cur = cur->ai_next;
      continue;
    }

    const void* addr = NULL;
    if (cur->ai_family == AF_INET) {
      addr = &((struct sockaddr_in*) cur->ai_addr)->sin_addr;
    } else if (cur->ai_family == AF_INET6) {
      addr = &((struct sockaddr_in6*) cur->ai_addr)->sin6_addr;
    }

    if (addr) {
      char buf[INET6_ADDRSTRLEN];
      const char* addr_str = inet_ntop(cur->ai_family, addr, buf, sizeof(buf));
      fprintf(stderr, " - connecting to %s\n", addr_str);
    } else {
      fprintf(stderr, " - connecting\n");
    }

    int r = connect(sock, cur->ai_addr, cur->ai_addrlen);
    if (r) {
      perror(" - connect");
      close(sock);
      sock = -1;
      cur = cur->ai_next;
    }

    cur = NULL;
  }

  freeaddrinfo(res);

  if (sock < 0) {
    fprintf(stderr, "Cannot connect\n");
    return 1;
  }

  fprintf(stderr, " - connected\n");

  OpenSSLContext context;
  tlsclient::Connection conn(&context);
  conn.set_host_name(hostname_str);
  conn.EnableDefault();
  conn.EnableFalseStart(false_start);
  conn.set_host_name(hostname_str);

  if (resume_state) {
    const int fd = open(resume_state, O_RDONLY);
    if (fd >= 0) {
      struct stat st;
      fstat(fd, &st);
      uint8_t* buffer = static_cast<uint8_t*>(malloc(st.st_size));
      read(fd, buffer, st.st_size);
      close(fd);

      tlsclient::Result r = conn.SetResumptionData(buffer, st.st_size);
      free(buffer);
      if (r)
        return fatal_result(r);
      fprintf(stderr, " - set resumption data\n");
    }
  }

  if (snap_start_state) {
    const int fd = open(snap_start_state, O_RDONLY);
    if (fd >= 0) {
      struct stat st;
      fstat(fd, &st);
      uint8_t* buffer = static_cast<uint8_t*>(malloc(st.st_size));
      read(fd, buffer, st.st_size);
      close(fd);

      const struct iovec iov = {buffer, st.st_size};
      tlsclient::Buffer buf(&iov, 1);

      uint16_t num_certs;
      if (!buf.U16(&num_certs))
        return fatal_error("failed to parse snap start data");

      std::vector<struct iovec> certs;

      for (unsigned i = 0; i < num_certs; i++) {
        bool ok;
        tlsclient::Buffer certbuf(buf.VariableLength(&ok, 3));
        if (!ok)
          return fatal_error("failed to parse snap start data");
        size_t cert_len = certbuf.remaining();
        uint8_t* bytes = certbuf.Get(NULL, cert_len);
        struct iovec cert = {bytes, cert_len};
        certs.push_back(cert);
      }

      conn.SetPredictedCertificates(&certs[0], certs.size());

      size_t snap_start_data_len = buf.remaining();
      uint8_t* bytes = buf.Get(NULL, snap_start_data_len);

      tlsclient::Result r = conn.SetSnapStartData(bytes, snap_start_data_len);
      free(buffer);
      if (r)
        return fatal_result(r);
      fprintf(stderr, " - set snap start data\n");
    } else {
      conn.CollectSnapStartData();
    }
  }

  const int efd = epoll_create(2);
  if (efd < 0) {
    perror("epoll_create");
    return 1;
  }

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = 0;
  epoll_ctl(efd, EPOLL_CTL_ADD, 0, &ev);

  ev.events = EPOLLOUT | EPOLLET;
  ev.data.fd = 1;
  epoll_ctl(efd, EPOLL_CTL_ADD, 1, &ev);

  ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
  ev.data.fd = sock;
  epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev);

  bool ready_in = true, ready_out = true, ready_sock_in = true, ready_sock_out = true, unprocessed_ready = true;

  fcntl(0, F_SETFL, O_NONBLOCK);
  fcntl(1, F_SETFL, O_NONBLOCK);
  fcntl(sock, F_SETFL, O_NONBLOCK);

  Buffer q_in_unprocessed;
  Buffer q_in;
  Buffer q_out;

  bool have_printed_cipher_suite = false;
  bool have_printed_did_resume = false;
  bool have_printed_did_snap_start = false;

  for (;;) {
    bool did_something = false;

    //fprintf(stderr, " - ready_out:%d ready_in:%d ready_sock_out:%d ready_sock_in:%d\n",
    //        ready_out, ready_in, ready_sock_out, ready_sock_in);
    //fprintf(stderr, " - q_in:%d q_out:%d q_in_unprocessed:%d wr:%d\n",
    //        q_in.size(), q_out.size(), q_in_unprocessed.size(), conn.need_to_write());

    if (ready_out && q_in.size()) {
      did_something = true;
      if (!q_in.Write(1))
        return 1;
      if (q_in.size())
        ready_out = false;
    }

    if (ready_sock_out && q_out.size()) {
      did_something = true;
      if (!q_out.Write(sock))
        return 1;
      if (q_out.size())
        ready_sock_out = false;
    }

    if (resume_state && conn.is_resumption_data_availible()) {
      struct iovec resume_data;
      tlsclient::Result r = conn.GetResumptionData(&resume_data);
      if (r)
        return fatal_result(r);
      fprintf(stderr, " - writing %u bytes of resume data out\n", (unsigned) resume_data.iov_len);
      const int fd = open(resume_state, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0)
        return fatal_error("failed to open resume state file for output");
      write(fd, resume_data.iov_base, resume_data.iov_len);
      close(fd);

      resume_state = NULL;
    }

    if (snap_start_state && conn.is_snap_start_data_available()) {
      struct iovec snap_start_data;
      tlsclient::Result r = conn.GetSnapStartData(&snap_start_data);
      if (r)
        return fatal_result(r);
      fprintf(stderr, " - writing snap start data out\n");

      const int fd = open(snap_start_state, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0)
        return fatal_error("failed to open snap start state file for output");

      const struct iovec* certs;
      unsigned num_certs;
      r = conn.server_certificates(&certs, &num_certs);
      if (r)
        return fatal_result(r);

      writeU16(fd, num_certs);
      for (unsigned i = 0; i < num_certs; i++) {
        writeU24(fd, certs[i].iov_len);
        write(fd, certs[i].iov_base, certs[i].iov_len);
      }

      write(fd, snap_start_data.iov_base, snap_start_data.iov_len);
      close(fd);
      snap_start_state = NULL;
    }

    if (!have_printed_cipher_suite && conn.is_ready_to_send_application_data()) {
      fprintf(stderr, " - using %s\n", conn.cipher_suite_name());
      have_printed_cipher_suite = true;
    }

    if (!have_printed_did_resume && conn.did_resume()) {
      fprintf(stderr, " - did resume\n");
      have_printed_did_resume = true;
    }

    if (!have_printed_did_snap_start && conn.did_snap_start()) {
      fprintf(stderr, " - did snap start\n");
      have_printed_did_snap_start = true;
    }

    if (!q_out.size() && conn.need_to_write() && ready_sock_out) {
      did_something = true;
      struct iovec iov;
      tlsclient::Result r = conn.Get(&iov);
      if (r)
        return fatal_result(r);
      const ssize_t n = write(sock, iov.iov_base, iov.iov_len);
      if (n == -1) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
          perror("write");
          return 1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          ready_sock_out = false;
      }

      if (static_cast<size_t>(n) != iov.iov_len)
        q_out.Enqueue(static_cast<uint8_t*>(iov.iov_base) + n, iov.iov_len - n);
    }

    if (conn.is_ready_to_send_application_data() && ready_in && ready_sock_out && !q_out.size()) {
      did_something = true;
      uint8_t buf[4096];
      ssize_t n = read(0, buf, sizeof(buf));
      if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          ready_in = false;
          continue;
        }
        if (errno == EINTR)
          continue;
        perror("read");
        return 1;
      } else if (n == 0) {
        return 0;
      }

      struct iovec iovs[3];
      iovs[1].iov_base = buf;
      iovs[1].iov_len = n;

      tlsclient::Result r = conn.Encrypt(&iovs[0], &iovs[2], &iovs[1], 1);
      if (r)
        return fatal_result(r);
      n = writev(sock, iovs, 3);
      if (n == -1) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
          perror("write");
          return 1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          ready_sock_out = false;
          n = 0;
        }
      }

      q_out.EnqueueUnused(iovs, 3, n);
    }

    if (ready_sock_in && ready_out && q_in.size() == 0) {
      did_something = true;
      uint8_t buf[4096];
      ssize_t n = read(sock, buf, sizeof(buf));
      if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          ready_sock_in = false;
          continue;
        }
        if (errno == EINTR)
          continue;
        perror("read");
        return 1;
      } else if (n == 0) {
        return 0;
      }

      q_in_unprocessed.Enqueue(buf, n);
      unprocessed_ready = true;
    }

    if (unprocessed_ready && q_in_unprocessed.size()) {
      did_something = true;
      struct iovec *iov;
      unsigned iov_len;
      size_t used;
      ssize_t n;

      tlsclient::Result r = conn.Process(&iov, &iov_len, &used, q_in_unprocessed.iovs(), q_in_unprocessed.size());
      if (r) {
        if (tlsclient::ErrorCodeFromResult(r) != tlsclient::ERR_ALERT_CLOSE_NOTIFY)
          return fatal_result(r);
      }

      if (iov_len) {
        n = writev(1, iov, iov_len);
        if (n == -1) {
          if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("writev");
            return 1;
          }
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ready_out = false;
            n = 0;
          }
        }

        q_in.EnqueueUnused(iov, iov_len, n);
      }
      q_in_unprocessed.Consume(used);

      if (!used && !conn.need_to_write())
        unprocessed_ready = false;
    }

    if (!did_something) {
      struct epoll_event event;
      if (epoll_wait(efd, &event, 1, -1) != 1) {
        perror("epoll_wait");
        return 1;
      }

      if (event.data.fd == 0) {
        ready_in = true;
      } else if (event.data.fd == 1) {
        ready_out = true;
      } else {
        if (event.events & EPOLLIN)
          ready_sock_in = true;
        if (event.events & EPOLLOUT)
          ready_sock_out = true;
      }
    }
  }
}
