// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022, SUSE. */

#include <linux/bpf.h>
#include "bpf_tcp_helpers.h"

char _license[] SEC("license") = "GPL";

SEC("struct_ops/mptcp_sched_red_init")
void BPF_PROG(mptcp_sched_red_init, struct mptcp_sock *msk)
{
}

SEC("struct_ops/mptcp_sched_red_release")
void BPF_PROG(mptcp_sched_red_release, struct mptcp_sock *msk)
{
}

void BPF_STRUCT_OPS(bpf_red_data_init, struct mptcp_sock *msk,
		    struct mptcp_sched_data *data)
{
	mptcp_sched_data_set_contexts(msk, data);
}

int BPF_STRUCT_OPS(bpf_red_get_subflow, struct mptcp_sock *msk,
		   const struct mptcp_sched_data *data)
{
	for (int i = 0; i < data->subflows && i < MPTCP_SUBFLOWS_MAX; i++) {
		if (!mptcp_subflow_ctx_by_pos(data, i))
			break;

		mptcp_subflow_set_scheduled(mptcp_subflow_ctx_by_pos(data, i), true);
	}

	return 0;
}

SEC(".struct_ops")
struct mptcp_sched_ops red = {
	.init		= (void *)mptcp_sched_red_init,
	.release	= (void *)mptcp_sched_red_release,
	.data_init	= (void *)bpf_red_data_init,
	.get_subflow	= (void *)bpf_red_get_subflow,
	.name		= "bpf_red",
};
