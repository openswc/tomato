/*
 *             Pseudo-driver for the intermediate queue device.
 *
 *             This program is free software; you can redistribute it and/or
 *             modify it under the terms of the GNU General Public License
 *             as published by the Free Software Foundation; either version
 *             2 of the License, or (at your option) any later version.
 *
 * Authors:    Patrick McHardy, <kaber@trash.net>
 *
 * 	       The first version was written by Martin Devera, <devik@cdi.cz>
 *
 * Credits:    Jan Rafaj <imq2t@cedric.vabo.cz>
 *              - Update patch to 2.4.21
 *             Sebastian Strollo <sstrollo@nortelnetworks.com>
 *              - Fix "Dead-loop on netdevice imq"-issue
 *	       After some time of inactivity there is a group taking care
 *	       of IMQ again: http://www.linuximq.net
 *
 *
 *
 *             More info at: http://www.linuximq.net/ (2004-04-22)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/config.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/if_arp.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
#include <linux/netfilter_ipv6.h>
#endif
#include <linux/imq.h>
#include <net/pkt_sched.h>

static nf_hookfn imq_nf_hook;

static struct nf_hook_ops imq_ingress_ipv4 = {
       .list           = { NULL, NULL},
       .hook           = imq_nf_hook,
       .pf             = PF_INET,
       .hooknum        = NF_IP_PRE_ROUTING,
       .priority       = NF_IP_PRI_MANGLE + 1
};

static struct nf_hook_ops imq_egress_ipv4 = {
       .list           = { NULL, NULL},
       .hook           = imq_nf_hook,
       .pf             = PF_INET,
       .hooknum        = NF_IP_POST_ROUTING,
       .priority       = NF_IP_PRI_NAT_SRC - 1
};

#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
static struct nf_hook_ops imq_ingress_ipv6 = {
       .list           = { NULL, NULL},
       .hook           = imq_nf_hook,
       .pf             = PF_INET6,
       .hooknum        = NF_IP6_PRE_ROUTING,
       .priority       = NF_IP6_PRI_MANGLE + 1
};

static struct nf_hook_ops imq_egress_ipv6 = {
       .list           = { NULL, NULL},
       .hook           = imq_nf_hook,
       .pf             = PF_INET6,
       .hooknum        = NF_IP6_POST_ROUTING,
       .priority       = NF_IP6_PRI_NAT_SRC - 1
};
#endif

#if defined(CONFIG_IMQ_NUM_DEVS)
static unsigned int numdevs = CONFIG_IMQ_NUM_DEVS;
#else
static unsigned int numdevs = 2;
#endif

static char *behaviour;

#if defined(CONFIG_IMQ_BEHAVIOR_BB)
static int imq_behaviour = 0;
#elif defined(CONFIG_IMQ_BEHAVIOR_BA)
static int imq_behaviour = IMQ_BEHAV_EGRESS_AFTER;
#elif defined(CONFIG_IMQ_BEHAVIOR_AB)
static int imq_behaviour = IMQ_BEHAV_INGRESS_AFTER;
#elif defined(CONFIG_IMQ_BEHAVIOR_AA)
static int imq_behaviour = IMQ_BEHAV_INGRESS_AFTER | IMQ_BEHAV_EGRESS_AFTER;
#else
static int imq_behaviour = IMQ_BEHAV_EGRESS_AFTER;
#endif

static struct net_device *imq_devs;


static struct net_device_stats *imq_get_stats(struct net_device *dev)
{
	return (struct net_device_stats *)dev->priv;
}

/* called for packets kfree'd in qdiscs at places other than enqueue */
static void imq_skb_destructor(struct sk_buff *skb)
{
	struct nf_info *info = skb->nf_info;

	if (info) {
		if (info->indev)
			dev_put(info->indev);
		if (info->outdev)
			dev_put(info->outdev);
		kfree(info);
	}
}

static int imq_dev_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct net_device_stats *stats = (struct net_device_stats*) dev->priv;
	
	stats->tx_bytes += skb->len;
	stats->tx_packets++;

	skb->imq_flags = 0;
	skb->destructor = NULL;
	
	dev->trans_start = jiffies;
	nf_reinject(skb, skb->nf_info, NF_ACCEPT);
	return 0;
}

