#include <rtems/freebsd/machine/rtems-bsd-config.h>

/*-
 * Copyright (c) 1982, 1986, 1988, 1990, 1993, 1994, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)tcp_input.c	8.12 (Berkeley) 5/24/95
 */

#include <rtems/freebsd/sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <rtems/freebsd/local/opt_inet.h>
#include <rtems/freebsd/local/opt_inet6.h>
#include <rtems/freebsd/local/opt_tcpdebug.h>

#include <rtems/freebsd/sys/param.h>
#include <rtems/freebsd/sys/kernel.h>
#include <rtems/freebsd/sys/malloc.h>
#include <rtems/freebsd/sys/mbuf.h>
#include <rtems/freebsd/sys/socket.h>
#include <rtems/freebsd/sys/socketvar.h>
#include <rtems/freebsd/sys/sysctl.h>
#include <rtems/freebsd/sys/syslog.h>
#include <rtems/freebsd/sys/systm.h>

#include <rtems/freebsd/vm/uma.h>

#include <rtems/freebsd/net/if.h>
#include <rtems/freebsd/net/route.h>
#include <rtems/freebsd/net/vnet.h>

#include <rtems/freebsd/netinet/in.h>
#include <rtems/freebsd/netinet/in_pcb.h>
#include <rtems/freebsd/netinet/in_systm.h>
#include <rtems/freebsd/netinet/in_var.h>
#include <rtems/freebsd/netinet/ip.h>
#include <rtems/freebsd/netinet/ip_var.h>
#include <rtems/freebsd/netinet/ip_options.h>
#include <rtems/freebsd/netinet/ip6.h>
#include <rtems/freebsd/netinet6/in6_pcb.h>
#include <rtems/freebsd/netinet6/ip6_var.h>
#include <rtems/freebsd/netinet6/nd6.h>
#include <rtems/freebsd/netinet/tcp.h>
#include <rtems/freebsd/netinet/tcp_fsm.h>
#include <rtems/freebsd/netinet/tcp_seq.h>
#include <rtems/freebsd/netinet/tcp_timer.h>
#include <rtems/freebsd/netinet/tcp_var.h>
#include <rtems/freebsd/netinet6/tcp6_var.h>
#include <rtems/freebsd/netinet/tcpip.h>
#ifdef TCPDEBUG
#include <rtems/freebsd/netinet/tcp_debug.h>
#endif /* TCPDEBUG */

static int tcp_reass_sysctl_maxseg(SYSCTL_HANDLER_ARGS);
static int tcp_reass_sysctl_qsize(SYSCTL_HANDLER_ARGS);

SYSCTL_NODE(_net_inet_tcp, OID_AUTO, reass, CTLFLAG_RW, 0,
    "TCP Segment Reassembly Queue");

static VNET_DEFINE(int, tcp_reass_maxseg) = 0;
#define	V_tcp_reass_maxseg		VNET(tcp_reass_maxseg)
SYSCTL_VNET_PROC(_net_inet_tcp_reass, OID_AUTO, maxsegments, CTLFLAG_RDTUN,
    &VNET_NAME(tcp_reass_maxseg), 0, &tcp_reass_sysctl_maxseg, "I",
    "Global maximum number of TCP Segments in Reassembly Queue");

static VNET_DEFINE(int, tcp_reass_qsize) = 0;
#define	V_tcp_reass_qsize		VNET(tcp_reass_qsize)
SYSCTL_VNET_PROC(_net_inet_tcp_reass, OID_AUTO, cursegments, CTLFLAG_RD,
    &VNET_NAME(tcp_reass_qsize), 0, &tcp_reass_sysctl_qsize, "I",
    "Global number of TCP Segments currently in Reassembly Queue");

static VNET_DEFINE(int, tcp_reass_overflows) = 0;
#define	V_tcp_reass_overflows		VNET(tcp_reass_overflows)
SYSCTL_VNET_INT(_net_inet_tcp_reass, OID_AUTO, overflows, CTLFLAG_RD,
    &VNET_NAME(tcp_reass_overflows), 0,
    "Global number of TCP Segment Reassembly Queue Overflows");

