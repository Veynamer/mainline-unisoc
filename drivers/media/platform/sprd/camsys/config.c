// SPDX-License-Identifier: GPL-2.0
/*
 * Unisoc CSI controller driver
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-vmalloc.h>

#include "camsys.h"

static const size_t dcam_config_block_sizes[SPRD_DCAM_BLOCK_NUM] = {
	[SPRD_DCAM_BLOCK_AE] = sizeof(struct sprd_camsys_ae_config),
	[SPRD_DCAM_BLOCK_AF] = sizeof(struct sprd_camsys_af_config),
	[SPRD_DCAM_BLOCK_AWB] = sizeof(struct sprd_camsys_awb_config),
	[SPRD_DCAM_BLOCK_BLC] = sizeof(struct sprd_camsys_blc_config),
};

static int sprd_config_querycap(struct file *file, void *fh,
				struct v4l2_capability *cap)
{
	strscpy(cap->driver, "sprd-camsys", sizeof(cap->driver));
	strscpy(cap->card, "Unisoc Camera Subsystem", sizeof(cap->card));

	return 0;
}

static int sprd_config_enum_fmt(struct file *file, void *fh,
				struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_SPRD_DCAM_CFG;

	return 0;
}

static int sprd_config_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	f->fmt.meta.dataformat = V4L2_META_FMT_SPRD_DCAM_CFG;
	f->fmt.meta.buffersize = SPRD_DCAM_MAX_CONFIG_SIZE;

	return 0;
}

static int sprd_config_queue_setup(struct vb2_queue *q,
				   unsigned int *num_buffers,
				   unsigned int *num_planes,
				   unsigned int sizes[],
				   struct device *alloc_devs[])
{
	if (*num_planes) {
		if (*num_planes != 1)
			return -EINVAL;
		if (sizes[0] != SPRD_DCAM_MAX_CONFIG_SIZE)
			return -EINVAL;
	} else {
		*num_planes = 1;
		sizes[0] = SPRD_DCAM_MAX_CONFIG_SIZE;
	}

	return 0;
}

static int sprd_config_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_config_buffer *buffer =
		container_of(vbuf, struct sprd_config_buffer, vbuf);

	buffer->config = kvmalloc(SPRD_DCAM_MAX_CONFIG_SIZE, GFP_KERNEL);
	if (!buffer->config)
		return -ENOMEM;

	return 0;
}

static void sprd_config_buf_cleanup(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_config_buffer *buffer =
		container_of(vbuf, struct sprd_config_buffer, vbuf);

	kvfree(buffer->config);
	buffer->config = NULL;
}

static int sprd_config_buf_prepare(struct vb2_buffer *vb)
{
	struct sprd_config_vdev *cfg =
		container_of(vb->vb2_queue, struct sprd_config_vdev, queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_config_buffer *buffer =
		container_of(vbuf, struct sprd_config_buffer, vbuf);
	size_t payload_size = vb2_get_plane_payload(vb, 0);
	void *config_data = buffer->config;

	if (payload_size > SPRD_DCAM_MAX_CONFIG_SIZE ||
	    payload_size < sizeof(struct sprd_camsys_block_config_header))
		return -EINVAL;

	memcpy(config_data, vb2_plane_vaddr(vb, 0), payload_size);

	while (payload_size > 0) {
		union sprd_camsys_block_config *block = config_data;
		size_t block_size;

		if (payload_size < sizeof(block->header.type)) {
			dev_dbg(&cfg->camsys->pdev->dev,
				"incomplete config block header\n");
			return -EINVAL;
		}

		if (block->header.type >= ARRAY_SIZE(dcam_config_block_sizes)) {
			dev_dbg(&cfg->camsys->pdev->dev,
				"invalid config block type %d\n",
				block->header.type);
			return -EINVAL;
		}

		block_size = ALIGN(dcam_config_block_sizes[block->header.type], 8);
		if (payload_size < block_size) {
			dev_dbg(&cfg->camsys->pdev->dev,
				"incomplete config block\n");
			return -EINVAL;
		}

		payload_size -= block_size;
		config_data += block_size;
	}

	return 0;
}

static void sprd_config_buf_queue(struct vb2_buffer *vb)
{
	struct sprd_config_vdev *cfg =
		container_of(vb->vb2_queue, struct sprd_config_vdev, queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_config_buffer *buffer =
		container_of(vbuf, struct sprd_config_buffer, vbuf);
	unsigned long flags;

	spin_lock_irqsave(&cfg->buf_lock, flags);
	list_add_tail(&buffer->link, &cfg->buf_queue);
	spin_unlock_irqrestore(&cfg->buf_lock, flags);
}

static void sprd_config_stop_streaming(struct vb2_queue *q)
{
	struct sprd_config_vdev *cfg =
		container_of(q, struct sprd_config_vdev, queue);
	struct sprd_config_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&cfg->buf_lock, flags);

	while (!list_empty(&cfg->buf_queue)) {
		buf = list_first_entry(&cfg->buf_queue,
				       struct sprd_config_buffer, link);
		list_del(&buf->link);
		vb2_buffer_done(&buf->vbuf.vb2_buf, VB2_BUF_STATE_ERROR);
	}

	spin_unlock_irqrestore(&cfg->buf_lock, flags);
}

static const struct v4l2_ioctl_ops sprd_config_ioctl_ops = {
	.vidioc_querycap		= sprd_config_querycap,
	.vidioc_enum_fmt_meta_out	= sprd_config_enum_fmt,
	.vidioc_g_fmt_meta_out		= sprd_config_g_fmt,
	.vidioc_s_fmt_meta_out		= sprd_config_g_fmt,
	.vidioc_try_fmt_meta_out	= sprd_config_g_fmt,
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

static const struct v4l2_file_operations sprd_config_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
};

static const struct vb2_ops sprd_config_vb2_ops = {
	.queue_setup	= sprd_config_queue_setup,
	.buf_init	= sprd_config_buf_init,
	.buf_cleanup	= sprd_config_buf_cleanup,
	.buf_prepare	= sprd_config_buf_prepare,
	.buf_queue	= sprd_config_buf_queue,
	.stop_streaming	= sprd_config_stop_streaming,
};

int sprd_config_vdev_register(struct sprd_config_vdev *cfg)
{
	struct sprd_camsys *cs = cfg->camsys;
	struct video_device *vdev = &cfg->vdev;
	struct vb2_queue *q = &cfg->queue;
	int ret;

	mutex_init(&cfg->lock);
	spin_lock_init(&cfg->buf_lock);
	INIT_LIST_HEAD(&cfg->buf_queue);

	/* vdev->name is set by the caller */
	vdev->device_caps = V4L2_CAP_META_OUTPUT | V4L2_CAP_STREAMING;
	vdev->v4l2_dev = &cs->v4l2_dev;
	vdev->fops = &sprd_config_fops;
	vdev->release = video_device_release_empty;
	vdev->ioctl_ops = &sprd_config_ioctl_ops;
	vdev->lock = &cfg->lock;
	vdev->vfl_dir = VFL_DIR_TX;

	video_set_drvdata(vdev, cfg);

	q->type = V4L2_BUF_TYPE_META_OUTPUT;
	q->io_modes = VB2_DMABUF | VB2_MMAP;
	q->ops = &sprd_config_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->buf_struct_size = sizeof(struct sprd_config_buffer);
	q->min_queued_buffers = 1;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &cfg->lock;
	q->dev = &cs->pdev->dev;
	ret = vb2_queue_init(q);
	if (ret) {
		dev_err(&cs->pdev->dev, "failed to init vb2 queue: %d\n", ret);
		goto err_destroy_mutex;
	}

	vdev->queue = q;

	cfg->pads[SPRD_CONFIG_PAD_SRC].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&vdev->entity, SPRD_CONFIG_PAD_NUM,
				     cfg->pads);
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
	mutex_destroy(&cfg->lock);
	return ret;
}

