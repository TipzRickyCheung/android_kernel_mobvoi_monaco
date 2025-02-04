// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include "hab.h"
#include "hab_grantable.h"

static int hab_rx_queue_empty(struct virtual_channel *vchan)
{
	int ret = 0;
	int irqs_disabled = irqs_disabled();

	hab_spin_lock(&vchan->rx_lock, irqs_disabled);
	ret = list_empty(&vchan->rx_list);
	hab_spin_unlock(&vchan->rx_lock, irqs_disabled);

	return ret;
}

static struct hab_message*
hab_scatter_msg_alloc(struct physical_channel *pchan, size_t sizebytes)
{
	struct hab_message *message = NULL;
	int i = 0;
	int allocated = 0;
	bool failed = false;
	void **scatter_buf = NULL;
	uint32_t total_num, page_num = 0U;

	/* The scatter routine is only for the message larger than one page size */
	if (sizebytes <= PAGE_SIZE)
		return NULL;

	page_num = sizebytes >> PAGE_SHIFT;
	total_num = (sizebytes % PAGE_SIZE == 0) ? page_num : (page_num + 1);
	message = kzalloc(sizeof(struct hab_message)
		+ (total_num * sizeof(void *)), GFP_ATOMIC);
	if (!message)
		return NULL;
	message->scatter = true;
	scatter_buf = (void **)message->data;

	/*
	 * All recv buffers need to be prepared before actual recv.
	 * If instant recving is performed when each page is allocated,
	 * we cannot ensure the success of the next allocation.
	 * Part of the message will stuck in the channel if allocation
	 * failed half way.
	 */
	for (i = 0; i < page_num; i++) {
		scatter_buf[i] = kzalloc(PAGE_SIZE, GFP_ATOMIC);
		if (scatter_buf[i] == NULL) {
			failed = true;
			allocated = i;
			break;
		}
	}
	if ((!failed) && (sizebytes % PAGE_SIZE != 0)) {
		scatter_buf[i] = kzalloc(sizebytes % PAGE_SIZE, GFP_ATOMIC);
		if (scatter_buf[i] == NULL) {
			failed = true;
			allocated = i;
		}
	}

	if (!failed) {
		for (i = 0; i < sizebytes / PAGE_SIZE; i++)
			message->sizebytes += physical_channel_read(pchan,
				scatter_buf[i], PAGE_SIZE);
		if (sizebytes % PAGE_SIZE)
			message->sizebytes += physical_channel_read(pchan,
				scatter_buf[i], sizebytes % PAGE_SIZE);
		message->sequence_rx = pchan->sequence_rx;
	} else {
		for (i = 0; i < allocated; i++)
			kfree(scatter_buf[i]);
		kfree(message);
		message = NULL;
	}

	return message;
}

static struct hab_message*
hab_msg_alloc(struct physical_channel *pchan, size_t sizebytes)
{
	struct hab_message *message;

	if (sizebytes > HAB_HEADER_SIZE_MAX) {
		pr_err("pchan %s send size too large %zd\n",
			pchan->name, sizebytes);
		return NULL;
	}

	message = kzalloc(sizeof(*message) + sizebytes, GFP_ATOMIC);
	if (!message)
		/*
		 * big buffer allocation may fail when memory fragment.
		 * Instead of one big consecutive kmem, try alloc one page at a time
		 */
		message = hab_scatter_msg_alloc(pchan, sizebytes);
	else {
		message->sizebytes =
			physical_channel_read(pchan, message->data, sizebytes);

		message->sequence_rx = pchan->sequence_rx;
	}

	return message;
}

void hab_msg_free(struct hab_message *message)
{
	int i = 0;
	uint32_t page_num = 0U;
	void **scatter_buf = NULL;

	if (unlikely(message->scatter)) {
		scatter_buf = (void **)message->data;
		page_num = message->sizebytes >> PAGE_SHIFT;

		if (message->sizebytes % PAGE_SIZE)
			page_num++;

		for (i = 0; i < page_num; i++)
			kfree(scatter_buf[i]);
	}

	kfree(message);
}

