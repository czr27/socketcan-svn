/*
 * af_can.c - Protocol family CAN core module
 *            (used by different CAN protocol modules)
 *
 * Copyright (c) 2002-2007 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <socketcan-users@lists.berlios.de>
 *
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
#include <linux/uaccess.h>
#else
#include <asm/uaccess.h>
#endif
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/socket.h>
#include <linux/if_ether.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <socketcan/can.h>
#include <socketcan/can/core.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
#include <net/net_namespace.h>
#endif
#include <net/sock.h>

#include "af_can.h"
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25)
#include "compat.h"
#endif

#include <socketcan/can/version.h> /* for RCSID. Removed by mkpatch script */
RCSID("$Id$");

static __initdata const char banner[] = KERN_INFO
	"can: controller area network core (" CAN_VERSION_STRING ")\n";

MODULE_DESCRIPTION("Controller Area Network PF_CAN core");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Urs Thuermann <urs.thuermann@volkswagen.de>, "
	      "Oliver Hartkopp <oliver.hartkopp@volkswagen.de>");

MODULE_ALIAS_NETPROTO(PF_CAN);

static int stats_timer __read_mostly = 1;
module_param(stats_timer, int, S_IRUGO);
MODULE_PARM_DESC(stats_timer, "enable timer for statistics (default:on)");

HLIST_HEAD(can_rx_dev_list);
static struct dev_rcv_lists can_rx_alldev_list;
static DEFINE_SPINLOCK(can_rcvlists_lock);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,20)
static struct kmem_cache *rcv_cache __read_mostly;
#else
static kmem_cache_t *rcv_cache;
#endif

/* table of registered CAN protocols */
static const struct can_proto *proto_tab[CAN_NPROTO] __read_mostly;
static DEFINE_MUTEX(proto_tab_lock);

struct timer_list can_stattimer;   /* timer for statistics update */
struct s_stats    can_stats;       /* packet statistics */
struct s_pstats   can_pstats;      /* receive list statistics */

/*
 * af_can socket functions
 */

int can_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk = sock->sk;

	switch (cmd) {

	case SIOCGSTAMP:
		return sock_get_timestamp(sk, (struct timeval __user *)arg);

	default:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16)
		return -ENOIOCTLCMD;
#else
		return dev_ioctl(cmd, (void __user *)arg);
#endif
	}
}
EXPORT_SYMBOL(can_ioctl);

static void can_sock_destruct(struct sock *sk)
{
	skb_queue_purge(&sk->sk_receive_queue);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,12)
	if (sk->sk_protinfo)
		kfree(sk->sk_protinfo);
#endif
}

static const struct can_proto *can_get_proto(int protocol)
{
	const struct can_proto *cp;

	rcu_read_lock();
	cp = rcu_dereference(proto_tab[protocol]);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12)
	if (cp && !try_module_get(cp->prot->owner))
		cp = NULL;
#else
	if (cp && !try_module_get(cp->owner))
		cp = NULL;
#endif
	rcu_read_unlock();

	return cp;
}

static inline void can_put_proto(const struct can_proto *cp)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12)
	module_put(cp->prot->owner);
#else
	module_put(cp->owner);
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33)
static int can_create(struct net *net, struct socket *sock, int protocol, int kern)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
static int can_create(struct net *net, struct socket *sock, int protocol)
#else
static int can_create(struct socket *sock, int protocol)
#endif
{
	struct sock *sk;
	const struct can_proto *cp;
	int err = 0;

	sock->state = SS_UNCONNECTED;

	if (protocol < 0 || protocol >= CAN_NPROTO)
		return -EINVAL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
	if (net != &init_net)
		return -EAFNOSUPPORT;
#endif

	cp = can_get_proto(protocol);

#ifdef CONFIG_MODULES
	if (!cp) {
		/* try to load protocol module if kernel is modular */

		err = request_module("can-proto-%d", protocol);

		/*
		 * In case of error we only print a message but don't
		 * return the error code immediately.  Below we will
		 * return -EPROTONOSUPPORT
		 */
		if (err && printk_ratelimit())
			printk(KERN_ERR "can: request_module "
			       "(can-proto-%d) failed.\n", protocol);

		cp = can_get_proto(protocol);
	}
#endif

	/* check for available protocol and correct usage */

	if (!cp)
		return -EPROTONOSUPPORT;

	if (cp->type != sock->type) {
		err = -EPROTOTYPE;
		goto errout;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,33)
	if (cp->capability >= 0 && !capable(cp->capability)) {
		err = -EPERM;
		goto errout;
	}
#endif
	sock->ops = cp->ops;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
	sk = sk_alloc(net, PF_CAN, GFP_KERNEL, cp->prot);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12)
	sk = sk_alloc(PF_CAN, GFP_KERNEL, cp->prot, 1);
