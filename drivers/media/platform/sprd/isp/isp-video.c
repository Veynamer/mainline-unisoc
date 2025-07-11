// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unisoc ISP driver - video device node
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#include <linux/pm_runtime.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "isp.h"

#define SPRD_ISP_MAX_SLICES 8

struct sprd_isp_format {
	u32 fourcc;
	unsigned int hsize_unit;
	unsigned int num_planes;
	struct {
		unsigned int bpp_line;
		unsigned int bpp_image;
	} plane[3];
};

static const struct sprd_isp_format sprd_isp_supported_input_fmts[] = {
	{ V4L2_PIX_FMT_YUYV,	1, 1, {{ 16, 16 }} },
	{ V4L2_PIX_FMT_UYVY,	1, 1, {{ 16, 16 }} },
	{ V4L2_PIX_FMT_YVYU,	1, 1, {{ 16, 16 }} },
	{ V4L2_PIX_FMT_VYUY,	1, 1, {{ 16, 16 }} },
	{ V4L2_PIX_FMT_YUV422M,	1, 3, {{ 8, 8 }, { 4, 4 }, { 4, 4 }} },
	{ V4L2_PIX_FMT_NV16M,	1, 2, {{ 8, 8 }, { 8, 8 }} },
	{ V4L2_PIX_FMT_NV16,	1, 1, {{ 8, 16 }} },
	{ V4L2_PIX_FMT_NV61M,	1, 2, {{ 8, 8 }, { 8, 8 }} },
	{ V4L2_PIX_FMT_NV61,	1, 1, {{ 8, 16 }} },
	{ V4L2_PIX_FMT_NV12M,	1, 2, {{ 8, 8 }, { 8, 4 }} },
	{ V4L2_PIX_FMT_NV12,	1, 1, {{ 8, 12 }} },
	{ V4L2_PIX_FMT_NV21M,	1, 2, {{ 8, 8 }, { 8, 4 }} },
	{ V4L2_PIX_FMT_NV21,	1, 1, {{ 8, 12 }} },
	{ V4L2_PIX_FMT_SBGGR10P, 4, 1, {{ 10, 10 }} },
	{ V4L2_PIX_FMT_SGBRG10P, 4, 1, {{ 10, 10 }} },
	{ V4L2_PIX_FMT_SGRBG10P, 4, 1, {{ 10, 10 }} },
	{ V4L2_PIX_FMT_SRGGB10P, 4, 1, {{ 10, 10 }} },
};

static const struct sprd_isp_format sprd_isp_supported_output_fmts[] = {
	{ V4L2_PIX_FMT_YUV420M,	1, 3, {{ 8, 8 }, { 4, 4 }, { 4, 4 }} },
	{ V4L2_PIX_FMT_NV12M,	1, 2, {{ 8, 8 }, { 8, 4 }} },
	{ V4L2_PIX_FMT_NV12,	1, 1, {{ 8, 12 }} },
	{ V4L2_PIX_FMT_NV21M,	1, 2, {{ 8, 8 }, { 8, 4 }} },
	{ V4L2_PIX_FMT_NV21,	1, 1, {{ 8, 12 }} },
};

struct sprd_isp_video_buffer {
	struct vb2_v4l2_buffer vbuf;
	struct list_head link;
};

static int sprd_isp_querycap(struct file *file, void *fh,
			     struct v4l2_capability *cap)
{
	strscpy(cap->driver, "sprd-isp", sizeof(cap->driver));
	strscpy(cap->card, "Unisoc ISP", sizeof(cap->card));

	return 0;
}

static int sprd_isp_enum_fmt(struct file *file, void *fh,
			     struct v4l2_fmtdesc *f)
{
	struct sprd_isp_video_node *node = video_drvdata(file);
	const struct sprd_isp_format *formats;
	unsigned int num_formats;

	if (node->index == SPRD_ISP_INPUT) {
		num_formats = ARRAY_SIZE(sprd_isp_supported_input_fmts);
		formats = sprd_isp_supported_input_fmts;
	} else if (node->index == SPRD_ISP_OUTPUT) {
		num_formats = ARRAY_SIZE(sprd_isp_supported_output_fmts);
		formats = sprd_isp_supported_output_fmts;
	} else {
		return -EINVAL;
	}