int
hab_msg_dequeue(struct virtual_channel *vchan, struct hab_message **msg,
		int *rsize, unsigned int flags)
{
	struct hab_message *message = NULL;
	int ret = 0;
	int wait = !(flags & HABMM_SOCKET_RECV_FLAGS_NON_BLOCKING);
	int interruptible = !(flags & HABMM_SOCKET_RECV_FLAGS_UNINTERRUPTIBLE);
	int irqs_disabled = irqs_disabled();

	if (wait) {
		if (hab_rx_queue_empty(vchan)) {
			if (interruptible)
				ret = wait_event_interruptible(vchan->rx_queue,
					!hab_rx_queue_empty(vchan) ||
					vchan->otherend_closed);
			else
				wait_event(vchan->rx_queue,
					!hab_rx_queue_empty(vchan) ||
					vchan->otherend_closed);
		}
	}

	/*
	 * return all the received messages before the remote close,
	 * and need empty check again in case the list is empty now due to
	 * dequeue by other threads
	 */
	hab_spin_lock(&vchan->rx_lock, irqs_disabled);

	if ((!ret || (ret == -ERESTARTSYS)) && !list_empty(&vchan->rx_list)) {
		message = list_first_entry(&vchan->rx_list,
				struct hab_message, node);
		if (message) {
			if (*rsize >= message->sizebytes) {
				/* msg can be safely retrieved in full */
				list_del(&message->node);
				ret = 0;
				*rsize = message->sizebytes;
			} else {
				pr_err("vcid %x rcv buf too small %d < %zd\n",
					   vchan->id, *rsize,
					   message->sizebytes);
				*rsize = message->sizebytes;
				message = NULL;
				ret = -EOVERFLOW; /* come back again */
			}
		}
	} else
		/* no message received, retain the original status */
		*rsize = 0;

	hab_spin_unlock(&vchan->rx_lock, irqs_disabled);

	*msg = message;
	return ret;
}

static void hab_msg_queue(struct virtual_channel *vchan,
					struct hab_message *message)
{
	int irqs_disabled = irqs_disabled();

	hab_spin_lock(&vchan->rx_lock, irqs_disabled);
	list_add_tail(&message->node, &vchan->rx_list);
	hab_spin_unlock(&vchan->rx_lock, irqs_disabled);

	wake_up(&vchan->rx_queue);
}

static int hab_export_enqueue(struct virtual_channel *vchan,
		struct export_desc *exp)
{
	struct uhab_context *ctx = vchan->ctx;
	int irqs_disabled = irqs_disabled();

	hab_spin_lock(&ctx->imp_lock, irqs_disabled);
	list_add_tail(&exp->node, &ctx->imp_whse);
	ctx->import_total++;
	hab_spin_unlock(&ctx->imp_lock, irqs_disabled);

	return 0;
}

static int hab_send_export_ack(struct virtual_channel *vchan,
				struct physical_channel *pchan,
				struct export_desc *exp)
{
	struct hab_export_ack exp_ack = {
		.export_id = exp->export_id,
		.vcid_local = exp->vcid_local,
		.vcid_remote = exp->vcid_remote
	};
	struct hab_header header = HAB_HEADER_INITIALIZER;

	HAB_HEADER_SET_SIZE(header, sizeof(exp_ack));
	HAB_HEADER_SET_TYPE(header, HAB_PAYLOAD_TYPE_EXPORT_ACK);
	HAB_HEADER_SET_ID(header, exp->vcid_local);
	HAB_HEADER_SET_SESSION_ID(header, vchan->session_id);
	return physical_channel_send(pchan, &header, &exp_ack);
}

static int hab_receive_create_export_ack(struct physical_channel *pchan,
		struct uhab_context *ctx, size_t sizebytes)
{
	struct hab_export_ack_recvd *ack_recvd =
		kzalloc(sizeof(*ack_recvd), GFP_ATOMIC);
	int irqs_disabled = irqs_disabled();

	if (!ack_recvd)
		return -ENOMEM;

	if (sizeof(ack_recvd->ack) != sizebytes)
		pr_err("%s exp ack size %zu is not as arrived %zu\n",
			   pchan->name, sizeof(ack_recvd->ack), sizebytes);

