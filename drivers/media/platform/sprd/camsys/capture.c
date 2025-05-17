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

static int sprd_cap_querycap(struct file *file, void *fh,
			     struct v4l2_capability *cap)
{
	strscpy(cap->driver, "sprd-camsys", sizeof(cap->driver));
	strscpy(cap->card, "Unisoc Camera Subsystem", sizeof(cap->card));

	return 0;
}

static int sprd_cap_enum_fmt(struct file *file, void *fh,
			     struct v4l2_fmtdesc *f)
{
	struct sprd_capture_vdev *cap = video_drvdata(file);
	const struct sprd_camsys_fmt_config *fmt;
	int i, n = -1;

	for (i = 0; i < cap->camsys->hw->num_formats; i++) {
		fmt = &cap->camsys->hw->formats[i];

		if (f->mbus_code && fmt->mbus_code != f->mbus_code)
			continue;

		if (++n == f->index)
			break;
	}

	if (n < f->index)
		return -EINVAL;

	f->pixelformat = fmt->pixelformat;

	return 0;
}

static int sprd_cap_enum_framesizes(struct file *file, void *fh,
				    struct v4l2_frmsizeenum *fsize)
{
	struct sprd_capture_vdev *cap = video_drvdata(file);

	if (fsize->index)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	fsize->stepwise.min_width = 64;
	fsize->stepwise.max_width = cap->camsys->hw->max_width;
	fsize->stepwise.min_height = 64;
	fsize->stepwise.max_height = cap->camsys->hw->max_height;
	fsize->stepwise.step_width = 1;
	fsize->stepwise.step_height = 1;

	return 0;
}

static int sprd_cap_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct sprd_capture_vdev *cap = video_drvdata(file);

	f->fmt.pix = cap->fmt;

	return 0;
}

static void __sprd_cap_try_fmt(struct sprd_capture_vdev *cap,
			       struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;
	const struct sprd_camsys_fmt_config *fmt;
	u32 bpl;
	int i;

	for (i = 0; i < cap->camsys->hw->num_formats; i++) {
		fmt = &cap->camsys->hw->formats[i];

		if (fmt->pixelformat == pix->pixelformat)
			break;
	}

	if (i == cap->camsys->hw->num_formats)
		fmt = &cap->camsys->hw->formats[0];

	pix->pixelformat = fmt->pixelformat;
	pix->width = clamp_t(u32, pix->width, 1,
			     cap->camsys->hw->max_width);
	pix->height = clamp_t(u32, pix->height, 1,
			      cap->camsys->hw->max_height);

	bpl = DIV_ROUND_UP(pix->width, fmt->hsize_unit) *
		fmt->hsize_unit * fmt->bits / 8;
	bpl = ALIGN(bpl, SPRD_DCAM_PITCH_STEP);
	pix->bytesperline = bpl;
	pix->sizeimage = pix->height * bpl;

	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_DEFAULT;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_DEFAULT;
}

static int sprd_cap_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct sprd_capture_vdev *cap = video_drvdata(file);

	if (vb2_is_busy(&cap->queue))
		return -EBUSY;

	__sprd_cap_try_fmt(cap, f);

	cap->fmt = f->fmt.pix;

	return 0;
}

static int sprd_cap_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct sprd_capture_vdev *cap = video_drvdata(file);

	__sprd_cap_try_fmt(cap, f);

	return 0;
}

static int sprd_cap_queue_setup(struct vb2_queue *q,
				unsigned int *num_buffers,
				unsigned int *num_planes,
				unsigned int sizes[],
				struct device *alloc_devs[])
{
	struct sprd_capture_vdev *cap =
		container_of(q, struct sprd_capture_vdev, queue);
	const struct v4l2_pix_format *pix = &cap->fmt;

	if (*num_planes) {
		if (*num_planes != 1 || sizes[0] < pix->sizeimage)
			return -EINVAL;
	} else {
		*num_planes = 1;
		sizes[0] = pix->sizeimage;
	}

	return 0;
}

static int sprd_cap_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_capture_buffer *buffer =
		container_of(vbuf, struct sprd_capture_buffer, vbuf);

	buffer->addr = vb2_dma_contig_plane_dma_addr(vb, 0);

	return 0;
}

static int sprd_cap_buf_prepare(struct vb2_buffer *vb)
{
	struct sprd_capture_vdev *cap =
		container_of(vb->vb2_queue, struct sprd_capture_vdev, queue);
	const struct v4l2_pix_format *pix = &cap->fmt;

	if (vb2_plane_size(vb, 0) < pix->sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, pix->sizeimage);

	return 0;
}