#else
	sk = sk_alloc(PF_CAN, GFP_KERNEL, 1, 0);
#endif
	if (!sk) {
		err = -ENOMEM;
		goto errout;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,12)
	if (cp->obj_size) {
		sk->sk_protinfo = kmalloc(cp->obj_size, GFP_KERNEL);
		if (!sk->sk_protinfo) {
			sk_free(sk);
			err = -ENOMEM;
			goto errout;
		}
	}
	sk_set_owner(sk, proto_tab[protocol]->owner);
#endif

	sock_init_data(sock, sk);
	sk->sk_destruct = can_sock_destruct;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12)
	if (sk->sk_prot->init)
		err = sk->sk_prot->init(sk);
#else
	if (cp->init)
		err = cp->init(sk);
#endif

	if (err) {
		/* release sk on errors */
		sock_orphan(sk);
		sock_put(sk);
	}

 errout:
	can_put_proto(cp);
	return err;
}

/*
 * af_can tx path
 */

/**
 * can_send - transmit a CAN frame (optional with local loopback)
 * @skb: pointer to socket buffer with CAN frame in data section
 * @loop: loopback for listeners on local CAN sockets (recommended default!)
 *
 * Due to the loopback this routine must not be called from hardirq context.
 *
 * Return:
 *  0 on success
 *  -ENETDOWN when the selected interface is down
 *  -ENOBUFS on full driver queue (see net_xmit_errno())
 *  -ENOMEM when local loopback failed at calling skb_clone()
 *  -EPERM when trying to send on a non-CAN interface
 *  -EINVAL when the skb->data does not contain a valid CAN frame
 */
int can_send(struct sk_buff *skb, int loop)
{
	struct sk_buff *newskb = NULL;
	struct can_frame *cf = (struct can_frame *)skb->data;
	int err;

	if (skb->len != sizeof(struct can_frame) || cf->can_dlc > 8) {
		kfree_skb(skb);
		return -EINVAL;
	}

	if (skb->dev->type != ARPHRD_CAN) {
		kfree_skb(skb);
		return -EPERM;
	}

	if (!(skb->dev->flags & IFF_UP)) {
		kfree_skb(skb);
		return -ENETDOWN;
	}

	skb->protocol = htons(ETH_P_CAN);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
#else
	skb->nh.raw = skb->data;
	skb->h.raw  = skb->data;
#endif

	if (loop) {
		/* local loopback of sent CAN frames */

		/* indication for the CAN driver: do loopback */
		skb->pkt_type = PACKET_LOOPBACK;

		/*
		 * The reference to the originating sock may be required
		 * by the receiving socket to check whether the frame is
		 * its own. Example: can_raw sockopt CAN_RAW_RECV_OWN_MSGS
		 * Therefore we have to ensure that skb->sk remains the
		 * reference to the originating sock by restoring skb->sk
		 * after each skb_clone() or skb_orphan() usage.
		 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25)
#define IFF_ECHO IFF_LOOPBACK
#endif
		if (!(skb->dev->flags & IFF_ECHO)) {
			/*
			 * If the interface is not capable to do loopback
			 * itself, we do it here.
			 */
			newskb = skb_clone(skb, GFP_ATOMIC);
			if (!newskb) {
				kfree_skb(skb);
				return -ENOMEM;
			}

			newskb->sk = skb->sk;
			newskb->ip_summed = CHECKSUM_UNNECESSARY;
			newskb->pkt_type = PACKET_BROADCAST;
		}
	} else {
		/* indication for the CAN driver: no loopback required */
		skb->pkt_type = PACKET_HOST;
	}

	/* send to netdevice */
	err = dev_queue_xmit(skb);
	if (err > 0)
		err = net_xmit_errno(err);

	if (err) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
		/* kfree_skb() does not check for !NULL on older kernels */
		if (newskb)
			kfree_skb(newskb);
#else
		kfree_skb(newskb);
#endif
		return err;
	}

	if (newskb)
		netif_rx_ni(newskb);

	/* update statistics */
	can_stats.tx_frames++;
	can_stats.tx_frames_delta++;

	return 0;
}
EXPORT_SYMBOL(can_send);

