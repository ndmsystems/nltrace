#include <stdio.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/prctl.h>
#include <stdbool.h>

#include <unistd.h>
#include <sys/syscall.h>        /* For SYS_xxx definitions */


#include <asm/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "utils.h"
#include "nl_stub.h"

static BITARRAY_TYPE netlink_sockets[65536 / WORDBITS];
static BITARRAY_TYPE known_fds[65536 / WORDBITS];

static void detect_fd_type (pid_t pid, int fd)
{
  char path[256];
  char result[256];
  int tmp;

  fprintf (stderr, "Detecting socket type!\n");
  tmp = snprintf (path, sizeof (path), "/proc/%d/fd/%d", pid, fd);

  if (tmp <= 0)
  {
    perror ("sprintf of path");
    return;
  }

  if (tmp >= (int) (sizeof (path)))
  {
    fprintf (stderr, "printf buffer overflow\n");
    return;
  }

  if (readlink (path, result, sizeof (result)) == -1)
  {
    perror ("readlink");
    return;
  }

  // TODO: %d? %ld?
  int inode = -1;
  if (sscanf (result, "socket:[%d]", &inode) != 1)
    return;

  FILE *netlink;

  if (!(netlink = fopen ("/proc/net/netlink", "rte")))
  {
    perror ("Opening /proc/net/netlink");
    goto end;
  }

  static const char *header = "sk       Eth Pid    Groups   Rmem     Wmem     Dump     Locks     Drops     Inode\n";

  if (!fgets (result, sizeof (result), netlink))
  {
    fprintf (stderr, "Error reading header of /proc/net/netlink\n");
    goto end;
  }

  if (strcmp (result, header))
  {
    fprintf (stderr, "header on your kernel does not match ours we expect!:\n"  /* */
             " our:%s"          /* */
             "your:%s",         /* */
             header, result);
    goto end;
  }

  while (fgets (result, sizeof (result), netlink))
  {

    if (strlen (result) == sizeof (result) - 1)
    {
      fprintf (stderr, "/proc/net/netlink line truncated\n");
      goto end;
    }

    // 0000000000000000 0   4195573 00000000 0        0        0000000000000000 2        0        8636
    if (sscanf (result, "%*s %*s %*s %*s %*s %*s %*s %*s %*s %d", &tmp) != 1)
    {
      fprintf (stderr, "Can not parse string %s\n", result);
      goto end;
    }

    if (inode != tmp)
      continue;

    fprintf (stderr, "Found that fd %d (inode %d) is netlink socket\n", fd, inode);
    bit_set (known_fds, fd);
    bit_set (netlink_sockets, fd);
    handle_netlink_appear (pid, fd);
    goto end;
  }

  fprintf (stderr, "Found that fd %d (inode %d) is not netlink socket\n", fd, inode);
  bit_set (known_fds, fd);
  bit_clear (netlink_sockets, fd);

end:
  if (netlink)
    fclose (netlink);
}


//TODO: exactly the same, but on UNIX sockets...
void handle_socket (pid_t pid, int ret, int domain, int type, int protocol)
{
  if (ret < 0)
    return;

  (void) type;
  (void) protocol;

  fprintf (stderr, "Setting %d as known (some socket)\n", ret);
  bit_set (known_fds, ret);

  if (domain != AF_NETLINK)
  {
    bit_clear (netlink_sockets, ret);
    return;
  }

  bit_set (netlink_sockets, ret);
  handle_netlink_appear (pid, ret);
}

void handle_sendto (pid_t pid, ssize_t ret, int sockfd, const char *buf, size_t buflen, const struct sockaddr *dest_addr, socklen_t addrlen)
{

  /* TODO: HAY! we should check for remote errno! suppose sending of valid message with wrong sequence */
  if (ret < 0)
    return;

  (void) buf;
  (void) buflen;
  (void) dest_addr;
  (void) addrlen;


  if (!bit_get (known_fds, sockfd))
    detect_fd_type (pid, sockfd);

  if (!bit_get (netlink_sockets, sockfd))
    return;


  //TODO: send not all, zero and so on

  char *data = NULL;

  if (!(data = malloc (ret)))
    goto end;

  handle_netlink_send (pid, sockfd, data, strlen (data));

end:
  free (data);
}


