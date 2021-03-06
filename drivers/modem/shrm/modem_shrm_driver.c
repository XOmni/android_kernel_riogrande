/*
 * Copyright (C) ST-Ericsson SA 2010
 *
 * Author: Biju Das <biju.das@stericsson.com> for ST-Ericsson
 * Author: Kumar Sanghvi <kumar.sanghvi@stericsson.com> for ST-Ericsson
 * Author: Arun Murthy <arun.murthy@stericsson.com> for ST-Ericsson
 * License terms: GNU General Public License (GPL) version 2
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <asm/atomic.h>
#include <linux/io.h>
#include <linux/skbuff.h>
#ifdef CONFIG_HIGH_RES_TIMERS
#include <linux/hrtimer.h>
static struct hrtimer timer;
#endif
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/phonet.h>
#include <linux/modem/shrm/shrm_driver.h>
#include <linux/modem/shrm/shrm_private.h>
#include <linux/modem/shrm/shrm_config.h>
#include <linux/modem/shrm/shrm_net.h>
#include <linux/modem/shrm/shrm.h>

#include <mach/isa_ioctl.h>
/* debug functionality */
#define ISA_DEBUG 0

/* count for no of msg to be check befor suspend */
#define CHK_SLP_MSG_CNT 3

#define PHONET_TASKLET
#define MAX_RCV_LEN	2048

static void do_phonet_rcv_tasklet(unsigned long unused);
struct tasklet_struct phonet_rcv_tasklet;

/**
 * audio_receive() - Receive audio channel completion callback
 * @shrm:	pointer to shrm device information structure
 * @data:	message pointer
 * @n_bytes:	message size
 * @l2_header:	L2 header/device ID 2->audio, 5->audio_loopback
 *
 * This fucntion is called from the audio receive handler. Copies the audio
 * message from the FIFO to the AUDIO queue. The message is later copied from
 * this queue to the user buffer through the char or net interface read
 * operation.
 */
static int audio_receive(struct shrm_dev *shrm, void *data,
					u32 n_bytes, u8 l2_header)
{
	u32 size = 0;
	int ret = 0;
	int idx;
	u8 *psrc;
	struct message_queue *q;
	struct isadev_context *audiodev;

	dev_dbg(shrm->dev, "%s IN\n", __func__);
	idx = shrm_get_cdev_index(l2_header);
	if (idx < 0) {
		dev_err(shrm->dev, "failed to get index\n");
		return idx;
	}
	audiodev = &shrm->isa_context->isadev[idx];
	q = &audiodev->dl_queue;
	spin_lock(&q->update_lock);
	/* Memcopy RX data first */
	if ((q->writeptr+n_bytes) >= q->size) {
		psrc = (u8 *)data;
		size = (q->size-q->writeptr);
		/* Copy First Part of msg */
		memcpy((q->fifo_base+q->writeptr), psrc, size);
		psrc += size;
		/* Copy Second Part of msg at the top of fifo */
		memcpy(q->fifo_base, psrc, (n_bytes-size));
	} else {
		memcpy((q->fifo_base+q->writeptr), data, n_bytes);
	}
	ret = add_msg_to_queue(q, n_bytes);
	spin_unlock(&q->update_lock);
	if (ret < 0)
		dev_err(shrm->dev, "Adding a msg to message queue failed");
	dev_dbg(shrm->dev, "%s OUT\n", __func__);
	return ret;
}

/**
 * common_receive() - Receive common channel completion callback
 * @shrm:	pointer to the shrm device information structure
 * @data:	message pointer
 * @n_bytes:	message size
 * @l2_header:	L2 header / device ID
 *
 * This function is called from the receive handler to copy the respective
 * ISI, RPC, SECURITY message to its respective queue. The message is then
 * copied from queue to the user buffer on char net interface read operation.
 */
