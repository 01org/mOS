/* IP tables module for matching IPsec policy
 *
 * Copyright (c) 2004 Patrick McHardy, <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <net/xfrm.h>

#include <linux/netfilter_ipv6.h>
#include <linux/netfilter_ipv6/ip6t_policy.h>
#include <linux/netfilter_ipv6/ip6_tables.h>

MODULE_AUTHOR("Patrick McHardy <kaber@trash.net>");
MODULE_DESCRIPTION("IPtables IPsec policy matching module");
MODULE_LICENSE("GPL");


static inline int ip6_masked_addrcmp(struct in6_addr addr1,
                                     struct in6_addr mask,
                                     struct in6_addr addr2)
{
	int i;

	for (i = 0; i < 16; i++) {
		if ((addr1.s6_addr[i] & mask.s6_addr[i]) !=
		    (addr2.s6_addr[i] & mask.s6_addr[i]))
			return 1;
	}
	return 0;
}


static inline int
match_xfrm_state(struct xfrm_state *x, const struct ip6t_policy_elem *e)
{
#define MISMATCH(x,y)	(e->match.x && ((e->x != (y)) ^ e->invert.x))
	
	struct in6_addr xfrm_saddr, xfrm_daddr;
	
	if ((e->match.saddr
	     && (ip6_masked_addrcmp(xfrm_saddr, e->saddr, e->smask))
	        ^ e->invert.saddr ) ||
	    (e->match.daddr
	     && (ip6_masked_addrcmp(xfrm_daddr, e->daddr, e->dmask))
	        ^ e->invert.daddr ) ||
	    MISMATCH(proto, x->id.proto) ||
	    MISMATCH(mode, x->props.mode) ||
	    MISMATCH(spi, x->id.spi) ||
	    MISMATCH(reqid, x->props.reqid))
		return 0;
	return 1;
}

static int
match_policy_in(const struct sk_buff *skb, const struct ip6t_policy_info *info)
{
	const struct ip6t_policy_elem *e;
	struct sec_path *sp = skb->sp;
	int strict = info->flags & POLICY_MATCH_STRICT;
	int i, pos;

	if (sp == NULL)
		return -1;
	if (strict && info->len != sp->len)
		return 0;

	for (i = sp->len - 1; i >= 0; i--) {
		pos = strict ? i - sp->len + 1 : 0;
		if (pos >= info->len)
			return 0;
		e = &info->pol[pos];

		if (match_xfrm_state(sp->x[i].xvec, e)) {
			if (!strict)
				return 1;
		} else if (strict)
			return 0;
	}

	return strict ? 1 : 0;
}

static int
match_policy_out(const struct sk_buff *skb, const struct ip6t_policy_info *info)
{
	const struct ip6t_policy_elem *e;
	struct dst_entry *dst = skb->dst;
	int strict = info->flags & POLICY_MATCH_STRICT;
	int i, pos;

	if (dst->xfrm == NULL)
		return -1;

	for (i = 0; dst && dst->xfrm; dst = dst->child, i++) {
		pos = strict ? i : 0;
		if (pos >= info->len)
			return 0;
		e = &info->pol[pos];

		if (match_xfrm_state(dst->xfrm, e)) {
			if (!strict)
				return 1;
		} else if (strict)
			return 0;
	}

	return strict ? 1 : 0;
}

static int match(const struct sk_buff *skb,
                 const struct net_device *in,
                 const struct net_device *out,
                 const void *matchinfo,
		 int offset,
		 unsigned int protoff,
		 int *hotdrop)
{
	const struct ip6t_policy_info *info = matchinfo;
	int ret;

	if (info->flags & POLICY_MATCH_IN)
		ret = match_policy_in(skb, info);
	else
		ret = match_policy_out(skb, info);

	if (ret < 0) {
		if (info->flags & POLICY_MATCH_NONE)
			ret = 1;
		else
			ret = 0;
	} else if (info->flags & POLICY_MATCH_NONE)
		ret = 0;

	return ret;
}

static int checkentry(const char *tablename, const struct ip6t_ip6 *ip,
                      void *matchinfo, unsigned int matchsize,
                      unsigned int hook_mask)
{
	struct ip6t_policy_info *info = matchinfo;

	if (matchsize != IP6T_ALIGN(sizeof(*info))) {
		printk(KERN_ERR "ip6t_policy: matchsize %u != %u\n",
		       matchsize, IP6T_ALIGN(sizeof(*info)));
		return 0;
	}
	if (!(info->flags & (POLICY_MATCH_IN|POLICY_MATCH_OUT))) {
		printk(KERN_ERR "ip6t_policy: neither incoming nor "
		                "outgoing policy selected\n");
		return 0;
	}
	if (hook_mask & (1 << NF_IP6_PRE_ROUTING | 1 << NF_IP6_LOCAL_IN)
	    && info->flags & POLICY_MATCH_OUT) {
		printk(KERN_ERR "ip6t_policy: output policy not valid in "
		                "PRE_ROUTING and INPUT\n");
		return 0;
	}
	if (hook_mask & (1 << NF_IP6_POST_ROUTING | 1 << NF_IP6_LOCAL_OUT)
	    && info->flags & POLICY_MATCH_IN) {
		printk(KERN_ERR "ip6t_policy: input policy not valid in "
		                "POST_ROUTING and OUTPUT\n");
		return 0;
	}
	if (info->len > POLICY_MAX_ELEM) {
		printk(KERN_ERR "ip6t_policy: too many policy elements\n");
		return 0;
	}

	return 1;
}

static struct ip6t_match policy_match =
{
	.name		= "policy",
	.match		= match,
	.checkentry 	= checkentry,
	.me		= THIS_MODULE,
};

static int __init init(void)
{
	return ip6t_register_match(&policy_match);
}

static void __exit fini(void)
{
	ip6t_unregister_match(&policy_match);
}

module_init(init);
module_exit(fini);
