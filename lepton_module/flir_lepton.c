/* flir_lepton.c
   Main source file for the FLIR Lepton VoSPI driver
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/property.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>
#include <media/videobuf2-memops.h>

#include "flir_lepton.h"
#include "lepton_vospi_funcs.h"

enum lepton_model {
	FLIR_LEPTON2	= 2,
	FLIR_LEPTON3	= 3,
};

struct spare_spi_buffer {
	unsigned	len;
	void		*rx_buf;
};

struct lepton {
	struct mutex mutex;
	spinlock_t lock;
	wait_queue_head_t xfer_wait;
	bool started;
	bool synced;
	bool telemetry_enabled;
	bool transfer_in_flight;
	bool removing;
	struct list_head unfilled_bufs; /* waiting to be filled with data */
	struct spare_spi_buffer spare_buf; /* when not using unfilled_bufs */
	struct v4l2_device *v4l2_dev;
	struct video_device *vid_dev;
	struct vb2_queue *q;
	struct spi_device *spi_dev;
	int irq;
	u64 vsync_count;
	u64 spi_complete_count;
	u64 valid_subframe_count;
	u64 invalid_subframe_count;
	u64 sync_loss_count;
	u64 resync_count;
	int last_spi_status;
	unsigned int discard_count;
	ktime_t resync_resume;
	lepton_vospi_info lep_vospi_info;
	struct lepton_buffer *current_lep_buf;
	struct spi_transfer *spi_xfer;
	struct spi_message *spi_msg;
	struct timespec64 last_spi_done_ts;
};

struct lepton_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

/*
 * interface to userspace apps: V4L2 video device
 */

static int lepton_querycap(struct file *file, void *priv,
			struct v4l2_capability *cap)
{
	strscpy(cap->driver, LEPTON_MODULE_NAME, sizeof(cap->driver));
	strscpy(cap->card, "FLIR Lepton", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", LEPTON_MODULE_NAME);
	return 0;
}

static int lepton_enum_input(struct file *file, void *priv,
			 struct v4l2_input *inp)
{
	if (inp->index > 0)
		return -EINVAL;

	inp->type = V4L2_INPUT_TYPE_CAMERA;
	snprintf(inp->name, sizeof(inp->name), LEPTON_MODULE_NAME);
	inp->capabilities = 0;
	return 0;
}

static int lepton_s_input(struct file *file, void *priv, unsigned int i)
{
	// only 1 input
	if (i != 0)
		return -EINVAL;
	return 0;
}
static int lepton_g_input(struct file *file, void *priv, unsigned int *i)
{
	// only 1 input
	*i = 0;
	return 0;
}
static int lepton_querystd(struct file *file, void *fh, v4l2_std_id *std)
{
	// nothing to say about the video standard
	return -ENODATA;
}
static int lepton_s_std(struct file *file, void *fh, v4l2_std_id std)
{
	return -ENODATA;
}
static int lepton_g_std(struct file *file, void *fh, v4l2_std_id *std)
{
	return -ENODATA;
}
static int lepton_enum_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_Y16;
	return 0;
}
static int lepton_set_fmt_fields(struct lepton *lep, struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = NULL;

	pix = &f->fmt.pix;

	pix->width = LEPTON_SUBFRAME_LINE_WORD_COUNT;
	pix->height = lep->lep_vospi_info.subframe_params.line_count;
	pix->pixelformat = V4L2_PIX_FMT_Y16;  // 16-bit grayscale
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->bytesperline = LEPTON_SUBFRAME_LINE_BYTE_WIDTH;
	pix->sizeimage = lep->lep_vospi_info.subframe_params.subframe_data_byte_size;
	return 0;
}
static int lepton_s_parm(struct file *file, void *priv,
			     struct v4l2_streamparm *parm)
{
	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	parm->parm.capture.timeperframe.numerator = 1;
	parm->parm.capture.timeperframe.denominator = 30;
	parm->parm.capture.readbuffers  = 1;

