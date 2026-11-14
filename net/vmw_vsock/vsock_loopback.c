// SPDX-License-Identifier: GPL-2.0-only
/*
 * AF_VSOCK loopback transport for the COS17 kernel port.
 *
 * Why this file exists:
 * On 4.19 the AF_VSOCK family is only registered when a *transport*
 * calls __vsock_core_init() (vsock_core_init). Phones have no virtual
 * machine / hypervisor, so no VMCI / virtio / Hyper-V transport can ever
 * register on a ruby (MT6877) device. That leaves AF_VSOCK completely
 * unregistered and userspace gets -EAFNOSUPPORT (97) on every
 * socket(AF_VSOCK) call, which makes perfetto's traced restart-loop on
 * "vsock://-1:2010".
 *
 * This minimal loopback transport registers the family without any
 * traffic: sockets can be created and bound/listened (which is all
 * perfetto's traced needs), but no data flows because there is no peer
 * on real hardware. Connection attempts fail with -EOPNOTSUPP.
 */

#include <linux/init.h>
#include <linux/socket.h>
#include <net/sock.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vm_sockets.h>

#include <net/af_vsock.h>

#define VSOCK_LOOPBACK_BUF_SIZE (16 * 1024 * 1024)

static int vsock_loopback_sock_init(struct vsock_sock *vsk,
			       struct vsock_sock *psk)
{
	return 0;
}

static void vsock_loopback_destruct(struct vsock_sock *vsk)
{
}

static void vsock_loopback_release(struct vsock_sock *vsk)
{
}

static int vsock_loopback_cancel_pkt(struct vsock_sock *vsk)
{
	return 0;
}

static int vsock_loopback_connect(struct vsock_sock *vsk)
{
	return -EOPNOTSUPP;
}

static int vsock_loopback_dgram_bind(struct vsock_sock *vsk,
				     struct sockaddr_vm *addr)
{
	return 0;
}

static int vsock_loopback_dgram_dequeue(struct vsock_sock *vsk,
					struct msghdr *msg, size_t len,
					int flags)
{
	return -EOPNOTSUPP;
}

static int vsock_loopback_dgram_enqueue(struct vsock_sock *vsk,
					struct sockaddr_vm *addr,
					struct msghdr *msg, size_t len)
{
	return -EOPNOTSUPP;
}

static bool vsock_loopback_dgram_allow(u32 cid, u32 port)
{
	return false;
}

static ssize_t vsock_loopback_stream_dequeue(struct vsock_sock *vsk,
					     struct msghdr *msg,
					     size_t len, int flags)
{
	return -EOPNOTSUPP;
}

static ssize_t vsock_loopback_stream_enqueue(struct vsock_sock *vsk,
					     struct msghdr *msg, size_t len)
{
	return -EOPNOTSUPP;
}

static s64 vsock_loopback_stream_has_data(struct vsock_sock *vsk)
{
	return 0;
}

static s64 vsock_loopback_stream_has_space(struct vsock_sock *vsk)
{
	return 0;
}

static u64 vsock_loopback_stream_rcvhiwat(struct vsock_sock *vsk)
{
	return VSOCK_LOOPBACK_BUF_SIZE;
}

static bool vsock_loopback_stream_is_active(struct vsock_sock *vsk)
{
	return false;
}

static bool vsock_loopback_stream_allow(u32 cid, u32 port)
{
	return false;
}

static int vsock_loopback_notify_poll_in(struct vsock_sock *vsk,
					 size_t ready,
					 bool *more)
{
	return 0;
}

static int vsock_loopback_notify_poll_out(struct vsock_sock *vsk,
					  size_t ready,
					  bool *more)
{
	return 0;
}

static int vsock_loopback_notify_recv_init(
	struct vsock_sock *vsk, size_t ready,
	struct vsock_transport_recv_notify_data *data)
{
	return 0;
}

static int vsock_loopback_notify_recv_pre_block(
	struct vsock_sock *vsk, size_t ready,
	struct vsock_transport_recv_notify_data *data)
{
	return 0;
}

static int vsock_loopback_notify_recv_pre_dequeue(
	struct vsock_sock *vsk, size_t ready,
	struct vsock_transport_recv_notify_data *data)
{
	return 0;
}

static int vsock_loopback_notify_recv_post_dequeue(
	struct vsock_sock *vsk, size_t ready, ssize_t copied, bool empty,
	struct vsock_transport_recv_notify_data *data)
{
	return 0;
}