	if (f->index >= num_formats)
		return -EINVAL;

	f->pixelformat = formats[f->index].fourcc;

	return 0;
}

static int sprd_isp_enum_framesizes(struct file *file, void *fh,
				    struct v4l2_frmsizeenum *fsize)
{
	struct sprd_isp_video_node *node = video_drvdata(file);

	if (fsize->index)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	fsize->stepwise.min_width = SPRD_ISP_MAX_SLICES * 2;
	fsize->stepwise.max_width = node->isp->hw->max_width * SPRD_ISP_MAX_SLICES;
	fsize->stepwise.min_height = 2;
	fsize->stepwise.max_height = node->isp->hw->max_height;
	fsize->stepwise.step_width = 2;
	fsize->stepwise.step_height = 2;

	return 0;
}

static int sprd_isp_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct sprd_isp_video_node *node = video_drvdata(file);

	f->fmt.pix_mp = node->fmt;

	return 0;
}

static const struct sprd_isp_format *
__sprd_isp_try_fmt(struct sprd_isp_video_node *node,
		   struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct sprd_isp_format *formats, *fmt;
	unsigned int i, num_formats;
	u32 bpl;

	if (node->index == SPRD_ISP_INPUT) {
		num_formats = ARRAY_SIZE(sprd_isp_supported_input_fmts);
		formats = sprd_isp_supported_input_fmts;
	} else if (node->index == SPRD_ISP_OUTPUT) {
		num_formats = ARRAY_SIZE(sprd_isp_supported_output_fmts);
		formats = sprd_isp_supported_output_fmts;
	} else {
		return NULL;
	}

	for (i = 0; i < num_formats; i++) {
		fmt = &formats[i];

		if (fmt->fourcc == pix_mp->pixelformat)
			break;
	}

	if (i == num_formats)
		fmt = &formats[0];

	pix_mp->pixelformat = fmt->fourcc;
	pix_mp->num_planes = fmt->num_planes;
	pix_mp->width = clamp_t(u32, ALIGN(pix_mp->width, 2), 1,
				node->isp->hw->max_width *
				SPRD_ISP_MAX_SLICES);
	pix_mp->height = clamp_t(u32, ALIGN(pix_mp->height, 2), 1,
				 node->isp->hw->max_height);

	for (i = 0; i < fmt->num_planes; i++) {
		struct v4l2_plane_pix_format *p = &pix_mp->plane_fmt[i];

		bpl = DIV_ROUND_UP(pix_mp->width, fmt->hsize_unit) *
			fmt->hsize_unit * fmt->plane[i].bpp_line / 8;
		bpl = max(p->bytesperline, bpl);
		bpl = ALIGN(bpl, 4);

		p->bytesperline = bpl;
		p->sizeimage = pix_mp->height * bpl *
				fmt->plane[i].bpp_image /
				fmt->plane[i].bpp_line;
	}

	pix_mp->field = V4L2_FIELD_NONE;

	if (!pix_mp->colorspace)
		pix_mp->colorspace = V4L2_COLORSPACE_SRGB;

	return fmt;
}

static int sprd_isp_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct sprd_isp_video_node *node = video_drvdata(file);

	if (vb2_is_busy(&node->vb2_queue))
		return -EBUSY;

	node->format_info = __sprd_isp_try_fmt(node, f);
	node->fmt = f->fmt.pix_mp;

	if (node->index == SPRD_ISP_INPUT) {
		struct v4l2_rect crop;

		crop.top = 0;
		crop.left = 0;
		crop.width = node->fmt.width;
		crop.height = node->fmt.height;
		node->isp->context[node->ctx_id].crop_rect = crop;
	}

	return 0;
}

static int sprd_isp_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct sprd_isp_video_node *node = video_drvdata(file);

	__sprd_isp_try_fmt(node, f);

	return 0;
}

static int sprd_isp_g_selection(struct file *file, void *fh,
				struct v4l2_selection *s)
{
	struct sprd_isp_video_node *node = video_drvdata(file);