static VNET_DEFINE(uma_zone_t, tcp_reass_zone);
#define	V_tcp_reass_zone		VNET(tcp_reass_zone)

/* Initialize TCP reassembly queue */
static void
tcp_reass_zone_change(void *tag)
{

	V_tcp_reass_maxseg = nmbclusters / 16;
	uma_zone_set_max(V_tcp_reass_zone, V_tcp_reass_maxseg);
}

void
tcp_reass_init(void)
{

	V_tcp_reass_maxseg = nmbclusters / 16;
	TUNABLE_INT_FETCH("net.inet.tcp.reass.maxsegments",
	    &V_tcp_reass_maxseg);
	V_tcp_reass_zone = uma_zcreate("tcpreass", sizeof (struct tseg_qent),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, UMA_ZONE_NOFREE);
	uma_zone_set_max(V_tcp_reass_zone, V_tcp_reass_maxseg);
	EVENTHANDLER_REGISTER(nmbclusters_change,
	    tcp_reass_zone_change, NULL, EVENTHANDLER_PRI_ANY);
}

#ifdef VIMAGE
void
tcp_reass_destroy(void)
{

	uma_zdestroy(V_tcp_reass_zone);
}
#endif

void
tcp_reass_flush(struct tcpcb *tp)
{
	struct tseg_qent *qe;

	INP_WLOCK_ASSERT(tp->t_inpcb);

	while ((qe = LIST_FIRST(&tp->t_segq)) != NULL) {
		LIST_REMOVE(qe, tqe_q);
		m_freem(qe->tqe_m);
		uma_zfree(V_tcp_reass_zone, qe);
		tp->t_segqlen--;
	}

	KASSERT((tp->t_segqlen == 0),
	    ("TCP reass queue %p segment count is %d instead of 0 after flush.",
	    tp, tp->t_segqlen));
}

static int
tcp_reass_sysctl_maxseg(SYSCTL_HANDLER_ARGS)
{
	V_tcp_reass_maxseg = uma_zone_get_max(V_tcp_reass_zone);
	return (sysctl_handle_int(oidp, arg1, arg2, req));
}

static int
tcp_reass_sysctl_qsize(SYSCTL_HANDLER_ARGS)
{
	V_tcp_reass_qsize = uma_zone_get_cur(V_tcp_reass_zone);
	return (sysctl_handle_int(oidp, arg1, arg2, req));
}