static int common_receive(struct shrm_dev *shrm, void *data,
					u32 n_bytes, u8 l2_header)
{
	u32 size = 0;
	int ret = 0;
	int idx;
	u8 *psrc;
	struct message_queue *q;
	struct isadev_context *isa_dev;

	dev_dbg(shrm->dev, "%s IN\n", __func__);
	idx = shrm_get_cdev_index(l2_header);
	if (idx < 0) {
		dev_err(shrm->dev, "failed to get index\n");
		return idx;
	}
	isa_dev = &shrm->isa_context->isadev[idx];
	q = &isa_dev->dl_queue;
	spin_lock(&q->update_lock);
	/* Memcopy RX data first */
	if ((q->writeptr+n_bytes) >= q->size) {
		dev_dbg(shrm->dev, "Inside Loop Back\n");
		psrc = (u8 *)data;
		size = (q->size-q->writeptr);
		/* Copy First Part of msg */
		memcpy((q->fifo_base+q->writeptr), psrc, size);
		psrc += size;
		/* Copy Second Part of msg at the top of fifo */
		memcpy(q->fifo_base, psrc, (n_bytes-size));
	} else {
		memcpy((q->fifo_base+q->writeptr), data, n_bytes);
	}
	ret = add_msg_to_queue(q, n_bytes);
	spin_unlock(&q->update_lock);
	if (ret < 0) {
		dev_err(shrm->dev, "Adding a msg to message queue failed");
		return ret;
	}


	if (l2_header == ISI_MESSAGING) {
		if (shrm->netdev_flag_up) {
			dev_dbg(shrm->dev,
				"scheduling the phonet tasklet from %s!\n",
				__func__);
			tasklet_schedule(&phonet_rcv_tasklet);
		}
		dev_dbg(shrm->dev,
				"Out of phonet tasklet %s!!!\n", __func__);
	}
	dev_dbg(shrm->dev, "%s OUT\n", __func__);
	return ret;
}

/**
 * rx_common_l2msg_handler() - common channel receive handler
 * @l2_header:		L2 header
 * @msg:		pointer to the receive buffer
 * @length:		length of the msg to read
 * @shrm:		pointer to shrm device information structure
 *
 * This function is called to receive the message from CaMsgPendingNotification
 * interrupt handler.
 */
static void rx_common_l2msg_handler(u8 l2_header,
				 void *msg, u32 length,
				 struct shrm_dev *shrm)
{
	int ret = 0;
	dev_dbg(shrm->dev, "%s IN\n", __func__);

	ret = common_receive(shrm, msg, length, l2_header);
	if (ret < 0)
		dev_err(shrm->dev,
			"common receive with l2 header %d failed\n", l2_header);

	dev_dbg(shrm->dev, "%s OUT\n", __func__);
}

/**
 * rx_audio_l2msg_handler() - audio channel receive handler
 * @l2_header:		L2 header
 * @msg:		pointer to the receive buffer
 * @length:		length of the msg to read
 * @shrm:		pointer to shrm device information structure
 *
 * This function is called to receive the message from CaMsgPendingNotification
 * interrupt handler.
 */
static void rx_audio_l2msg_handler(u8 l2_header,
				void *msg, u32 length,
				struct shrm_dev *shrm)
{
	int ret = 0;

	dev_dbg(shrm->dev, "%s IN\n", __func__);
	ret = audio_receive(shrm, msg, length, l2_header);
	if (ret < 0)
		dev_err(shrm->dev, "audio receive failed\n");
	dev_dbg(shrm->dev, "%s OUT\n", __func__);
}

