/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Otto Pflüger
 */

#ifndef __SPRD_CAMSYS_COMMON_H
#define __SPRD_CAMSYS_COMMON_H

#include <dt-bindings/media/sprd-camsys.h>
#include <media/media-device.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>
#include <uapi/linux/media/sprd/camsys-config.h>

#define SPRD_CSI_NUM		3
#define SPRD_DCAM_NUM		2

#define SPRD_DCAM_PITCH_STEP	16

struct sprd_camsys_fmt_config {
	u32 mbus_code;
	u32 pixelformat;
	unsigned int hsize_unit;
	unsigned int bits;
};

#define SPRD_CAMSYS_FMT(_code, _fmt, _hsize_unit, _bits)	\
{								\
	.mbus_code = MEDIA_BUS_FMT_##_code,			\
	.pixelformat = V4L2_PIX_FMT_##_fmt,			\
	.hsize_unit = _hsize_unit,				\
	.bits = _bits,						\
}

struct sprd_config_buffer {
	struct vb2_v4l2_buffer vbuf;
	struct list_head link;
	void *config;
};

enum {
	SPRD_CONFIG_PAD_SRC,
	SPRD_CONFIG_PAD_NUM
};

struct sprd_config_vdev {
	struct video_device vdev;
	struct vb2_queue queue;
	struct media_pad pads[SPRD_CONFIG_PAD_NUM];
	struct mutex lock;

	struct sprd_camsys *camsys;

	spinlock_t buf_lock;
	struct list_head buf_queue;
};

struct sprd_capture_buffer {
	struct vb2_v4l2_buffer vbuf;
	struct list_head link;
	dma_addr_t addr;
};

enum {
	SPRD_CAPTURE_PAD_SINK,
	SPRD_CAPTURE_PAD_NUM
};

struct sprd_capture_vdev {
	struct video_device vdev;
	struct vb2_queue queue;
	struct media_pad pads[SPRD_CAPTURE_PAD_NUM];
	struct v4l2_pix_format fmt;
	struct mutex lock;

	struct sprd_camsys *camsys;

	spinlock_t buf_lock;
	struct list_head buf_queue;
	struct sprd_capture_buffer *current_buf;
	struct sprd_capture_buffer *next_buf;
};

enum {
	SPRD_CAMSYS_STATS_AE = BIT(0),
};

struct sprd_stats_buffer {
	struct vb2_v4l2_buffer vbuf;
	struct list_head link;
	unsigned long blocks_enabled;
	unsigned long blocks_done;
	dma_addr_t ae_addr;
};

enum {
	SPRD_STATS_PAD_SINK,
	SPRD_STATS_PAD_NUM
};

struct sprd_stats_vdev {
	struct video_device vdev;
	struct vb2_queue queue;
	struct media_pad pads[SPRD_STATS_PAD_NUM];
	struct mutex lock;

	struct sprd_camsys *camsys;

	spinlock_t buf_lock;
	struct list_head buf_queue;
	struct sprd_stats_buffer *current_buf;
	struct sprd_stats_buffer *next_buf;
	unsigned long blocks_enabled;
};

enum {
	SPRD_CSI_PAD_SINK,
	SPRD_CSI_PAD_SRC,
	SPRD_CSI_PAD_NUM
};

struct sprd_csi {
	const struct sprd_csi_hw_ops *hw;
	struct sprd_camsys *camsys;
	struct device *dev;
	int index;

	struct v4l2_subdev subdev;
	struct media_pad pads[SPRD_CSI_PAD_NUM];

	struct clk *clk;
	struct clk *mipi_clk;
	void __iomem *regs;
	unsigned int phy_id;
	unsigned int dcam_id;
};

struct sprd_csi_hw_ops {
	void (*power_phy)(struct sprd_csi *csi, int enable);
	void (*setup_phy)(struct sprd_csi *csi, int enable);
	void (*connect_dcam)(struct sprd_csi *csi, int enable);
};

enum {
	SPRD_DCAM_PAD_INPUT,
	SPRD_DCAM_PAD_CONFIG,
	SPRD_DCAM_PAD_CAPTURE,
	SPRD_DCAM_PAD_STATS,
	SPRD_DCAM_PAD_NUM
};

