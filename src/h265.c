/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "mpeg2.h"
#include "context.h"
#include "request.h"
#include "surface.h"

#include <assert.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>
#include <hevc-ctrls.h>

#include "v4l2.h"

#define H265_NAL_UNIT_TYPE_SHIFT		1
#define H265_NAL_UNIT_TYPE_MASK			((1 << 6) - 1)
#define H265_NUH_TEMPORAL_ID_PLUS1_SHIFT	0
#define H265_NUH_TEMPORAL_ID_PLUS1_MASK		((1 << 3) - 1)

static void h265_fill_pps(VAPictureParameterBufferHEVC *picture,
			  VASliceParameterBufferHEVC *slice,
			  struct v4l2_ctrl_hevc_pps *pps)
{
	memset(pps, 0, sizeof(*pps));

	// Only assign fields that exist in the struct.
	pps->num_extra_slice_header_bits = picture->num_extra_slice_header_bits;
	pps->init_qp_minus26 = picture->init_qp_minus26;
	pps->diff_cu_qp_delta_depth = picture->diff_cu_qp_delta_depth;
	pps->pps_cb_qp_offset = picture->pps_cb_qp_offset;
	pps->pps_cr_qp_offset = picture->pps_cr_qp_offset;
	pps->num_tile_columns_minus1 = picture->num_tile_columns_minus1;
	pps->num_tile_rows_minus1 = picture->num_tile_rows_minus1;
	pps->pps_beta_offset_div2 = picture->pps_beta_offset_div2;
	pps->pps_tc_offset_div2 = picture->pps_tc_offset_div2;
	pps->log2_parallel_merge_level_minus2 = picture->log2_parallel_merge_level_minus2;

	// If you need to set flags (e.g. constrained_intra_pred_flag), set pps->flags with appropriate bitmask.
	pps->flags = 0;
	// Example: if you want to set a specific flag, use the bitmask provided by the kernel headers (if any).
}

static void h265_fill_sps(VAPictureParameterBufferHEVC *picture,
			  struct v4l2_ctrl_hevc_sps *sps)
{
	memset(sps, 0, sizeof(*sps));

	sps->pic_width_in_luma_samples = picture->pic_width_in_luma_samples;
	sps->pic_height_in_luma_samples = picture->pic_height_in_luma_samples;
	sps->bit_depth_luma_minus8 = picture->bit_depth_luma_minus8;
	sps->bit_depth_chroma_minus8 = picture->bit_depth_chroma_minus8;
	sps->log2_max_pic_order_cnt_lsb_minus4 = picture->log2_max_pic_order_cnt_lsb_minus4;
	sps->sps_max_dec_pic_buffering_minus1 = picture->sps_max_dec_pic_buffering_minus1;
	// The following are not always directly available; set 0 if unsure or map from your picture struct if you have the value.
	sps->sps_max_num_reorder_pics = 0;
	sps->sps_max_latency_increase_plus1 = 0;
	sps->log2_min_luma_coding_block_size_minus3 = picture->log2_min_luma_coding_block_size_minus3;
	sps->log2_diff_max_min_luma_coding_block_size = picture->log2_diff_max_min_luma_coding_block_size;
	sps->log2_min_luma_transform_block_size_minus2 = picture->log2_min_transform_block_size_minus2;
	sps->log2_diff_max_min_luma_transform_block_size = picture->log2_diff_max_min_transform_block_size;
	sps->max_transform_hierarchy_depth_inter = picture->max_transform_hierarchy_depth_inter;
	sps->max_transform_hierarchy_depth_intra = picture->max_transform_hierarchy_depth_intra;
	sps->pcm_sample_bit_depth_luma_minus1 = picture->pcm_sample_bit_depth_luma_minus1;
	sps->pcm_sample_bit_depth_chroma_minus1 = picture->pcm_sample_bit_depth_chroma_minus1;
	sps->log2_min_pcm_luma_coding_block_size_minus3 = picture->log2_min_pcm_luma_coding_block_size_minus3;
	sps->log2_diff_max_min_pcm_luma_coding_block_size = picture->log2_diff_max_min_pcm_luma_coding_block_size;
	sps->num_short_term_ref_pic_sets = picture->num_short_term_ref_pic_sets;
	sps->num_long_term_ref_pics_sps = picture->num_long_term_ref_pic_sps;
	sps->chroma_format_idc = picture->pic_fields.bits.chroma_format_idc;
	sps->sps_max_sub_layers_minus1 = 0;
	memset(sps->reserved, 0, sizeof(sps->reserved));
	sps->flags = 0;
}

static void h265_fill_slice_params(VAPictureParameterBufferHEVC *picture,
				   VASliceParameterBufferHEVC *slice,
				   struct object_heap *surface_heap,
				   void *source_data,
				   struct v4l2_ctrl_hevc_slice_params *slice_params)
{
	uint8_t nal_unit_type, nuh_temporal_id_plus1;
	uint8_t *b;
	unsigned int count, i, j;
	uint8_t slice_type;

	// Extract NAL unit header info
	b = (uint8_t *)source_data + slice->slice_data_offset;
	nal_unit_type = (b[0] >> H265_NAL_UNIT_TYPE_SHIFT) & H265_NAL_UNIT_TYPE_MASK;
	nuh_temporal_id_plus1 = (b[1] >> H265_NUH_TEMPORAL_ID_PLUS1_SHIFT) & H265_NUH_TEMPORAL_ID_PLUS1_MASK;

	memset(slice_params, 0, sizeof(*slice_params));

