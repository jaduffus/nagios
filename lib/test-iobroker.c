#include <signal.h>
#include <stdio.h>
#include <malloc.h>
#include <netdb.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#include "iobroker.c"

static int fail, pass;
static iobroker_set *iobs;
#define CHECKPOINT(x__) \
	do { \
		fprintf(stderr, "ALIVE @ %s:%s:%d\n", __FILE__, __func__, __LINE__); \
	} while(0)

#define TRY(x__) \
	do { \
		fprintf(stderr, "Trying '%s;'\n", #x__); \
		x__; \
		fprintf(stderr, "Seems to have worked out ok\n"); \
	} while (0)

static char *msg[] = {
	"Welcome to the echo service!\n",
	"test msg nr 2",
	"random piece of string\nwith\nlots\nof\nnewlines\n\n\n\n\0",
	"another totally random message\n",
	"and this one's from emma. She's alright, really",
	NULL,
};

static int echo_service(int fd, int events, void *arg)
{
	char buf[1024];
	int len;

	len = read(fd, buf, sizeof(buf));
	if (len < 0) {
		perror("read");
		iobroker_close(iobs, fd);
		return 0;
	}
	/* zero read means we're disconnected */
	if (!len) {
		iobroker_close(iobs, fd);
		return 0;
	}

	write(fd, buf, len);

	return 0;
}

static int connected_handler(int fd, int events, void *arg)
{
	int *counter = (int *)arg;
	int i;

	i = *counter;

	if (events == IOBROKER_POLLIN) {
		char buf[1024];
		int len = read(fd, buf, sizeof(buf));

		buf[len] = 0;

		if (len != strlen(msg[i]) || memcmp(buf, msg[i], len)) {
			printf("fd: %d, i: %d; len: %d; buf: %s\n", fd, i, len, buf);
			fprintf(stderr, "Upping fail at #1\n");
			fail++;
		} else {
			pass++;
		}
	}

	i++;

	if (i < 0 || i >= ARRAY_SIZE(msg)) {
		fprintf(stderr, "i = %d in connected_handler(). What's up with that?\n", i);
		return 0;
	}

	if (!msg[i]) {
		//printf("OK: %d messages sent on socket %d\n", i, fd);
		iobroker_close(iobs, fd);
		return 0;
	}

	write(fd, msg[i], strlen(msg[i]));
	*counter = i;

	return 0;
}

static int listen_handler(int fd, int events, void *arg)
{
	int sock;
	struct sockaddr_in sain;
	socklen_t addrlen;

	if (!arg || arg != iobs) {
		printf("Argument passing seems to fail spectacularly\n");
	}

	//printf("listen_handler(%d, %d, %p) called\n", fd, events, arg);
	sock = accept(fd, (struct sockaddr *)&sain, &addrlen);
	if (sock < 0) {
		perror("accept");
		return -1;
	}

	write(sock, msg[0], strlen(msg[0]));
	iobroker_register(iobs, sock, iobs, echo_service);
	return 0;
}

int sighandler(int sig)
{
	/* test failed */
	fprintf(stderr, "Caught signal %d (%sSIGALRM). Fail = %d\n",
			sig, sig == SIGALRM ? "" : "not ", fail);
	exit(1);
}


#define NUM_PROCS 500
static int proc_counter[NUM_PROCS];
static int conn_spam(struct sockaddr_in *sain)
{
	int i;

	signal(SIGALRM, (__sighandler_t)sighandler);
	signal(SIGINT, (__sighandler_t)sighandler);
	signal(SIGPIPE, SIG_IGN);
	alarm(20);

	for (i = 0; i < NUM_PROCS; i++) {
		int fd, sockopt = 1;

		fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt));
		proc_counter[i] = 0;
		iobroker_register(iobs, fd, (void *)&proc_counter[i], connected_handler);
		if (connect(fd, (struct sockaddr *)sain, sizeof(*sain))) {
			perror("connect");
		}
		iobroker_poll(iobs, -1);
	}
	printf("%d connections spammed\n", i);
	return 0;
}

int main(int argc, char **argv)
{
	int listen_fd, flags, sockopt = 1;
	struct sockaddr_in sain;

	iobs = iobroker_create();
	listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	flags = fcntl(listen_fd, F_GETFD);
	flags |= FD_CLOEXEC;
	fcntl(listen_fd, F_SETFD, flags);

	(void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt));

	memset(&sain, 0, sizeof(sain));
	sain.sin_port = ntohs(9123);
	sain.sin_family = AF_INET;
	bind(listen_fd, (struct sockaddr *)&sain, sizeof(sain));
	printf("Listening on %s:%d with socket %d\n",
		   inet_ntoa(sain.sin_addr), ntohs(sain.sin_port), listen_fd);
	listen(listen_fd, 128);
	printf("Registering listener socket %d with I/O broker\n", listen_fd);
	iobroker_register(iobs, listen_fd, iobs, listen_handler);

	if (argc == 1)
		conn_spam(&sain);

	for (;;) {
		iobroker_poll(iobs, -1);
		if (iobroker_get_num_fds(iobs) <= 1) {
			break;
		}
	}

	iobroker_close(iobs, listen_fd);
	printf("Destroying iobs\n");
	iobroker_destroy(iobs, 0);

	if (fail) {
		printf("FAIL: %d tests failed\n", fail);
		return 1;
	}

	if (pass) {
		if (pass == (ARRAY_SIZE(msg) - 1) * NUM_PROCS) {
			printf("PASS: All %d tests ran just fine\n", pass);
		} else {
			printf("PASS: %d tests passed, with connection problems\n", pass);
		}
	}
	return 0;
}