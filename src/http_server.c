#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "json.h"

#define MAX_HEADER 16384
#define MAX_BODY (4 * 1024 * 1024)

typedef struct {
  int fd;
  http_handler_t handler;
} conn_t;

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void url_decode(const char *in, char *out, size_t outlen) {
  size_t o = 0;
  for (const char *p = in; *p && o + 1 < outlen; p++) {
    if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
      out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
      p += 2;
    } else if (*p == '+') {
      out[o++] = ' ';
    } else {
      out[o++] = *p;
    }
  }
  out[o] = 0;
}

bool query_param(const char *query, const char *name, char *out, size_t outlen) {
  size_t nl = strlen(name);
  const char *p = query;
  while (p && *p) {
    const char *amp = strchr(p, '&');
    size_t seg = amp ? (size_t)(amp - p) : strlen(p);
    if (seg > nl && !strncmp(p, name, nl) && p[nl] == '=') {
      char raw[2048];
      size_t vl = seg - nl - 1;
      if (vl >= sizeof raw) vl = sizeof raw - 1;
      memcpy(raw, p + nl + 1, vl);
      raw[vl] = 0;
      url_decode(raw, out, outlen);
      return true;
    }
    p = amp ? amp + 1 : NULL;
  }
  return false;
}

static int send_all(int fd, const void *data, size_t len) {
  const char *p = data;
  while (len) {
    ssize_t n = send(fd, p, len, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static const char *reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    default: return "Status";
  }
}

void res_json(http_res_t *res, int status, sbuf_t *json) {
  res->status = status;
  res->content_type = "application/json; charset=utf-8";
  res->body_len = json->len;
  res->owned = sbuf_detach(json);
  res->body = res->owned;
}

void res_error(http_res_t *res, int status, const char *message) {
  sbuf_t b;
  sbuf_init(&b);
  sbuf_puts(&b, "{");
  jw_kv_str(&b, "error", message, false);
  sbuf_puts(&b, "}");
  res_json(res, status, &b);
}

static void respond(int fd, http_res_t *res) {
  sbuf_t h;
  sbuf_init(&h);
  sbuf_printf(&h, "HTTP/1.1 %d %s\r\n", res->status, reason(res->status));
  sbuf_printf(&h, "Content-Type: %s\r\n", res->content_type ? res->content_type : "text/plain");
  sbuf_printf(&h, "Content-Length: %zu\r\n", res->body_len);
  sbuf_puts(&h, "Connection: close\r\n");
  sbuf_puts(&h, "Access-Control-Allow-Origin: *\r\n");
  sbuf_puts(&h, "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n");
  sbuf_puts(&h, "Access-Control-Allow-Headers: Content-Type\r\n");
  if (res->extra_headers) sbuf_puts(&h, res->extra_headers);
  sbuf_puts(&h, "\r\n");
  if (send_all(fd, h.data, h.len) == 0 && res->body_len) send_all(fd, res->body, res->body_len);
  sbuf_free(&h);
}

static void *conn_thread(void *arg) {
  conn_t *c = arg;
  int fd = c->fd;
  struct timeval tv = { 30, 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  sbuf_t in;
  sbuf_init(&in);
  char buf[8192];
  char *header_end = NULL;
  while (!header_end) {
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) goto done;
    sbuf_append(&in, buf, (size_t)n);
    header_end = strstr(in.data, "\r\n\r\n");
    if (!header_end && in.len > MAX_HEADER) goto done;
  }

  http_req_t req;
  memset(&req, 0, sizeof req);
  http_res_t res;
  memset(&res, 0, sizeof res);

  char target[2048] = { 0 };
  if (sscanf(in.data, "%7s %2047s", req.method, target) != 2) goto done;
  char *q = strchr(target, '?');
  if (q) {
    *q = 0;
    snprintf(req.query, sizeof req.query, "%s", q + 1);
  }
  url_decode(target, req.path, sizeof req.path);

  size_t header_len = (size_t)(header_end - in.data) + 4;
  long content_length = 0;
  for (char *line = strstr(in.data, "\r\n"); line && line < header_end; line = strstr(line + 2, "\r\n")) {
    if (!strncasecmp(line + 2, "Content-Length:", 15)) content_length = strtol(line + 17, NULL, 10);
  }
  if (content_length < 0 || content_length > MAX_BODY) {
    res_error(&res, 413, "Request too large");
    respond(fd, &res);
    free(res.owned);
    goto done;
  }
  while (in.len < header_len + (size_t)content_length) {
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) goto done;
    sbuf_append(&in, buf, (size_t)n);
  }
  req.body = in.data + header_len;
  req.body_len = (size_t)content_length;
  req.body[req.body_len] = 0;

  if (!strcmp(req.method, "OPTIONS")) {
    res.status = 204;
  } else {
    c->handler(&req, &res);
  }
  respond(fd, &res);
  free(res.owned);

done:
  sbuf_free(&in);
  close(fd);
  free(c);
  return NULL;
}

typedef struct {
  int fd;
  http_handler_t handler;
  volatile int stopped;
} server_t;

static server_t *running;

static void *accept_thread(void *arg) {
  server_t *s = arg;
  for (;;) {
    int fd = accept(s->fd, NULL, NULL);
    if (s->stopped) {
      if (fd >= 0) close(fd);
      free(s);
      return NULL;
    }
    if (fd < 0) {
      if (errno == EINTR) continue;
      usleep(100000);
      continue;
    }
    conn_t *c = malloc(sizeof *c);
    c->fd = fd;
    c->handler = s->handler;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, conn_thread, c)) {
      close(fd);
      free(c);
    }
    pthread_attr_destroy(&attr);
  }
  return NULL;
}

int http_server_start(int port, http_handler_t handler) {
  signal(SIGPIPE, SIG_IGN);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) || listen(fd, 32)) {
    close(fd);
    return -1;
  }
  server_t *s = calloc(1, sizeof *s);
  s->fd = fd;
  s->handler = handler;
  pthread_t t;
  if (pthread_create(&t, NULL, accept_thread, s)) {
    close(fd);
    free(s);
    return -1;
  }
  pthread_detach(t);
  running = s;
  return 0;
}

void http_server_stop(void) {
  server_t *s = running;
  if (!s) return;
  running = NULL;
  s->stopped = 1;
  shutdown(s->fd, SHUT_RDWR);
  close(s->fd);
}