static int __init shm_initialise_irq(struct shrm_dev *shrm)
{
	int err = 0;

	err  = shrm_protocol_init(shrm,
			rx_common_l2msg_handler, rx_audio_l2msg_handler);
	if (err < 0) {
		dev_err(shrm->dev, "SHM Protocol Init Failure\n");
		return err;
	}

	err = request_irq(shrm->ca_wake_irq,
			ca_wake_irq_handler, IRQF_TRIGGER_RISING,
				 "ca_wake-up", shrm);
	if (err < 0) {
		dev_err(shrm->dev,
				"Unable to allocate shm tx interrupt line\n");
		free_irq(shrm->ca_wake_irq, shrm);
		return err;
	}

	err = request_irq(shrm->ac_read_notif_0_irq,
		ac_read_notif_0_irq_handler, 0,
		"ac_read_notif_0", shrm);

	if (err < 0) {
		dev_err(shrm->dev,
				"error ac_read_notif_0_irq interrupt line\n");
		goto irq_err1;
	}

	err = request_irq(shrm->ac_read_notif_1_irq,
		ac_read_notif_1_irq_handler, 0,
		"ac_read_notif_1", shrm);

	if (err < 0) {
		dev_err(shrm->dev,
				"error ac_read_notif_1_irq interrupt line\n");
		goto irq_err2;
	}

	err = request_irq(shrm->ca_msg_pending_notif_0_irq,
		 ca_msg_pending_notif_0_irq_handler, 0,
		"ca_msg_pending_notif_0", shrm);

	if (err < 0) {
		dev_err(shrm->dev,
				"error ca_msg_pending_notif_0_irq line\n");
		goto irq_err3;
	}

	err = request_irq(shrm->ca_msg_pending_notif_1_irq,
		 ca_msg_pending_notif_1_irq_handler, 0,
		"ca_msg_pending_notif_1", shrm);

	if (err < 0) {
		dev_err(shrm->dev,
			"error ca_msg_pending_notif_1_irq interrupt line\n");
		goto irq_err4;
	}
	return err;
irq_err4:
	free_irq(shrm->ca_msg_pending_notif_0_irq, shrm);
irq_err3:
	free_irq(shrm->ac_read_notif_1_irq, shrm);
irq_err2:
	free_irq(shrm->ac_read_notif_0_irq, shrm);
irq_err1:
	free_irq(shrm->ca_wake_irq, shrm);
	return err;
}

static void free_shm_irq(struct shrm_dev *shrm)
{
	free_irq(shrm->ca_wake_irq, shrm);
	free_irq(shrm->ac_read_notif_0_irq, shrm);
	free_irq(shrm->ac_read_notif_1_irq, shrm);
	free_irq(shrm->ca_msg_pending_notif_0_irq, shrm);
	free_irq(shrm->ca_msg_pending_notif_1_irq, shrm);
}



#ifdef CONFIG_HIGH_RES_TIMERS
static enum hrtimer_restart callback(struct hrtimer *timer)
{
	return HRTIMER_NORESTART;
}
#endif

void do_phonet_rcv_tasklet(unsigned long unused)
{
	ssize_t ret;
	struct shrm_dev *shrm = (struct shrm_dev *)unused;

	dev_dbg(shrm->dev, "%s IN\n", __func__);
	for (;;) {
		ret = shrm_net_receive(shrm->ndev);
		if (ret == 0) {
			dev_dbg(shrm->dev, "len is zero, queue empty\n");
			break;
		}
		if (ret < 0) {
			dev_err(shrm->dev, "len < 0 !!! error!!!\n");
			break;
		}
	}
	dev_dbg(shrm->dev, "%s OUT\n", __func__);
}

