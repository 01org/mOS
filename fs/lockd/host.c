/*
 * linux/fs/lockd/host.c
 *
 * Management for NLM peer hosts. The nlm_host struct is shared
 * between client and server implementation. The only reason to
 * do so is to reduce code bloat.
 *
 * Copyright (C) 1996, Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/in.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/svc.h>
#include <linux/lockd/lockd.h>
#include <linux/lockd/sm_inter.h>


#define NLMDBG_FACILITY		NLMDBG_HOSTCACHE
#define NLM_HOST_NRHASH		32
#define NLM_ADDRHASH(addr)	(ntohl(addr) & (NLM_HOST_NRHASH-1))
#define NLM_HOST_REBIND		(60 * HZ)
#define NLM_HOST_EXPIRE		((nrhosts > nlm_max_hosts)? 300 * HZ : 120 * HZ)
#define NLM_HOST_COLLECT	((nrhosts > nlm_max_hosts)? 120 * HZ :  60 * HZ)
#define NLM_HOST_ADDR(sv)	(&(sv)->s_nlmclnt->cl_xprt->addr)

static struct nlm_host *	nlm_hosts[NLM_HOST_NRHASH];
static unsigned long		next_gc;
static int			nrhosts;
static DECLARE_MUTEX(nlm_host_sema);

/*
 * Function pointers - will reference either the
 * "standard" statd functions that do upcalls to user land,
 * or the kernel statd functions.
 */
int				(*nsm_monitor)(struct nlm_host *);
int				(*nsm_unmonitor)(struct nlm_host *);


static void			nlm_gc_hosts(void);
static struct nsm_handle *	__nsm_find(const struct sockaddr_in *, const char *, int);

/*
 * Find an NLM server handle in the cache. If there is none, create it.
 */
struct nlm_host *
nlmclnt_lookup_host(struct sockaddr_in *sin, int proto, int version, const char *hostname)
{
	return nlm_lookup_host(0, sin, proto, version, hostname);
}

/*
 * Find an NLM client handle in the cache. If there is none, create it.
 */
struct nlm_host *
nlmsvc_lookup_host(struct svc_rqst *rqstp, const char *hostname)
{
	return nlm_lookup_host(1, &rqstp->rq_addr,
			       rqstp->rq_prot, rqstp->rq_vers,
			       hostname);
}

/*
 * Common host lookup routine for server & client
 */
struct nlm_host *
nlm_lookup_host(int server, struct sockaddr_in *sin,
					int proto, int version,
					const char *hostname)
{
	struct nlm_host	*host, **hp;
	struct nsm_handle *nsm = NULL;
	int		hash;

	dprintk("lockd: nlm_lookup_host(%08x, p=%d, v=%d, my role=%s, name=%s)\n",
			(unsigned)(sin? ntohl(sin->sin_addr.s_addr) : 0), proto, version,
			server? "server" : "client",
			hostname? hostname : "<none>");

	hash = NLM_ADDRHASH(sin->sin_addr.s_addr);

	/* Lock hash table */
	down(&nlm_host_sema);

	if (time_after_eq(jiffies, next_gc))
		nlm_gc_hosts();

	/* We may keep several nlm_host objects for a peer, because each
	 * nlm_host is identified by
	 * (address, protocol, version, server/client)
	 * We could probably simplify this a little by putting all those
	 * different NLM rpc_clients into one single nlm_host object.
	 * This would allow us to have one nlm_host per address.
	 */
	for (hp = &nlm_hosts[hash]; (host = *hp) != 0; hp = &host->h_next) {
		if (!nlm_cmp_addr(&host->h_addr, sin))
			continue;

		/* See if we have an NSM handle for this client */
		if (!nsm && (nsm = host->h_nsmhandle) != 0)
			atomic_inc(&nsm->sm_count);

		if (host->h_proto != proto)
			continue;
		if (host->h_version != version)
			continue;
		if (host->h_server != server)
			continue;

		/* Move it to the head of the hash chain. */
		if (hp != nlm_hosts + hash) {
			*hp = host->h_next;
			host->h_next = nlm_hosts[hash];
			nlm_hosts[hash] = host;
		}
		nlm_get_host(host);
		goto out;
	}

	/* Sadly, the host isn't in our hash table yet. See if
	 * we have an NSM handle for it. If not, create one.
	 */
	if (!nsm && !(nsm = nsm_find(sin, hostname)))
		goto out;

	if (!(host = (struct nlm_host *) kmalloc(sizeof(*host), GFP_KERNEL))) {
		nsm_release(nsm);
		goto out;
	}
	memset(host, 0, sizeof(*host));
	host->h_name	   = nsm->sm_name;
	host->h_addr       = *sin;
	host->h_addr.sin_port = 0;	/* ouch! */
	host->h_version    = version;
	host->h_proto      = proto;
	host->h_rpcclnt    = NULL;
	init_MUTEX(&host->h_sema);
	host->h_nextrebind = jiffies + NLM_HOST_REBIND;
	host->h_expires    = jiffies + NLM_HOST_EXPIRE;
	atomic_set(&host->h_count, 1);
	init_waitqueue_head(&host->h_gracewait);
	host->h_state      = 0;			/* pseudo NSM state */
	host->h_nsmstate   = 0;			/* real NSM state */
	host->h_nsmhandle  = nsm;
	host->h_server	   = server;
	host->h_next       = nlm_hosts[hash];
	nlm_hosts[hash]    = host;
	INIT_LIST_HEAD(&host->h_lockowners);
	spin_lock_init(&host->h_lock);

	if (++nrhosts > nlm_max_hosts)
		next_gc = 0;

out:
	up(&nlm_host_sema);
	return host;
}