static void sprd_cap_buf_queue(struct vb2_buffer *vb)
{
	struct sprd_capture_vdev *cap =
		container_of(vb->vb2_queue, struct sprd_capture_vdev, queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sprd_capture_buffer *buffer =
		container_of(vbuf, struct sprd_capture_buffer, vbuf);
	unsigned long flags;

	spin_lock_irqsave(&cap->buf_lock, flags);
	list_add_tail(&buffer->link, &cap->buf_queue);
	spin_unlock_irqrestore(&cap->buf_lock, flags);
}

static void sprd_cap_cancel_stream(struct sprd_capture_vdev *cap,
				   enum vb2_buffer_state state)
{
	struct sprd_capture_buffer *buf;
	unsigned long flags;

	spin_lock_irqsave(&cap->buf_lock, flags);

	while (!list_empty(&cap->buf_queue)) {
		buf = list_first_entry(&cap->buf_queue,
				       struct sprd_capture_buffer, link);
		list_del(&buf->link);
		vb2_buffer_done(&buf->vbuf.vb2_buf, state);
	}

	if (cap->current_buf) {
		vb2_buffer_done(&cap->current_buf->vbuf.vb2_buf, state);
		cap->current_buf = NULL;
	}

	if (cap->next_buf) {
		vb2_buffer_done(&cap->next_buf->vbuf.vb2_buf, state);
		cap->next_buf = NULL;
	}

	spin_unlock_irqrestore(&cap->buf_lock, flags);
}

static int sprd_cap_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct sprd_capture_vdev *cap =
		container_of(q, struct sprd_capture_vdev, queue);
	struct video_device *vdev = &cap->vdev;
	struct media_entity *entity;
	struct media_pad *pad;
	struct v4l2_subdev *subdev;
	int ret;

	ret = video_device_pipeline_alloc_start(vdev);
	if (ret < 0) {
		dev_err(q->dev, "failed to start media pipeline: %d\n", ret);
		goto err_flush;
	}

	ret = v4l2_pipeline_pm_get(&vdev->entity);
	if (ret) {
		dev_err(q->dev, "failed to power up pipeline: %d\n", ret);
		goto err_stop;
	}

	entity = &vdev->entity;
	while (1) {
		pad = &entity->pads[0];
		if (!(pad->flags & MEDIA_PAD_FL_SINK))
			break;

		pad = media_pad_remote_pad_first(pad);
		if (!pad || !is_media_entity_v4l2_subdev(pad->entity))
			break;

		entity = pad->entity;
		subdev = media_entity_to_v4l2_subdev(entity);

		ret = v4l2_subdev_call(subdev, video, s_stream, 1);
		if (ret < 0 && ret != -ENOIOCTLCMD)
			goto err_pm_put;
	}

	return 0;

err_pm_put:
	v4l2_pipeline_pm_put(&vdev->entity);
err_stop:
	video_device_pipeline_stop(vdev);
err_flush:
	sprd_cap_cancel_stream(cap, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void sprd_cap_stop_streaming(struct vb2_queue *q)
{
	struct sprd_capture_vdev *cap =
		container_of(q, struct sprd_capture_vdev, queue);
	struct video_device *vdev = &cap->vdev;
	struct media_entity *entity;
	struct media_pad *pad;
	struct v4l2_subdev *subdev;
	int ret;

	entity = &vdev->entity;
	while (1) {
		pad = &entity->pads[0];
		if (!(pad->flags & MEDIA_PAD_FL_SINK))
			break;

		pad = media_pad_remote_pad_first(pad);
		if (!pad || !is_media_entity_v4l2_subdev(pad->entity))
			break;

		entity = pad->entity;
		subdev = media_entity_to_v4l2_subdev(entity);

		ret = v4l2_subdev_call(subdev, video, s_stream, 0);
		if (ret < 0 && ret != -ENOIOCTLCMD)
			dev_err(cap->queue.dev,
				"subdev %s failed to stop: %d\n", subdev->name,
				ret);
	}

	v4l2_pipeline_pm_put(&vdev->entity);
	video_device_pipeline_stop(vdev);

	sprd_cap_cancel_stream(cap, VB2_BUF_STATE_ERROR);
}

static int sprd_cap_link_validate(struct media_link *link)
{
	struct video_device *vdev =
		media_entity_to_video_device(link->sink->entity);
	struct v4l2_subdev *sd =
		media_entity_to_v4l2_subdev(link->source->entity);
	struct sprd_capture_vdev *cap = video_get_drvdata(vdev);
	const struct sprd_camsys_fmt_config *fmt;
	struct v4l2_subdev_format sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = link->source->index,
	};
	int i, ret;

	ret = v4l2_subdev_call(sd, pad, get_fmt, NULL, &sd_fmt);
	if (ret)
		return ret;

	if (sd_fmt.format.height != cap->fmt.height ||
	    sd_fmt.format.width != cap->fmt.width) {
		dev_err(cap->queue.dev,
			"link \"%s\":%u -> \"%s\":%u size mismatch: %ux%u != %ux%u\n",
			link->source->entity->name, link->source->index,
			link->sink->entity->name, link->sink->index,
			sd_fmt.format.width, sd_fmt.format.height,
			cap->fmt.width, cap->fmt.height);
		return -EPIPE;
	}

	for (i = 0; i < cap->camsys->hw->num_formats; i++) {
		fmt = &cap->camsys->hw->formats[i];

		if (fmt->mbus_code == sd_fmt.format.code &&
		    fmt->pixelformat == cap->fmt.pixelformat)
			break;
	}

	if (i == cap->camsys->hw->num_formats) {
		dev_err(cap->queue.dev,
			"link \"%s\":%u -> \"%s\":%u format mismatch\n",
			link->source->entity->name, link->source->index,
			link->sink->entity->name, link->sink->index);
		return -EPIPE;
	}

	return 0;
}

static const struct v4l2_ioctl_ops sprd_capture_ioctl_ops = {
	.vidioc_querycap		= sprd_cap_querycap,
	.vidioc_enum_fmt_vid_cap	= sprd_cap_enum_fmt,
	.vidioc_enum_framesizes		= sprd_cap_enum_framesizes,
	.vidioc_g_fmt_vid_cap		= sprd_cap_g_fmt,
	.vidioc_s_fmt_vid_cap		= sprd_cap_s_fmt,
	.vidioc_try_fmt_vid_cap		= sprd_cap_try_fmt,
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

static const struct v4l2_file_operations sprd_capture_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
};

static const struct vb2_ops sprd_capture_vb2_ops = {
	.queue_setup	 = sprd_cap_queue_setup,
	.buf_init	 = sprd_cap_buf_init,
	.buf_prepare	 = sprd_cap_buf_prepare,
	.buf_queue	 = sprd_cap_buf_queue,
	.start_streaming = sprd_cap_start_streaming,
	.stop_streaming	 = sprd_cap_stop_streaming,
};

static const struct media_entity_operations sprd_capture_media_ops = {
	.link_validate = sprd_cap_link_validate,
};

int sprd_capture_vdev_register(struct sprd_capture_vdev *cap)
{
	struct v4l2_format format = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.fmt.pix = {
			.width = 1280,
			.height = 960,
			.pixelformat = cap->camsys->hw->formats[0].pixelformat,
		},
	};
	struct sprd_camsys *cs = cap->camsys;
	struct video_device *vdev = &cap->vdev;
	struct vb2_queue *q = &cap->queue;
	int ret;

	mutex_init(&cap->lock);
	spin_lock_init(&cap->buf_lock);
	INIT_LIST_HEAD(&cap->buf_queue);

	/* vdev->name is set by the caller */
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_IO_MC;
	vdev->v4l2_dev = &cs->v4l2_dev;
	vdev->fops = &sprd_capture_fops;
	vdev->release = video_device_release_empty;
	vdev->ioctl_ops = &sprd_capture_ioctl_ops;
	vdev->lock = &cap->lock;
	vdev->entity.ops = &sprd_capture_media_ops;
	vdev->vfl_dir = VFL_DIR_RX;

	video_set_drvdata(vdev, cap);

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_DMABUF | VB2_MMAP;
	q->ops = &sprd_capture_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->buf_struct_size = sizeof(struct sprd_capture_buffer);
	q->min_queued_buffers = 1;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &cap->lock;
	q->dev = &cs->pdev->dev;
	ret = vb2_queue_init(q);
	if (ret) {
		dev_err(&cs->pdev->dev, "failed to init vb2 queue: %d\n", ret);
		goto err_destroy_mutex;
	}

	vdev->queue = q;

	cap->pads[SPRD_CAPTURE_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vdev->entity, SPRD_CAPTURE_PAD_NUM,
				     cap->pads);
	if (ret)
		goto err_release_queue;

	__sprd_cap_try_fmt(cap, &format);
	cap->fmt = format.fmt.pix;

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
	mutex_destroy(&cap->lock);
	return ret;
}