/*
 * af_can rx path
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
static struct dev_rcv_lists *find_dev_rcv_lists(struct net_device *dev)
{
	/*
	 * find receive list for this device
	 *
	 * Since 2.6.26 a new "midlevel private" ml_priv pointer has been
	 * introduced in struct net_device. We use this pointer to omit the
	 * linear walk through the can_rx_dev_list. A similar speedup has been
	 * queued for 2.6.34 mainline but using the new netdev_rcu lists.
	 * Therefore the can_rx_dev_list is still needed (e.g. in proc.c)
	 */

	/* dev == NULL is the indicator for the 'all' filterlist */
	if (!dev)
		return &can_rx_alldev_list;
	else
		return (struct dev_rcv_lists *)dev->ml_priv;
}
#else
static struct dev_rcv_lists *find_dev_rcv_lists(struct net_device *dev)
{
	struct dev_rcv_lists *d = NULL;
	struct hlist_node *n;

	/*
	 * find receive list for this device
	 *
	 * The hlist_for_each_entry*() macros curse through the list
	 * using the pointer variable n and set d to the containing
	 * struct in each list iteration.  Therefore, after list
	 * iteration, d is unmodified when the list is empty, and it
	 * points to last list element, when the list is non-empty
	 * but no match in the loop body is found.  I.e. d is *not*
	 * NULL when no match is found.  We can, however, use the
	 * cursor variable n to decide if a match was found.
	 */

	hlist_for_each_entry_rcu(d, n, &can_rx_dev_list, list) {
		if (d->dev == dev)
			break;
	}

	return n ? d : NULL;
}
#endif

/**
 * find_rcv_list - determine optimal filterlist inside device filter struct
 * @can_id: pointer to CAN identifier of a given can_filter
 * @mask: pointer to CAN mask of a given can_filter
 * @d: pointer to the device filter struct
 *
 * Description:
 *  Returns the optimal filterlist to reduce the filter handling in the
 *  receive path. This function is called by service functions that need
 *  to register or unregister a can_filter in the filter lists.
 *
 *  A filter matches in general, when
 *
 *          <received_can_id> & mask == can_id & mask
 *
 *  so every bit set in the mask (even CAN_EFF_FLAG, CAN_RTR_FLAG) describe
 *  relevant bits for the filter.
 *
 *  The filter can be inverted (CAN_INV_FILTER bit set in can_id) or it can
 *  filter for error frames (CAN_ERR_FLAG bit set in mask). For error frames
 *  there is a special filterlist and a special rx path filter handling.
 *
 * Return:
 *  Pointer to optimal filterlist for the given can_id/mask pair.
 *  Constistency checked mask.
 *  Reduced can_id to have a preprocessed filter compare value.
 */
static struct hlist_head *find_rcv_list(canid_t *can_id, canid_t *mask,
					struct dev_rcv_lists *d)
{
	canid_t inv = *can_id & CAN_INV_FILTER; /* save flag before masking */

	/* filter for error frames in extra filterlist */
	if (*mask & CAN_ERR_FLAG) {
		/* clear CAN_ERR_FLAG in filter entry */
		*mask &= CAN_ERR_MASK;
		return &d->rx[RX_ERR];
	}

	/* with cleared CAN_ERR_FLAG we have a simple mask/value filterpair */

#define CAN_EFF_RTR_FLAGS (CAN_EFF_FLAG | CAN_RTR_FLAG)

	/* ensure valid values in can_mask for 'SFF only' frame filtering */
	if ((*mask & CAN_EFF_FLAG) && !(*can_id & CAN_EFF_FLAG))
		*mask &= (CAN_SFF_MASK | CAN_EFF_RTR_FLAGS);

	/* reduce condition testing at receive time */
	*can_id &= *mask;

	/* inverse can_id/can_mask filter */
	if (inv)
		return &d->rx[RX_INV];

	/* mask == 0 => no condition testing at receive time */
	if (!(*mask))
		return &d->rx[RX_ALL];

	/* extra filterlists for the subscription of a single non-RTR can_id */
	if (((*mask & CAN_EFF_RTR_FLAGS) == CAN_EFF_RTR_FLAGS)
	    && !(*can_id & CAN_RTR_FLAG)) {

		if (*can_id & CAN_EFF_FLAG) {
			if (*mask == (CAN_EFF_MASK | CAN_EFF_RTR_FLAGS)) {
				/* RFC: a future use-case for hash-tables? */
				return &d->rx[RX_EFF];
			}
		} else {
			if (*mask == (CAN_SFF_MASK | CAN_EFF_RTR_FLAGS))
				return &d->rx_sff[*can_id];
		}
	}

	/* default: filter via can_id/can_mask */
	return &d->rx[RX_FIL];
}

