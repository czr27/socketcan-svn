/*
 * A real quick'n'dirty hack to add a netlink CAN gateway entry.
 *
 * Parts of this code were taken from the iproute source and the original
 * vcan.c from Urs Thuermann.
 *
 * Oliver Hartkopp 2010-02-18
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <net/if.h>

#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <socketcan/can/gw.h>

#include <linux/if_link.h>

#define NLMSG_TAIL(nmsg) \
        ((struct rtattr *)(((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

int addattr_l(struct nlmsghdr *n, int maxlen, int type, const void *data,
	      int alen)
{
	int len = RTA_LENGTH(alen);
	struct rtattr *rta;

	if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen) {
		fprintf(stderr, "addattr_l: message exceeded bound of %d\n",
			maxlen);
		return -1;
	}
	rta = NLMSG_TAIL(n);
	rta->rta_type = type;
	rta->rta_len = len;
	memcpy(RTA_DATA(rta), data, alen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
	return 0;
}

#define USE_PACKED_STRUCT

int main(int argc, char **argv)
{
	int s;
	int err = 0;

	struct {
		struct nlmsghdr n;
		struct rtcanmsg r;
		char buf[1000];

	} req;

	struct can_filter filter;
	struct sockaddr_nl nladdr;

#ifdef USE_PACKED_STRUCT
	struct modattr {
		struct can_frame cf;
		__u8 modtype;
	} __attribute__((packed));

	struct modattr modmsg;
#else
	static struct can_frame modframe;
	char modbuf[CGW_MODATTR_LEN];
#endif

	s = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);

	memset(&req, 0, sizeof(req));

	req.n.nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtcanmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
	req.n.nlmsg_type  = RTM_NEWROUTE;
	req.n.nlmsg_seq   = 0;

	req.r.can_family  = AF_CAN;
	req.r.src_ifindex = if_nametoindex("vcan2");
	req.r.dst_ifindex = if_nametoindex("vcan3");
	req.r.can_txflags = CAN_GW_TXFLAGS_ECHO;

	/* add new attributes here */

	filter.can_id = 0x400;
	filter.can_mask = 0x700;

	addattr_l(&req.n, sizeof(req), CGW_FILTER, &filter, sizeof(filter));

#ifdef USE_PACKED_STRUCT

	if (sizeof(modmsg) != CGW_MODATTR_LEN) {
		printf("Problem with packed msg. Use linear copy instead.\n");
		return 1;
	}

	modmsg.cf.can_id  = 0x555;
	modmsg.cf.can_dlc = 5;
	*(unsigned long long *)modmsg.cf.data = 0x5555555555555555ULL;

	modmsg.modtype = CGW_MOD_ID;
	addattr_l(&req.n, sizeof(req), CGW_MOD_SET, &modmsg, CGW_MODATTR_LEN);

	modmsg.modtype = CGW_MOD_DLC;
	addattr_l(&req.n, sizeof(req), CGW_MOD_AND, &modmsg, CGW_MODATTR_LEN);

	modmsg.modtype = CGW_MOD_DATA;
	addattr_l(&req.n, sizeof(req), CGW_MOD_XOR, &modmsg, CGW_MODATTR_LEN);

#else

	modframe.can_id  = 0x555;
	modframe.can_dlc = 5;
	*(unsigned long long *)modframe.data = 0x5555555555555555ULL;

	memcpy(modbuf, &modframe, sizeof(struct can_frame));

	modbuf[sizeof(struct can_frame)] = CGW_MOD_ID;
	addattr_l(&req.n, sizeof(req), CGW_MOD_SET, modbuf, CGW_MODATTR_LEN);

	modbuf[sizeof(struct can_frame)] = CGW_MOD_DLC;
	addattr_l(&req.n, sizeof(req), CGW_MOD_AND, modbuf, CGW_MODATTR_LEN);

	modbuf[sizeof(struct can_frame)] = CGW_MOD_DATA;
	addattr_l(&req.n, sizeof(req), CGW_MOD_XOR, modbuf, CGW_MODATTR_LEN);

#endif

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;
	nladdr.nl_pid    = 0;
	nladdr.nl_groups = 0;

	err = sendto(s, &req, req.n.nlmsg_len, 0,
		     (struct sockaddr*)&nladdr, sizeof(nladdr));

	perror("netlink says ");
	close(s);

	return 0;
}