void sprd_capture_vdev_unregister(struct sprd_capture_vdev *cap)
{
	video_unregister_device(&cap->vdev);
	media_entity_cleanup(&cap->vdev.entity);
	vb2_queue_release(&cap->queue);
	mutex_destroy(&cap->lock);
}

struct sprd_capture_buffer *
sprd_capture_next_frame(struct sprd_capture_vdev *cap)
{
	unsigned long flags;

	spin_lock_irqsave(&cap->buf_lock, flags);

	if (cap->current_buf) {
		dev_err(&cap->camsys->pdev->dev,
			"capture buffer was not done before next frame!\n");
		spin_unlock_irqrestore(&cap->buf_lock, flags);
		return NULL;
	}

	/* The previously staged buffer is now being processed. */
	cap->current_buf = cap->next_buf;

	/* Stage a new buffer. */
	if (!list_empty(&cap->buf_queue)) {
		cap->next_buf = list_first_entry(&cap->buf_queue,
						 struct sprd_capture_buffer,
						 link);
		list_del(&cap->next_buf->link);
	} else {
		cap->next_buf = NULL;
	}

	spin_unlock_irqrestore(&cap->buf_lock, flags);

	return cap->next_buf;
}

void sprd_capture_done(struct sprd_capture_vdev *cap, unsigned int sequence)
{
	struct sprd_capture_buffer *buffer;

	spin_lock(&cap->buf_lock);

	buffer = cap->current_buf;
	if (buffer) {
		buffer->vbuf.vb2_buf.timestamp = ktime_get_ns();
		buffer->vbuf.sequence = sequence;
		vb2_buffer_done(&buffer->vbuf.vb2_buf, VB2_BUF_STATE_DONE);
		cap->current_buf = NULL;
	}

	spin_unlock(&cap->buf_lock);
}