/**
 * can_rx_register - subscribe CAN frames from a specific interface
 * @dev: pointer to netdevice (NULL => subcribe from 'all' CAN devices list)
 * @can_id: CAN identifier (see description)
 * @mask: CAN mask (see description)
 * @func: callback function on filter match
 * @data: returned parameter for callback function
 * @ident: string for calling module indentification
 *
 * Description:
 *  Invokes the callback function with the received sk_buff and the given
 *  parameter 'data' on a matching receive filter. A filter matches, when
 *
 *          <received_can_id> & mask == can_id & mask
 *
 *  The filter can be inverted (CAN_INV_FILTER bit set in can_id) or it can
 *  filter for error frames (CAN_ERR_FLAG bit set in mask).
 *
 *  The provided pointer to the sk_buff is guaranteed to be valid as long as
 *  the callback function is running. The callback function must *not* free
 *  the given sk_buff while processing it's task. When the given sk_buff is
 *  needed after the end of the callback function it must be cloned inside
 *  the callback function with skb_clone().
 *
 * Return:
 *  0 on success
 *  -ENOMEM on missing cache mem to create subscription entry
 *  -ENODEV unknown device
 */
int can_rx_register(struct net_device *dev, canid_t can_id, canid_t mask,
		    void (*func)(struct sk_buff *, void *), void *data,
		    char *ident)
{
	struct receiver *r;
	struct hlist_head *rl;
	struct dev_rcv_lists *d;
	int err = 0;

	/* insert new receiver  (dev,canid,mask) -> (func,data) */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	if (dev && dev->type != ARPHRD_CAN)
		return -ENODEV;
#endif

	r = kmem_cache_alloc(rcv_cache, GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	spin_lock(&can_rcvlists_lock);

	d = find_dev_rcv_lists(dev);
	if (d) {
		rl = find_rcv_list(&can_id, &mask, d);

		r->can_id  = can_id;
		r->mask    = mask;
		r->matches = 0;
		r->func    = func;
		r->data    = data;
		r->ident   = ident;

		hlist_add_head_rcu(&r->list, rl);
		d->entries++;

		can_pstats.rcv_entries++;
		if (can_pstats.rcv_entries_max < can_pstats.rcv_entries)
			can_pstats.rcv_entries_max = can_pstats.rcv_entries;
	} else {
		kmem_cache_free(rcv_cache, r);
		err = -ENODEV;
	}

	spin_unlock(&can_rcvlists_lock);

	return err;
}
EXPORT_SYMBOL(can_rx_register);

/*
 * can_rx_delete_device - rcu callback for dev_rcv_lists structure removal
 */
static void can_rx_delete_device(struct rcu_head *rp)
{
	struct dev_rcv_lists *d = container_of(rp, struct dev_rcv_lists, rcu);

	kfree(d);
}

/*
 * can_rx_delete_receiver - rcu callback for single receiver entry removal
 */
static void can_rx_delete_receiver(struct rcu_head *rp)
{
	struct receiver *r = container_of(rp, struct receiver, rcu);

	kmem_cache_free(rcv_cache, r);
}

/**
 * can_rx_unregister - unsubscribe CAN frames from a specific interface
 * @dev: pointer to netdevice (NULL => unsubcribe from 'all' CAN devices list)
 * @can_id: CAN identifier
 * @mask: CAN mask
 * @func: callback function on filter match
 * @data: returned parameter for callback function
 *
 * Description:
 *  Removes subscription entry depending on given (subscription) values.
 */
void can_rx_unregister(struct net_device *dev, canid_t can_id, canid_t mask,
		       void (*func)(struct sk_buff *, void *), void *data)
{
	struct receiver *r = NULL;
	struct hlist_head *rl;
	struct hlist_node *next;
	struct dev_rcv_lists *d;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	if (dev && dev->type != ARPHRD_CAN)
		return;
#endif

	spin_lock(&can_rcvlists_lock);

	d = find_dev_rcv_lists(dev);
	if (!d) {
		printk(KERN_ERR "BUG: receive list not found for "
		       "dev %s, id %03X, mask %03X\n",
		       DNAME(dev), can_id, mask);
		goto out;
	}

	rl = find_rcv_list(&can_id, &mask, d);

	/*
	 * Search the receiver list for the item to delete.  This should
	 * exist, since no receiver may be unregistered that hasn't
	 * been registered before.
	 */

	hlist_for_each_entry_rcu(r, next, rl, list) {
		if (r->can_id == can_id && r->mask == mask
		    && r->func == func && r->data == data)
			break;
	}

	/*
	 * Check for bugs in CAN protocol implementations:
	 * If no matching list item was found, the list cursor variable next
	 * will be NULL, while r will point to the last item of the list.
	 */

	if (!next) {
		printk(KERN_ERR "BUG: receive list entry not found for "
		       "dev %s, id %03X, mask %03X\n",
		       DNAME(dev), can_id, mask);
		r = NULL;
		d = NULL;
		goto out;
	}

	hlist_del_rcu(&r->list);
	d->entries--;

	if (can_pstats.rcv_entries > 0)
		can_pstats.rcv_entries--;

	/* remove device structure requested by NETDEV_UNREGISTER */
	if (d->remove_on_zero_entries && !d->entries) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
		dev->ml_priv = NULL;
#endif
		hlist_del_rcu(&d->list);
	} else
		d = NULL;

 out:
	spin_unlock(&can_rcvlists_lock);

	/* schedule the receiver item for deletion */
	if (r)
		call_rcu(&r->rcu, can_rx_delete_receiver);

	/* schedule the device structure for deletion */
	if (d)
		call_rcu(&d->rcu, can_rx_delete_device);
}
EXPORT_SYMBOL(can_rx_unregister);

