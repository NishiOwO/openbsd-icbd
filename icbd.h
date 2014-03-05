/*
 * Copyright (c) 2009 Mike Belopuhov
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

#define ICBD_USER	"_icbd"

#define TCP_BACKLOG	5

#define EVBUFFER_FD(x)	(EVENT_FD(&(x)->ev_read))

extern int verbose;

/* icbd.c */
inline struct icb_session *icbd_session_lookup(uint64_t);
time_t	getmonotime(void);

/* dns.c */
struct sockaddr_storage;
int	dns_init(void);
int	dns_rresolv(struct icb_session *, struct sockaddr_storage *);

/* logger.c */
int	logger_init(void);
void	logger(time_t, char *, char *, char *);