	if (sizebytes > sizeof(ack_recvd->ack)) {
		pr_err("pchan %s read size too large %zd %zd\n",
			pchan->name, sizebytes, sizeof(ack_recvd->ack));
		kfree(ack_recvd);
		return -EINVAL;
	}

	/*
	 * If the hab version on remote side is different with local side,
	 * the size of the ack structure may differ. Under this circumstance,
	 * the sizebytes is still trusted. Thus, we need to read it out and
	 * drop the mismatched ack message from channel.
	 * Dropping such message could avoid the [payload][header][payload]
	 * data layout which will make the whole channel unusable.
	 * But for security reason, we cannot perform it when sizebytes is
	 * larger than expected.
	 */
	if (physical_channel_read(pchan,
		&ack_recvd->ack,
		sizebytes) != sizebytes) {
		kfree(ack_recvd);
		return -EIO;
	}

	/* add ack_recvd node into rx queue only if the sizebytes is expected */
	if (sizeof(ack_recvd->ack) == sizebytes) {
		hab_spin_lock(&ctx->expq_lock, irqs_disabled);
		list_add_tail(&ack_recvd->node, &ctx->exp_rxq);
		hab_spin_unlock(&ctx->expq_lock, irqs_disabled);
	} else {
		kfree(ack_recvd);
		return -EINVAL;
	}

	return 0;
}

static void hab_msg_drop(struct physical_channel *pchan, size_t sizebytes)
{
	uint8_t *data = NULL;

	if (sizebytes > HAB_HEADER_SIZE_MAX) {
		pr_err("%s read size too large %zd\n", pchan->name, sizebytes);
		return;
	}

	data = kmalloc(sizebytes, GFP_ATOMIC);
	if (data == NULL)
		return;
	physical_channel_read(pchan, data, sizebytes);
	kfree(data);
}

int hab_msg_recv(struct physical_channel *pchan,
		struct hab_header *header)
{
	int ret = 0;
	struct hab_message *message;
	struct hab_device *dev = pchan->habdev;
	size_t sizebytes = HAB_HEADER_GET_SIZE(*header);
	uint32_t payload_type = HAB_HEADER_GET_TYPE(*header);
	uint32_t vchan_id = HAB_HEADER_GET_ID(*header);
	uint32_t session_id = HAB_HEADER_GET_SESSION_ID(*header);
	struct virtual_channel *vchan = NULL;
	struct export_desc *exp_desc;
	struct timespec64 ts = {0};
	unsigned long long rx_mpm_tv;
	size_t exp_desc_size_expected = 0;
	struct compressed_pfns *pfn_table = NULL;

	/* get the local virtual channel if it isn't an open message */
	if (payload_type != HAB_PAYLOAD_TYPE_INIT &&
		payload_type != HAB_PAYLOAD_TYPE_INIT_ACK &&
		payload_type != HAB_PAYLOAD_TYPE_INIT_DONE &&
		payload_type != HAB_PAYLOAD_TYPE_INIT_CANCEL) {

		/* sanity check the received message */
		if (payload_type >= HAB_PAYLOAD_TYPE_MAX ||
			vchan_id > (HAB_HEADER_ID_MASK >> HAB_HEADER_ID_SHIFT)
			|| !vchan_id ||	!session_id) {
			pr_err("@@ %s Invalid msg type %d vcid %x bytes %zx sn %d\n",
				pchan->name, payload_type,
				vchan_id, sizebytes, session_id);
			dump_hab_wq(pchan);
		}

		/*
		 * need both vcid and session_id to be accurate.
		 * this is from pchan instead of ctx
		 */
		vchan = hab_vchan_get(pchan, header);
		if (!vchan) {
			pr_debug("vchan not found type %d vcid %x sz %zx sesn %d\n",
				payload_type, vchan_id, sizebytes, session_id);

			if (sizebytes) {
				hab_msg_drop(pchan, sizebytes);
				pr_err("%s msg dropped type %d size %d vcid %X session id %d\n",
				pchan->name, payload_type,
				sizebytes, vchan_id,
				session_id);
			}
			return -EINVAL;
		} else if (vchan->otherend_closed) {
			hab_vchan_put(vchan);
			pr_info("vchan remote is closed payload type %d, vchan id %x, sizebytes %zx, session %d\n",
				payload_type, vchan_id,
				sizebytes, session_id);
			if (sizebytes) {
				hab_msg_drop(pchan, sizebytes);
				pr_err("%s message %d dropped remote close, session id %d\n",
				pchan->name, payload_type,
				session_id);
			}
			return -ENODEV;
		}
	} else {
		if (sizebytes != sizeof(struct hab_open_send_data)) {
			pr_err("%s Invalid open req type %d vcid %x bytes %zx session %d\n",
				pchan->name, payload_type, vchan_id,
				sizebytes, session_id);
			if (sizebytes) {
				hab_msg_drop(pchan, sizebytes);
				pr_err("%s msg %d dropped unknown reason session id %d\n",
					pchan->name,
					payload_type,
					session_id);
				dump_hab_wq(pchan);
			}
			return -ENODEV;
		}
	}

