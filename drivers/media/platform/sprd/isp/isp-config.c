// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unisoc ISP driver - configuration device node
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "isp.h"

struct sprd_isp_config_buffer {
	struct vb2_v4l2_buffer vbuf;
	struct list_head link;
	void *config;
};

static const size_t isp_config_block_sizes[SPRD_ISP_BLOCK_NUM] = {
	[SPRD_ISP_BLOCK_CCM] = sizeof(struct sprd_camsys_ccm_config),
	[SPRD_ISP_BLOCK_GAMMA] = sizeof(struct sprd_camsys_gamma_config),
};

static int sprd_isp_querycap(struct file *file, void *fh,
			     struct v4l2_capability *cap)
{
	strscpy(cap->driver, "sprd-isp", sizeof(cap->driver));
	strscpy(cap->card, "Unisoc ISP", sizeof(cap->card));

	return 0;
}

static int sprd_isp_cfg_enum_fmt(struct file *file, void *fh,
				 struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_SPRD_ISP_CFG;

	return 0;
}

static int sprd_isp_cfg_g_fmt(struct file *file, void *fh,
			      struct v4l2_format *f)
{
	f->fmt.meta.dataformat = V4L2_META_FMT_SPRD_ISP_CFG;
	f->fmt.meta.buffersize = SPRD_ISP_MAX_CONFIG_SIZE;

	return 0;
}

static int sprd_isp_cfg_queue_setup(struct vb2_queue *q,
				    unsigned int *num_buffers,
				    unsigned int *num_planes,
				    unsigned int sizes[],
				    struct device *alloc_devs[])
{
	if (*num_planes) {
		if (*num_planes != 1)
			return -EINVAL;
		if (sizes[0] != SPRD_ISP_MAX_CONFIG_SIZE)
			return -EINVAL;
	} else {
		*num_planes = 1;
		sizes[0] = SPRD_ISP_MAX_CONFIG_SIZE;
	}

	return 0;
}

static int sprd_isp_cfg_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_isp_config_buffer *buf =
		container_of(vbuf, struct sprd_isp_config_buffer, vbuf);

	buf->config = kvmalloc(SPRD_ISP_MAX_CONFIG_SIZE, GFP_KERNEL);
	if (!buf->config)
		return -ENOMEM;

	return 0;
}

static void sprd_isp_cfg_buf_cleanup(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_isp_config_buffer *buf =
		container_of(vbuf, struct sprd_isp_config_buffer, vbuf);

	kvfree(buf->config);
	buf->config = NULL;
}