static inline void deliver(struct sk_buff *skb, struct receiver *r)
{
	r->func(skb, r->data);
	r->matches++;
}

static int can_rcv_filter(struct dev_rcv_lists *d, struct sk_buff *skb)
{
	struct receiver *r;
	struct hlist_node *n;
	int matches = 0;
	struct can_frame *cf = (struct can_frame *)skb->data;
	canid_t can_id = cf->can_id;

	if (d->entries == 0)
		return 0;

	if (can_id & CAN_ERR_FLAG) {
		/* check for error frame entries only */
		hlist_for_each_entry_rcu(r, n, &d->rx[RX_ERR], list) {
			if (can_id & r->mask) {
				deliver(skb, r);
				matches++;
			}
		}
		return matches;
	}

	/* check for unfiltered entries */
	hlist_for_each_entry_rcu(r, n, &d->rx[RX_ALL], list) {
		deliver(skb, r);
		matches++;
	}

	/* check for can_id/mask entries */
	hlist_for_each_entry_rcu(r, n, &d->rx[RX_FIL], list) {
		if ((can_id & r->mask) == r->can_id) {
			deliver(skb, r);
			matches++;
		}
	}

	/* check for inverted can_id/mask entries */
	hlist_for_each_entry_rcu(r, n, &d->rx[RX_INV], list) {
		if ((can_id & r->mask) != r->can_id) {
			deliver(skb, r);
			matches++;
		}
	}

	/* check filterlists for single non-RTR can_ids */
	if (can_id & CAN_RTR_FLAG)
		return matches;

	if (can_id & CAN_EFF_FLAG) {
		hlist_for_each_entry_rcu(r, n, &d->rx[RX_EFF], list) {
			if (r->can_id == can_id) {
				deliver(skb, r);
				matches++;
			}
		}
	} else {
		can_id &= CAN_SFF_MASK;
		hlist_for_each_entry_rcu(r, n, &d->rx_sff[can_id], list) {
			deliver(skb, r);
			matches++;
		}
	}

	return matches;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,14)
static int can_rcv(struct sk_buff *skb, struct net_device *dev,
		   struct packet_type *pt, struct net_device *orig_dev)
#else
static int can_rcv(struct sk_buff *skb, struct net_device *dev,
		   struct packet_type *pt)
#endif
{
	struct dev_rcv_lists *d;
	struct can_frame *cf = (struct can_frame *)skb->data;
	int matches;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	if (!net_eq(dev_net(dev), &init_net))
		goto drop;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
	if (dev->nd_net != &init_net)
		goto drop;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
	if (WARN_ONCE(dev->type != ARPHRD_CAN ||
		      skb->len != sizeof(struct can_frame) ||
		      cf->can_dlc > 8,
		      "PF_CAN: dropped non conform skbuf: "
		      "dev type %d, len %d, can_dlc %d\n",
		      dev->type, skb->len, cf->can_dlc))
		goto drop;
#else
	BUG_ON(dev->type != ARPHRD_CAN ||
	       skb->len != sizeof(struct can_frame) ||
	       cf->can_dlc > 8);
#endif

	/* update statistics */
	can_stats.rx_frames++;
	can_stats.rx_frames_delta++;

	rcu_read_lock();

	/* deliver the packet to sockets listening on all devices */
	matches = can_rcv_filter(&can_rx_alldev_list, skb);

	/* find receive list for this device */
	d = find_dev_rcv_lists(dev);
	if (d)
		matches += can_rcv_filter(d, skb);

	rcu_read_unlock();

	/* consume the skbuff allocated by the netdevice driver */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
	consume_skb(skb);
#else
	kfree_skb(skb);
#endif

	if (matches > 0) {
		can_stats.matches++;
		can_stats.matches_delta++;
	}

	return NET_RX_SUCCESS;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
drop:
	kfree_skb(skb);
	return NET_RX_DROP;
#endif
}

