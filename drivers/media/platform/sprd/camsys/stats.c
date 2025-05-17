// SPDX-License-Identifier: GPL-2.0
/*
 * Unisoc CSI controller driver
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-contig.h>

#include "camsys.h"

static int sprd_stats_querycap(struct file *file, void *fh,
			       struct v4l2_capability *cap)
{
	strscpy(cap->driver, "sprd-camsys", sizeof(cap->driver));
	strscpy(cap->card, "Unisoc Camera Subsystem", sizeof(cap->card));

	return 0;
}

static int sprd_stats_enum_fmt(struct file *file, void *fh,
			       struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_SPRD_DCAM_STATS;

	return 0;
}

static int sprd_stats_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	f->fmt.meta.dataformat = V4L2_META_FMT_SPRD_DCAM_STATS;
	f->fmt.meta.buffersize = sizeof(struct sprd_dcam_statistics);

	return 0;
}

static int sprd_stats_queue_setup(struct vb2_queue *q,
				unsigned int *num_buffers,
				unsigned int *num_planes,
				unsigned int sizes[],
				struct device *alloc_devs[])
{
	if (*num_planes) {
		if (*num_planes != 1)
			return -EINVAL;
		if (sizes[0] != sizeof(struct sprd_dcam_statistics))
			return -EINVAL;
	} else {
		*num_planes = 1;
		sizes[0] = sizeof(struct sprd_dcam_statistics);
	}

	return 0;
}

static int sprd_stats_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_stats_buffer *buffer =
		container_of(vbuf, struct sprd_stats_buffer, vbuf);

	buffer->ae_addr = vb2_dma_contig_plane_dma_addr(vb, 0);

	return 0;
}

static int sprd_stats_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < sizeof(struct sprd_dcam_statistics))
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, sizeof(struct sprd_dcam_statistics));

	return 0;
}

static void sprd_stats_buf_queue(struct vb2_buffer *vb)
{
	struct sprd_stats_vdev *stats =
		container_of(vb->vb2_queue, struct sprd_stats_vdev, queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_stats_buffer *buffer =
		container_of(vbuf, struct sprd_stats_buffer, vbuf);
	unsigned long flags;

	spin_lock_irqsave(&stats->buf_lock, flags);
	list_add_tail(&buffer->link, &stats->buf_queue);
	spin_unlock_irqrestore(&stats->buf_lock, flags);
}

static void sprd_stats_stop_streaming(struct vb2_queue *q)
{
	struct sprd_stats_vdev *stats =
		container_of(q, struct sprd_stats_vdev, queue);
	struct sprd_stats_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&stats->buf_lock, flags);

	while (!list_empty(&stats->buf_queue)) {
		buf = list_first_entry(&stats->buf_queue,
				       struct sprd_stats_buffer, link);
		list_del(&buf->link);
		vb2_buffer_done(&buf->vbuf.vb2_buf, VB2_BUF_STATE_ERROR);
	}

	if (stats->current_buf) {
		vb2_buffer_done(&stats->current_buf->vbuf.vb2_buf,
				VB2_BUF_STATE_ERROR);
		stats->current_buf = NULL;
	}

	if (stats->next_buf) {
		vb2_buffer_done(&stats->next_buf->vbuf.vb2_buf,
				VB2_BUF_STATE_ERROR);
		stats->next_buf = NULL;
	}

	spin_unlock_irqrestore(&stats->buf_lock, flags);
}

static const struct v4l2_ioctl_ops sprd_stats_ioctl_ops = {
	.vidioc_querycap		= sprd_stats_querycap,
	.vidioc_enum_fmt_meta_cap	= sprd_stats_enum_fmt,
	.vidioc_g_fmt_meta_cap		= sprd_stats_g_fmt,
	.vidioc_s_fmt_meta_cap		= sprd_stats_g_fmt,
	.vidioc_try_fmt_meta_cap	= sprd_stats_g_fmt,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations sprd_stats_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
};

static const struct vb2_ops sprd_stats_vb2_ops = {
	.queue_setup	= sprd_stats_queue_setup,
	.buf_init	= sprd_stats_buf_init,
	.buf_prepare	= sprd_stats_buf_prepare,
	.buf_queue	= sprd_stats_buf_queue,
	.stop_streaming	= sprd_stats_stop_streaming,
};

int sprd_stats_vdev_register(struct sprd_stats_vdev *stats)
{
	struct sprd_camsys *cs = stats->camsys;
	struct video_device *vdev = &stats->vdev;
	struct vb2_queue *q = &stats->queue;
	int ret;

	mutex_init(&stats->lock);
	spin_lock_init(&stats->buf_lock);
	INIT_LIST_HEAD(&stats->buf_queue);

	/* vdev->name is set by the caller */
	vdev->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
	vdev->v4l2_dev = &cs->v4l2_dev;
	vdev->fops = &sprd_stats_fops;
	vdev->release = video_device_release_empty;
	vdev->ioctl_ops = &sprd_stats_ioctl_ops;
	vdev->lock = &stats->lock;
	vdev->vfl_dir = VFL_DIR_RX;

	video_set_drvdata(vdev, stats);

	q->type = V4L2_BUF_TYPE_META_CAPTURE;
	q->io_modes = VB2_DMABUF | VB2_MMAP;
	q->ops = &sprd_stats_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->buf_struct_size = sizeof(struct sprd_stats_buffer);
	q->min_queued_buffers = 1;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &stats->lock;
	q->dev = &cs->pdev->dev;
	ret = vb2_queue_init(q);
	if (ret) {
		dev_err(&cs->pdev->dev, "failed to init vb2 queue: %d\n", ret);
		goto err_destroy_mutex;
	}

	vdev->queue = q;

	stats->pads[SPRD_STATS_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vdev->entity, SPRD_STATS_PAD_NUM,
				     stats->pads);
	if (ret)
		goto err_release_queue;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&cs->pdev->dev, "failed to register %s: %d\n",
			vdev->name, ret);
		goto err_cleanup_entity;
	}

	return 0;