void sprd_config_vdev_unregister(struct sprd_config_vdev *cfg)
{
	video_unregister_device(&cfg->vdev);
	media_entity_cleanup(&cfg->vdev.entity);
	vb2_queue_release(&cfg->queue);
	mutex_destroy(&cfg->lock);
}

void sprd_config_next_frame(struct sprd_dcam *dcam)
{
	struct sprd_config_vdev *cfg = &dcam->config;
	struct sprd_config_buffer *buffer;
	size_t payload_size;
	void *config_data;
	unsigned long flags;

	spin_lock_irqsave(&cfg->buf_lock, flags);

	if (list_empty(&cfg->buf_queue))
		goto out;

	buffer = list_first_entry(&cfg->buf_queue, struct sprd_config_buffer,
				  link);
	list_del(&buffer->link);

	payload_size = vb2_get_plane_payload(&buffer->vbuf.vb2_buf, 0);
	config_data = buffer->config;

	while (payload_size > 0) {
		union sprd_camsys_block_config *block = config_data;
		size_t block_size =
			ALIGN(dcam_config_block_sizes[block->header.type], 8);

		if (dcam->hw->config_block[block->header.type]) {
			dev_dbg(dcam->dev, "configuring block %d\n",
				block->header.type);
			dcam->hw->config_block[block->header.type](dcam, block);
		} else {
			dev_dbg(dcam->dev, "block %d not supported by HW\n",
				block->header.type);
		}

		payload_size -= block_size;
		config_data += block_size;
	}

	buffer->vbuf.vb2_buf.timestamp = ktime_get_ns();
	buffer->vbuf.sequence = dcam->sequence;
	vb2_buffer_done(&buffer->vbuf.vb2_buf, VB2_BUF_STATE_DONE);

out:
	spin_unlock_irqrestore(&cfg->buf_lock, flags);
}