/*
 * af_can protocol functions
 */

/**
 * can_proto_register - register CAN transport protocol
 * @cp: pointer to CAN protocol structure
 *
 * Return:
 *  0 on success
 *  -EINVAL invalid (out of range) protocol number
 *  -EBUSY  protocol already in use
 *  -ENOBUF if proto_register() fails
 */
int can_proto_register(const struct can_proto *cp)
{
	int proto = cp->protocol;
	int err = 0;

	if (proto < 0 || proto >= CAN_NPROTO) {
		printk(KERN_ERR "can: protocol number %d out of range\n",
		       proto);
		return -EINVAL;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12)
	err = proto_register(cp->prot, 0);
	if (err < 0)
		return err;
#endif

	mutex_lock(&proto_tab_lock);

	if (proto_tab[proto]) {
		printk(KERN_ERR "can: protocol %d already registered\n",
		       proto);
		err = -EBUSY;
	} else
		rcu_assign_pointer(proto_tab[proto], cp);

	mutex_unlock(&proto_tab_lock);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12)
	if (err < 0)
		proto_unregister(cp->prot);
#endif

	return err;
}
EXPORT_SYMBOL(can_proto_register);

/**
 * can_proto_unregister - unregister CAN transport protocol
 * @cp: pointer to CAN protocol structure
 */
void can_proto_unregister(const struct can_proto *cp)
{
	int proto = cp->protocol;

	mutex_lock(&proto_tab_lock);
	BUG_ON(proto_tab[proto] != cp);
	rcu_assign_pointer(proto_tab[proto], NULL);
	mutex_unlock(&proto_tab_lock);

	synchronize_rcu();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12)
	proto_unregister(cp->prot);
#endif
}
EXPORT_SYMBOL(can_proto_unregister);

/*
 * af_can notifier to create/remove CAN netdevice specific structs
 */
static int can_notifier(struct notifier_block *nb, unsigned long msg,
			void *data)
{
	struct net_device *dev = (struct net_device *)data;
	struct dev_rcv_lists *d;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
	if (!net_eq(dev_net(dev), &init_net))
		return NOTIFY_DONE;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
	if (dev->nd_net != &init_net)
		return NOTIFY_DONE;
#endif

	if (dev->type != ARPHRD_CAN)
		return NOTIFY_DONE;

	switch (msg) {

	case NETDEV_REGISTER:

		/*
		 * create new dev_rcv_lists for this device
		 *
		 * N.B. zeroing the struct is the correct initialization
		 * for the embedded hlist_head structs.
		 * Another list type, e.g. list_head, would require
		 * explicit initialization.
		 */

		d = kzalloc(sizeof(*d), GFP_KERNEL);
		if (!d) {
			printk(KERN_ERR
			       "can: allocation of receive list failed\n");
			return NOTIFY_DONE;
		}
		d->dev = dev;

		spin_lock(&can_rcvlists_lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
		BUG_ON(dev->ml_priv);
		dev->ml_priv = d;
#endif
		hlist_add_head_rcu(&d->list, &can_rx_dev_list);
		spin_unlock(&can_rcvlists_lock);

		break;

	case NETDEV_UNREGISTER:
		spin_lock(&can_rcvlists_lock);

		d = find_dev_rcv_lists(dev);
		if (d) {
			if (d->entries) {
				d->remove_on_zero_entries = 1;
				d = NULL;
			} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
				dev->ml_priv = NULL;
#endif
				hlist_del_rcu(&d->list);
			}
		} else
			printk(KERN_ERR "can: notifier: receive list not "
			       "found for dev %s\n", dev->name);

		spin_unlock(&can_rcvlists_lock);

		if (d)
			call_rcu(&d->rcu, can_rx_delete_device);

		break;
	}

	return NOTIFY_DONE;
}

/*
 * af_can module init/exit functions
 */

static struct packet_type can_packet __read_mostly = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
	.type = cpu_to_be16(ETH_P_CAN),
#else
	.type = __constant_htons(ETH_P_CAN),
#endif
	.dev  = NULL,
	.func = can_rcv,
};

