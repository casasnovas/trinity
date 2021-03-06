#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "trinity.h"
#include "constants.h"
#include "shm.h"
#include "net.h"
#include "log.h"
#include "params.h"	// victim_path, verbose, do_specific_proto
#include "random.h"

unsigned int nr_sockets = 0;

static const char *cachefilename="trinity.socketcache";

#define MAX_PER_DOMAIN 5
#define MAX_TRIES_PER_DOMAIN 10

static int open_socket(unsigned int domain, unsigned int type, unsigned int protocol)
{
	int fd;
	struct sockaddr sa;
	socklen_t salen;

	fd = socket(domain, type, protocol);
	if (fd == -1)
		return fd;

	shm->socket_fds[nr_sockets] = fd;

	output(2, "fd[%i] = domain:%i (%s) type:0x%x protocol:%i\n",
		fd, domain, get_proto_name(domain), type, protocol);

	nr_sockets++;

	/* Sometimes, listen on created sockets. */
	if (rand_bool()) {
		__unused__ int ret;

		/* fake a sockaddr. */
		generate_sockaddr((unsigned long *) &sa, (unsigned long *) &salen, domain);

		ret = bind(fd, &sa, salen);
/*		if (ret == -1)
			printf("bind: %s\n", strerror(errno));
		else
			printf("bind: success!\n");
*/
		ret = listen(fd, (rand() % 2) + 1);
/*		if (ret == -1)
			printf("listen: %s\n", strerror(errno));
		else
			printf("listen: success!\n");
*/
	}

	return fd;
}

static void lock_cachefile(int cachefile, int type)
{
	struct flock fl = {
		.l_len = 0,
		.l_start = 0,
		.l_whence = SEEK_SET,
	};

	fl.l_pid = getpid();
	fl.l_type = type;

	if (verbose)
		output(2, "waiting on lock for cachefile\n");

	if (fcntl(cachefile, F_SETLKW, &fl) == -1) {
		perror("fcntl F_SETLKW");
		exit(1);
	}

	if (verbose)
		output(2, "took lock for cachefile\n");
}

static void unlock_cachefile(int cachefile)
{
	struct flock fl = {
		.l_len = 0,
		.l_start = 0,
		.l_whence = SEEK_SET,
	};

	fl.l_pid = getpid();
	fl.l_type = F_UNLCK;

	if (fcntl(cachefile, F_SETLK, &fl) == -1) {
		perror("fcntl F_UNLCK F_SETLK ");
		exit(1);
	}

	if (verbose)
		output(2, "dropped lock for cachefile\n");
}

static void generate_sockets(void)
{
	int fd, n;
	int cachefile;
	unsigned int nr_to_create = NR_SOCKET_FDS;
	unsigned int buffer[3];

	cachefile = creat(cachefilename, S_IWUSR|S_IRUSR);
	if (cachefile < 0) {
		printf("Couldn't open cachefile for writing! (%s)\n",
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	lock_cachefile(cachefile, F_WRLCK);

	while (nr_to_create > 0) {

		struct socket_triplet st;

		if (shm->exit_reason != STILL_RUNNING) {
			close(cachefile);
			return;
		}

		for (st.family = 0; st.family < TRINITY_PF_MAX; st.family++) {

			if (do_specific_proto == TRUE)
				st.family = specific_proto;

			if (get_proto_name(st.family) == NULL)
				goto skip;

			if (sanitise_socket_triplet(&st) == -1)
				rand_proto_type(&st);

			fd = open_socket(st.family, st.type, st.protocol);
			if (fd > -1) {
				nr_to_create--;

				buffer[0] = st.family;
				buffer[1] = st.type;
				buffer[2] = st.protocol;
				n = write(cachefile, &buffer, sizeof(int) * 3);
				if (n == -1) {
					printf("something went wrong writing the cachefile!\n");
					exit(EXIT_FAILURE);
				}

				if (nr_to_create == 0)
					goto done;
			} else {
				//printf("Couldn't open family:%d (%s)\n", st.family, get_proto_name(st.family));
			}
skip:

			/* check for ctrl-c */
			if (shm->exit_reason != STILL_RUNNING)
				return;

			//FIXME: If we've passed -P and we're spinning here without making progress
			// then we should abort after a few hundred loops.
		}
	}

done:
	unlock_cachefile(cachefile);

	output(1, "created %d sockets\n", nr_sockets);

	close(cachefile);
}


static void close_sockets(void)
{
	unsigned int i;
	int fd;

	for (i = 0; i < nr_sockets; i++) {
		fd = shm->socket_fds[i];
		shm->socket_fds[i] = 0;
		if (close(fd) != 0) {
			printf("failed to close socket.(%s)\n", strerror(errno));
		}
	}

	nr_sockets = 0;
}

void open_sockets(void)
{
	int cachefile;
	unsigned int domain, type, protocol;
	unsigned int buffer[3];
	int bytesread=-1;
	int fd;

	/* If we have victim files, don't worry about sockets. */
	if (victim_path != NULL)
		return;

	cachefile = open(cachefilename, O_RDONLY);
	if (cachefile < 0) {
		printf("Couldn't find socket cachefile. Regenerating.\n");
		generate_sockets();
		return;
	}

	lock_cachefile(cachefile, F_RDLCK);

	while (bytesread != 0) {
		bytesread = read(cachefile, buffer, sizeof(int) * 3);
		if (bytesread == 0)
			break;

		domain = buffer[0];
		type = buffer[1];
		protocol = buffer[2];

		if (do_specific_proto == TRUE) {
			if (domain != specific_proto) {
				printf("ignoring socket cachefile due to specific protocol request, and stale data in cachefile.\n");
regenerate:
				unlock_cachefile(cachefile);	/* drop the reader lock. */
				close(cachefile);
				unlink(cachefilename);
				generate_sockets();
				return;
			}
		}

		fd = open_socket(domain, type, protocol);
		if (fd < 0) {
			printf("Cachefile is stale. Need to regenerate.\n");
			close_sockets();
			goto regenerate;
		}

		/* check for ctrl-c */
		if (shm->exit_reason != STILL_RUNNING) {
			close(cachefile);
			return;
		}
	}

	if (nr_sockets < NR_SOCKET_FDS) {
		printf("Insufficient sockets in cachefile (%d). Regenerating.\n", nr_sockets);
		goto regenerate;
	}

	output(1, "%d sockets created based on info from socket cachefile.\n", nr_sockets);

	unlock_cachefile(cachefile);
	close(cachefile);
}
