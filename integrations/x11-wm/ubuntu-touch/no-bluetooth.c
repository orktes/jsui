// LD_PRELOAD for apps started from jsui-wl on the Galaxy Tab A7: this
// kernel hits a BUG() in bt_sock_create on any AF_BLUETOOTH socket and
// panics, and System Settings (among others) opens one at start. Refuse
// them as a kernel without Bluetooth would.
#define _GNU_SOURCE
#include <errno.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

int socket(int domain, int type, int protocol) {
  if (domain == AF_BLUETOOTH) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  return (int)syscall(SYS_socket, domain, type, protocol);
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
  if (domain == AF_BLUETOOTH) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  return (int)syscall(SYS_socketpair, domain, type, protocol, sv);
}
