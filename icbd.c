/*
 * Copyright (c) 2009 Mike Belopuhov
 * Copyright (c) 2007 Oleg Safiullin
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sysexits.h>
#include <syslog.h>
#include <pwd.h>
#include <locale.h>
#include <ctype.h>
#include <netdb.h>
#include <event.h>
#include <errno.h>
#include <err.h>
#include <resolv.h>

#include "icb.h"
#include "icbd.h"

struct stat modtabst;
char modtabpath[PATH_MAX];
char modtab[ICB_MTABLEN][ICB_MAXNICKLEN];
int  modtabcnt;
char srvname[NI_MAXHOST];
int  creategroups;
int  foreground;
char logprefix[PATH_MAX/2];
int  dodns = 1;
int  dologging;
int  verbose;

void usage(void);
void getpeerinfo(struct icb_session *);
void icbd_accept(int, short, void *);
void icbd_paused(int, short, void *);
void icbd_drop(struct icb_session *, char *);
void icbd_ioerr(struct bufferevent *, short, void *);
void icbd_dispatch(struct bufferevent *, void *);
void icbd_log(struct icb_session *, int, const char *, ...);
void icbd_restrict(void);
void icbd_send(struct icb_session *, char *, ssize_t);

struct icbd_listener {
	struct event ev, pause;
};

int
main(int argc, char *argv[])
{
	const char *cause = NULL;
	int ch, nsocks = 0, save_errno = 0;
	int inet4 = 0, inet6 = 0;
	char group[ICB_MAXGRPLEN], *grplist = NULL;
	char *ptr = NULL;

	/* init group lists before calling icb_addgroup */
	icb_init();

	while ((ch = getopt(argc, argv, "46CdG:M:nL:S:v")) != -1)
		switch (ch) {
		case '4':
			inet4++;
			break;
		case '6':
			inet6++;
			break;
		case 'C':
			creategroups++;
			break;
		case 'd':
			foreground++;
			break;
		case 'G':
			grplist = optarg;
			break;
		case 'L':
			strlcpy(logprefix, optarg, sizeof logprefix);
			dologging++;
			break;
		case 'M':
			strlcpy(modtabpath, optarg, sizeof modtabpath);
			break;
		case 'n':
			dodns = 0;
			break;
		case 'S':
			strlcpy(srvname, optarg, sizeof srvname);
			break;
		case 'v':
			verbose++;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	argc -= optind;
	argv += optind;

	/* add group "1" as it's a login group for most of the clients */
	if (icb_addgroup(NULL, "1") == NULL)
		err(EX_UNAVAILABLE, NULL);

	if (grplist) {
		while (icb_token(grplist, strlen(grplist), &ptr, group,
		    ICB_MAXGRPLEN, ',', 0) > 0)
			if (icb_addgroup(NULL, group) == NULL)
				err(EX_UNAVAILABLE, NULL);
	}

	if (argc == 0)
		argc++;

	if (inet4 && inet6)
		errx(EX_USAGE, "Can't specify both -4 and -6");

	tzset();
	(void)setlocale(LC_ALL, "C");

	if (foreground)
		openlog("icbd", LOG_PID | LOG_PERROR, LOG_DAEMON);
	else
		openlog("icbd", LOG_PID | LOG_NDELAY, LOG_DAEMON);

	if (!foreground && daemon(0, 0) < 0)
		err(EX_OSERR, NULL);

	(void)event_init();

	for (ch = 0; ch < argc; ch++) {
		struct addrinfo hints, *res, *res0;
		struct icbd_listener *l;
		char *addr, *port;
		int error, s, on = 1;

		addr = port = NULL;
		if (argv[ch] != NULL) {
			if (argv[ch][0] != ':')
				addr = argv[ch];
			if ((port = strrchr(argv[ch], ':')) != NULL)
				*port++ = '\0';
		}

		bzero(&hints, sizeof hints);
		if (inet4 || inet6)
			hints.ai_family = inet4 ? PF_INET : PF_INET6;
		else
			hints.ai_family = PF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		if ((error = getaddrinfo(addr, port ? port : "7326", &hints,
		    &res0)) != 0) {
			syslog(LOG_ERR, "%s", gai_strerror(error));
			return (EX_UNAVAILABLE);
		}

		for (res = res0; res != NULL; res = res->ai_next) {
			if ((s = socket(res->ai_family, res->ai_socktype,
			    res->ai_protocol)) < 0) {
				cause = "socket";
				save_errno = errno;
				continue;
			}

			if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on,
			    sizeof on) < 0) {
				cause = "SO_REUSEADDR";
				save_errno = errno;
				(void)close(s);
				continue;
			}

			if (bind(s, res->ai_addr, res->ai_addrlen) < 0) {
				cause = "bind";
				save_errno = errno;
				(void)close(s);
				continue;
			}

			(void)listen(s, TCP_BACKLOG);

			if ((l = calloc(1, sizeof *l)) == NULL)
				err(EX_UNAVAILABLE, NULL);
			event_set(&l->ev, s, EV_READ | EV_PERSIST,
			    icbd_accept, l);
			if (event_add(&l->ev, NULL) < 0) {
				syslog(LOG_ERR, "event_add: %m");
				return (EX_UNAVAILABLE);
			}
			evtimer_set(&l->pause, icbd_paused, l);

			nsocks++;
		}

		freeaddrinfo(res0);
	}

	if (nsocks == 0) {
		errno = save_errno;
		syslog(LOG_ERR, "%s: %m", cause);
		return (EX_UNAVAILABLE);
	}

	/* start the logger service */
	logger_init();

	/* initialize resolver */
	res_init();

	icbd_restrict();

	icbd_modupdate();

	(void)signal(SIGPIPE, SIG_IGN);

	(void)event_dispatch();

	syslog(LOG_ERR, "event_dispatch: %m");

	return (EX_UNAVAILABLE);
}

