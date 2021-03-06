#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <stdlib.h>
#include "net.h"
#include "random.h"

void netlink_gen_sockaddr(unsigned long *addr, unsigned long *addrlen)
{
	struct sockaddr_nl *nl;

	nl = malloc(sizeof(struct sockaddr_nl));
	if (nl == NULL)
		return;

	nl->nl_family = PF_NETLINK;
	nl->nl_pid = rand();
	nl->nl_groups = rand();
	*addr = (unsigned long) nl;
	*addrlen = sizeof(struct sockaddr_nl);
}

void netlink_rand_socket(struct socket_triplet *st)
{
	if (rand_bool())
		st->type = SOCK_RAW;
	else
		st->type = SOCK_DGRAM;

	st->protocol = rand() % (NETLINK_CRYPTO + 1);       // Current highest netlink socket.
}