static int vsock_loopback_notify_send_init(
	struct vsock_sock *vsk,
	struct vsock_transport_send_notify_data *data)
{
	return 0;
}

static int vsock_loopback_notify_send_pre_block(
	struct vsock_sock *vsk,
	struct vsock_transport_send_notify_data *data)
{
	return 0;
}

static int vsock_loopback_notify_send_pre_enqueue(
	struct vsock_sock *vsk,
	struct vsock_transport_send_notify_data *data)
{
	return 0;
}

static int vsock_loopback_notify_send_post_enqueue(
	struct vsock_sock *vsk, ssize_t written,
	struct vsock_transport_send_notify_data *data)
{
	return 0;
}

static int vsock_loopback_shutdown(struct vsock_sock *vsk, int mode)
{
	return 0;
}

static void vsock_loopback_set_buffer_size(struct vsock_sock *vsk, u64 val)
{
}

static void vsock_loopback_set_min_buffer_size(struct vsock_sock *vsk, u64 val)
{
}

static void vsock_loopback_set_max_buffer_size(struct vsock_sock *vsk, u64 val)
{
}

static u64 vsock_loopback_get_buffer_size(struct vsock_sock *vsk)
{
	return VSOCK_LOOPBACK_BUF_SIZE;
}

static u64 vsock_loopback_get_min_buffer_size(struct vsock_sock *vsk)
{
	return VSOCK_LOOPBACK_BUF_SIZE;
}

static u64 vsock_loopback_get_max_buffer_size(struct vsock_sock *vsk)
{
	return VSOCK_LOOPBACK_BUF_SIZE;
}

static u32 vsock_loopback_get_local_cid(void)
{
	return VMADDR_CID_ANY;
}

static const struct vsock_transport vsock_loopback_transport = {
	.init			= vsock_loopback_sock_init,
	.destruct		= vsock_loopback_destruct,
	.release		= vsock_loopback_release,

	.cancel_pkt		= vsock_loopback_cancel_pkt,
	.connect		= vsock_loopback_connect,

	.dgram_bind		= vsock_loopback_dgram_bind,
	.dgram_dequeue		= vsock_loopback_dgram_dequeue,
	.dgram_enqueue		= vsock_loopback_dgram_enqueue,
	.dgram_allow		= vsock_loopback_dgram_allow,

	.stream_dequeue		= vsock_loopback_stream_dequeue,
	.stream_enqueue		= vsock_loopback_stream_enqueue,
	.stream_has_data	= vsock_loopback_stream_has_data,
	.stream_has_space	= vsock_loopback_stream_has_space,
	.stream_rcvhiwat	= vsock_loopback_stream_rcvhiwat,
	.stream_is_active	= vsock_loopback_stream_is_active,
	.stream_allow		= vsock_loopback_stream_allow,

	.notify_poll_in		= vsock_loopback_notify_poll_in,
	.notify_poll_out		= vsock_loopback_notify_poll_out,
	.notify_recv_init	= vsock_loopback_notify_recv_init,
	.notify_recv_pre_block	= vsock_loopback_notify_recv_pre_block,
	.notify_recv_pre_dequeue	= vsock_loopback_notify_recv_pre_dequeue,
	.notify_recv_post_dequeue	= vsock_loopback_notify_recv_post_dequeue,
	.notify_send_init	= vsock_loopback_notify_send_init,
	.notify_send_pre_block	= vsock_loopback_notify_send_pre_block,
	.notify_send_pre_enqueue	= vsock_loopback_notify_send_pre_enqueue,
	.notify_send_post_enqueue	= vsock_loopback_notify_send_post_enqueue,

	.shutdown		= vsock_loopback_shutdown,

	.set_buffer_size	= vsock_loopback_set_buffer_size,
	.set_min_buffer_size	= vsock_loopback_set_min_buffer_size,
	.set_max_buffer_size	= vsock_loopback_set_max_buffer_size,
	.get_buffer_size	= vsock_loopback_get_buffer_size,
	.get_min_buffer_size	= vsock_loopback_get_min_buffer_size,
	.get_max_buffer_size	= vsock_loopback_get_max_buffer_size,

	.get_local_cid		= vsock_loopback_get_local_cid,
};

static int __init vsock_loopback_init(void)
{
	return vsock_core_init(&vsock_loopback_transport);
}

static void __exit vsock_loopback_exit(void)
{
	vsock_core_exit();
}

module_init(vsock_loopback_init);
module_exit(vsock_loopback_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("COS17 port");
MODULE_DESCRIPTION("AF_VSOCK loopback transport (phone compat)");