	if (node->index != SPRD_ISP_INPUT)
		return -EINVAL;

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		s->r = node->isp->context[node->ctx_id].crop_rect;
		break;
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		s->r.width = node->fmt.width;
		s->r.height = node->fmt.height;
		s->r.left = 0;
		s->r.top = 0;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int sprd_isp_s_selection(struct file *file, void *fh,
				struct v4l2_selection *s)
{
	struct sprd_isp_video_node *node = video_drvdata(file);

	if (node->index != SPRD_ISP_INPUT)
		return -EINVAL;

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		s->r.left = min_t(u32, ALIGN_DOWN(s->r.left, 2),
				  node->fmt.width - 2);
		s->r.top = min_t(u32, ALIGN_DOWN(s->r.top, 2),
				 node->fmt.height - 2);
		s->r.width = min(ALIGN(s->r.width, 2),
				 node->fmt.width - s->r.left);
		s->r.height = min(ALIGN(s->r.height, 2),
				  node->fmt.height - s->r.top);
		node->isp->context[node->ctx_id].crop_rect = s->r;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int sprd_isp_queue_setup(struct vb2_queue *q, unsigned int *nbuffers,
				unsigned int *nplanes, unsigned int sizes[],
				struct device *alloc_devs[])
{
	struct sprd_isp_video_node *node =
		container_of(q, struct sprd_isp_video_node, vb2_queue);
	unsigned int i;

	if (*nplanes) {
		if (*nplanes != node->fmt.num_planes)
			return -EINVAL;

		for (i = 0; i < *nplanes; i++) {
			if (sizes[i] < node->fmt.plane_fmt[i].sizeimage)
				return -EINVAL;
		}

		return 0;
	}

	*nplanes = node->fmt.num_planes;
	for (i = 0; i < *nplanes; i++)
		sizes[i] = node->fmt.plane_fmt[i].sizeimage;

	return 0;
}

static int sprd_isp_buf_prepare(struct vb2_buffer *vb)
{
	struct sprd_isp_video_node *node =
		container_of(vb->vb2_queue, struct sprd_isp_video_node, vb2_queue);
	unsigned int i;

	for (i = 0; i < node->fmt.num_planes; i++) {
		if (vb2_plane_size(vb, i) < node->fmt.plane_fmt[i].sizeimage)
			return -EINVAL;

		vb2_set_plane_payload(vb, i, node->fmt.plane_fmt[i].sizeimage);
	}

	return 0;
}

static void sprd_isp_buf_queue(struct vb2_buffer *vb)
{
	struct sprd_isp_video_node *node =
		container_of(vb->vb2_queue, struct sprd_isp_video_node, vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_isp_video_buffer *buffer =
		container_of(vbuf, struct sprd_isp_video_buffer, vbuf);
	struct sprd_isp *isp = node->isp;
	unsigned long flags;

	spin_lock_irqsave(&isp->lock, flags);

	list_add_tail(&buffer->link, &node->buf_queue);

	if (isp->context[node->ctx_id].streaming & BIT(node->index))
		sprd_isp_schedule(node->isp, node->ctx_id);

	spin_unlock_irqrestore(&isp->lock, flags);
}

static void sprd_isp_flush_queue(struct sprd_isp_video_node *node,
				 enum vb2_buffer_state state)
{
	struct sprd_isp_video_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&node->isp->lock, flags);

	while (!list_empty(&node->buf_queue)) {
		buf = list_first_entry(&node->buf_queue,
				       struct sprd_isp_video_buffer, link);
		list_del(&buf->link);
		vb2_buffer_done(&buf->vbuf.vb2_buf, state);
	}

	spin_unlock_irqrestore(&node->isp->lock, flags);
}

static int sprd_isp_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct sprd_isp_video_node *node =
		container_of(q, struct sprd_isp_video_node, vb2_queue);
	struct sprd_isp *isp = node->isp;
	struct sprd_isp_ctx *ctx = &isp->context[node->ctx_id];
	unsigned long flags;
	int ret;

	ret = pm_runtime_resume_and_get(isp->dev);
	if (ret < 0)
		goto err_return_buffers;

	spin_lock_irqsave(&isp->lock, flags);

	ctx->streaming |= BIT(node->index);