static int imq_nf_queue(struct sk_buff *skb, struct nf_info *info, void *data)
{
	struct net_device *dev;
	struct net_device_stats *stats;
	struct sk_buff *skb2 = NULL;
	struct Qdisc *q;
	unsigned int index = skb->imq_flags&IMQ_F_IFMASK;
	int ret = -1;

	if (index > numdevs) 
		return -1;
	
	dev = imq_devs + index;
	if (!(dev->flags & IFF_UP)) {
		skb->imq_flags = 0;
		nf_reinject(skb, info, NF_ACCEPT);
		return 0;
	}
	dev->last_rx = jiffies;

	if (skb->destructor) {
		skb2 = skb;
		skb = skb_clone(skb, GFP_ATOMIC);
		if (!skb)
			return -1;
	}
	skb->nf_info = info;

	stats = (struct net_device_stats *)dev->priv;
	stats->rx_bytes+= skb->len;
	stats->rx_packets++;
	
	spin_lock_bh(&dev->queue_lock);
	q = dev->qdisc;
	if (q->enqueue) {
		q->enqueue(skb_get(skb), q);
		if (skb_shared(skb)) {
			skb->destructor = imq_skb_destructor;
			kfree_skb(skb);
			ret = 0;
		}
	}
	if (spin_is_locked(&dev->xmit_lock))
		netif_schedule(dev);
	else
		qdisc_run(dev);
	spin_unlock_bh(&dev->queue_lock);

	if (skb2)
		kfree_skb(ret ? skb : skb2);

	return ret;
}

static unsigned int imq_nf_hook(unsigned int hook, struct sk_buff **pskb,
		   const struct net_device *indev,
		   const struct net_device *outdev,
		   int (*okfn)(struct sk_buff *))
{
	if ((*pskb)->imq_flags & IMQ_F_ENQUEUE)
		return NF_QUEUE;

	return NF_ACCEPT;
}


static int __init imq_init_hooks(void)
{
	int err;

	if (behaviour) {
		if (strlen(behaviour) == 2) {
		       if (behaviour[0] == 'b') {
			       imq_behaviour &= ~IMQ_BEHAV_INGRESS_AFTER;
		       } else if (behaviour[0] == 'a') {
			       imq_behaviour |= IMQ_BEHAV_INGRESS_AFTER;
		       } else
				goto bad_behaviour;
		       if (behaviour[1] == 'b') {
			       imq_behaviour &= ~IMQ_BEHAV_EGRESS_AFTER;
                       } else if (behaviour[1] == 'a') {
			       imq_behaviour |= IMQ_BEHAV_EGRESS_AFTER;
		       } else
				goto bad_behaviour;
		} else {
bad_behaviour:
			printk(KERN_ERR "IMQ: behaviour must be one of these: \"bb\", \"ba\", \"ab\", \"aa\".\n");
                       return -EINVAL;
		}
	}

	if (imq_behaviour & IMQ_BEHAV_INGRESS_AFTER) {
	       imq_ingress_ipv4.priority = NF_IP_PRI_NAT_DST + 1;
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
	       imq_ingress_ipv6.priority = NF_IP6_PRI_NAT_DST + 1;
#endif
	}
	if (imq_behaviour & IMQ_BEHAV_EGRESS_AFTER) {
	       imq_egress_ipv4.priority = NF_IP_PRI_LAST;
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
	       imq_egress_ipv6.priority = NF_IP6_PRI_LAST;
#endif
	}

	if ((err = nf_register_queue_handler(PF_INET, imq_nf_queue, NULL)))
		goto err1;
	if ((err = nf_register_hook(&imq_ingress_ipv4)))
		goto err2;
	if ((err = nf_register_hook(&imq_egress_ipv4)))
		goto err3;
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
	if ((err = nf_register_queue_handler(PF_INET6, imq_nf_queue, NULL)))
		goto err4;
	if ((err = nf_register_hook(&imq_ingress_ipv6)))
		goto err5;
	if ((err = nf_register_hook(&imq_egress_ipv6)))
		goto err6;
#endif
	
	return 0;
	
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
err6:
	nf_unregister_hook(&imq_ingress_ipv6);
err5:
	nf_unregister_queue_handler(PF_INET6);
err4:
	nf_unregister_hook(&imq_egress_ipv4);
#endif
err3:
	nf_unregister_hook(&imq_ingress_ipv4);
err2:
	nf_unregister_queue_handler(PF_INET);
err1:
	return err;
}