static int sprd_isp_cfg_buf_prepare(struct vb2_buffer *vb)
{
	struct sprd_isp_config_node *node =
		container_of(vb->vb2_queue, struct sprd_isp_config_node, vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_isp_config_buffer *buf =
		container_of(vbuf, struct sprd_isp_config_buffer, vbuf);
	struct sprd_isp *isp = node->isp;
	size_t payload_size = vb2_get_plane_payload(vb, 0);
	void *config_data = buf->config;

	if (payload_size > SPRD_ISP_MAX_CONFIG_SIZE ||
	    payload_size < sizeof(struct sprd_camsys_block_config_header))
		return -EINVAL;

	memcpy(config_data, vb2_plane_vaddr(vb, 0), payload_size);

	while (payload_size > 0) {
		union sprd_camsys_block_config *block = config_data;
		size_t block_size;

		if (payload_size < sizeof(block->header.type)) {
			dev_dbg(isp->dev, "incomplete config block header\n");
			return -EINVAL;
		}

		if (block->header.type >= ARRAY_SIZE(isp_config_block_sizes)) {
			dev_dbg(isp->dev, "invalid config block type %d\n",
				block->header.type);
			return -EINVAL;
		}

		block_size = ALIGN(isp_config_block_sizes[block->header.type], 8);
		if (payload_size < block_size) {
			dev_dbg(isp->dev, "incomplete config block\n");
			return -EINVAL;
		}

		payload_size -= block_size;
		config_data += block_size;
	}

	return 0;
}

static void sprd_isp_cfg_buf_queue(struct vb2_buffer *vb)
{
	struct sprd_isp_config_node *node =
		container_of(vb->vb2_queue, struct sprd_isp_config_node, vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_isp_config_buffer *buf =
		container_of(vbuf, struct sprd_isp_config_buffer, vbuf);
	struct sprd_isp *isp = node->isp;
	unsigned long flags;

	spin_lock_irqsave(&isp->lock, flags);
	list_add_tail(&buf->link, &node->buf_queue);
	spin_unlock_irqrestore(&isp->lock, flags);
}

static void sprd_isp_cfg_stop_streaming(struct vb2_queue *q)
{
	struct sprd_isp_config_node *node =
		container_of(q, struct sprd_isp_config_node, vb2_queue);
	struct sprd_isp_config_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&node->isp->lock, flags);

	while (!list_empty(&node->buf_queue)) {
		buf = list_first_entry(&node->buf_queue,
				       struct sprd_isp_config_buffer, link);
		list_del(&buf->link);
		vb2_buffer_done(&buf->vbuf.vb2_buf, VB2_BUF_STATE_ERROR);
	}

	spin_unlock_irqrestore(&node->isp->lock, flags);
}

static const struct vb2_ops sprd_isp_cfg_vb2_ops = {
	.queue_setup	= sprd_isp_cfg_queue_setup,
	.buf_init	= sprd_isp_cfg_buf_init,
	.buf_cleanup	= sprd_isp_cfg_buf_cleanup,
	.buf_prepare	= sprd_isp_cfg_buf_prepare,
	.buf_queue	= sprd_isp_cfg_buf_queue,
	.stop_streaming	= sprd_isp_cfg_stop_streaming,
};

static const struct v4l2_file_operations sprd_isp_cfg_fops = {
	.owner          = THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = vb2_fop_release,
	.poll           = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = vb2_fop_mmap
};

static const struct v4l2_ioctl_ops sprd_isp_cfg_ioctl_ops = {
	.vidioc_querycap = sprd_isp_querycap,
	.vidioc_g_fmt_meta_out = sprd_isp_cfg_g_fmt,
	.vidioc_try_fmt_meta_out = sprd_isp_cfg_g_fmt,
	.vidioc_s_fmt_meta_out = sprd_isp_cfg_g_fmt,
	.vidioc_enum_fmt_meta_out = sprd_isp_cfg_enum_fmt,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

int sprd_isp_config_register(struct sprd_isp_config_node *node)
{
	struct sprd_isp *isp = node->isp;
	struct video_device *vdev = &node->vdev;
	struct vb2_queue *q = &node->vb2_queue;
	struct v4l2_subdev *sd = &isp->context[node->ctx_id].sd;
	int ret;

	mutex_init(&node->lock);
	INIT_LIST_HEAD(&node->buf_queue);

	vdev->device_caps = V4L2_CAP_META_OUTPUT | V4L2_CAP_STREAMING;
	vdev->v4l2_dev = &isp->v4l2_dev;
	vdev->fops = &sprd_isp_cfg_fops;
	vdev->release = video_device_release_empty;
	vdev->ioctl_ops = &sprd_isp_cfg_ioctl_ops;
	vdev->lock = &node->lock;
	vdev->vfl_dir = VFL_DIR_TX;

	video_set_drvdata(vdev, node);

	q->type = V4L2_BUF_TYPE_META_OUTPUT;
	q->io_modes = VB2_DMABUF | VB2_MMAP;
	q->ops = &sprd_isp_cfg_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->buf_struct_size = sizeof(struct sprd_isp_config_buffer);
	q->min_queued_buffers = 1;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &node->lock;
	q->dev = isp->dev;
	ret = vb2_queue_init(q);
	if (ret) {
		dev_err(isp->dev, "failed to init vb2 queue: %d\n", ret);
		goto err_destroy_mutex;
	}

	vdev->queue = q;

	node->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&vdev->entity, 1, &node->pad);
	if (ret)
		goto err_release_queue;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(isp->dev, "failed to register %s: %d\n", vdev->name,
			ret);
		goto err_cleanup_entity;
	}

	ret = media_create_pad_link(&vdev->entity, 0,
				    &sd->entity, SPRD_ISP_CONFIG,
				    MEDIA_LNK_FL_IMMUTABLE |
				    MEDIA_LNK_FL_ENABLED);
	if (ret)
		goto err_unregister_vdev;

	return 0;

err_unregister_vdev:
	video_unregister_device(vdev);
err_cleanup_entity:
	media_entity_cleanup(&vdev->entity);
err_release_queue:
	vb2_queue_release(q);
err_destroy_mutex:
	mutex_destroy(&node->lock);
	return ret;
}

void sprd_isp_config_unregister(struct sprd_isp_config_node *node)
{
	video_unregister_device(&node->vdev);
	media_entity_cleanup(&node->vdev.entity);
	vb2_queue_release(&node->vb2_queue);
	mutex_destroy(&node->lock);
}

void sprd_isp_configure(struct sprd_isp_config_node *node)
{
	struct sprd_isp *isp = node->isp;
	struct sprd_isp_config_buffer *buf;
	size_t payload_size;
	void *config_data;

	lockdep_assert_held(&isp->lock);

	if (list_empty(&node->buf_queue))
		return;

	buf = list_first_entry(&node->buf_queue,
			       struct sprd_isp_config_buffer, link);
	list_del(&buf->link);

	payload_size = vb2_get_plane_payload(&buf->vbuf.vb2_buf, 0);
	config_data = buf->config;

	while (payload_size > 0) {
		union sprd_camsys_block_config *block = config_data;
		size_t block_size =
			ALIGN(isp_config_block_sizes[block->header.type], 8);

		if (isp->hw->config_block[block->header.type]) {
			dev_dbg(isp->dev, "configuring block %d\n",
				block->header.type);
			isp->hw->config_block[block->header.type](isp,
								  node->ctx_id,
								  block);
		} else {
			dev_dbg(isp->dev, "block %d not supported by HW\n",
				block->header.type);
		}

		payload_size -= block_size;
		config_data += block_size;
	}

	buf->vbuf.vb2_buf.timestamp = ktime_get_ns();
	buf->vbuf.sequence = isp->context[node->ctx_id].sequence;
	vb2_buffer_done(&buf->vbuf.vb2_buf, VB2_BUF_STATE_DONE);
}