err_cleanup_entity:
	media_entity_cleanup(&vdev->entity);
err_release_queue:
	vb2_queue_release(q);
err_destroy_mutex:
	mutex_destroy(&stats->lock);
	return ret;
}

void sprd_stats_vdev_unregister(struct sprd_stats_vdev *stats)
{
	video_unregister_device(&stats->vdev);
	media_entity_cleanup(&stats->vdev.entity);
	vb2_queue_release(&stats->queue);
	mutex_destroy(&stats->lock);
}

struct sprd_stats_buffer *
sprd_stats_next_frame(struct sprd_stats_vdev *stats)
{
	unsigned long flags;

	spin_lock_irqsave(&stats->buf_lock, flags);

	if (stats->current_buf) {
		dev_err(&stats->camsys->pdev->dev,
			"stats buffer was not done before next frame!\n");
		spin_unlock_irqrestore(&stats->buf_lock, flags);
		return NULL;
	}

	/* The previously staged buffer is now being processed. */
	stats->current_buf = stats->next_buf;

	if (stats->blocks_enabled == 0) {
		spin_unlock_irqrestore(&stats->buf_lock, flags);
		return NULL;
	}

	/* Stage a new buffer. */
	if (!list_empty(&stats->buf_queue)) {
		stats->next_buf = list_first_entry(&stats->buf_queue,
						   struct sprd_stats_buffer,
						   link);
		list_del(&stats->next_buf->link);
		stats->next_buf->blocks_enabled = stats->blocks_enabled;
		stats->next_buf->blocks_done = 0;
	} else {
		stats->next_buf = NULL;
	}

	spin_unlock_irqrestore(&stats->buf_lock, flags);

	return stats->next_buf;
}

void sprd_stats_done(struct sprd_stats_vdev *stats, unsigned int sequence,
		     unsigned int block)
{
	struct sprd_stats_buffer *buffer;

	spin_lock(&stats->buf_lock);

	buffer = stats->current_buf;
	if (buffer && (block & buffer->blocks_enabled)) {
		buffer->blocks_done |= block;
		if (buffer->blocks_done == buffer->blocks_enabled) {
			buffer->vbuf.vb2_buf.timestamp = ktime_get_ns();
			buffer->vbuf.sequence = sequence;
			vb2_buffer_done(&buffer->vbuf.vb2_buf,
					VB2_BUF_STATE_DONE);
			stats->current_buf = NULL;
		}
	}

	spin_unlock(&stats->buf_lock);
}