void
icbd_accept(int fd, short event __attribute__((__unused__)),
    void *arg)
{
	struct icbd_listener *l = arg;
	struct sockaddr_storage ss;
	struct timeval p = { 1, 0 };
	struct icb_session *is;
	socklen_t ss_len = sizeof ss;
	int s, on = 1, tos = IPTOS_LOWDELAY;

	ss.ss_len = ss_len;
	s = accept(fd, (struct sockaddr *)&ss, &ss_len);
	if (s == -1) {
		switch (errno) {
		case EINTR:
		case EWOULDBLOCK:
		case ECONNABORTED:
			return;
		case EMFILE:
		case ENFILE:
			event_del(&l->ev);
			evtimer_add(&l->pause, &p);
			return;
		default:
			syslog(LOG_ERR, "accept: %m");
			return;
		}
	}

	if (ss.ss_family == AF_INET)
		if (setsockopt(s, IPPROTO_IP, IP_TOS, &tos, sizeof tos) < 0)
			syslog(LOG_WARNING, "IP_TOS: %m");
	if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on) < 0)
		syslog(LOG_WARNING, "SO_KEEPALIVE: %m");
	if ((is = calloc(1, sizeof *is)) == NULL) {
		syslog(LOG_ERR, "calloc: %m");
		(void)close(s);
		return;
	}
	if ((is->bev = bufferevent_new(s, icbd_dispatch, NULL, icbd_ioerr,
	    is)) == NULL) {
		syslog(LOG_ERR, "bufferevent_new: %m");
		(void)close(s);
		free(is);
		return;
	}
	if (bufferevent_enable(is->bev, EV_READ)) {
		syslog(LOG_ERR, "bufferevent_enable: %m");
		(void)close(s);
		bufferevent_free(is->bev);
		free(is);
		return;
	}

	/* save host information */
	getpeerinfo(is);

	/* start icb conversation */
	icb_start(is);
}

void
icbd_paused(int fd __attribute__((__unused__)),
	short events __attribute__((__unused__)), void *arg)
{
	struct icbd_listener *l = arg;
	event_add(&l->ev, NULL);
}

__dead void
usage(void)
{
	extern char *__progname;

	(void)fprintf(stderr, "usage: %s [-46Cdv] [-G group1[,group2,...]] "
	   "[-L prefix] [-M modtab]\n\t[-S name] [[addr][:port] ...]\n",
	    __progname);
	exit(EX_USAGE);
}