	switch (payload_type) {
	case HAB_PAYLOAD_TYPE_MSG:
	case HAB_PAYLOAD_TYPE_SCHE_RESULT_REQ:
	case HAB_PAYLOAD_TYPE_SCHE_RESULT_RSP:
		message = hab_msg_alloc(pchan, sizebytes);
		if (!message)
			break;

		hab_msg_queue(vchan, message);
		break;

	case HAB_PAYLOAD_TYPE_INIT:
	case HAB_PAYLOAD_TYPE_INIT_ACK:
	case HAB_PAYLOAD_TYPE_INIT_DONE:
		ret = hab_open_request_add(pchan, sizebytes, payload_type);
		if (ret) {
			pr_err("%s open request add failed, ret %d, payload type %d, sizebytes %zx\n",
				pchan->name, ret, payload_type, sizebytes);
			break;
		}
		wake_up_interruptible(&dev->openq);
		break;

	case HAB_PAYLOAD_TYPE_INIT_CANCEL:
		pr_info("remote open cancel header vcid %X session %d local %d remote %d\n",
			vchan_id, session_id, pchan->vmid_local,
			pchan->vmid_remote);
		ret = hab_open_receive_cancel(pchan, sizebytes);
		if (ret)
			pr_err("%s open cancel handling failed ret %d vcid %X session %d\n",
				pchan->name, ret, vchan_id, session_id);
		break;

	case HAB_PAYLOAD_TYPE_EXPORT:
		exp_desc_size_expected = sizeof(struct export_desc)
							+ sizeof(struct compressed_pfns);
		if (sizebytes > (size_t)(HAB_HEADER_SIZE_MAX) ||
			sizebytes < exp_desc_size_expected) {
			pr_err("%s exp size too large/small %zu header %zu\n",
				pchan->name, sizebytes, sizeof(*exp_desc));
			break;
		}

		pr_debug("%s exp payload %zu bytes\n",
				pchan->name, sizebytes);

		exp_desc = kzalloc(sizebytes, GFP_ATOMIC);
		if (!exp_desc)
			break;

		if (physical_channel_read(pchan, exp_desc, sizebytes) !=
			sizebytes) {
			pr_err("%s corrupted exp expect %zd bytes vcid %X remote %X open %d!\n",
				pchan->name, sizebytes, vchan->id,
				vchan->otherend_id, vchan->session_id);
			kfree(exp_desc);
			break;
		}

		if (pchan->vmid_local != exp_desc->domid_remote ||
			pchan->vmid_remote != exp_desc->domid_local)
			pr_err("%s corrupted vmid %d != %d %d != %d\n",
			pchan->name, pchan->vmid_local, exp_desc->domid_remote,
			pchan->vmid_remote, exp_desc->domid_local);
		exp_desc->domid_remote = pchan->vmid_remote;
		exp_desc->domid_local = pchan->vmid_local;
		exp_desc->pchan = pchan;

		/*
		 * We should do all the checks here.
		 * But in order to improve performance, we put the
		 * checks related to exp->payload_count and pfn_table->region[i].size
		 * into function pages_list_create. So any potential usage of such data
		 * from the remote side after the checks here and before the checks in
		 * pages_list_create needs to add some more checks if necessary.
		 */
		pfn_table = (struct compressed_pfns *)exp_desc->payload;
		if (pfn_table->nregions <= 0 ||
			(pfn_table->nregions > SIZE_MAX / sizeof(struct region)) ||
			(SIZE_MAX - exp_desc_size_expected <
			pfn_table->nregions * sizeof(struct region))) {
			pr_err("%s nregions is too large or negative, nregions:%d!\n",
					pchan->name, pfn_table->nregions);
			kfree(exp_desc);
			break;
		}

		if (pfn_table->nregions > exp_desc->payload_count) {
			pr_err("%s nregions %d greater than payload_count %d\n",
				pchan->name, pfn_table->nregions, exp_desc->payload_count);
			kfree(exp_desc);
			break;
		}

		if (exp_desc->payload_count > MAX_EXP_PAYLOAD_COUNT) {
			pr_err("payload_count out of range: %d size overflow\n",
				exp_desc->payload_count);
			kfree(exp_desc);
			break;
		}

		exp_desc_size_expected += pfn_table->nregions * sizeof(struct region);
		if (sizebytes != exp_desc_size_expected) {
			pr_err("%s exp size not equal %zu expect %zu\n",
				pchan->name, sizebytes, exp_desc_size_expected);
			kfree(exp_desc);
			break;
		}

		hab_export_enqueue(vchan, exp_desc);
		hab_send_export_ack(vchan, pchan, exp_desc);
		break;

	case HAB_PAYLOAD_TYPE_EXPORT_ACK:
		ret = hab_receive_create_export_ack(pchan, vchan->ctx,
				sizebytes);
		if (ret) {
			pr_err("%s failed to handled export ack %d\n",
				pchan->name, ret);
			break;
		}
		wake_up_interruptible(&vchan->ctx->exp_wq);
		break;

	case HAB_PAYLOAD_TYPE_CLOSE:
		/* remote request close */
		pr_debug("remote close vcid %pK %X other id %X session %d refcnt %d\n",
			vchan, vchan->id, vchan->otherend_id,
			session_id, get_refcnt(vchan->refcount));
		hab_vchan_stop(vchan);
		break;

	case HAB_PAYLOAD_TYPE_PROFILE:
		ktime_get_ts64(&ts);

		if (sizebytes < sizeof(struct habmm_xing_vm_stat)) {
			pr_err("%s expected size greater than %zd at least %zd\n",
				pchan->name, sizebytes, sizeof(struct habmm_xing_vm_stat));
			break;
		}

		/* pull down the incoming data */
		message = hab_msg_alloc(pchan, sizebytes);
		if (!message)
			pr_err("%s failed to allocate msg Arrived msg will be lost\n",
					pchan->name);
		else {
			struct habmm_xing_vm_stat *pstat =
				(struct habmm_xing_vm_stat *)message->data;
			pstat->rx_sec = ts.tv_sec;
			pstat->rx_usec = ts.tv_nsec/NSEC_PER_USEC;
			hab_msg_queue(vchan, message);
		}
		break;

	case HAB_PAYLOAD_TYPE_SCHE_MSG:
	case HAB_PAYLOAD_TYPE_SCHE_MSG_ACK:
		if (sizebytes < sizeof(unsigned long long)) {
			pr_err("%s expected size greater than %zd at least %zd\n",
				pchan->name, sizebytes, sizeof(unsigned long long));
			break;
		}

		rx_mpm_tv = msm_timer_get_sclk_ticks();
		/* pull down the incoming data */
		message = hab_msg_alloc(pchan, sizebytes);
		if (!message)
			pr_err("%s failed to allocate msg Arrived msg will be lost\n",
					pchan->name);
		else {
			((unsigned long long *)message->data)[0] = rx_mpm_tv;
			hab_msg_queue(vchan, message);
		}
		break;

	default:
		pr_err("%s unknown msg received, payload type %d, vchan id %x, sizebytes %zx, session %d\n",
			pchan->name, payload_type, vchan_id,
			sizebytes, session_id);
		break;
	}
	if (vchan)
		hab_vchan_put(vchan);
	return ret;
}