	return 0;
}
static int lepton_g_parm(struct file *file, void *priv,
			     struct v4l2_streamparm *parm)
{
	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	parm->parm.capture.timeperframe.numerator = 1;
	parm->parm.capture.timeperframe.denominator = 30;
	parm->parm.capture.readbuffers  = 1;
	return 0;
}
static int lepton_g_fmt_vid_cap(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct lepton *lep = NULL;

	lep = video_drvdata(file);

	lepton_set_fmt_fields(lep, f);
	return 0;
}
static int lepton_try_fmt_vid_cap(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct lepton *lep = NULL;

	lep = video_drvdata(file);
	// we only ever have one format, so always send it back.
	lepton_set_fmt_fields(lep, f);
	return 0;
}
static int lepton_s_fmt_vid_cap(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct lepton *lep = NULL;

	lep = video_drvdata(file);
	// we only ever have one format, so always send it back.
	lepton_set_fmt_fields(lep, f);
	return 0;
}
static int lepton_enum_frameintervals(struct file *file, void *priv,
				   struct v4l2_frmivalenum *f)
{
	if (f->index != 0)
		return -EINVAL;
	f->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	// always runs at 30Hz
	f->discrete.numerator = 1;
	f->discrete.denominator = 30;
	return 0;
}
static int lepton_enum_framesizes(struct file *file, void *priv,
			       struct v4l2_frmsizeenum *f)
{
	struct lepton *lep = NULL;

	lep = video_drvdata(file);
	if (f->index != 0)
		return -EINVAL;
	f->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	f->discrete.width = LEPTON_SUBFRAME_LINE_WORD_COUNT;
	f->discrete.height = lep->lep_vospi_info.subframe_params.line_count;
	return 0;
}