struct nlm_host *
nlm_find_client(void)
{
	/* Find the next NLM client host and remove it from the
	 * list. The caller is supposed to release all resources
	 * held by this client, and release the nlm_host afterwards.
	 */
	int hash;
	down(&nlm_host_sema);
	for (hash = 0 ; hash < NLM_HOST_NRHASH; hash++) {
		struct nlm_host *host, **hp;
		for (hp = &nlm_hosts[hash]; (host = *hp) != 0; hp = &host->h_next) {
			if (host->h_server) {
				*hp = host->h_next;
				nlm_get_host(host);
				up(&nlm_host_sema);
				return host;
			}
		}
	}
	up(&nlm_host_sema);
	return NULL;
}

				
/*
 * Create the NLM RPC client for an NLM peer
 */
struct rpc_clnt *
nlm_bind_host(struct nlm_host *host)
{
	struct rpc_clnt	*clnt;
	struct rpc_xprt	*xprt;

	dprintk("lockd: nlm_bind_host(%08x)\n",
			(unsigned)ntohl(host->h_addr.sin_addr.s_addr));

	/* Lock host handle */
	down(&host->h_sema);

	/* If we've already created an RPC client, check whether
	 * RPC rebind is required
	 * Note: why keep rebinding if we're on a tcp connection?
	 */
	if ((clnt = host->h_rpcclnt) != NULL) {
		xprt = clnt->cl_xprt;
		if (!xprt->stream && time_after_eq(jiffies, host->h_nextrebind)) {
			clnt->cl_port = 0;
			host->h_nextrebind = jiffies + NLM_HOST_REBIND;
			dprintk("lockd: next rebind in %ld jiffies\n",
					host->h_nextrebind - jiffies);
		}
	} else {
		xprt = xprt_create_proto(host->h_proto, &host->h_addr, NULL);
		if (IS_ERR(xprt))
			goto forgetit;

		xprt_set_timeout(&xprt->timeout, 5, nlmsvc_timeout);
		xprt->nocong = 1;	/* No congestion control for NLM */
		xprt->resvport = 1;	/* NLM requires a reserved port */

		/* Existing NLM servers accept AUTH_UNIX only */
		clnt = rpc_create_client(xprt, host->h_name, &nlm_program,
					host->h_version, RPC_AUTH_UNIX);
		if (IS_ERR(clnt))
			goto forgetit;
		clnt->cl_autobind = 1;	/* turn on pmap queries */

		host->h_rpcclnt = clnt;
	}

	up(&host->h_sema);
	return clnt;

forgetit:
	printk("lockd: couldn't create RPC handle for %s\n", host->h_name);
	up(&host->h_sema);
	return NULL;
}