	slice_params->bit_size = slice->slice_data_size * 8;
	// Best guess for data_byte_offset (cannot use data_bit_offset, not present in struct)
	slice_params->data_byte_offset = slice->slice_data_offset + slice->slice_data_byte_offset;
	slice_params->num_entry_point_offsets = 0; // Set to 0 unless you know you need it

	slice_params->nal_unit_type = nal_unit_type;
	slice_params->nuh_temporal_id_plus1 = nuh_temporal_id_plus1;

	slice_type = slice->LongSliceFlags.fields.slice_type;

	slice_params->slice_type = slice_type;
	slice_params->colour_plane_id = slice->LongSliceFlags.fields.color_plane_id;
	slice_params->slice_pic_order_cnt = picture->CurrPic.pic_order_cnt;
	slice_params->num_ref_idx_l0_active_minus1 = slice->num_ref_idx_l0_active_minus1;
	slice_params->num_ref_idx_l1_active_minus1 = slice->num_ref_idx_l1_active_minus1;
	slice_params->collocated_ref_idx = slice->collocated_ref_idx;
	slice_params->five_minus_max_num_merge_cand = slice->five_minus_max_num_merge_cand;
	slice_params->slice_qp_delta = slice->slice_qp_delta;
	slice_params->slice_cb_qp_offset = slice->slice_cb_qp_offset;
	slice_params->slice_cr_qp_offset = slice->slice_cr_qp_offset;
	slice_params->slice_act_y_qp_offset = 0;
	slice_params->slice_act_cb_qp_offset = 0;
	slice_params->slice_act_cr_qp_offset = 0;
	slice_params->slice_beta_offset_div2 = slice->slice_beta_offset_div2;
	slice_params->slice_tc_offset_div2 = slice->slice_tc_offset_div2;
	slice_params->pic_struct = 0; // Set as needed

	slice_params->slice_segment_addr = 0; // Set as needed

	count = slice_params->num_ref_idx_l0_active_minus1 + 1;
	for (i = 0; i < count && slice_type != V4L2_HEVC_SLICE_TYPE_I; i++)
		slice_params->ref_idx_l0[i] = slice->RefPicList[0][i];

	count = slice_params->num_ref_idx_l1_active_minus1 + 1;
	for (i = 0; i < count && slice_type == V4L2_HEVC_SLICE_TYPE_B; i++)
		slice_params->ref_idx_l1[i] = slice->RefPicList[1][i];

	// Weighted prediction table
	slice_params->pred_weight_table.luma_log2_weight_denom = slice->luma_log2_weight_denom;
	slice_params->pred_weight_table.delta_chroma_log2_weight_denom = slice->delta_chroma_log2_weight_denom;
	for (i = 0; i < 15 && slice_type != V4L2_HEVC_SLICE_TYPE_I; i++) {
		slice_params->pred_weight_table.delta_luma_weight_l0[i] = slice->delta_luma_weight_l0[i];
		slice_params->pred_weight_table.luma_offset_l0[i] = slice->luma_offset_l0[i];
		for (j = 0; j < 2; j++) {
			slice_params->pred_weight_table.delta_chroma_weight_l0[i][j] = slice->delta_chroma_weight_l0[i][j];
			slice_params->pred_weight_table.chroma_offset_l0[i][j] = slice->ChromaOffsetL0[i][j];
		}
	}
	for (i = 0; i < 15 && slice_type == V4L2_HEVC_SLICE_TYPE_B; i++) {
		slice_params->pred_weight_table.delta_luma_weight_l1[i] = slice->delta_luma_weight_l1[i];
		slice_params->pred_weight_table.luma_offset_l1[i] = slice->luma_offset_l1[i];
		for (j = 0; j < 2; j++) {
			slice_params->pred_weight_table.delta_chroma_weight_l1[i][j] = slice->delta_chroma_weight_l1[i][j];
			slice_params->pred_weight_table.chroma_offset_l1[i][j] = slice->ChromaOffsetL1[i][j];
		}
	}

	slice_params->short_term_ref_pic_set_size = 0; // Set as needed
	slice_params->long_term_ref_pic_set_size = 0; // Set as needed

	slice_params->flags = 0;
}

int h265_set_controls(struct request_data *driver_data,
		      struct object_context *context_object,
		      struct object_surface *surface_object)
{
	VAPictureParameterBufferHEVC *picture =
		&surface_object->params.h265.picture;
	VASliceParameterBufferHEVC *slice =
		&surface_object->params.h265.slice;
	// VAIQMatrixBufferHEVC *iqmatrix =
	// 	&surface_object->params.h265.iqmatrix;
	// bool iqmatrix_set = surface_object->params.h265.iqmatrix_set;
	struct v4l2_ctrl_hevc_pps pps;
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_slice_params slice_params;
	int rc;

	h265_fill_pps(picture, slice, &pps);
	rc = v4l2_set_control(driver_data->video_fd, surface_object->request_fd,
			      V4L2_CID_STATELESS_HEVC_PPS, &pps, sizeof(pps));
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	h265_fill_sps(picture, &sps);
	rc = v4l2_set_control(driver_data->video_fd, surface_object->request_fd,
			      V4L2_CID_STATELESS_HEVC_SPS, &sps, sizeof(sps));
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	h265_fill_slice_params(picture, slice, &driver_data->surface_heap,
			       surface_object->source_data, &slice_params);
	rc = v4l2_set_control(driver_data->video_fd, surface_object->request_fd,
			      V4L2_CID_STATELESS_HEVC_SLICE_PARAMS,
			      &slice_params, sizeof(slice_params));
	if (rc < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	return 0;
}