static struct net_proto_family can_family_ops __read_mostly = {
	.family = PF_CAN,
	.create = can_create,
	.owner  = THIS_MODULE,
};

/* notifier block for netdevice event */
static struct notifier_block can_netdev_notifier __read_mostly = {
	.notifier_call = can_notifier,
};

/*
 * RTNETLINK
 */
static int can_rtnl_doit(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg)
{
	int ret, protocol;
	const struct can_proto *cp;
	rtnl_doit_func fn;

	protocol = ((struct rtgencanmsg *)NLMSG_DATA(nlh))->can_protocol;
	/* since rtnl_lock is held, dont try to load protocol */
	cp = can_get_proto(protocol);
	if (!cp)
		return -EPROTONOSUPPORT;

	switch (nlh->nlmsg_type) {
	case RTM_NEWADDR:
		fn = cp->rtnl_new_addr;
		break;
	case RTM_DELADDR:
		fn = cp->rtnl_del_addr;
		break;
	default:
		fn = 0;
		break;
	}
	if (fn)
		ret = fn(skb, nlh, arg);
	else
		ret = -EPROTONOSUPPORT;
	can_put_proto(cp);
	return ret;
}

static int can_rtnl_dumpit(struct sk_buff *skb, struct netlink_callback *cb,
		int offset)
{
	int ret, j;
	const struct can_proto *cp;
	rtnl_dumpit_func fn;

	ret = 0;
	for (j = cb->args[0]; j < CAN_NPROTO; ++j) {
		/* save state */
		cb->args[0] = j;
		cp = can_get_proto(j);
		if (!cp)
			/* we are looping, any error is our own fault */
			continue;
		fn = *((rtnl_dumpit_func *)(&((const uint8_t *)cp)[offset]));
		if (fn)
			ret = fn(skb, cb);
		can_put_proto(cp);
		if (ret < 0)
			/* suspend this skb */
			return ret;
	}
	return ret;
}

static int can_rtnl_dump_addr(struct sk_buff *skb, struct netlink_callback *cb)
{
	return can_rtnl_dumpit(skb, cb,
			offsetof(struct can_proto, rtnl_dump_addr));
}

/*
 * LINK AF properties
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
static size_t can_get_link_af_size(const struct net_device *dev)
{
	int ret, j, total;
	const struct can_proto *cp;

	if (!net_eq(dev_net(dev), &init_net) || (dev->type != ARPHRD_CAN))
		return 0;

	total = 0;
	for (j = 0; j < CAN_NPROTO; ++j) {
		cp = can_get_proto(j);
		if (!cp)
			/* no worry */
			continue;
		ret = 0;
		if (cp->rtnl_link_ops && cp->rtnl_link_ops->get_link_af_size)
			ret = cp->rtnl_link_ops->get_link_af_size(dev) +
				nla_total_size(sizeof(struct nlattr));
		can_put_proto(cp);
		if (ret < 0)
			return ret;
		total += ret;
	}
	return nla_total_size(total);
}

static int can_fill_link_af(struct sk_buff *skb, const struct net_device *dev)
{
	int ret, j, n;
	struct nlattr *nla;
	const struct can_proto *cp;

	if (!net_eq(dev_net(dev), &init_net) || (dev->type != ARPHRD_CAN))
		return -ENODATA;

	n = 0;
	for (j = 0; j < CAN_NPROTO; ++j) {
		cp = can_get_proto(j);
		if (!cp)
			/* no worry */
			continue;
		if (cp->rtnl_link_ops && cp->rtnl_link_ops->fill_link_af) {
			nla = nla_nest_start(skb, j);
			if (!nla)
				goto nla_put_failure;

			ret = cp->rtnl_link_ops->fill_link_af(skb, dev);
			/*
			 * Caller may return ENODATA to indicate that there
			 * was no data to be dumped. This is not an error, it
			 * means we should trim the attribute header and
			 * continue.
			 */
			if (ret == -ENODATA)
				nla_nest_cancel(skb, nla);
			else if (ret < 0)
				goto nla_put_failure;
			nla_nest_end(skb, nla);
			++n;
		}
		can_put_proto(cp);
	}
	return n ? 0 : -ENODATA;

nla_put_failure:
	nla_nest_cancel(skb, nla);
	can_put_proto(cp);
	return -EMSGSIZE;
}