static int __init shrm_probe(struct platform_device *pdev)
{
	int err = 0;
	struct resource *res;
	struct shrm_dev *shrm = NULL;

	shrm = kzalloc(sizeof(struct shrm_dev), GFP_KERNEL);
	if (shrm == NULL) {
		dev_err(&pdev->dev,
			"Could not allocate memory for struct shm_dev\n");
		return -ENOMEM;
	}

	shrm->dev = &pdev->dev;
	shrm->modem = modem_get(shrm->dev, "u8500-shrm-modem");
	if (shrm->modem == NULL) {
		dev_err(shrm->dev, " Could not retrieve the modem.\n");
		err = -ENODEV;
		goto rollback_intr;
	}

	/* initialise the SHM */
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		dev_err(shrm->dev,
				"Unable to map Ca Wake up interrupt\n");
		err = -EBUSY;
		goto rollback_intr;
	}
	shrm->ca_wake_irq = res->start;
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 1);

	if (!res) {
		dev_err(shrm->dev,
			"Unable to map APE_Read_notif_common IRQ base\n");
		err = -EBUSY;
		goto rollback_intr;
	}
	shrm->ac_read_notif_0_irq = res->start;
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 2);

	if (!res) {
		dev_err(shrm->dev,
			"Unable to map APE_Read_notif_audio IRQ base\n");
		err = -EBUSY;
		goto rollback_intr;
	}
	shrm->ac_read_notif_1_irq = res->start;
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 3);

	if (!res) {
		dev_err(shrm->dev,
			"Unable to map Cmt_msg_pending_notif_common IRQbase\n");
		err = -EBUSY;
		goto rollback_intr;
	}
	shrm->ca_msg_pending_notif_0_irq = res->start;
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 4);

	if (!res) {
		dev_err(shrm->dev,
			"Unable to map Cmt_msg_pending_notif_audio IRQ base\n");
		err = -EBUSY;
		goto rollback_intr;
	}
	shrm->ca_msg_pending_notif_1_irq = res->start;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (!res) {
		dev_err(shrm->dev,
				"Could not get SHM IO memory information\n");
		err = -ENODEV;
		goto rollback_intr;
	}
	shrm->intr_base = (void __iomem *)ioremap_nocache(res->start,
					res->end - res->start + 1);
	if (!(shrm->intr_base)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_intr;
	}
	shrm->ape_common_fifo_base_phy =
			(u32 *)U8500_SHM_FIFO_APE_COMMON_BASE;
	shrm->ape_common_fifo_base =
		(void __iomem *)ioremap_nocache(
					U8500_SHM_FIFO_APE_COMMON_BASE,
					SHM_FIFO_0_SIZE);
	shrm->ape_common_fifo_size = (SHM_FIFO_0_SIZE)/4;

	if (!(shrm->ape_common_fifo_base)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_ape_common_fifo_base;
	}
	shrm->cmt_common_fifo_base_phy =
		(u32 *)U8500_SHM_FIFO_CMT_COMMON_BASE;
	shrm->cmt_common_fifo_base =
		(void __iomem *)ioremap_nocache(
			U8500_SHM_FIFO_CMT_COMMON_BASE, SHM_FIFO_0_SIZE);
	shrm->cmt_common_fifo_size = (SHM_FIFO_0_SIZE)/4;

	if (!(shrm->cmt_common_fifo_base)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_cmt_common_fifo_base;
	}
	shrm->ape_audio_fifo_base_phy =
			(u32 *)U8500_SHM_FIFO_APE_AUDIO_BASE;
	shrm->ape_audio_fifo_base =
		(void __iomem *)ioremap_nocache(U8500_SHM_FIFO_APE_AUDIO_BASE,
							SHM_FIFO_1_SIZE);
	shrm->ape_audio_fifo_size = (SHM_FIFO_1_SIZE)/4;

	if (!(shrm->ape_audio_fifo_base)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_ape_audio_fifo_base;
	}
	shrm->cmt_audio_fifo_base_phy =
			(u32 *)U8500_SHM_FIFO_CMT_AUDIO_BASE;
	shrm->cmt_audio_fifo_base =
		(void __iomem *)ioremap_nocache(U8500_SHM_FIFO_CMT_AUDIO_BASE,
							SHM_FIFO_1_SIZE);
	shrm->cmt_audio_fifo_size = (SHM_FIFO_1_SIZE)/4;

	if (!(shrm->cmt_audio_fifo_base)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_cmt_audio_fifo_base;
	}
	shrm->ac_common_shared_wptr =
		(void __iomem *)ioremap(SHM_ACFIFO_0_WRITE_AMCU, SHM_PTR_SIZE);

	if (!(shrm->ac_common_shared_wptr)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_ac_common_shared_wptr;
	}
	shrm->ac_common_shared_rptr =
		(void __iomem *)ioremap(SHM_ACFIFO_0_READ_AMCU, SHM_PTR_SIZE);

	if (!(shrm->ac_common_shared_rptr)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_map;
	}
	shrm->ca_common_shared_wptr =
		(void __iomem *)ioremap(SHM_CAFIFO_0_WRITE_AMCU, SHM_PTR_SIZE);

	if (!(shrm->ca_common_shared_wptr)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_map;
	}
	shrm->ca_common_shared_rptr =
		(void __iomem *)ioremap(SHM_CAFIFO_0_READ_AMCU, SHM_PTR_SIZE);

	if (!(shrm->ca_common_shared_rptr)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_map;
	}
	shrm->ac_audio_shared_wptr =
		(void __iomem *)ioremap(SHM_ACFIFO_1_WRITE_AMCU, SHM_PTR_SIZE);

	if (!(shrm->ac_audio_shared_wptr)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_map;
	}
	shrm->ac_audio_shared_rptr =
		(void __iomem *)ioremap(SHM_ACFIFO_1_READ_AMCU, SHM_PTR_SIZE);

	if (!(shrm->ac_audio_shared_rptr)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_map;
	}
	shrm->ca_audio_shared_wptr =
		(void __iomem *)ioremap(SHM_CAFIFO_1_WRITE_AMCU, SHM_PTR_SIZE);

	if (!(shrm->ca_audio_shared_wptr)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_map;
	}
	shrm->ca_audio_shared_rptr =
		(void __iomem *)ioremap(SHM_CAFIFO_1_READ_AMCU, SHM_PTR_SIZE);

	if (!(shrm->ca_audio_shared_rptr)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_map;
	}
	shrm->ca_reset_status_rptr =
		(void __iomem *)ioremap(SHM_CA_MOD_RESET_STATUS_AMCU, SHM_PTR_SIZE);
	if (!(shrm->ca_reset_status_rptr)) {
		dev_err(shrm->dev, "Unable to map register base\n");
		err = -EBUSY;
		goto rollback_map;
	}

	if (isa_init(shrm) != 0) {
		dev_err(shrm->dev, "Driver Initialization Error\n");
		err = -EBUSY;
	}