/*
 * Force a portmap lookup of the remote lockd port
 */
void
nlm_rebind_host(struct nlm_host *host)
{
	dprintk("lockd: rebind host %s\n", host->h_name);
	if (host->h_rpcclnt && time_after_eq(jiffies, host->h_nextrebind)) {
		host->h_rpcclnt->cl_port = 0;
		host->h_nextrebind = jiffies + NLM_HOST_REBIND;
	}
}

/*
 * Increment NLM host count
 */
struct nlm_host * nlm_get_host(struct nlm_host *host)
{
	if (host) {
		dprintk("lockd: get host %s\n", host->h_name);
		atomic_inc(&host->h_count);
		host->h_expires = jiffies + NLM_HOST_EXPIRE;
	}
	return host;
}

/*
 * Release NLM host after use
 */
void nlm_release_host(struct nlm_host *host)
{
	if (host != NULL) {
		dprintk("lockd: release host %s, refcount is now %u\n",
				host->h_name, atomic_read(&host->h_count)-1);
		atomic_dec(&host->h_count);
		BUG_ON(atomic_read(&host->h_count) < 0);
	}
}

/*
 * Given an IP address or a host name, initiate recovery and ditch all locks.
 */
void
nlm_host_rebooted(const struct sockaddr_in *sin, const char *hostname, u32 new_state)
{
	struct nsm_handle *nsm;
	struct nlm_host	*host, **hp;
	int		hash;

	dprintk("lockd: nlm_host_rebooted(%s, %u.%u.%u.%u)\n",
			hostname, NIPQUAD(sin->sin_addr));

	/* Find the NSM handle for this peer */
	if (!(nsm = __nsm_find(sin, hostname, 0)))
		return;

	/* When reclaiming locks on this peer, make sure that
	 * we set up a new notification */
	nsm->sm_monitored = 0;

	/* Mark all hosts tied to this NSM state as having rebooted.
	 * We run the loop repeatedly, because we drop the host table
	 * lock for this.
	 * To avoid processing a host several times, we match the nsmstate.
	 */
again:	down(&nlm_host_sema);
	for (hash = 0; hash < NLM_HOST_NRHASH; hash++) {
		for (hp = &nlm_hosts[hash]; (host = *hp); hp = &host->h_next) {
			if (host->h_nsmhandle == nsm
			 && host->h_nsmstate != new_state) {
				host->h_nsmstate = new_state;
				host->h_state++;

				nlm_get_host(host);
				up(&nlm_host_sema);

				if (host->h_server) {
					/* We're server for this guy, just ditch
					 * all the locks he held. */
					nlmsvc_free_host_resources(host);
				} else {
					/* He's the server, initiate lock recovery. */
					nlmclnt_recovery(host, new_state);
				}

				nlm_release_host(host);
				goto again;
			}
		}
	}

	up(&nlm_host_sema);
}

/*
 * Shut down the hosts module.
 * Note that this routine is called only at server shutdown time.
 */
void
nlm_shutdown_hosts(void)
{
	struct nlm_host	*host;
	int		i;

	dprintk("lockd: shutting down host module\n");
	down(&nlm_host_sema);

	/* First, make all hosts eligible for gc */
	dprintk("lockd: nuking all hosts...\n");
	for (i = 0; i < NLM_HOST_NRHASH; i++) {
		for (host = nlm_hosts[i]; host; host = host->h_next)
			host->h_expires = jiffies - 1;
	}

	/* Then, perform a garbage collection pass */
	nlm_gc_hosts();
	up(&nlm_host_sema);

	/* complain if any hosts are left */
	if (nrhosts) {
		printk(KERN_WARNING "lockd: couldn't shutdown host module!\n");
		dprintk("lockd: %d hosts left:\n", nrhosts);
		for (i = 0; i < NLM_HOST_NRHASH; i++) {
			for (host = nlm_hosts[i]; host; host = host->h_next) {
				dprintk("       %s (cnt %d use %d exp %ld)\n",
					host->h_name, atomic_read(&host->h_count),
					host->h_inuse, host->h_expires);
			}
		}
	}
}

