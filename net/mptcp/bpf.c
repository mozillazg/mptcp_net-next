// SPDX-License-Identifier: GPL-2.0
/* Multipath TCP
 *
 * Copyright (c) 2020, Tessares SA.
 * Copyright (c) 2022, SUSE.
 *
 * Author: Nicolas Rybowski <nicolas.rybowski@tessares.net>
 */

#define pr_fmt(fmt) "MPTCP: " fmt

#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <net/bpf_sk_storage.h>
#include "protocol.h"

#ifdef CONFIG_BPF_JIT
extern struct bpf_struct_ops bpf_mptcp_sched_ops;
static const struct btf_type *mptcp_sched_type __read_mostly;
static u32 mptcp_sched_id;

static const struct bpf_func_proto *
bpf_mptcp_sched_get_func_proto(enum bpf_func_id func_id,
			       const struct bpf_prog *prog)
{
	switch (func_id) {
	case BPF_FUNC_sk_storage_get:
		return &bpf_sk_storage_get_proto;
	case BPF_FUNC_sk_storage_delete:
		return &bpf_sk_storage_delete_proto;
	case BPF_FUNC_skc_to_tcp6_sock:
		return &bpf_skc_to_tcp6_sock_proto;
	case BPF_FUNC_skc_to_tcp_sock:
		return &bpf_skc_to_tcp_sock_proto;
	default:
		return bpf_base_func_proto(func_id);
	}
}

static int bpf_mptcp_sched_btf_struct_access(struct bpf_verifier_log *log,
					     const struct bpf_reg_state *reg,
					     int off, int size)
{
	const struct btf_type *t;
	size_t end;

	t = btf_type_by_id(reg->btf, reg->btf_id);
	if (t != mptcp_sched_type) {
		bpf_log(log, "only access to mptcp_subflow_context is supported\n");
		return -EACCES;
	}

	switch (off) {
	case offsetof(struct mptcp_subflow_context, scheduled):
		end = offsetofend(struct mptcp_subflow_context, scheduled);
		break;
	case offsetof(struct mptcp_subflow_context, avg_pacing_rate):
		end = offsetofend(struct mptcp_subflow_context, avg_pacing_rate);
		break;
	default:
		bpf_log(log, "no write support to mptcp_subflow_context at off %d\n", off);
		return -EACCES;
	}

	if (off + size > end) {
		bpf_log(log, "access beyond mptcp_subflow_context at off %u size %u ended at %zu",
			off, size, end);
		return -EACCES;
	}

	return NOT_INIT;
}

static const struct bpf_verifier_ops bpf_mptcp_sched_verifier_ops = {
	.get_func_proto		= bpf_mptcp_sched_get_func_proto,
	.is_valid_access	= bpf_tracing_btf_ctx_access,
	.btf_struct_access	= bpf_mptcp_sched_btf_struct_access,
};

static int bpf_mptcp_sched_reg(void *kdata)
{
	return mptcp_register_scheduler(kdata);
}

static void bpf_mptcp_sched_unreg(void *kdata)
{
	mptcp_unregister_scheduler(kdata);
}

static int bpf_mptcp_sched_check_member(const struct btf_type *t,
					const struct btf_member *member,
					const struct bpf_prog *prog)
{
	return 0;
}

static int bpf_mptcp_sched_init_member(const struct btf_type *t,
				       const struct btf_member *member,
				       void *kdata, const void *udata)
{
	const struct mptcp_sched_ops *usched;
	struct mptcp_sched_ops *sched;
	u32 moff;

	usched = (const struct mptcp_sched_ops *)udata;
	sched = (struct mptcp_sched_ops *)kdata;

	moff = __btf_member_bit_offset(t, member) / 8;
	switch (moff) {
	case offsetof(struct mptcp_sched_ops, name):
		if (bpf_obj_name_cpy(sched->name, usched->name,
				     sizeof(sched->name)) <= 0)
			return -EINVAL;
		if (mptcp_sched_find(usched->name))
			return -EEXIST;
		return 1;
	}

	return 0;
}

static int bpf_mptcp_sched_init(struct btf *btf)
{
	s32 type_id;

	type_id = btf_find_by_name_kind(btf, "mptcp_subflow_context",
					BTF_KIND_STRUCT);
	if (type_id < 0)
		return -EINVAL;
	mptcp_sched_id = type_id;
	mptcp_sched_type = btf_type_by_id(btf, mptcp_sched_id);

	return 0;
}

struct bpf_struct_ops bpf_mptcp_sched_ops = {
	.verifier_ops	= &bpf_mptcp_sched_verifier_ops,
	.reg		= bpf_mptcp_sched_reg,
	.unreg		= bpf_mptcp_sched_unreg,
	.check_member	= bpf_mptcp_sched_check_member,
	.init_member	= bpf_mptcp_sched_init_member,
	.init		= bpf_mptcp_sched_init,
	.name		= "mptcp_sched_ops",
};
#endif /* CONFIG_BPF_JIT */

struct mptcp_sock *bpf_mptcp_sock_from_subflow(struct sock *sk)
{
	if (sk && sk_fullsock(sk) && sk->sk_protocol == IPPROTO_TCP && sk_is_mptcp(sk))
		return mptcp_sk(mptcp_subflow_ctx(sk)->conn);

	return NULL;
}

BTF_SET8_START(bpf_mptcp_fmodret_ids)
BTF_ID_FLAGS(func, update_socket_protocol)
BTF_SET8_END(bpf_mptcp_fmodret_ids)

static const struct btf_kfunc_id_set bpf_mptcp_fmodret_set = {
	.owner = THIS_MODULE,
	.set   = &bpf_mptcp_fmodret_ids,
};

BTF_SET8_START(bpf_mptcp_sched_kfunc_ids)
BTF_ID_FLAGS(func, mptcp_subflow_set_scheduled)
BTF_ID_FLAGS(func, mptcp_sched_data_set_contexts)
BTF_ID_FLAGS(func, mptcp_subflow_ctx_by_pos)
BTF_SET8_END(bpf_mptcp_sched_kfunc_ids)

static const struct btf_kfunc_id_set bpf_mptcp_sched_kfunc_set = {
	.owner	= THIS_MODULE,
	.set	= &bpf_mptcp_sched_kfunc_ids,
};

static int __init bpf_mptcp_kfunc_init(void)
{
	int ret;

	ret = register_btf_fmodret_id_set(&bpf_mptcp_fmodret_set);
	return ret ?: register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
						&bpf_mptcp_sched_kfunc_set);
}
late_initcall(bpf_mptcp_kfunc_init);