#ifdef CONFIG_HIGH_RES_TIMERS
	hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer.function = callback;
	hrtimer_start(&timer, ktime_set(0, 2*NSEC_PER_MSEC), HRTIMER_MODE_REL);
#endif
	err = shrm_register_netdev(shrm);
	if (err < 0)
		goto rollback_map;

	tasklet_init(&phonet_rcv_tasklet, do_phonet_rcv_tasklet, 0);
	phonet_rcv_tasklet.data = (unsigned long)shrm;

	/* request sysclk which is required for modem */
	shrm->clk  = clk_get(shrm->dev, "sysclk");
	if (IS_ERR(shrm->clk)) {
		dev_err(shrm->dev, "failed to request sysclk\n");
		err = PTR_ERR(shrm->clk);
		shrm->clk = NULL;
		goto rollback_map;
	}

	/* install handlers and tasklets */
	if (shm_initialise_irq(shrm)) {
		dev_err(shrm->dev,
				"shm error in interrupt registration\n");
		goto rollback_irq;
	}
	platform_set_drvdata(pdev, shrm);

	return err;
rollback_irq:
	clk_put(shrm->clk);
	free_shm_irq(shrm);
rollback_map:
	iounmap(shrm->ac_common_shared_wptr);
	iounmap(shrm->ac_common_shared_rptr);
	iounmap(shrm->ca_common_shared_wptr);
	iounmap(shrm->ca_common_shared_rptr);
	iounmap(shrm->ac_audio_shared_wptr);
	iounmap(shrm->ac_audio_shared_rptr);
	iounmap(shrm->ca_audio_shared_wptr);
	iounmap(shrm->ca_audio_shared_rptr);
rollback_ac_common_shared_wptr:
	iounmap(shrm->cmt_audio_fifo_base);
rollback_cmt_audio_fifo_base:
	iounmap(shrm->ape_audio_fifo_base);
rollback_ape_audio_fifo_base:
	iounmap(shrm->cmt_common_fifo_base);
rollback_cmt_common_fifo_base:
	iounmap(shrm->ape_common_fifo_base);
rollback_ape_common_fifo_base:
	iounmap(shrm->intr_base);
rollback_intr:
	kfree(shrm);
	return err;
}

static int __exit shrm_remove(struct platform_device *pdev)
{
	struct shrm_dev *shrm = platform_get_drvdata(pdev);

	free_shm_irq(shrm);
	iounmap(shrm->intr_base);
	iounmap(shrm->ape_common_fifo_base);
	iounmap(shrm->cmt_common_fifo_base);
	iounmap(shrm->ape_audio_fifo_base);
	iounmap(shrm->cmt_audio_fifo_base);
	iounmap(shrm->ac_common_shared_wptr);
	iounmap(shrm->ac_common_shared_rptr);
	iounmap(shrm->ca_common_shared_wptr);
	iounmap(shrm->ca_common_shared_rptr);
	iounmap(shrm->ac_audio_shared_wptr);
	iounmap(shrm->ac_audio_shared_rptr);
	iounmap(shrm->ca_audio_shared_wptr);
	iounmap(shrm->ca_audio_shared_rptr);
	shrm_unregister_netdev(shrm);
	isa_exit(shrm);
	clk_put(shrm->clk);
	kfree(shrm);

	return 0;
}