static int can_validate_link_af(const struct net_device *dev,
				 const struct nlattr *nla)
{
	int ret, rem;
	const struct can_proto *cp;
	struct nlattr *prot;

	if (!net_eq(dev_net(dev), &init_net) || (dev->type != ARPHRD_CAN))
		return -EPROTONOSUPPORT;

	nla_for_each_nested(prot, nla, rem) {
		cp = can_get_proto(nla_type(prot));
		if (!cp)
			return -EPROTONOSUPPORT;
		if (!cp->rtnl_link_ops)
			ret = -EPROTONOSUPPORT;
		else if (!cp->rtnl_link_ops->validate_link_af)
			ret = 0;
		else
			ret = cp->rtnl_link_ops->validate_link_af(dev, prot);
		can_put_proto(cp);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int can_set_link_af(struct net_device *dev, const struct nlattr *nla)
{
	int ret, rem;
	const struct can_proto *cp;
	struct nlattr *prot;

	if (!net_eq(dev_net(dev), &init_net) || (dev->type != ARPHRD_CAN))
		return -EPROTONOSUPPORT;

	nla_for_each_nested(prot, nla, rem) {
		cp = can_get_proto(nla_type(prot));
		if (!cp)
			return -EPROTONOSUPPORT;
		if (!cp->rtnl_link_ops || !cp->rtnl_link_ops->set_link_af)
			ret = -EPROTONOSUPPORT;
		else
			ret = cp->rtnl_link_ops->set_link_af(dev, prot);
		can_put_proto(cp);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static struct rtnl_af_ops can_rtnl_af_ops = {
	.family		  = AF_CAN,
	.fill_link_af	  = can_fill_link_af,
	.get_link_af_size = can_get_link_af_size,
	.validate_link_af = can_validate_link_af,
	.set_link_af	  = can_set_link_af,
};
#endif

/* exported init */

static __init int can_init(void)
{
	printk(banner);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)
	rcv_cache = kmem_cache_create("can_receiver", sizeof(struct receiver),
				      0, 0, NULL);
#else
	rcv_cache = kmem_cache_create("can_receiver", sizeof(struct receiver),
				      0, 0, NULL, NULL);
#endif
	if (!rcv_cache)
		return -ENOMEM;

	/*
	 * Insert can_rx_alldev_list for reception on all devices.
	 * This struct is zero initialized which is correct for the
	 * embedded hlist heads, the dev pointer, and the entries counter.
	 */

	spin_lock(&can_rcvlists_lock);
	hlist_add_head_rcu(&can_rx_alldev_list.list, &can_rx_dev_list);
	spin_unlock(&can_rcvlists_lock);

	if (stats_timer) {
		/* the statistics are updated every second (timer triggered) */
		setup_timer(&can_stattimer, can_stat_update, 0);
		mod_timer(&can_stattimer, round_jiffies(jiffies + HZ));
	} else
		can_stattimer.function = NULL;

	can_init_proc();

	/* protocol register */
	sock_register(&can_family_ops);
	register_netdevice_notifier(&can_netdev_notifier);
	dev_add_pack(&can_packet);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
	rtnl_af_register(&can_rtnl_af_ops);
#endif
	rtnl_register(PF_CAN, RTM_NEWADDR, can_rtnl_doit, NULL);
	rtnl_register(PF_CAN, RTM_DELADDR, can_rtnl_doit, NULL);
	rtnl_register(PF_CAN, RTM_GETADDR, NULL, can_rtnl_dump_addr);

	return 0;
}

static __exit void can_exit(void)
{
	struct dev_rcv_lists *d;
	struct hlist_node *n, *next;

	if (stats_timer)
		del_timer(&can_stattimer);

	rtnl_unregister(PF_CAN, RTM_NEWADDR);
	rtnl_unregister(PF_CAN, RTM_DELADDR);
	rtnl_unregister(PF_CAN, RTM_GETADDR);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
	rtnl_af_unregister(&can_rtnl_af_ops);
#endif

	can_remove_proc();

	/* protocol unregister */
	dev_remove_pack(&can_packet);
	unregister_netdevice_notifier(&can_netdev_notifier);
	sock_unregister(PF_CAN);

	/* remove can_rx_dev_list */
	spin_lock(&can_rcvlists_lock);
	hlist_del(&can_rx_alldev_list.list);
	hlist_for_each_entry_safe(d, n, next, &can_rx_dev_list, list) {
		hlist_del(&d->list);
		BUG_ON(d->entries);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
		d->dev->ml_priv = NULL;
#endif
		kfree(d);
	}
	spin_unlock(&can_rcvlists_lock);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,15)
	rcu_barrier(); /* Wait for completion of call_rcu()'s */
#endif

	kmem_cache_destroy(rcv_cache);
}

module_init(can_init);
module_exit(can_exit);