static struct v4l2_file_operations lepton_fops = {
	.owner =    THIS_MODULE,
	.open =     v4l2_fh_open,
	.release =  vb2_fop_release,
	.read =     vb2_fop_read,    
	.poll =     vb2_fop_poll,
	.mmap =     vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static const struct v4l2_ioctl_ops lepton_ioctl_ops = {
	.vidioc_s_parm			= lepton_s_parm,
	.vidioc_g_parm			= lepton_g_parm,
	.vidioc_querycap	= lepton_querycap,
	.vidioc_enum_input	= lepton_enum_input,
	.vidioc_g_input		= lepton_g_input,
	.vidioc_s_input		= lepton_s_input,

	.vidioc_querystd	= lepton_querystd,
	.vidioc_g_std		= lepton_g_std,
	.vidioc_s_std		= lepton_s_std,

	.vidioc_enum_fmt_vid_cap = lepton_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap	= lepton_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap	= lepton_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap	= lepton_s_fmt_vid_cap,
	.vidioc_enum_frameintervals	= lepton_enum_frameintervals,
	.vidioc_enum_framesizes		= lepton_enum_framesizes,

	.vidioc_reqbufs		= vb2_ioctl_reqbufs,
	.vidioc_create_bufs	= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf	= vb2_ioctl_prepare_buf,
	.vidioc_querybuf	= vb2_ioctl_querybuf,
	.vidioc_qbuf		= vb2_ioctl_qbuf,
	.vidioc_dqbuf		= vb2_ioctl_dqbuf,
	.vidioc_expbuf		= vb2_ioctl_expbuf,

	.vidioc_streamon	= vb2_ioctl_streamon,
	.vidioc_streamoff	= vb2_ioctl_streamoff,

	.vidioc_log_status	= v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static struct video_device lepton_videodev_template = {
	.name		= LEPTON_MODULE_NAME,
	.fops		= &lepton_fops,
	.ioctl_ops	= &lepton_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
	.device_caps	= V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE |
			  V4L2_CAP_STREAMING,
};

/*
 * video buffer queue management
 */

static int lepton_queue_setup(struct vb2_queue *vq,
			   unsigned int *nbuffers, unsigned int *nplanes,
			   unsigned int sizes[], struct device *alloc_devs[])
{
	struct lepton *lep = NULL;
	unsigned int size = -1;

	lep = vb2_get_drv_priv(vq);
	size = lep->lep_vospi_info.subframe_params.subframe_data_byte_size;
	if (*nplanes)
	{
		// only allow 1 plane per buffer, and verify size is large enough
		if ((*nplanes != 1) || (sizes[0] < size))
			return -EINVAL;
		size = sizes[0];
	}
	if (*nbuffers < 2)
		*nbuffers = 2;
	*nplanes = 1;
	sizes[0] = size;
	pr_debug("get %d buffers, each holding %d bytes.\n", *nbuffers, sizes[0]);
	return 0;
}

static int lepton_buf_prepare(struct vb2_buffer *vb)
{
	struct lepton *lep = NULL;

	lep = vb2_get_drv_priv(vb->vb2_queue);
	if (vb2_plane_size(vb, 0) < lep->lep_vospi_info.subframe_params.subframe_data_byte_size)
	{
		pr_debug("%s: data will not fit into buf size %ld\n", __func__, vb2_plane_size(vb, 0));
		return -EINVAL;
	}

	/* amount of data that will be filled in this buffer,
	 * which will get passed to userspace client in buffer descriptor */
	vb2_set_plane_payload(vb, 0, lep->lep_vospi_info.subframe_params.subframe_data_byte_size);
	return 0;
}

static void lepton_buf_queue(struct vb2_buffer *vb)
{
	struct lepton_buffer *buf =
		container_of(to_vb2_v4l2_buffer(vb), struct lepton_buffer, vb);
	struct lepton *lep = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long flags;

	spin_lock_irqsave(&lep->lock, flags);
	list_add_tail(&buf->list, &lep->unfilled_bufs);
	spin_unlock_irqrestore(&lep->lock, flags);
}

static int lepton_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct lepton *lep = vb2_get_drv_priv(vq);
	unsigned long flags;

	spin_lock_irqsave(&lep->lock, flags);
	lep->started = 1;
	spin_unlock_irqrestore(&lep->lock, flags);
	return 0;
}

static void lepton_stop_streaming(struct vb2_queue *vq)
{
	struct lepton *lep = vb2_get_drv_priv(vq);
	struct lepton_buffer *lep_buf = NULL;
	struct list_head *pos, *q;
	unsigned long flags;

	spin_lock_irqsave(&lep->lock, flags);
	list_for_each_safe(pos, q, &lep->unfilled_bufs) {
		lep_buf = list_entry(pos, struct lepton_buffer, list);
		vb2_buffer_done(&lep_buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		list_del(&lep_buf->list);
	}

	lep->started = 0;
	spin_unlock_irqrestore(&lep->lock, flags);
	vb2_wait_for_all_buffers(lep->q);
}

static const struct vb2_ops lepton_video_qops = {
	.queue_setup     = lepton_queue_setup,
	.buf_prepare     = lepton_buf_prepare,
	.buf_queue       = lepton_buf_queue,
	.start_streaming = lepton_start_streaming,
	.stop_streaming	 = lepton_stop_streaming,
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
};

/*
 * tables to find matching device-tree entries
 *
 * loaded device-tree will be searched for matching "compatible" strings,
 * and driver probe function will be called on any matched nodes
 */

static const struct spi_device_id lepton_id_table[] = {
	{
		.name		= "lepton2",
		.driver_data	= (kernel_ulong_t)FLIR_LEPTON2,
	},
	{
		.name		= "lepton3",
		.driver_data	= (kernel_ulong_t)FLIR_LEPTON3,
	},
	{ }
};
MODULE_DEVICE_TABLE(spi, lepton_id_table);

static const struct of_device_id lepton_of_match[] = {
	{
		.compatible	= "flir,lepton2",
		.data		= (void *)FLIR_LEPTON2,
	},
	{
		.compatible	= "flir,lepton3",
		.data		= (void *)FLIR_LEPTON3,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, lepton_of_match);

/*
 * toplevel module setup and teardown
 */

static void lepton_spi_done_callback(void *context)
{
	struct lepton *lep = (struct lepton *)context;
	unsigned long flags;
	unsigned short *subframe_data = NULL;
	struct lepton_buffer *lep_buf = NULL;
	void *dst = NULL;
	struct timespec64 now;
	bool subframe_is_good = false;
	int status;

	ktime_get_ts64(&now);

	spin_lock_irqsave(&lep->lock, flags);
	status = lep->spi_msg->status;
	lep->last_spi_done_ts.tv_sec = now.tv_sec;
	lep->last_spi_done_ts.tv_nsec = now.tv_nsec;
	lep->last_spi_status = status;
	lep->spi_complete_count++;

	/* current_lep_buf will already be NULL if spare buffer is in use;
	 * non-NULL if a V4L-allocated buffer should receive a copy after
	 * the private SPI capture buffer has been validated.
	 */
	lep_buf = lep->current_lep_buf;
	lep->current_lep_buf = NULL;
	spin_unlock_irqrestore(&lep->lock, flags);

	/* analyze data to decide if data is synced up into proper subframes yet,
	 * so that data can be sent to userspace when V4L buffers are available
	 */
	subframe_data = lep->spare_buf.rx_buf;
	if (!status) {
		subframe_is_good =
			is_subframe_line_counter_valid(&lep->lep_vospi_info, subframe_data) &&
			is_subframe_index_valid(&lep->lep_vospi_info, subframe_data);
	}

	spin_lock_irqsave(&lep->lock, flags);
	if (subframe_is_good) {
		lep->synced = true;
		lep->discard_count = 0;
		lep->valid_subframe_count++;
	}
	else {
		if (lep->synced)
			lep->sync_loss_count++;
		lep->synced = false;
		lep->discard_count++;
		lep->invalid_subframe_count++;
		if (lep->discard_count >= MAX_CONSEC_DISCARD_COUNT) {
			lep->lep_vospi_info.next_subframe_index = 1;
			lep->discard_count = 0;
			lep->resync_count++;
			lep->resync_resume = ktime_add_ms(ktime_get(), 200);
		}
	}

	spin_unlock_irqrestore(&lep->lock, flags);

	/* V4L buffers need to be dispatched back to userspace. The driver keeps
	 * validation counters for diagnostics, but userspace needs raw VoSPI data
	 * to recover subframe ordering after a sync loss.
	 */
	if (lep_buf) {
		dst = vb2_plane_vaddr(&lep_buf->vb.vb2_buf, 0);
		if (!status && dst) {
			memcpy(dst, lep->spare_buf.rx_buf,
			       lep->lep_vospi_info.subframe_params.subframe_data_byte_size);
			vb2_set_plane_payload(&lep_buf->vb.vb2_buf, 0,
				lep->lep_vospi_info.subframe_params.subframe_data_byte_size);
			vb2_buffer_done(&lep_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		}
		else {
			dev_warn_ratelimited(&lep->spi_dev->dev,
					     "failed VoSPI buffer, status=%d dst=%p\n",
					     status, dst);
			vb2_buffer_done(&lep_buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		}
	}

	spin_lock_irqsave(&lep->lock, flags);
	lep->transfer_in_flight = false;
	spin_unlock_irqrestore(&lep->lock, flags);
	wake_up(&lep->xfer_wait);
}

/* kick off a SPI transfer in interrupt context */
static int lepton_start_transfer(struct lepton *lep, size_t rx_len)
{
	unsigned long flags;
	int ret;

	/* SPI message consists of one or more transfers,
	 * in this case only one */

	memset(lep->spare_buf.rx_buf, 0, rx_len);

	spin_lock_irqsave(&lep->lock, flags);
	spi_message_init(lep->spi_msg);
	lep->spi_msg->complete = lepton_spi_done_callback;
	lep->spi_msg->context = (void *)lep;

	memset(lep->spi_xfer, 0, sizeof(*lep->spi_xfer));
	lep->spi_xfer->rx_buf = lep->spare_buf.rx_buf;
	lep->spi_xfer->len = rx_len;
	lep->transfer_in_flight = true;
	lep->last_spi_status = -EINPROGRESS;

	/* assign this one transfer to message and send it to controller */
	spi_message_add_tail(lep->spi_xfer, lep->spi_msg);
	spin_unlock_irqrestore(&lep->lock, flags);
	ret = spi_async(lep->spi_dev, lep->spi_msg);
	if (ret) {
		spin_lock_irqsave(&lep->lock, flags);
		lep->transfer_in_flight = false;
		lep->last_spi_status = ret;
		lep->sync_loss_count++;
		spin_unlock_irqrestore(&lep->lock, flags);
		wake_up(&lep->xfer_wait);
	}

	return ret;
}

static int lepton_timing_ok(struct lepton *lep, struct timespec64 *now)
{
	struct timespec64 delta;
	int timing_ok = 1;

	if (lep->last_spi_done_ts.tv_sec == 0) {
		pr_debug("VSYNC %llu miss!\n", lep->vsync_count);
		timing_ok = 0;
	}
	else {
		delta = timespec64_sub(*now, lep->last_spi_done_ts);
		if (timespec64_to_ns(&delta) < MINIMUM_SPI_TRANSFER_QUIET_TIME) {
			pr_debug("VSYNC warning!\n");
		}
	}
	return timing_ok;
}

static irqreturn_t lepton_vsync_handler(int irq, void *data)
{
	struct spi_device *spi = (struct spi_device *)data;
	struct device *dev = NULL;
	struct lepton *lep = NULL;
	struct lepton_buffer *lep_buf = NULL;
	unsigned long flags;
	int synced = 0;
	unsigned rx_len;
	ktime_t now_ktime;
	struct timespec64 now;
	int ret;

	now_ktime = ktime_get();
	ktime_get_ts64(&now); /* time at beginning of IRQ handler */
	dev = &spi->dev;
	lep = dev_get_drvdata(dev);

#if 0
	if (printk_ratelimit()) {
		pr_debug("VSYNC %llu", lep->vsync_count);
		// printk(KERN_INFO "spi=%p dev=%p lep=%p\n", spi, dev, lep);
	}
#endif

	spin_lock_irqsave(&lep->lock, flags);
	lep->vsync_count++;

	if (lep->removing) {
		spin_unlock_irqrestore(&lep->lock, flags);
		return IRQ_HANDLED;
	}

	/* Do not kick off another DMA if the previous has not
	 * finished (which is a SERIOUS problem, since missing subframes
	 * can knock lepton into a bad state that requires hardware reset 
	 */
	if (lep->transfer_in_flight) {
		lep->sync_loss_count++;
		spin_unlock_irqrestore(&lep->lock, flags);
		return IRQ_HANDLED;
	}

	if (ktime_to_ns(lep->resync_resume) > 0 &&
	    ktime_compare(now_ktime, lep->resync_resume) < 0) {
		spin_unlock_irqrestore(&lep->lock, flags);
		return IRQ_HANDLED;
	}

	if (!lepton_timing_ok(lep, &now)) {
		spin_unlock_irqrestore(&lep->lock, flags);
		return IRQ_HANDLED;
	}

	/* If streaming is active and there are V4L buffers available, attach the
	 * next buffer to this transfer. Validation happens after the SPI transfer,
	 * so this also preserves the first valid subframe after a resync pause.
	 */
	if (lep->started && !list_empty(&lep->unfilled_bufs)) {
		lep_buf = list_first_entry(&lep->unfilled_bufs, struct lepton_buffer, list);
		list_del(&lep_buf->list);
		lep->current_lep_buf = lep_buf;
	}
	synced = lep->synced;  /* cache this for use outside spinlock */
	lep->last_spi_done_ts.tv_sec = 0; /* reset timer for upcoming spi transfer */
	spin_unlock_irqrestore(&lep->lock, flags);

	/* driver provides a spare buffer for two purposes:
	 * - achieving sync of video frames
	 * - place to stash SPI data when no V4L buffers are available
	 *
	 * Achieving sync:
	 *   When video streaming starts up, there can be some extra lines
	 * of data before the real start of frame (marked by line counter=0).
	 * In that case driver will "catch up" by using the spare buffer,
	 * which has one extra line worth of space. Reading out full frame
	 * plus a line will gradually drain out the extra data until a frame 
	 * starts with line 0 at the beginning. On that first synced frame,
	 * the extra transferred line will be a "discard packet" which can 
	 * just be ignored.
	 *
	 * Fallback when no V4L buffers are available:
	 *   After the extra lines have been cleared out, frame-sized
	 * buffers allocated by V4L layer can be used to receive data
	 * that will then be passed to userspace. However, it is up
	 * to userspace to allocate V4L buffers and to keep returning
	 * them to the driver. The lepton is most stable if data
	 * is *always* clocked out in a timely manner, so the spare buffer
	 * is used to clock out data when no V4L buffer is available.
	 */
	if (synced) {
		rx_len = lep->lep_vospi_info.subframe_params.subframe_data_byte_size;
	}
	else {
		/* kick off spi read to spare buffer, which has an extra line */
		rx_len = lep->spare_buf.len;
	}
	ret = lepton_start_transfer(lep, rx_len);
	if (ret && lep_buf) {
		spin_lock_irqsave(&lep->lock, flags);
		lep->current_lep_buf = NULL;
		spin_unlock_irqrestore(&lep->lock, flags);
		vb2_buffer_done(&lep_buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}

	return IRQ_HANDLED;
}

static lepton_version lepton_dt_version(struct spi_device *spi)
{
	const void *match_data;
	const struct spi_device_id *id;
	enum lepton_model model = FLIR_LEPTON2;

	match_data = device_get_match_data(&spi->dev);
	if (match_data) {
		model = (enum lepton_model)(kernel_ulong_t)match_data;
	}
	else {
		id = spi_get_device_id(spi);
		if (id)
			model = (enum lepton_model)id->driver_data;
	}

	return model == FLIR_LEPTON3 ? LEPTON_VERSION_3X : LEPTON_VERSION_2X;
}

#define LEPTON_COUNTER_ATTR(_name, _field, _fmt)			\
static ssize_t _name##_show(struct device *dev,			\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct lepton *lep = dev_get_drvdata(dev);			\
									\
	return sysfs_emit(buf, _fmt "\n", lep->_field);			\
}									\
static DEVICE_ATTR_RO(_name)

LEPTON_COUNTER_ATTR(vsync_count, vsync_count, "%llu");
LEPTON_COUNTER_ATTR(spi_complete_count, spi_complete_count, "%llu");
LEPTON_COUNTER_ATTR(valid_subframe_count, valid_subframe_count, "%llu");
LEPTON_COUNTER_ATTR(invalid_subframe_count, invalid_subframe_count, "%llu");
LEPTON_COUNTER_ATTR(sync_loss_count, sync_loss_count, "%llu");
LEPTON_COUNTER_ATTR(resync_count, resync_count, "%llu");
LEPTON_COUNTER_ATTR(last_spi_status, last_spi_status, "%d");
LEPTON_COUNTER_ATTR(transfer_in_flight, transfer_in_flight, "%d");

static struct attribute *lepton_attrs[] = {
	&dev_attr_vsync_count.attr,
	&dev_attr_spi_complete_count.attr,
	&dev_attr_valid_subframe_count.attr,
	&dev_attr_invalid_subframe_count.attr,
	&dev_attr_sync_loss_count.attr,
	&dev_attr_resync_count.attr,
	&dev_attr_last_spi_status.attr,
	&dev_attr_transfer_in_flight.attr,
	NULL,
};

static const struct attribute_group lepton_attr_group = {
	.attrs = lepton_attrs,
};

static int lepton_probe(struct spi_device *spi)
{
	struct lepton *lep = NULL;
	struct device *dev = NULL;
	struct device_node *of_node = NULL;
	struct v4l2_device *v4l2_dev = NULL;
	struct video_device *vid_dev = NULL;
	struct vb2_queue *q = NULL;
	struct spi_transfer *spi_xfer = NULL;
	struct spi_message *spi_msg = NULL;
	lepton_version lep_version;
	int ret, irq = -1;

	dev = &spi->dev;
	of_node = dev->of_node;
	if (!of_node) {
		dev_err(dev, "missing device tree entry");
		return -EINVAL;
	}

	/* lepton struct keeps track of both video and spi-related structs,
	 * and will be available in driver callbacks via private data pointers
	 */
	printk(KERN_INFO LEPTON_MODULE_NAME ": Allocate struct lepton, init mutex\n");
	lep = devm_kzalloc(dev, sizeof(*lep), GFP_KERNEL);
	if (lep == NULL) {
		dev_err(dev, "failed to allocate lepton struct");
		return -ENOMEM;
	}
	mutex_init(&lep->mutex);
	spin_lock_init(&lep->lock);
	init_waitqueue_head(&lep->xfer_wait);

	/* initialize frame dimensions
	 */

	lep_version = lepton_dt_version(spi);
	init_lepton_info(&lep->lep_vospi_info, lep_version, TELEMETRY_OFF);

	/* initialize v4l2_device -- used for tracking relationships among 
	 * video-related hardware managed by the V4L2 subsystem 
	 */

	printk(KERN_INFO LEPTON_MODULE_NAME ": Allocate struct v4l2_dev, register V4L2 device\n");
	v4l2_dev = devm_kzalloc(dev, sizeof(*v4l2_dev), GFP_KERNEL);
	if (v4l2_dev == NULL) {
		dev_err(dev, "failed to allocate v4l2 struct");
		return -ENOMEM;
	}
	ret = v4l2_device_register(dev, v4l2_dev);
	if (ret) {
		dev_err(dev, "failed to register v4l2");
		return ret;
	}

	/* initialize video_device -- used for managing device file
	 * (e.g. /dev/videoN) owned by parent v4l2_device 
	 */

	vid_dev = video_device_alloc();
	if (vid_dev == NULL) {
		dev_err(dev, "failed to allocate video struct");
		ret = -ENOMEM;
		goto unreg_v4l2_device;
	}

	*vid_dev = lepton_videodev_template;
	vid_dev->v4l2_dev = v4l2_dev;

	/* initialize vb2_queue -- used to manage buffers for
	 * capturing video data 
	 */

	q = devm_kzalloc(dev, sizeof(*q), GFP_KERNEL);
	if (q == NULL) {
		/* now have non-devm (i.e. not automatically released when
		   owning device struct is gone) resources to free */
		dev_err(dev, "failed to allocate queue struct");
		ret = -ENOMEM;
		goto release_video_and_v4l_device;
	}

	q->dev = dev;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//	q->io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ; //@@@ is DMABUF freebie with vb2 boilerplate?
	q->io_modes = VB2_MMAP | VB2_READ;
	q->buf_struct_size = sizeof(struct lepton_buffer);
	q->ops = &lepton_video_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &lep->mutex;
	q->min_queued_buffers = 3;
	q->min_reqbufs_allocation = 3;

	ret = vb2_queue_init(q);
	if (ret) {
		/* now have non-devm (i.e. not automatically released when
		   owning device struct is gone) resources to free */
		dev_err(dev, "failed to init queue struct");
		goto release_video_and_v4l_device;
	}

	/* initialize spi descriptors and transmit buffer
	 */
	spi_msg = devm_kzalloc(dev, sizeof(*spi_msg), GFP_KERNEL);
	spi_xfer = devm_kzalloc(dev, sizeof(*spi_xfer), GFP_KERNEL);
	if (spi_xfer == NULL || spi_msg == NULL) {
		dev_err(dev, "failed to allocate SPI message/transfer structs");
		ret = -ENOMEM;
		goto release_video_and_v4l_device;
	}
	/* spare rx buffer when not using allocated V4L buf is subframe size + 1 line 
	 * so that eventually we will sync up if we start out in middle of a subframe */
	lep->spare_buf.len = lep->lep_vospi_info.subframe_params.subframe_data_byte_size + LEPTON_SUBFRAME_LINE_BYTE_WIDTH;
	lep->spare_buf.rx_buf = devm_kzalloc(dev, lep->spare_buf.len, GFP_KERNEL);
	if (lep->spare_buf.rx_buf == NULL) {
		dev_err(dev, "failed to allocate SPI rx buffer");
		ret = -ENOMEM;
		goto release_video_and_v4l_device;
	}

    /* set up data pointers to be able to find any of the core structs
	 * when only one is passed into a callback function 
	 */

	lep->v4l2_dev = v4l2_dev;
	lep->vid_dev = vid_dev;
	lep->q = q;
	lep->spi_dev = spi;
	lep->spi_xfer = spi_xfer;
	lep->spi_msg = spi_msg;
	lep->current_lep_buf = NULL;
	lep->irq = irq;
	lep->last_spi_status = 0;

	dev_set_drvdata(dev, lep);
	video_set_drvdata(vid_dev, lep);
	vid_dev->queue = q;
	q->drv_priv = lep;

	lep->last_spi_done_ts.tv_sec = -1; /* no spi transfer has been started yet */
	lep->last_spi_done_ts.tv_nsec = 0;

	INIT_LIST_HEAD(&lep->unfilled_bufs);

	ret = video_register_device(vid_dev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto release_video_and_v4l_device;

	/* set up interrupt handler for lepton VSYNC (frame ready signal) 
	 */

	irq = irq_of_parse_and_map(of_node, 0);
	if (irq < 0) {
		dev_err(dev, "failed to map irq");
		goto unreg_video_and_v4l_device;
	}
	lep->irq = irq;

	ret = devm_request_irq(dev, irq, lepton_vsync_handler, 0, dev_name(dev), spi);
	if (ret) {
		dev_err(dev, "failed to register irq");
		goto unreg_video_and_v4l_device;
	}

	ret = sysfs_create_group(&dev->kobj, &lepton_attr_group);
	if (ret) {
		dev_err(dev, "failed to create diagnostics attributes");
		goto unreg_video_and_v4l_device;
	}

	printk(KERN_INFO LEPTON_MODULE_NAME ": Probe complete\n");
	return 0;

unreg_video_and_v4l_device:
	video_unregister_device(lep->vid_dev);
	goto unreg_v4l2_device;
release_video_and_v4l_device:
	video_device_release(vid_dev);
unreg_v4l2_device:
	v4l2_device_unregister(v4l2_dev);

	return ret;
}

static void lepton_remove(struct spi_device *spi)
{
	struct lepton *lep = dev_get_drvdata(&spi->dev);
	unsigned long flags;

	sysfs_remove_group(&spi->dev.kobj, &lepton_attr_group);

	spin_lock_irqsave(&lep->lock, flags);
	lep->removing = true;
	spin_unlock_irqrestore(&lep->lock, flags);

	if (lep->irq > 0) {
		disable_irq(lep->irq);
		synchronize_irq(lep->irq);
	}

	if (!wait_event_timeout(lep->xfer_wait, !READ_ONCE(lep->transfer_in_flight),
				msecs_to_jiffies(1000)))
		dev_warn(&spi->dev, "timed out waiting for SPI transfer completion\n");

	/* tear down the things that are not "devm" (device-managed) */
	video_unregister_device(lep->vid_dev);
	v4l2_device_unregister(lep->v4l2_dev);
}

static struct spi_driver lepton_spi_driver = {
	.driver = {
		.name	= LEPTON_MODULE_NAME,
		.of_match_table	= lepton_of_match,
	},
	.id_table	= lepton_id_table,
	.probe		= lepton_probe,
	.remove		= lepton_remove,
};

module_spi_driver(lepton_spi_driver);

MODULE_AUTHOR("Team Lockwood-Childs, VCT Labs, Inc.");
MODULE_DESCRIPTION("VoSPI driver for FLIR lepton 2.x/3.x");
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");

