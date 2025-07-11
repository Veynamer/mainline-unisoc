// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unisoc ISP driver
 *
 * Copyright (C) 2025 Otto Pflüger
 */

#ifndef __SPRD_ISP_H
#define __SPRD_ISP_H

#include <linux/reset.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>
#include <uapi/linux/media/sprd/camsys-config.h>

#define SPRD_ISP_NUM_IRQS	2
#define SPRD_ISP_NUM_CONTEXTS	4

#define SPRD_ISP_REG_SIZE	0x40000

struct sprd_isp_config_node {
	struct sprd_isp *isp;
	unsigned int ctx_id;

	struct mutex lock;
	struct video_device vdev;
	struct media_pad pad;

	struct vb2_queue vb2_queue;
	struct list_head buf_queue;
};

struct sprd_isp_video_node {
	struct sprd_isp *isp;
	unsigned int ctx_id;
	unsigned int index;

	struct mutex lock;
	struct video_device vdev;
	struct media_pad pad;

	struct vb2_queue vb2_queue;
	struct list_head buf_queue;

	struct sprd_isp_video_buffer *current_buf;

	const struct sprd_isp_format *format_info;
	struct v4l2_pix_format_mplane fmt;

	struct v4l2_rect rect;
	unsigned int total_width;
};

enum {
	SPRD_ISP_CONFIG,
	SPRD_ISP_INPUT,
	SPRD_ISP_OUTPUT,
	SPRD_ISP_NUM_NODES
};

struct sprd_isp_ctx {
	struct v4l2_subdev sd;
	struct media_pad pad[SPRD_ISP_NUM_NODES];

	struct sprd_isp_config_node config;
	struct sprd_isp_video_node input;
	struct sprd_isp_video_node output;

	struct v4l2_rect crop_rect;
	unsigned int slice_idx;
	unsigned int num_slices;

	unsigned int sequence;
	u32 streaming;
	bool running;
	void *cfg_base;
	dma_addr_t cfg_dma_addr;
};

struct sprd_isp {
	struct device *dev;
	const struct sprd_isp_hardware *hw;

	struct v4l2_device v4l2_dev;
	struct media_device mdev;

	struct reset_control *reset;
	struct reset_control *ahb_reset;
	struct clk *clk;
	int irq[SPRD_ISP_NUM_IRQS];
	void __iomem *base;

	spinlock_t lock;
	struct sprd_isp_ctx context[SPRD_ISP_NUM_CONTEXTS];
};

/* Used by the video device implementation */
void sprd_isp_schedule(struct sprd_isp *isp, int ctx_id);
int sprd_isp_start(struct sprd_isp *isp, int ctx_id);
void sprd_isp_try_stop(struct sprd_isp *isp, int ctx_id, int node_idx);

/* Provided by the video device implementation */
int sprd_isp_video_register(struct sprd_isp_video_node *node);
void sprd_isp_video_unregister(struct sprd_isp_video_node *node);

bool sprd_isp_get_addrs(struct sprd_isp_video_node *node,
			dma_addr_t *addr, u32 *pitch);
void sprd_isp_video_done(struct sprd_isp_video_node *node);
void sprd_isp_video_cancel(struct sprd_isp_video_node *node);

int sprd_isp_config_register(struct sprd_isp_config_node *node);
void sprd_isp_config_unregister(struct sprd_isp_config_node *node);

void sprd_isp_configure(struct sprd_isp_config_node *node);

/* Used by hardware-specific code */
void sprd_isp_process(struct sprd_isp *isp, int ctx_id);

typedef void (*sprd_isp_cfg_handler_t)(struct sprd_isp *isp, int ctx_id,
				       union sprd_camsys_block_config *block);

struct sprd_isp_hardware {
	void (*init_regs)(struct sprd_isp *isp);
	int (*set_formats)(struct sprd_isp *isp, int ctx_id);
	int (*start_slice)(struct sprd_isp *isp, int ctx_id,
			   dma_addr_t *in_addr, dma_addr_t *out_addr,
			   u32 *in_pitch, u32 *out_pitch);
	void (*stop)(struct sprd_isp *isp);
	irqreturn_t (*handle_irq)(int irq, void *devdata);

	sprd_isp_cfg_handler_t config_block[SPRD_ISP_BLOCK_NUM];

	unsigned int max_width;
	unsigned int max_height;
};

extern const struct sprd_isp_hardware ums9230_isp_hw;

#endif
