#include "uSynergy.h"
#include "wayland.h"
#include "fdio_full.h"
#include "clip.h"
#include "net.h"
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <limits.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <netdb.h>
#include <time.h>

static struct addrinfo *hostinfo;
static int synsock = -1;
extern struct wlContext wlContext;


static bool syn_connect(uSynergyCookie cookie)
{
	struct addrinfo *h;
	uSynergyContext *syn_ctx = cookie;
	logDbg("syn_connect trying to connect");
	synNetDisconnect();
	for (h = hostinfo; h; h = h->ai_next) {
		if ((synsock = socket(h->ai_family, h->ai_socktype, h->ai_protocol)) == -1)
			continue;
		if (connect(synsock, h->ai_addr, h->ai_addrlen))
			continue;
		syn_ctx->m_lastMessageTime = syn_ctx->m_getTimeFunc();
		return true;
	}
	return false;
}

static bool syn_send(uSynergyCookie cookie, const uint8_t *buf, int len)
{
	return write_full(synsock, buf, len);
}

enum net_pollfd_id {
	POLLFD_WL,
	POLLFD_SYN,
	POLLFD_CB,
	POLLFD_P,
	POLLFD_COUNT
};
static bool syn_recv(uSynergyCookie cookie, uint8_t *buf, int max_len, int *out_len)
{
	int ret;
	uint32_t psize;
	uSynergyContext *syn_ctx = cookie;
	int wlfd = wlPrepareFd(&wlContext);
	struct pollfd pollfds[] = {
		/* POLLFD_WL */
		{
			.fd = wlfd,
			.events = POLLIN | POLLHUP,
			.revents = 0
		},
		/* POLLFD_SYN */
		{
			.fd = synsock,
			.events = POLLIN | POLLHUP,
			.revents = 0
		},
		/* POLLFD_CB */
		{
			.fd = clipMonitorFd[0],
			.events = POLLIN | POLLHUP,
			.revents = 0
		},
		/* POLLFD_P */
		{
			.fd = clipMonitorFd[1],
			.events = POLLIN | POLLHUP,
			.revents = 0
		}
	};
	while ((ret = poll(pollfds, POLLFD_COUNT, USYNERGY_IDLE_TIMEOUT)) > 0) {
		if (pollfds[POLLFD_SYN].revents & POLLIN) {
			break;
		}
		wlPollProc(&wlContext, pollfds[POLLFD_WL].revents);
		clipMonitorPollProc(&pollfds[POLLFD_CB]);
		clipMonitorPollProc(&pollfds[POLLFD_P]);
		if ((syn_ctx->m_getTimeFunc() - syn_ctx->m_lastMessageTime) > USYNERGY_IDLE_TIMEOUT) {
			logErr("Synergy imeout encountered, read failed");
			return false;
		}
	}
	if (!ret) {
		logErr("Synergy poll timeout");
		return false;
	}
	alarm(USYNERGY_IDLE_TIMEOUT/1000);
	if (!read_full_i(synsock, &psize, sizeof(psize))) {
		perror("read_full");
		return false;
	}
	alarm(0);
	memmove(buf, &psize, sizeof(psize));
	buf += 4;
	max_len -=4;
	*out_len = 4;
	psize = ntohl(psize);
	alarm(USYNERGY_IDLE_TIMEOUT/1000);
	if (!read_full_i(synsock, buf, psize)) {
		perror("read_full");
		return false;
	}
	alarm(0);
	*out_len += psize;
	return true;
}

static void syn_sleep(uSynergyCookie cookie, int ms)
{
	usleep(ms * 1000);
}

static uint32_t syn_get_time(void)
{
	uint32_t ms;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	ms = ts.tv_sec * 1000;
	ms += ts.tv_nsec / 1000000;
	return ms;
}

bool synNetConfig(uSynergyContext *context, char *host, char *port)
{
	/* trim away any newline garbage */
	for (char *c = host; *c; ++c) {
		if (*c == '\n') {
			*c = '\0';
			break;
		}
	}
	if (getaddrinfo(host, port, NULL, &hostinfo))
		return false;
	context->m_connectFunc = syn_connect;
	context->m_sendFunc = syn_send;
	context->m_receiveFunc = syn_recv;
	context->m_sleepFunc = syn_sleep;
	context->m_getTimeFunc = syn_get_time;
	return true;
}

bool synNetDisconnect(void)
{
	if (synsock == -1)
		return false;
	shutdown(synsock, SHUT_RDWR);
	close(synsock);
	return true;
}