static int check_shrm_unread_msg(struct shrm_dev *shrm)
{
	struct message_queue *q;
	struct isadev_context *isa_dev;
	int idx;
	u8 cnt;

	struct sleep_msg_list {
		u8 l2_header;
		char *name;
	};

	/* list of messages or l2 header to be check before going susped */
	struct sleep_msg_list slp_msg[] = {
		{RPC_MESSAGING, "RPC"},
		{SECURITY_MESSAGING, "Security"},
		{ISI_MESSAGING, "ISI"},
	};

	for (cnt = 0; cnt < CHK_SLP_MSG_CNT; cnt++) {
		idx = shrm_get_cdev_index(slp_msg[cnt].l2_header);
		isa_dev = &shrm->isa_context->isadev[idx];
		q = &isa_dev->dl_queue;
		if (!list_empty(&q->msg_list)) {

			if (atomic_dec_and_test(&shrm->isa_context->is_open[idx])) {
				atomic_inc(&shrm->isa_context->is_open[idx]);
				dev_info(shrm->dev, "%s device not opened yet, flush queue\n",
					slp_msg[cnt].name);
				shrm_char_reset_queues(shrm);
			} else {
				atomic_inc(&shrm->isa_context->is_open[idx]);
				dev_info(shrm->dev, "Some %s msg unread = %d\n",
					slp_msg[cnt].name, get_size_of_new_msg(q));
				return -EBUSY;
			}
		}
	}

	return 0;
}

#ifdef CONFIG_PM
/**
 * u8500_shrm_suspend() - This routine puts the SHRM in to sustend state.
 * @dev:	pointer to device structure.
 *
 * This routine checks the current ongoing communication with Modem by
 * examining the ca_wake state and prevents suspend if modem communication
 * is on-going.
 * If ca_wake = 1 (high), modem comm. is on-going; don't suspend
 * If ca_wake = 0 (low), no comm. with modem on-going.Allow suspend
 */
int u8500_shrm_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct shrm_dev *shrm = platform_get_drvdata(pdev);
	int err;

	dev_dbg(&pdev->dev, "%s called...\n", __func__);
	dev_dbg(&pdev->dev, "ca_wake_req_state = %x\n",
						get_ca_wake_req_state());

	/*
	* Is there are any messages unread in the RPC or Security queue,
	* dont suspend as these are real time and modem expects response
	* within 1sec else will end up in a crash. If userspace doesn't
	* open the device, then will flush the queue and allow device go to suspend
	*/

	if (check_shrm_unread_msg(shrm))
		return -EBUSY;

	/* if ca_wake_req is high, prevent system suspend */
	if (!get_ca_wake_req_state()) {
		err = shrm_suspend_netdev(shrm->ndev);
		return err;
	} else
		return -EBUSY;
}

/**
 * u8500_shrm_resume() - This routine resumes the SHRM from suspend state.
 * @dev:	pointer to device structure
 *
 * This routine restore back the current state of the SHRM
 */
int u8500_shrm_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct shrm_dev *shrm = platform_get_drvdata(pdev);
	int err;

	dev_dbg(&pdev->dev, "%s called...\n", __func__);
	err = shrm_resume_netdev(shrm->ndev);

	return err;
}

static const struct dev_pm_ops shrm_dev_pm_ops = {
	.suspend_noirq = u8500_shrm_suspend,
	.resume_noirq = u8500_shrm_resume,
};
#endif

static struct platform_driver shrm_driver = {
	.remove = __exit_p(shrm_remove),
	.driver = {
		.name = "u8500_shrm",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &shrm_dev_pm_ops,
#endif
	},
};

static int __init shrm_driver_init(void)
{
	return platform_driver_probe(&shrm_driver, shrm_probe);
}

static void __exit shrm_driver_exit(void)
{
	platform_driver_unregister(&shrm_driver);
}

module_init(shrm_driver_init);
module_exit(shrm_driver_exit);

MODULE_AUTHOR("Biju Das, Kumar Sanghvi, Arun Murthy");
MODULE_DESCRIPTION("Shared Memory Modem Driver Interface");
MODULE_LICENSE("GPL v2");
