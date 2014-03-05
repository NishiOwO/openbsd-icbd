/*
 * Copyright (c) 2014 Mike Belopuhov
 * Copyright (c) 2009 Michael Shalayeff
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <sysexits.h>
#include <login_cap.h>
#include <event.h>
#include <pwd.h>
#include <netdb.h>

#include "icb.h"
#include "icbd.h"

void logger_dispatch(int, short, void *);

int logger_pipe;

struct icbd_logentry {
	time_t	timestamp;
	char	group[ICB_MAXGRPLEN];
	char	nick[ICB_MAXNICKLEN];
	size_t	length;
};

int
logger_init(void)
{
	static struct event ev;
	struct passwd *pw;
	int pipes[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, pipes) == -1) {
		syslog(LOG_ERR, "socketpair: %m");
		exit(EX_OSERR);
	}

	switch (fork()) {
	case -1:
		syslog(LOG_ERR, "fork: %m");
		exit(EX_OSERR);
	case 0:
		break;

	default:
		close(pipes[1]);
		logger_pipe = pipes[0];
		return (0);
	}

	setproctitle("logger");
	close(pipes[0]);

	if ((pw = getpwnam(ICBD_USER)) == NULL) {
		syslog(LOG_ERR, "No passwd entry for %s", ICBD_USER);
		exit(EX_NOUSER);
	}

	if (setusercontext(NULL, pw, pw->pw_uid,
	    LOGIN_SETALL & ~LOGIN_SETUSER) < 0)
		exit(EX_NOPERM);

	if (setuid(pw->pw_uid) < 0) {
		syslog(LOG_ERR, "%d: %m", pw->pw_uid);
		exit(EX_NOPERM);
	}

	if (chdir(pw->pw_dir) < 0) {
		syslog(LOG_ERR, "chdir: %m");
		exit(EX_UNAVAILABLE);
	}

	event_init();

	/* event for message processing */
	event_set(&ev, pipes[1], EV_READ | EV_PERSIST, logger_dispatch, NULL);
	if (event_add(&ev, NULL) < 0) {
		syslog(LOG_ERR, "event_add: %m");
		exit (EX_UNAVAILABLE);
	}

	return event_dispatch();
}

void
logger_dispatch(int fd, short event, void *arg __attribute__((unused)))
{
	char buf[512];
	struct icbd_logentry e;
	struct iovec iov[2];

	if (event != EV_READ)
		return;

	bzero(&e, sizeof e);
	iov[0].iov_base = &e;
	iov[0].iov_len = sizeof e;

	iov[1].iov_base = buf;
	iov[1].iov_len = sizeof buf;

	if (readv(fd, iov, 2) < (ssize_t)sizeof e) {
		syslog(LOG_ERR, "logger read: %m");
		exit(EX_DATAERR);
	}

	/* XXX */
	if (iov[1].iov_len < e.length) {
		syslog(LOG_ERR, "logger read %lu out of %lu",
		    iov[1].iov_len, e.length);
	}

	/* TODO: check time of the day and open the next file */

	fprintf(stderr, "%s@%s: %s\n", e.nick, e.group, buf);
}

void
logger(time_t timestamp, char *group, char *nick, char *what)
{
	struct icbd_logentry e;
	struct iovec iov[2];

	e.timestamp = timestamp;
	strlcpy(e.group, group, ICB_MAXGRPLEN);
	strlcpy(e.nick, nick, ICB_MAXNICKLEN);
	e.length = strlen(what) + 1;

	iov[0].iov_base = &e;
	iov[0].iov_len = sizeof e;

	iov[1].iov_base = what;
	iov[1].iov_len = e.length;

	if (writev(logger_pipe, iov, 2) == -1)
		syslog(LOG_ERR, "logger write: %m");
}
