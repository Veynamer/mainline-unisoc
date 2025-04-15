// SPDX-License-Identifier: GPL-2.0-only
#ifndef __SOC_SPRD_AGDSP_H
#define __SOC_SPRD_AGDSP_H

struct sprd_agdsp_ipc;

enum {
	AGDSP_CH_VBC_CTL,
	AGDSP_CH_MP3_OFFLOAD,
	AGDSP_CH_DSP_ASSERT_CTL,
	AGDSP_CH_DSP_PCM,
	AGDSP_CH_DSP_LOG,
	AGDSP_CH_DSP_MEM,
	AGDSP_CH_MP3_OFFLOAD_DRAIN,
	AGDSP_CH_DSP_GET_PARAM_FROM_SMSG_NOREPLY,
	AGDSP_CH_EFFECT_OFFLOAD,
	AGDSP_CH_RECORD_PROCESS,
	AGDSP_CH_DVFS,
	AGDSP_CH_DSP_BTHAL,
	AGDSP_CH_DSP_HIFI,
};

int sprd_agdsp_send_msg(struct sprd_agdsp_ipc *ipc, u16 channel, u16 cmd,
			u32 param0, u32 param1, u32 param2, u32 param3);
int sprd_agdsp_send_cmd(struct sprd_agdsp_ipc *ipc, u16 channel, u16 cmd,
			s32 id, s32 stream, void *params, size_t param_size);
int sprd_agdsp_shm_cmd(struct sprd_agdsp_ipc *ipc, u16 channel, u16 cmd,
		       s32 id, s32 stream, u32 type, dma_addr_t addr,
		       size_t size);

struct sprd_agdsp_ipc *sprd_get_agdsp_ipc(struct device_node *node,
					  const char *name);
void sprd_put_agdsp_ipc(struct sprd_agdsp_ipc *ipc);

#endif