/*
 *  bufferevent functions
 */
void
icbd_ioerr(struct bufferevent *bev __attribute__((__unused__)), short what,
    void *arg)
{
	struct icb_session *is = (struct icb_session *)arg;

	if (what & EVBUFFER_TIMEOUT)
		icbd_drop(is, "timeout");
	else if (what & EVBUFFER_EOF)
		icbd_drop(is, NULL);
	else if (what & EVBUFFER_ERROR)
		icbd_drop(is, (what & EVBUFFER_READ) ? "read error" :
		    "write error");
}

void
icbd_dispatch(struct bufferevent *bev, void *arg)
{
	struct icb_session *is = (struct icb_session *)arg;
	unsigned char length;
	size_t res;

	while (EVBUFFER_LENGTH(EVBUFFER_INPUT(bev)) > 0) {
		if (is->length == 0) {
			/* read length */
			bufferevent_read(bev, &length, 1);
			if (length == 0) {
				/*
				 * An extension has been proposed:
				 * if length is 0, the packet is part of an
				 * "extended packet". The packet should be
				 * treated as if length was 255 and the next
				 * packet received from the sender should be
				 * appended to this packet.
				 *
				 * This server doesn't support this yet.
				 */
				icbd_drop(is, "invalid packet");
				return;
			}
			is->length = (size_t)length;
			is->rlen = 0;
		}
		/* read as much as we can */
		res = bufferevent_read(bev, &is->buffer[is->rlen],
		    is->length - is->rlen);
		is->rlen += res;
#ifdef DEBUG
		{
			int i;

			printf("-> read %lu out of %lu from %s:%d:\n",
			    is->rlen, is->length, is->host, is->port);
			for (i = 0; i < (int)is->rlen; i++)
				printf(isprint(is->buffer[i]) ? "%c" :
				    "\\x%02x", (unsigned char)is->buffer[i]);
			printf("\n");
		}
#endif
		/* see you next time around */
		if (is->rlen < is->length)
			return;
		/* nul-terminate the data */
		is->buffer[MIN(is->rlen, ICB_MSGSIZE - 1)] = '\0';
		/* process the message in full */
		if (icb_input(is))
			return;
		/* cleanup the input buffer */
		memset(is->buffer, 0, ICB_MSGSIZE);
		is->rlen = is->length = 0;
	}
}

void
icbd_send(struct icb_session *is, char *buf, ssize_t size)
{
	if (bufferevent_write(is->bev, buf, size) == -1)
		syslog(LOG_ERR, "bufferevent_write: %m");
#ifdef DEBUG
	{
		int i;

		printf("-> wrote %lu to %s:%d:\n", size, is->host, is->port);
		for (i = 0; i < size; i++)
			printf(isprint(buf[i]) ? "%c" : "\\x%02x",
			    (unsigned char)buf[i]);
		printf("\n");
	}
#endif
}

void
icbd_drop(struct icb_session *is, char *reason)
{
	if (reason) {
		icb_remove(is, reason);
		icbd_log(is, LOG_DEBUG, reason);
	} else
		icb_remove(is, NULL);

	/* cleanup the input buffer */
	memset(is->buffer, 0, ICB_MSGSIZE);
	is->rlen = is->length = 0;

	(void)close(EVBUFFER_FD(is->bev));
	bufferevent_free(is->bev);
	if (!ISSETF(is->flags, ICB_SF_DNSINPROGRESS))
		free(is);
	else
		SETF(is->flags, ICB_SF_PENDINGDROP);
}

void
icbd_log(struct icb_session *is, int level, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	if (!verbose && level == LOG_DEBUG)
		return;

	va_start(ap, fmt);
	(void)vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (is)
		syslog(level, "%s:%u: %s", is->host, is->port, buf);
	else
		syslog(level, "%s", buf);
}