struct sprd_dcam {
	struct v4l2_subdev subdev;
	struct media_pad pads[SPRD_DCAM_PAD_NUM];
	struct sprd_config_vdev config;
	struct sprd_capture_vdev capture;
	struct sprd_stats_vdev stats;

	const struct sprd_dcam_hw_ops *hw;
	struct sprd_camsys *camsys;
	struct device *dev;
	int index;

	struct reset_control *reset;
	void __iomem *regs;
	int irq;

	unsigned int sequence;
};

typedef void (*sprd_dcam_cfg_handler_t)(struct sprd_dcam *dcam,
					union sprd_camsys_block_config *block);

struct sprd_dcam_hw_ops {
	sprd_dcam_cfg_handler_t config_block[SPRD_DCAM_BLOCK_NUM];

	void (*set_path)(struct sprd_dcam *dcam, u32 mbus_code, u32 width,
			 u32 height);
	void (*set_capture_buf)(struct sprd_dcam *dcam,
				struct sprd_capture_buffer *buf);
	void (*set_stats_buf)(struct sprd_dcam *dcam,
			      struct sprd_stats_buffer *buf);
	void (*start)(struct sprd_dcam *dcam);
	void (*stop)(struct sprd_dcam *dcam);
	irqreturn_t (*handle_irq)(int irq, void *irqdata);
};

struct sprd_camsys_sensor_link {
	struct v4l2_async_connection async;
	unsigned int phy_id;
};

struct sprd_camsys {
	const struct sprd_camsys_hardware *hw;
	struct media_device mdev;
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;
	struct sprd_csi csi[SPRD_CSI_NUM];
	struct sprd_dcam dcam[SPRD_DCAM_NUM];

	struct platform_device *pdev;
	struct regmap *cam_ahb_regs;
	struct regmap *anlg_phy_regs;
	struct clk *dcam_clk;
	struct clk *cphy_cfg_en;
	struct clk *mipi_gate;
};

struct sprd_camsys_hardware {
	const struct sprd_csi_hw_ops *csi_ops;
	const struct sprd_dcam_hw_ops *dcam_ops;
	unsigned int max_width;
	unsigned int max_height;
	const struct sprd_camsys_fmt_config *formats;
	unsigned int num_formats;
};

extern const struct sprd_camsys_hardware ums9230_camsys_info;

static inline int sprd_csi_port_get_lane_count(unsigned int port)
{
	switch (port) {
	case CAMSYS_CSI_4LANE:
	case CAMSYS_CSI_M_S:
	case CAMSYS_CSI_RO_M_S:
		return 4;
	case CAMSYS_CSI_M:
	case CAMSYS_CSI_S:
	case CAMSYS_CSI_RO_M:
	case CAMSYS_CSI_RO_S:
		return 2;
	default:
		return -EINVAL;
	}
}

int sprd_csi_register(struct sprd_camsys *cs, unsigned int index);
void sprd_csi_unregister(struct sprd_camsys *cs, unsigned int index);

int sprd_dcam_register(struct sprd_camsys *cs, unsigned int index);
void sprd_dcam_unregister(struct sprd_camsys *cs, unsigned int index);

int sprd_config_vdev_register(struct sprd_config_vdev *cap);
void sprd_config_vdev_unregister(struct sprd_config_vdev *cap);

int sprd_capture_vdev_register(struct sprd_capture_vdev *cap);
void sprd_capture_vdev_unregister(struct sprd_capture_vdev *cap);

int sprd_stats_vdev_register(struct sprd_stats_vdev *cap);
void sprd_stats_vdev_unregister(struct sprd_stats_vdev *cap);

void sprd_dcam_sof(struct sprd_dcam *dcam);

void sprd_config_next_frame(struct sprd_dcam *dcam);

struct sprd_capture_buffer *
sprd_capture_next_frame(struct sprd_capture_vdev *cap);

void sprd_capture_done(struct sprd_capture_vdev *cap, unsigned int sequence);

struct sprd_stats_buffer *
sprd_stats_next_frame(struct sprd_stats_vdev *stats);

void sprd_stats_done(struct sprd_stats_vdev *stats, unsigned int sequence,
		     unsigned int block);

#endif