static void __exit imq_unhook(void)
{
	nf_unregister_hook(&imq_ingress_ipv4);
	nf_unregister_hook(&imq_egress_ipv4);
	nf_unregister_queue_handler(PF_INET);
#if defined(CONFIG_IPV6) || defined (CONFIG_IPV6_MODULE)
	nf_unregister_hook(&imq_ingress_ipv6);
	nf_unregister_hook(&imq_egress_ipv6);
	nf_unregister_queue_handler(PF_INET6);
#endif
}

static int __init imq_dev_init(struct net_device *dev)
{
	dev->hard_start_xmit	= imq_dev_xmit;
	dev->type		= ARPHRD_VOID;
	dev->mtu		= 1500;
	dev->tx_queue_len	= 30;
	dev->flags		= IFF_NOARP;
	dev->priv = kmalloc(sizeof(struct net_device_stats), GFP_KERNEL);
	if (dev->priv == NULL)
		return -ENOMEM;
	memset(dev->priv, 0, sizeof(struct net_device_stats));
	dev->get_stats		= imq_get_stats;

	return 0;
}

static void imq_dev_uninit(struct net_device *dev)
{
	kfree(dev->priv);
}

static int __init imq_init_devs(void)
{
	struct net_device *dev;
        int i,j;
        j = numdevs;

	if (!numdevs || numdevs > IMQ_MAX_DEVS) {
		printk(KERN_ERR "IMQ: numdevs has to be betweed 1 and %u\n",
		       IMQ_MAX_DEVS);
		return -EINVAL;
	}

	imq_devs = kmalloc(sizeof(struct net_device) * numdevs, GFP_KERNEL);
	if (!imq_devs)
		return -ENOMEM;
	memset(imq_devs, 0, sizeof(struct net_device) * numdevs);

	/* we start counting at zero */
	numdevs--;

	for (i = 0, dev = imq_devs; i <= numdevs; i++, dev++) {
		SET_MODULE_OWNER(dev);
		strcpy(dev->name, "imq%d");
		dev->init   = imq_dev_init;
		dev->uninit = imq_dev_uninit;

		if (register_netdev(dev) < 0)
			goto err_register;
	}
	return 0;

err_register:
	for (; i; i--)
		unregister_netdev(--dev);
	kfree(imq_devs);
	return -EIO;
}

static void imq_cleanup_devs(void)
{
	int i;
	struct net_device *dev = imq_devs;
	
	for (i = 0; i <= numdevs; i++)
		unregister_netdev(dev++);

	kfree(imq_devs);
}

static int __init imq_init_module(void)
{
	int err;

	if ((err = imq_init_devs())) {
		return err;
	}
	if ((err = imq_init_hooks())) {
		imq_cleanup_devs();
		return err;
	}

       printk(KERN_INFO "IMQ driver loaded.\n");

	printk(KERN_INFO "\tHooking IMQ %s NAT on %s.\n",
			 (imq_behaviour & IMQ_BEHAV_INGRESS_AFTER) ? "after" : "before",
			 "PREROUTING");
	printk(KERN_INFO "\tHooking IMQ %s NAT on %s.\n",
			 (imq_behaviour & IMQ_BEHAV_EGRESS_AFTER) ? "after" : "before",
			 "POSTROUTING");

	return 0;
}

static void __exit imq_cleanup_module(void)
{
	imq_unhook();
	imq_cleanup_devs();
}

module_init(imq_init_module);
module_exit(imq_cleanup_module);

MODULE_PARM(numdevs, "i");
MODULE_PARM_DESC(numdevs, "number of IMQ devices");
MODULE_PARM(behaviour, "s");
MODULE_PARM_DESC(behaviour, "where to hook in PREROUTING and POSTROUTING - Before or After NAT (aa,ab,ba,bb)");
MODULE_AUTHOR("");
MODULE_DESCRIPTION("Pseudo-driver for the intermediate queue device. See http://www.linuximq.net/ for more information.");
MODULE_LICENSE("GPL");