int
tcp_reass(struct tcpcb *tp, struct tcphdr *th, int *tlenp, struct mbuf *m)
{
	struct tseg_qent *q;
	struct tseg_qent *p = NULL;
	struct tseg_qent *nq;
	struct tseg_qent *te = NULL;
	struct socket *so = tp->t_inpcb->inp_socket;
	int flags;

	INP_WLOCK_ASSERT(tp->t_inpcb);

	/*
	 * XXX: tcp_reass() is rather inefficient with its data structures
	 * and should be rewritten (see NetBSD for optimizations).
	 */

	/*
	 * Call with th==NULL after become established to
	 * force pre-ESTABLISHED data up to user socket.
	 */
	if (th == NULL)
		goto present;

	/*
	 * Limit the number of segments that can be queued to reduce the
	 * potential for mbuf exhaustion. For best performance, we want to be
	 * able to queue a full window's worth of segments. The size of the
	 * socket receive buffer determines our advertised window and grows
	 * automatically when socket buffer autotuning is enabled. Use it as the
	 * basis for our queue limit.
	 * Always let the missing segment through which caused this queue.
	 * NB: Access to the socket buffer is left intentionally unlocked as we
	 * can tolerate stale information here.
	 *
	 * XXXLAS: Using sbspace(so->so_rcv) instead of so->so_rcv.sb_hiwat
	 * should work but causes packets to be dropped when they shouldn't.
	 * Investigate why and re-evaluate the below limit after the behaviour
	 * is understood.
	 */
	if (th->th_seq != tp->rcv_nxt &&
	    tp->t_segqlen >= (so->so_rcv.sb_hiwat / tp->t_maxseg) + 1) {
		V_tcp_reass_overflows++;
		TCPSTAT_INC(tcps_rcvmemdrop);
		m_freem(m);
		*tlenp = 0;
		return (0);
	}

	/*
	 * Allocate a new queue entry. If we can't, or hit the zone limit
	 * just drop the pkt.
	 */
	te = uma_zalloc(V_tcp_reass_zone, M_NOWAIT);
	if (te == NULL) {
		TCPSTAT_INC(tcps_rcvmemdrop);
		m_freem(m);
		*tlenp = 0;
		return (0);
	}
	tp->t_segqlen++;

	/*
	 * Find a segment which begins after this one does.
	 */
	LIST_FOREACH(q, &tp->t_segq, tqe_q) {
		if (SEQ_GT(q->tqe_th->th_seq, th->th_seq))
			break;
		p = q;
	}

	/*
	 * If there is a preceding segment, it may provide some of
	 * our data already.  If so, drop the data from the incoming
	 * segment.  If it provides all of our data, drop us.
	 */
	if (p != NULL) {
		int i;
		/* conversion to int (in i) handles seq wraparound */
		i = p->tqe_th->th_seq + p->tqe_len - th->th_seq;
		if (i > 0) {
			if (i >= *tlenp) {
				TCPSTAT_INC(tcps_rcvduppack);
				TCPSTAT_ADD(tcps_rcvdupbyte, *tlenp);
				m_freem(m);
				uma_zfree(V_tcp_reass_zone, te);
				tp->t_segqlen--;
				/*
				 * Try to present any queued data
				 * at the left window edge to the user.
				 * This is needed after the 3-WHS
				 * completes.
				 */
				goto present;	/* ??? */
			}
			m_adj(m, i);
			*tlenp -= i;
			th->th_seq += i;
		}
	}
	TCPSTAT_INC(tcps_rcvoopack);
	TCPSTAT_ADD(tcps_rcvoobyte, *tlenp);

	/*
	 * While we overlap succeeding segments trim them or,
	 * if they are completely covered, dequeue them.
	 */
	while (q) {
		int i = (th->th_seq + *tlenp) - q->tqe_th->th_seq;
		if (i <= 0)
			break;
		if (i < q->tqe_len) {
			q->tqe_th->th_seq += i;
			q->tqe_len -= i;
			m_adj(q->tqe_m, i);
			break;
		}

		nq = LIST_NEXT(q, tqe_q);
		LIST_REMOVE(q, tqe_q);
		m_freem(q->tqe_m);
		uma_zfree(V_tcp_reass_zone, q);
		tp->t_segqlen--;
		q = nq;
	}

	/* Insert the new segment queue entry into place. */
	te->tqe_m = m;
	te->tqe_th = th;
	te->tqe_len = *tlenp;

	if (p == NULL) {
		LIST_INSERT_HEAD(&tp->t_segq, te, tqe_q);
	} else {
		LIST_INSERT_AFTER(p, te, tqe_q);
	}

present:
	/*
	 * Present data to user, advancing rcv_nxt through
	 * completed sequence space.
	 */
	if (!TCPS_HAVEESTABLISHED(tp->t_state))
		return (0);
	q = LIST_FIRST(&tp->t_segq);
	if (!q || q->tqe_th->th_seq != tp->rcv_nxt)
		return (0);
	SOCKBUF_LOCK(&so->so_rcv);
	do {
		tp->rcv_nxt += q->tqe_len;
		flags = q->tqe_th->th_flags & TH_FIN;
		nq = LIST_NEXT(q, tqe_q);
		LIST_REMOVE(q, tqe_q);
		if (so->so_rcv.sb_state & SBS_CANTRCVMORE)
			m_freem(q->tqe_m);
		else
			sbappendstream_locked(&so->so_rcv, q->tqe_m);
		uma_zfree(V_tcp_reass_zone, q);
		tp->t_segqlen--;
		q = nq;
	} while (q && q->tqe_th->th_seq == tp->rcv_nxt);
	ND6_HINT(tp);
	sorwakeup_locked(so);
	return (flags);
}