	if (((ctx->streaming & BIT(SPRD_ISP_INPUT)) &&
	     node->index == SPRD_ISP_OUTPUT) ||
	    ((ctx->streaming & BIT(SPRD_ISP_OUTPUT)) &&
	     node->index == SPRD_ISP_INPUT)) {
		ret = sprd_isp_start(isp, node->ctx_id);
		if (ret) {
			spin_unlock_irqrestore(&isp->lock, flags);
			goto err_pm_put;
		}
	}

	spin_unlock_irqrestore(&isp->lock, flags);

	return 0;

err_pm_put:
	pm_runtime_put_autosuspend(isp->dev);
err_return_buffers:
	sprd_isp_flush_queue(node, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void sprd_isp_next_buffer(struct sprd_isp_video_node *node)
{
	struct sprd_isp_video_buffer *buf;

	buf = list_first_entry_or_null(&node->buf_queue,
				       struct sprd_isp_video_buffer, link);
	if (buf)
		list_del(&buf->link);

	node->current_buf = buf;
}

bool sprd_isp_get_addrs(struct sprd_isp_video_node *node,
			dma_addr_t *addr, u32 *pitch)
{
	const struct sprd_isp_format *fmt = node->format_info;
	struct vb2_buffer *vb;
	unsigned int i;

	lockdep_assert_held(&node->isp->lock);

	if (!node->current_buf)
		sprd_isp_next_buffer(node);

	if (!node->current_buf)
		return false;

	vb = &node->current_buf->vbuf.vb2_buf;

	for (i = 0; i < node->fmt.num_planes; i++) {
		addr[i] = vb2_dma_contig_plane_dma_addr(vb, i);
		addr[i] += node->fmt.plane_fmt[i].bytesperline * node->rect.top;
		addr[i] += (node->rect.left / fmt->hsize_unit) *
			fmt->hsize_unit * fmt->plane[i].bpp_line / 8;
		/* For MIPI packed formats, point to the pixel's first byte */
		addr[i] += node->rect.left % fmt->hsize_unit;

		pitch[i] = node->fmt.plane_fmt[i].bytesperline;
	}

	if (node->fmt.pixelformat == V4L2_PIX_FMT_NV16 ||
	    node->fmt.pixelformat == V4L2_PIX_FMT_NV61 ||
	    node->fmt.pixelformat == V4L2_PIX_FMT_NV12 ||
	    node->fmt.pixelformat == V4L2_PIX_FMT_NV21) {
		unsigned int uv_div =
			(node->fmt.pixelformat == V4L2_PIX_FMT_NV16 ||
			 node->fmt.pixelformat == V4L2_PIX_FMT_NV61) ? 1 : 2;

		addr[1] = vb2_dma_contig_plane_dma_addr(vb, 0);
		addr[1] += node->fmt.plane_fmt[0].bytesperline *
			(node->fmt.height + node->rect.top / uv_div);
		addr[1] += node->rect.left;

		pitch[1] = pitch[0];
	}

	return true;
}

void sprd_isp_video_done(struct sprd_isp_video_node *node)
{
	struct sprd_isp_video_buffer *buf = node->current_buf;

	lockdep_assert_held(&node->isp->lock);

	if (!buf)
		return;

	buf->vbuf.sequence = node->isp->context[node->ctx_id].sequence;
	buf->vbuf.vb2_buf.timestamp = ktime_get_boottime_ns();
	vb2_buffer_done(&buf->vbuf.vb2_buf, VB2_BUF_STATE_DONE);

	node->current_buf = NULL;
}

void sprd_isp_video_cancel(struct sprd_isp_video_node *node)
{
	struct sprd_isp_video_buffer *buf = node->current_buf;

	lockdep_assert_held(&node->isp->lock);

	if (!buf)
		return;

	vb2_buffer_done(&buf->vbuf.vb2_buf, VB2_BUF_STATE_ERROR);

	node->current_buf = NULL;
}

static void sprd_isp_stop_streaming(struct vb2_queue *q)
{
	struct sprd_isp_video_node *node =
		container_of(q, struct sprd_isp_video_node, vb2_queue);
	struct sprd_isp *isp = node->isp;

	sprd_isp_flush_queue(node, VB2_BUF_STATE_ERROR);

	sprd_isp_try_stop(isp, node->ctx_id, node->index);

	/*
	 * The hardware does not support stopping a single context immediately,
	 * so we need to wait for all buffers to be returned. If this is the
	 * last context being stopped, all buffers should have been returned
	 * in sprd_isp_try_stop by now.
	 */
	vb2_wait_for_all_buffers(&node->vb2_queue);

	pm_runtime_mark_last_busy(isp->dev);
	pm_runtime_put_autosuspend(isp->dev);
}

static const struct vb2_ops sprd_isp_vb2_ops = {
	.queue_setup	 = sprd_isp_queue_setup,
	.buf_prepare	 = sprd_isp_buf_prepare,
	.buf_queue	 = sprd_isp_buf_queue,
	.start_streaming = sprd_isp_start_streaming,
	.stop_streaming	 = sprd_isp_stop_streaming,
};

static const struct v4l2_file_operations sprd_isp_fops = {
	.owner          = THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = vb2_fop_release,
	.poll           = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = vb2_fop_mmap
};

static const struct v4l2_ioctl_ops sprd_isp_ioctl_ops = {
	.vidioc_querycap = sprd_isp_querycap,
	.vidioc_g_fmt_vid_cap_mplane = sprd_isp_g_fmt,
	.vidioc_g_fmt_vid_out_mplane = sprd_isp_g_fmt,
	.vidioc_try_fmt_vid_cap_mplane = sprd_isp_try_fmt,
	.vidioc_try_fmt_vid_out_mplane = sprd_isp_try_fmt,
	.vidioc_s_fmt_vid_cap_mplane = sprd_isp_s_fmt,
	.vidioc_s_fmt_vid_out_mplane = sprd_isp_s_fmt,
	.vidioc_enum_fmt_vid_cap = sprd_isp_enum_fmt,
	.vidioc_enum_fmt_vid_out = sprd_isp_enum_fmt,
	.vidioc_enum_framesizes = sprd_isp_enum_framesizes,
	.vidioc_g_selection = sprd_isp_g_selection,
	.vidioc_s_selection = sprd_isp_s_selection,
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

int sprd_isp_video_register(struct sprd_isp_video_node *node)
{
	struct v4l2_format format = {
		.fmt.pix = {
			.width = 1280,
			.height = 960,
			.pixelformat = V4L2_PIX_FMT_NV12,
		},
	};
	struct sprd_isp *isp = node->isp;
	struct video_device *vdev = &node->vdev;
	struct vb2_queue *q = &node->vb2_queue;
	struct v4l2_subdev *sd = &isp->context[node->ctx_id].sd;
	int ret;

	mutex_init(&node->lock);
	INIT_LIST_HEAD(&node->buf_queue);

	vdev->v4l2_dev = &isp->v4l2_dev;
	vdev->fops = &sprd_isp_fops;
	vdev->release = video_device_release_empty;
	vdev->ioctl_ops = &sprd_isp_ioctl_ops;
	vdev->lock = &node->lock;

	video_set_drvdata(vdev, node);

	q->io_modes = VB2_DMABUF | VB2_MMAP;
	q->ops = &sprd_isp_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->buf_struct_size = sizeof(struct sprd_isp_video_buffer);
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

	node->pad.flags = vdev->vfl_dir == VFL_DIR_TX
		? MEDIA_PAD_FL_SOURCE : MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vdev->entity, 1, &node->pad);
	if (ret)
		goto err_release_queue;

	format.type = q->type;
	__sprd_isp_try_fmt(node, &format);
	node->fmt = format.fmt.pix_mp;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(isp->dev, "failed to register %s: %d\n", vdev->name,
			ret);
		goto err_cleanup_entity;
	}

	if (vdev->vfl_dir == VFL_DIR_TX)
		ret = media_create_pad_link(&vdev->entity, 0,
					    &sd->entity, node->index,
					    MEDIA_LNK_FL_IMMUTABLE |
					    MEDIA_LNK_FL_ENABLED);
	else
		ret = media_create_pad_link(&sd->entity, node->index,
					    &vdev->entity, 0,
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

void sprd_isp_video_unregister(struct sprd_isp_video_node *node)
{
	video_unregister_device(&node->vdev);
	media_entity_cleanup(&node->vdev.entity);
	vb2_queue_release(&node->vb2_queue);
	mutex_destroy(&node->lock);
}
