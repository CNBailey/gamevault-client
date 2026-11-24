#include "http_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int connect_to(const url_t *u, char *err, size_t errlen) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)u->port);
  if (inet_pton(AF_INET, u->host, &addr.sin_addr) != 1) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u->host, NULL, &hints, &res) || !res) {
      snprintf(err, errlen, "Could not find %s on the network (try the NAS's IP address)", u->host);
      return -1;
    }
    addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    snprintf(err, errlen, "socket: %s", strerror(errno));
    return -1;
  }
  struct timeval tv = { 30, 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  /* Large receive buffers keep a fast link full; the PS5 caps them, so step down until one is accepted. */
  for (int rcvbuf = 4 << 20; rcvbuf >= 256 << 10; rcvbuf /= 2) {
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf) == 0) break;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) {
    snprintf(err, errlen, "Could not connect to %s:%d (%s)", u->host, u->port, strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

static int send_all(int fd, const char *p, size_t len) {
  while (len) {
    ssize_t n = send(fd, p, len, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

#define BUFSZ (256 * 1024)

static int http_get_impl(char *buf, const char *url, int64_t from, int64_t to, http_body_cb cb, void *ctx,
                         http_info_t *info, volatile int *stop, char *err, size_t errlen);

int http_get(const char *url, int64_t from, int64_t to, http_body_cb cb, void *ctx,
             http_info_t *info, volatile int *stop, char *err, size_t errlen) {
  char *buf = malloc(BUFSZ);
  if (!buf) { snprintf(err, errlen, "Out of memory"); return -1; }
  int rc = http_get_impl(buf, url, from, to, cb, ctx, info, stop, err, errlen);
  free(buf);
  return rc;
}

static int http_get_impl(char *buf, const char *url, int64_t from, int64_t to, http_body_cb cb, void *ctx,
                         http_info_t *info, volatile int *stop, char *err, size_t errlen) {
  http_info_t dummy;
  if (!info) info = &dummy;
  info->status = 0;
  info->content_length = -1;
  info->total_size = -1;

  url_t u;
  if (!url_parse(url, &u)) {
    snprintf(err, errlen, "Unsupported address: %s", url);
    return -1;
  }
  int fd = connect_to(&u, err, errlen);
  if (fd < 0) return -1;

  sbuf_t req;
  sbuf_init(&req);
  sbuf_printf(&req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: GameVault-PS5\r\nAccept-Encoding: identity\r\nConnection: close\r\n", u.path, u.host, u.port);
  if (from >= 0) {
    if (to >= 0) sbuf_printf(&req, "Range: bytes=%lld-%lld\r\n", (long long)from, (long long)to);
    else sbuf_printf(&req, "Range: bytes=%lld-\r\n", (long long)from);
  }
  sbuf_puts(&req, "\r\n");
  int rc = send_all(fd, req.data, req.len);
  sbuf_free(&req);
  if (rc) {
    snprintf(err, errlen, "Could not send the request");
    close(fd);
    return -1;
  }

  /* Read headers. */
  size_t have = 0;
  char *body = NULL;
  while (!body) {
    if (stop && *stop) { close(fd); snprintf(err, errlen, "Stopped"); return -2; }
    ssize_t n = recv(fd, buf + have, BUFSZ - have - 1, 0);
    if (n <= 0) {
      snprintf(err, errlen, n == 0 ? "The server closed the connection" : "Network error: %s", strerror(errno));
      close(fd);
      return -1;
    }
    have += (size_t)n;
    buf[have] = 0;
    char *end = strstr(buf, "\r\n\r\n");
    if (end) body = end + 4;
    else if (have >= BUFSZ - 1) {
      snprintf(err, errlen, "Response headers too large");
      close(fd);
      return -1;
    }
  }
  sscanf(buf, "HTTP/%*s %d", &info->status);
  bool chunked = false;
  for (char *line = strstr(buf, "\r\n"); line && line + 2 < body; line = strstr(line + 2, "\r\n")) {
    char *h = line + 2;
    if (!strncasecmp(h, "Content-Length:", 15)) info->content_length = strtoll(h + 15, NULL, 10);
    else if (!strncasecmp(h, "Content-Range:", 14)) {
      char *slash = strchr(h, '/');
      if (slash && slash[1] != '*') info->total_size = strtoll(slash + 1, NULL, 10);
    } else if (!strncasecmp(h, "Transfer-Encoding:", 18) && strstr(h, "chunked")) chunked = true;
  }
  if (info->total_size < 0) info->total_size = info->content_length;
  if (info->status < 200 || info->status >= 300) {
    snprintf(err, errlen, "The server answered HTTP %d", info->status);
    close(fd);
    return -1;
  }
  if (from >= 0 && info->status != 206) {
    snprintf(err, errlen, "The server does not support resuming (HTTP %d)", info->status);
    close(fd);
    return -1;
  }

  /* Body. */
  size_t pending = have - (size_t)(body - buf);
  memmove(buf, body, pending);
  int64_t received = 0;
  int result = 0;

  if (chunked) {
    sbuf_t acc;
    sbuf_init(&acc);
    sbuf_append(&acc, buf, pending);
    for (;;) {
      ssize_t n = recv(fd, buf, BUFSZ, 0);
      if (n < 0) { result = -1; snprintf(err, errlen, "Network error"); break; }
      if (n == 0) break;
      sbuf_append(&acc, buf, (size_t)n);
    }
    size_t pos = 0;
    while (result == 0 && pos < acc.len) {
      long sz = strtol(acc.data + pos, NULL, 16);
      char *crlf = strstr(acc.data + pos, "\r\n");
      if (!crlf) break;
      pos = (size_t)(crlf - acc.data) + 2;
      if (sz <= 0) break;
      if (pos + (size_t)sz > acc.len) { result = -1; snprintf(err, errlen, "Truncated response"); break; }
      if (cb && cb(ctx, acc.data + pos, (size_t)sz)) { result = -2; snprintf(err, errlen, "Stopped"); break; }
      pos += (size_t)sz + 2;
    }
    sbuf_free(&acc);
    close(fd);
    return result;
  }

  if (pending) {
    if (cb && cb(ctx, buf, pending)) { close(fd); snprintf(err, errlen, "Stopped"); return -2; }
    received += (int64_t)pending;
  }
  while (info->content_length < 0 || received < info->content_length) {
    if (stop && *stop) { result = -2; snprintf(err, errlen, "Stopped"); break; }
    ssize_t n = recv(fd, buf, BUFSZ, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { result = -1; snprintf(err, errlen, "Network error: %s", strerror(errno)); break; }
    if (n == 0) {
      if (info->content_length >= 0) { result = -1; snprintf(err, errlen, "The connection dropped"); }
      break;
    }
    if (cb && cb(ctx, buf, (size_t)n)) { result = -2; snprintf(err, errlen, "Stopped"); break; }
    received += n;
  }
  close(fd);
  return result;
}

static int to_sbuf(void *ctx, const char *data, size_t len) {
  sbuf_t *b = ctx;
  if (b->len + len > 8 * 1024 * 1024) return 1;
  sbuf_append(b, data, len);
  return 0;
}

int http_get_text(const char *url, sbuf_t *out, int *status, char *err, size_t errlen) {
  http_info_t info;
  int rc = http_get(url, -1, -1, to_sbuf, out, &info, NULL, err, errlen);
  if (status) *status = info.status;
  if (!out->data) sbuf_append(out, "", 0);
  return rc;
}