void
icbd_restrict(void)
{
	struct stat	sb;
	struct passwd	*pw;

	if ((pw = getpwnam(ICBD_USER)) == NULL) {
		syslog(LOG_ERR, "No passwd entry for %s", ICBD_USER);
		exit(EX_NOUSER);
	}

	if (stat(pw->pw_dir, &sb) == -1) {
		syslog(LOG_ERR, "%s: %m", pw->pw_name);
		exit(EX_NOPERM);
	}

	if (sb.st_uid != 0 || (sb.st_mode & (S_IWGRP|S_IWOTH)) != 0) {
		syslog(LOG_ERR, "bad directory permissions");
		exit(EX_NOPERM);
	}

	if (chroot(pw->pw_dir) < 0) {
		syslog(LOG_ERR, "%s: %m", pw->pw_dir);
		exit(EX_UNAVAILABLE);
	}

	if (chdir("/") < 0) {
		syslog(LOG_ERR, "/" ICBD_HOME ": %m");
		exit(EX_UNAVAILABLE);
	}

	chdir(ICBD_HOME);

	if (setuid(pw->pw_uid) < 0) {
		syslog(LOG_ERR, "%d: %m", pw->pw_uid);
		exit(EX_NOPERM);
	}

#ifdef __OpenBSD__
	if (dodns) {
		if (pledge("stdio inet rpath dns", NULL) == -1) {
			syslog(LOG_ERR, "pledge");
			exit(EX_NOPERM);
		}
	} else {
		if (pledge("stdio inet rpath", NULL) == -1) {
			syslog(LOG_ERR, "pledge");
			exit(EX_NOPERM);
		}
	}
#endif

	(void)setproctitle("icbd");
}

void
icbd_modupdate(void)
{
	struct stat st;
	FILE *fp;
	char *buf, *lbuf;
	size_t len;

	if (strlen(modtabpath) == 0)
		return;
	if (stat(modtabpath, &st)) {
		syslog(LOG_ERR, "stat %s: %m", modtabpath);
		return;
	}
	/* see if there are any changes */
	if (timespeccmp(&st.st_mtim, &modtabst.st_mtim, ==) ||
	    st.st_size == 0)
		return;

	if ((fp = fopen(modtabpath, "r")) == NULL) {
		syslog(LOG_ERR, "open %s: %m", modtabpath);
		return;
	}

	modtabcnt = 0;
	bzero(modtab, ICB_MTABLEN * ICB_MAXNICKLEN);
	lbuf = NULL;
	while ((buf = fgetln(fp, &len)) && modtabcnt < ICB_MTABLEN) {
		if (buf[len - 1] == '\n')
			buf[len - 1] = '\0';
		else {
			/* EOF without EOL, copy and add the NUL */
			if ((lbuf = malloc(len + 1)) == NULL)
				err(1, NULL);
			memcpy(lbuf, buf, len);
			lbuf[len] = '\0';
			buf = lbuf;
		}
		while (buf[0] == ' ' || buf[0] == '\t')
			buf++;
		if (buf[0] == '#' || buf[0] == '\0')
			continue;
		strlcpy(modtab[modtabcnt++], buf, ICB_MAXNICKLEN);
	}
	free(lbuf);

	qsort(modtab, modtabcnt, ICB_MAXNICKLEN,
	    (int (*)(const void *, const void *))strcmp);

	fclose(fp);

	memcpy(&modtabst, &st, sizeof modtabst);
}

time_t
getmonotime(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		syslog(LOG_ERR, "%m");
		exit(EX_OSERR);
	}
	return (ts.tv_sec);
}

void
getpeerinfo(struct icb_session *is)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)&is->ss;
	struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&is->ss;
	socklen_t ss_len = sizeof is->ss;

	bzero(&is->ss, sizeof is->ss);
	if (getpeername(EVBUFFER_FD(is->bev), (struct sockaddr *)&is->ss,
	    &ss_len) != 0)
		return;

	is->port = 0;
	switch (is->ss.ss_family) {
	case AF_INET:
		is->port = ntohs(sin->sin_port);
		break;

	case AF_INET6:
		is->port = ntohs(sin6->sin6_port);
		break;
	}

	inet_ntop(is->ss.ss_family, is->ss.ss_family == AF_INET ?
	    (void *)&sin->sin_addr : (void *)&sin6->sin6_addr,
	    is->host, sizeof is->host);

	dns_resolve(is);
}