/*
 * Garbage collect any unused NLM hosts.
 * This GC combines reference counting for async operations with
 * mark & sweep for resources held by remote clients.
 */
static void
nlm_gc_hosts(void)
{
	struct nlm_host	**q, *host;
	struct rpc_clnt	*clnt;
	int		i;

	dprintk("lockd: host garbage collection\n");
	for (i = 0; i < NLM_HOST_NRHASH; i++) {
		for (host = nlm_hosts[i]; host; host = host->h_next)
			host->h_inuse = 0;
	}

	/* Mark all hosts that hold locks, blocks or shares */
	nlmsvc_mark_resources();

	for (i = 0; i < NLM_HOST_NRHASH; i++) {
		q = &nlm_hosts[i];
		while ((host = *q) != NULL) {
			if (atomic_read(&host->h_count) || host->h_inuse
			 || time_before(jiffies, host->h_expires)) {
				dprintk("nlm_gc_hosts skipping %s (cnt %d use %d exp %ld)\n",
					host->h_name, atomic_read(&host->h_count),
					host->h_inuse, host->h_expires);
				q = &host->h_next;
				continue;
			}
			dprintk("lockd: delete host %s\n", host->h_name);
			*q = host->h_next;

			/* Release the NSM handle. Unmonitor unless
			 * host was invalidated (i.e. lockd restarted)
			 */
			nsm_unmonitor(host);

			if ((clnt = host->h_rpcclnt) != NULL) {
				if (atomic_read(&clnt->cl_users)) {
					printk(KERN_WARNING
						"lockd: active RPC handle\n");
					clnt->cl_dead = 1;
				} else {
					rpc_destroy_client(host->h_rpcclnt);
				}
			}
			BUG_ON(!list_empty(&host->h_lockowners));
			kfree(host);
			nrhosts--;
		}
	}

	next_gc = jiffies + NLM_HOST_COLLECT;
}


/*
 * Manage NSM handles
 */
static LIST_HEAD(nsm_handles);
static DECLARE_MUTEX(nsm_sema);

static struct nsm_handle *
__nsm_find(const struct sockaddr_in *sin, const char *hostname, int create)
{
	struct nsm_handle *nsm = NULL;
	struct list_head *pos;
	int hlen;

	if (!hostname || !sin)
		return NULL;

	if (nsm_use_hostnames && strchr(hostname, '/') != NULL) {
		if (printk_ratelimit()) {
			printk(KERN_WARNING "Invalid hostname \"%s\" "
					    "in NFS lock request\n",
				hostname);
		}
		return NULL;
	}

	down(&nsm_sema);
	list_for_each(pos, &nsm_handles) {
		nsm = list_entry(pos, struct nsm_handle, sm_link);
		if (nsm_use_hostnames) {
			if (strcmp(nsm->sm_name, hostname))
				continue;
		} else if (!nlm_cmp_addr(&nsm->sm_addr, sin)) {
			continue;
		}
		atomic_inc(&nsm->sm_count);
		goto out;
	}

	if (!create) {
		nsm = NULL;
		goto out;
	}

	hlen = strlen(hostname) + 1;

	nsm = (struct nsm_handle *) kmalloc(sizeof(*nsm) + hlen, GFP_KERNEL);
	if (nsm != NULL) {
		memset(nsm, 0, sizeof(*nsm));
		nsm->sm_addr = *sin;
		nsm->sm_name = (char *) (nsm + 1);
		strcpy(nsm->sm_name, hostname);
		atomic_set(&nsm->sm_count, 1);

		list_add(&nsm->sm_link, &nsm_handles);
	}

out:	up(&nsm_sema);
	return nsm;
}

struct nsm_handle *
nsm_find(const struct sockaddr_in *sin, const char *hostname)
{
	return __nsm_find(sin, hostname, 1);
}

/*
 * Release an NSM handle
 */
void
nsm_release(struct nsm_handle *nsm)
{
	if (!nsm)
		return;
	if (atomic_read(&nsm->sm_count) == 1) {
		down(&nsm_sema);
		if (atomic_dec_and_test(&nsm->sm_count)) {
			list_del(&nsm->sm_link);
			kfree(nsm);
		}
		up(&nsm_sema);
	}
}