void handle_recvfrom (pid_t pid, ssize_t ret, int sockfd, const char *buf, size_t buflen, int flags, const struct sockaddr *src_addr, const socklen_t *addrlen)
{
  if (ret < 0)
    return;


  (void) buf;
  (void) buflen;
  (void) flags;
  (void) src_addr;
  (void) addrlen;

  if (!bit_get (known_fds, sockfd))
    detect_fd_type (pid, sockfd);

  if (!bit_get (netlink_sockets, sockfd))
    return;

  if (ret == 0)
  {
    fprintf (stderr, "netlink recv returns zero...\n");
    return;
  }

  char *data = NULL;

  if (!(data = malloc (ret)))
    goto end;

  handle_netlink_recv (pid, sockfd, data, strlen (data));
end:
  free (data);
}

void handle_recvmsg (pid_t pid, ssize_t ret, int sockfd, struct msghdr *msg, int flags)
{
  if (ret < 0)
    return;

  (void) msg;
  (void) flags;

  if (!bit_get (known_fds, sockfd))
    detect_fd_type (pid, sockfd);

  if (!bit_get (netlink_sockets, sockfd))
    return;

  fprintf (stderr, "recvmsg(%d) netlink returns %zu\n", sockfd, ret);
}

void handle_sendmsg (pid_t pid, ssize_t ret, int sockfd, const struct msghdr *msg, int flags)
{
  if (ret < 0)
    return;

  (void) msg;
  (void) flags;

  if (!bit_get (known_fds, sockfd))
    detect_fd_type (pid, sockfd);

  if (!bit_get (netlink_sockets, sockfd))
    return;

  fprintf (stderr, "netlink sendmsg stillnot implemented\n");

}


void handle_close (pid_t pid, int ret, int fd)
{
  if (ret < 0)
    return;

  if (!bit_get (known_fds, fd))
  {
    fprintf (stderr, "Setting %d as known (closed)\n", fd);
    bit_set (known_fds, fd);
  }

  if (!bit_get (netlink_sockets, fd))
    return;

  bit_clear (netlink_sockets, fd);
  handle_netlink_close (pid, fd);
}

void handle_dup2 (pid_t pid, int ret, int oldfd, int newfd)
{
  if (ret < 0)
    return;

  if (ret != newfd)
  {
    fprintf (stderr, "Internal error! dup2 returns not that descriptor (!)\n");
  }

  if (!bit_get (known_fds, oldfd))
    detect_fd_type (pid, oldfd);

  bool new_was_known = bit_get (known_fds, newfd);

  if (new_was_known && bit_get (netlink_sockets, newfd))
  {
    fprintf (stderr, "Warning, overriding netlink socket with fd %d with another one\n", newfd);
    bit_clear (netlink_sockets, newfd);
    handle_netlink_close (pid, newfd);
  }

  if (!new_was_known)
  {
    fprintf (stderr, "Setting %d as known (result of DUP)\n", newfd);
    bit_set (known_fds, newfd);
  }

  if (bit_get (netlink_sockets, oldfd))
  {
    bit_set (netlink_sockets, newfd);
    handle_netlink_appear (pid, newfd);
  }
  // if was unknown, netlink bit already cleaned.
  // if was known: if was netlink socket     - was cleaned earlier
  //               if was not netlink socket - cleaning not required


}

void handle_dup (pid_t pid, int ret, int oldfd)
{
  handle_dup2 (pid, ret, oldfd, ret);
}

void handle_dup3 (pid_t pid, int ret, int oldfd, int newfd, int flags)
{
  (void) flags;
  handle_dup2 (pid, ret, oldfd, newfd);
}