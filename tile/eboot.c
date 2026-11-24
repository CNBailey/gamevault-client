/*
 * GameVault home-screen tile.
 *
 * The tile is a tiny app with no libraries: when you press it, it asks the
 * GameVault service (port 8687) to open its interface. If the service isn't
 * running, it sends the copy of gamevault.elf kept next to it to the ELF
 * loader, which starts the service (and the service opens the interface).
 *
 * Based on the install_app sample of the ps5-payload-dev SDK
 * (Copyright (C) 2025 John Törnblom). GPL-3.0-or-later.
 */

#define SYS_exit    1
#define SYS_read    3
#define SYS_write   4
#define SYS_open    5
#define SYS_close   6
#define SYS_socket  97
#define SYS_connect 98
#define SYS_klog    0x259

typedef unsigned long size_t;

long
__syscall(long n, ...) {
  long a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0;
  __builtin_va_list ap;
  unsigned long ret;
  char iserror;

  __builtin_va_start(ap, n);
  a1 = __builtin_va_arg(ap, long);
  a2 = __builtin_va_arg(ap, long);
  a3 = __builtin_va_arg(ap, long);
  a4 = __builtin_va_arg(ap, long);
  a5 = __builtin_va_arg(ap, long);
  a6 = __builtin_va_arg(ap, long);
  __builtin_va_end(ap);

  register long r10 __asm__("r10") = a4;
  register long r8  __asm__("r8")  = a5;
  register long r9  __asm__("r9")  = a6;

  __asm__ __volatile__("syscall"
                       : "=a"(ret), "=@ccc"(iserror), "+r"(r10), "+r"(r8), "+r"(r9)
                       : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                       : "rcx", "r11", "memory");

  return iserror ? -(long)ret : (long)ret;
}

static size_t
slen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

static void
klog(const char *s) {
  __syscall(SYS_klog, 7, s, 0);
}

struct sockaddr_in {
  unsigned char sin_len;
  unsigned char sin_family;
  unsigned short sin_port;
  unsigned int sin_addr;
  char sin_zero[8];
};

/* Connects to 127.0.0.1:port; returns a socket or -1. */
static int
connect_local(unsigned short port) {
  int fd = (int)__syscall(SYS_socket, 2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
  if (fd < 0) return -1;
  struct sockaddr_in a;
  char *p = (char *)&a;
  for (size_t i = 0; i < sizeof a; i++) p[i] = 0;
  a.sin_len = sizeof a;
  a.sin_family = 2;
  a.sin_port = (unsigned short)((port >> 8) | (port << 8));
  a.sin_addr = 0x0100007f; /* 127.0.0.1 in network order */
  if (__syscall(SYS_connect, fd, &a, sizeof a) < 0) {
    __syscall(SYS_close, fd);
    return -1;
  }
  return fd;
}

static int
write_all(int fd, const char *p, size_t n) {
  while (n) {
    long w = __syscall(SYS_write, fd, p, n);
    if (w <= 0) return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

/* Asks the running service to open its interface. */
static int
ask_service(void) {
  int fd = connect_local(8687);
  if (fd < 0) return -1;
  const char *req = "POST /api/open HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
  int rc = write_all(fd, req, slen(req));
  char buf[256];
  if (rc == 0) __syscall(SYS_read, fd, buf, sizeof buf);
  __syscall(SYS_close, fd);
  return rc;
}

/* In .data: the linker script has no .bss. */
static char elf_buf[16 * 1024] __attribute__((section(".data")));

/* Sends gamevault.elf to the ELF loader. */
static int
start_service(void) {
  static const char *paths[] = {
    "/app0/gamevault.elf",
    "/data/homebrew/PPSA99687/gamevault.elf",
    0
  };
  static const unsigned short ports[] = { 9021, 9027, 0 };
  int file = -1;
  for (int i = 0; paths[i] && file < 0; i++) file = (int)__syscall(SYS_open, paths[i], 0 /* O_RDONLY */, 0);
  if (file < 0) {
    klog("GameVault tile: gamevault.elf not found next to the tile\n");
    return -1;
  }
  int sock = -1;
  for (int i = 0; ports[i] && sock < 0; i++) sock = connect_local(ports[i]);
  if (sock < 0) {
    klog("GameVault tile: no ELF loader on port 9021 or 9027\n");
    __syscall(SYS_close, file);
    return -1;
  }
  int rc = 0;
  for (;;) {
    long n = __syscall(SYS_read, file, elf_buf, sizeof elf_buf);
    if (n < 0) { rc = -1; break; }
    if (n == 0) break;
    if (write_all(sock, elf_buf, (size_t)n)) { rc = -1; break; }
  }
  __syscall(SYS_close, file);
  __syscall(SYS_close, sock);
  return rc;
}

int
main(void) {
  klog("GameVault tile started\n");
  if (ask_service() == 0) return 0;
  if (start_service() == 0) {
    klog("GameVault tile: started the service\n");
    return 0;
  }
  return 1;
}

void
_start(void) {
  __syscall(SYS_exit, main());
  __builtin_trap();
}
