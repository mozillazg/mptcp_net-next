// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, SUSE. */

#include <linux/bpf.h>
#include <limits.h>
#include "bpf_tcp_helpers.h"

char _license[] SEC("license") = "GPL";

#define MPTCP_SEND_BURST_SIZE	65428

struct subflow_send_info {
	struct sock *ssk;
	__u64 linger_time;
};

static struct mptcp_subflow_context *
bpf_mptcp_subflow_ctx(struct sock *ssk, struct mptcp_sched_data *data)
{
	int i, nr = 0;

	for (i = 0; i < MPTCP_SUBFLOWS_MAX; i++) {
		if (!ssk || !data->contexts[i])
			break;

		if (mptcp_subflow_tcp_sock(data->contexts[i]) == ssk) {
			nr = i;
			break;
		}
	}

	return data->contexts[nr];
}

static inline __u64 div_u64_rem(__u64 dividend, __u32 divisor, __u32 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}

static inline __u64 div_u64(__u64 dividend, __u32 divisor)
{
	__u32 remainder;

	return div_u64_rem(dividend, divisor, &remainder);
}

extern bool mptcp_subflow_active(struct mptcp_subflow_context *subflow) __ksym;
extern void mptcp_set_timeout(struct sock *sk) __ksym;
extern __u64 mptcp_wnd_end(const struct mptcp_sock *msk) __ksym;
extern bool bpf_sk_stream_memory_free(const struct sock *sk) __ksym;
extern bool bpf_tcp_rtx_and_write_queues_empty(const struct sock *sk) __ksym;
extern void mptcp_pm_subflow_chk_stale(const struct mptcp_sock *msk, struct sock *ssk) __ksym;

#define SSK_MODE_ACTIVE	0
#define SSK_MODE_BACKUP	1
#define SSK_MODE_MAX	2

SEC("struct_ops/mptcp_sched_burst_init")
void BPF_PROG(mptcp_sched_burst_init, const struct mptcp_sock *msk)
{
}

SEC("struct_ops/mptcp_sched_burst_release")
void BPF_PROG(mptcp_sched_burst_release, const struct mptcp_sock *msk)
{
}

void BPF_STRUCT_OPS(bpf_burst_data_init, const struct mptcp_sock *msk,
		    struct mptcp_sched_data *data)
{
	mptcp_sched_data_set_contexts(msk, data);
}

static int bpf_burst_get_send(const struct mptcp_sock *msk,
			      struct mptcp_sched_data *data)
{
	struct subflow_send_info send_info[SSK_MODE_MAX];
	struct mptcp_subflow_context *subflow;
	struct sock *sk = (struct sock *)msk;
	__u32 pace, burst, wmem;
	int i, nr_active = 0;
	__u64 linger_time;
	struct sock *ssk;

	/* pick the subflow with the lower wmem/wspace ratio */
	for (i = 0; i < SSK_MODE_MAX; ++i) {
		send_info[i].ssk = NULL;
		send_info[i].linger_time = -1;
	}

	for (i = 0; i < MPTCP_SUBFLOWS_MAX; i++) {
		if (!data->contexts[i])
			break;

		subflow = data->contexts[i];
		ssk = mptcp_subflow_tcp_sock(subflow);
		if (!mptcp_subflow_active(subflow))
			continue;

		nr_active += !subflow->backup;
		pace = subflow->avg_pacing_rate;
		if (!pace) {
			/* init pacing rate from socket */
			subflow->avg_pacing_rate = ssk->sk_pacing_rate;
			pace = subflow->avg_pacing_rate;
			if (!pace)
				continue;
		}

		linger_time = div_u64((__u64)ssk->sk_wmem_queued << 32, pace);
		if (linger_time < send_info[subflow->backup].linger_time) {
			send_info[subflow->backup].ssk = ssk;
			send_info[subflow->backup].linger_time = linger_time;
		}
	}
	mptcp_set_timeout(sk);

	/* pick the best backup if no other subflow is active */
	if (!nr_active)
		send_info[SSK_MODE_ACTIVE].ssk = send_info[SSK_MODE_BACKUP].ssk;

	/* Get the ssk directly like this "ssk = send_info[SSK_MODE_ACTIVE].ssk;"
	 * will get an error:
	 *   arg#0 pointer type STRUCT sock must point to scalar, or struct with scalar
	 * So here use bpf_mptcp_subflow_ctx() to get the subflow,
	 * then use mptcp_subflow_tcp_sock() to get the ssk.
	 */
	subflow = bpf_mptcp_subflow_ctx(send_info[SSK_MODE_ACTIVE].ssk, data);
	ssk = mptcp_subflow_tcp_sock(subflow);
	if (!ssk || !bpf_sk_stream_memory_free(ssk))
		return -1;

	burst = min(MPTCP_SEND_BURST_SIZE, mptcp_wnd_end(msk) - msk->snd_nxt);
	wmem = ssk->sk_wmem_queued;
	if (!burst)
		goto out;

	subflow->avg_pacing_rate = div_u64((__u64)subflow->avg_pacing_rate * wmem +
					   ssk->sk_pacing_rate * burst,
					   burst + wmem);
	data->snd_burst = burst;

out:
	mptcp_subflow_set_scheduled(subflow, true);
	return 0;
}

static int bpf_burst_get_retrans(const struct mptcp_sock *msk,
				 struct mptcp_sched_data *data)
{
	struct sock *backup = NULL, *pick = NULL;
	struct mptcp_subflow_context *subflow;
	int min_stale_count = INT_MAX;
	struct sock *ssk;
	int i;

	for (i = 0; i < MPTCP_SUBFLOWS_MAX; i++) {
		if (!data->contexts[i])
			break;

		subflow = data->contexts[i];
		ssk = mptcp_subflow_tcp_sock(subflow);
		if (!mptcp_subflow_active(subflow))
			continue;

		/* still data outstanding at TCP level? skip this */
		if (!bpf_tcp_rtx_and_write_queues_empty(ssk)) {
			mptcp_pm_subflow_chk_stale(msk, ssk);
			min_stale_count = min(min_stale_count, subflow->stale_count);
			continue;
		}

		if (subflow->backup) {
			if (!backup)
				backup = ssk;
			continue;
		}

		if (!pick)
			pick = ssk;
	}

	if (pick)
		goto out;
	pick = min_stale_count > 1 ? backup : NULL;

out:
	mptcp_subflow_set_scheduled(bpf_mptcp_subflow_ctx(pick, data), true);
	return 0;
}

int BPF_STRUCT_OPS(bpf_burst_get_subflow, const struct mptcp_sock *msk,
		   struct mptcp_sched_data *data)
{
	if (data->reinject)
		return bpf_burst_get_retrans(msk, data);
	return bpf_burst_get_send(msk, data);
}

SEC(".struct_ops")
struct mptcp_sched_ops burst = {
	.init		= (void *)mptcp_sched_burst_init,
	.release	= (void *)mptcp_sched_burst_release,
	.data_init	= (void *)bpf_burst_data_init,
	.get_subflow	= (void *)bpf_burst_get_subflow,
	.name		= "bpf_burst",
};
