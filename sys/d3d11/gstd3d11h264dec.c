/* GStreamer
 * Copyright (C) 2019 Seungha Yang <seungha.yang@navercorp.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * NOTE: some of implementations are copied/modified from Chromium code
 *
 * Copyright 2015 The Chromium Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *    * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstd3d11h264dec.h"
#include "gstd3d11memory.h"
#include "gstd3d11bufferpool.h"
#include <string.h>

/* HACK: to expose dxva data structure on UWP */
#ifdef WINAPI_PARTITION_DESKTOP
#undef WINAPI_PARTITION_DESKTOP
#endif
#define WINAPI_PARTITION_DESKTOP 1
#include <d3d9.h>
#include <dxva.h>

GST_DEBUG_CATEGORY_EXTERN (gst_d3d11_h264_dec_debug);
#define GST_CAT_DEFAULT gst_d3d11_h264_dec_debug

enum
{
  PROP_0,
  PROP_ADAPTER
};

#define DEFAULT_ADAPTER -1

/* copied from d3d11.h since mingw header doesn't define them */
DEFINE_GUID (GST_GUID_D3D11_DECODER_PROFILE_H264_IDCT_FGT, 0x1b81be67, 0xa0c7,
    0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);
DEFINE_GUID (GST_GUID_D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 0x1b81be68, 0xa0c7,
    0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);
DEFINE_GUID (GST_GUID_D3D11_DECODER_PROFILE_H264_VLD_FGT, 0x1b81be69, 0xa0c7,
    0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);

/* worst case 16 (non-interlaced) + 4 margin */
#define NUM_OUTPUT_VIEW 20

static GstStaticPadTemplate sink_template =
GST_STATIC_PAD_TEMPLATE (GST_VIDEO_DECODER_SINK_NAME,
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, "
        "stream-format=(string) { avc, avc3, byte-stream }, "
        "alignment=(string) au, profile = (string) { high, main }")
    );

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE (GST_VIDEO_DECODER_SRC_NAME,
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, "{ NV12, P010_10LE }") "; "
        GST_VIDEO_CAPS_MAKE ("{ NV12, P010_10LE }")));

struct _GstD3D11H264DecPrivate
{
  /* Need to hide DXVA_PicEntry_H264 structure from header for UWP */
  DXVA_PicEntry_H264 ref_frame_list[16];
  INT field_order_cnt_list[16][2];
  USHORT frame_num_list[16];
  UINT used_for_reference_flags;
  USHORT non_existing_frame_flags;
};

#define parent_class gst_d3d11_h264_dec_parent_class
G_DEFINE_TYPE_WITH_PRIVATE (GstD3D11H264Dec,
    gst_d3d11_h264_dec, GST_TYPE_H264_DECODER);

static void gst_d3d11_h264_dec_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_d3d11_h264_dec_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_d3d11_h264_dec_dispose (GObject * object);
static void gst_d3d11_h264_dec_set_context (GstElement * element,
    GstContext * context);

static gboolean gst_d3d11_h264_dec_open (GstVideoDecoder * decoder);
static gboolean gst_d3d11_h264_dec_close (GstVideoDecoder * decoder);
static gboolean gst_d3d11_h264_dec_start (GstVideoDecoder * decoder);
static gboolean gst_d3d11_h264_dec_stop (GstVideoDecoder * decoder);
static GstFlowReturn gst_d3d11_h264_dec_handle_frame (GstVideoDecoder *
    decoder, GstVideoCodecFrame * frame);
static gboolean gst_d3d11_h264_dec_negotiate (GstVideoDecoder * decoder);
static gboolean gst_d3d11_h264_dec_decide_allocation (GstVideoDecoder *
    decoder, GstQuery * query);
static gboolean gst_d3d11_h264_dec_src_query (GstVideoDecoder * decoder,
    GstQuery * query);

/* GstH264Decoder */
static gboolean gst_d3d11_h264_dec_new_sequence (GstH264Decoder * decoder,
    const GstH264SPS * sps);
static gboolean gst_d3d11_h264_dec_new_picture (GstH264Decoder * decoder,
    GstH264Picture * picture);
static GstFlowReturn gst_d3d11_h264_dec_output_picture (GstH264Decoder *
    decoder, GstH264Picture * picture);
static gboolean gst_d3d11_h264_dec_start_picture (GstH264Decoder * decoder,
    GstH264Picture * picture, GstH264Slice * slice, GstH264Dpb * dpb);
static gboolean gst_d3d11_h264_dec_decode_slice (GstH264Decoder * decoder,
    GstH264Picture * picture, GstH264Slice * slice);
static gboolean gst_d3d11_h264_dec_end_picture (GstH264Decoder * decoder,
    GstH264Picture * picture);

static void
gst_d3d11_h264_dec_class_init (GstD3D11H264DecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS (klass);
  GstH264DecoderClass *h264decoder_class = GST_H264_DECODER_CLASS (klass);

  gobject_class->set_property = gst_d3d11_h264_dec_set_property;
  gobject_class->get_property = gst_d3d11_h264_dec_get_property;
  gobject_class->dispose = gst_d3d11_h264_dec_dispose;

  g_object_class_install_property (gobject_class, PROP_ADAPTER,
      g_param_spec_int ("adapter", "Adapter",
          "Adapter index for creating device (-1 for default)",
          -1, G_MAXINT32, DEFAULT_ADAPTER,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
          G_PARAM_STATIC_STRINGS));

  element_class->set_context =
      GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_set_context);

  gst_element_class_set_static_metadata (element_class,
      "Direct3D11 H.264 Video Decoder",
      "Codec/Decoder/Video/Hardware",
      "A Direct3D11 based H.264 video decoder",
      "Seungha Yang <seungha.yang@navercorp.com>");

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);

  decoder_class->open = GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_open);
  decoder_class->close = GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_close);
  decoder_class->start = GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_start);
  decoder_class->stop = GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_stop);
  decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_handle_frame);
  decoder_class->negotiate = GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_negotiate);
  decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_decide_allocation);
  decoder_class->src_query = GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_src_query);

  h264decoder_class->new_sequence =
      GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_new_sequence);
  h264decoder_class->new_picture =
      GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_new_picture);
  h264decoder_class->output_picture =
      GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_output_picture);
  h264decoder_class->start_picture =
      GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_start_picture);
  h264decoder_class->decode_slice =
      GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_decode_slice);
  h264decoder_class->end_picture =
      GST_DEBUG_FUNCPTR (gst_d3d11_h264_dec_end_picture);
}

static void
gst_d3d11_h264_dec_init (GstD3D11H264Dec * self)
{
  self->priv = gst_d3d11_h264_dec_get_instance_private (self);
  self->slice_list = g_array_new (FALSE, TRUE, sizeof (DXVA_Slice_H264_Short));
  self->adapter = DEFAULT_ADAPTER;
}

static void
gst_d3d11_h264_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (object);

  switch (prop_id) {
    case PROP_ADAPTER:
      self->adapter = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_d3d11_h264_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (object);

  switch (prop_id) {
    case PROP_ADAPTER:
      g_value_set_int (value, self->adapter);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_d3d11_h264_dec_dispose (GObject * object)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (object);

  if (self->slice_list) {
    g_array_unref (self->slice_list);
    self->slice_list = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_d3d11_h264_dec_set_context (GstElement * element, GstContext * context)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (element);

  gst_d3d11_handle_set_context (element, context, self->adapter, &self->device);

  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static gboolean
gst_d3d11_h264_dec_open (GstVideoDecoder * decoder)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);

  if (!gst_d3d11_ensure_element_data (GST_ELEMENT_CAST (self), self->adapter,
          &self->device)) {
    GST_ERROR_OBJECT (self, "Cannot create d3d11device");
    return FALSE;
  }

  self->d3d11_decoder = gst_d3d11_decoder_new (self->device);

  if (!self->d3d11_decoder) {
    GST_ERROR_OBJECT (self, "Cannot create d3d11 decoder");
    gst_clear_object (&self->device);
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_d3d11_h264_dec_close (GstVideoDecoder * decoder)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);

  gst_clear_object (&self->d3d11_decoder);
  gst_clear_object (&self->device);

  return TRUE;
}

static gboolean
gst_d3d11_h264_dec_start (GstVideoDecoder * decoder)
{
  return GST_VIDEO_DECODER_CLASS (parent_class)->start (decoder);
}

static gboolean
gst_d3d11_h264_dec_stop (GstVideoDecoder * decoder)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);

  gst_h264_picture_replace (&self->current_picture, NULL);
  if (self->output_state)
    gst_video_codec_state_unref (self->output_state);
  self->output_state = NULL;

  return GST_VIDEO_DECODER_CLASS (parent_class)->stop (decoder);
}

static GstFlowReturn
gst_d3d11_h264_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);
  GstBuffer *in_buf = frame->input_buffer;

  GST_LOG_OBJECT (self,
      "handle frame, PTS: %" GST_TIME_FORMAT ", DTS: %"
      GST_TIME_FORMAT, GST_TIME_ARGS (GST_BUFFER_PTS (in_buf)),
      GST_TIME_ARGS (GST_BUFFER_DTS (in_buf)));

  if (!self->current_picture) {
    GST_ERROR_OBJECT (self, "No current picture");
    gst_video_decoder_drop_frame (decoder, frame);

    return GST_FLOW_ERROR;
  }

  gst_video_codec_frame_set_user_data (frame,
      self->current_picture, (GDestroyNotify) gst_h264_picture_unref);
  self->current_picture = NULL;

  gst_video_codec_frame_unref (frame);

  return GST_FLOW_OK;
}

static gboolean
gst_d3d11_h264_dec_negotiate (GstVideoDecoder * decoder)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);
  GstH264Decoder *h264dec = GST_H264_DECODER (decoder);
  GstCaps *peer_caps;

  GST_DEBUG_OBJECT (self, "negotiate");

  if (self->output_state)
    gst_video_codec_state_unref (self->output_state);

  self->output_state =
      gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self),
      self->out_format, self->width, self->height, h264dec->input_state);

  self->output_state->caps = gst_video_info_to_caps (&self->output_state->info);

  peer_caps = gst_pad_get_allowed_caps (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_DEBUG_OBJECT (self, "Allowed caps %" GST_PTR_FORMAT, peer_caps);

  self->use_d3d11_output = FALSE;

  if (!peer_caps || gst_caps_is_any (peer_caps)) {
    GST_DEBUG_OBJECT (self,
        "cannot determine output format, use system memory");
  } else {
    GstCapsFeatures *features;
    guint size = gst_caps_get_size (peer_caps);
    guint i;

    for (i = 0; i < size; i++) {
      features = gst_caps_get_features (peer_caps, i);
      if (features && gst_caps_features_contains (features,
              GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY)) {
        GST_DEBUG_OBJECT (self, "found D3D11 memory feature");
        gst_caps_set_features (self->output_state->caps, 0,
            gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, NULL));

        self->use_d3d11_output = TRUE;
        break;
      }
    }
  }
  gst_clear_caps (&peer_caps);

  return GST_VIDEO_DECODER_CLASS (parent_class)->negotiate (decoder);
}

static gboolean
gst_d3d11_h264_dec_decide_allocation (GstVideoDecoder * decoder,
    GstQuery * query)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);
  GstCaps *outcaps;
  GstBufferPool *pool = NULL;
  guint n, size, min, max;
  GstVideoInfo vinfo = { 0, };
  GstStructure *config;
  GstD3D11AllocationParams *d3d11_params;

  GST_DEBUG_OBJECT (self, "decide allocation");

  gst_query_parse_allocation (query, &outcaps, NULL);

  if (!outcaps) {
    GST_DEBUG_OBJECT (self, "No output caps");
    return FALSE;
  }

  gst_video_info_from_caps (&vinfo, outcaps);
  n = gst_query_get_n_allocation_pools (query);
  if (n > 0)
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

  /* create our own pool */
  if (pool && (self->use_d3d11_output && !GST_D3D11_BUFFER_POOL (pool))) {
    gst_object_unref (pool);
    pool = NULL;
  }

  if (!pool) {
    if (self->use_d3d11_output)
      pool = gst_d3d11_buffer_pool_new (self->device);
    else
      pool = gst_video_buffer_pool_new ();

    min = max = 0;
    size = (guint) vinfo.size;
  }

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);

  if (self->use_d3d11_output) {
    d3d11_params = gst_buffer_pool_config_get_d3d11_allocation_params (config);
    if (!d3d11_params)
      d3d11_params = gst_d3d11_allocation_params_new (self->device,
          &vinfo, 0, 0);

    /* dxva2 decoder uses non-resource format
     * (e.g., use NV12 instead of R8 + R8G8 */
    d3d11_params->desc[0].Width = GST_VIDEO_INFO_WIDTH (&vinfo);
    d3d11_params->desc[0].Height = GST_VIDEO_INFO_HEIGHT (&vinfo);
    d3d11_params->desc[0].Format = d3d11_params->d3d11_format->dxgi_format;

    gst_buffer_pool_config_set_d3d11_allocation_params (config, d3d11_params);
    gst_d3d11_allocation_params_free (d3d11_params);
  }

  gst_buffer_pool_set_config (pool, config);
  if (self->use_d3d11_output)
    size = GST_D3D11_BUFFER_POOL (pool)->buffer_size;

  if (n > 0)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);
  gst_object_unref (pool);

  return GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation
      (decoder, query);
}

static gboolean
gst_d3d11_h264_dec_src_query (GstVideoDecoder * decoder, GstQuery * query)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      if (gst_d3d11_handle_context_query (GST_ELEMENT (decoder),
              query, self->device)) {
        return TRUE;
      }
      break;
    default:
      break;
  }

  return GST_VIDEO_DECODER_CLASS (parent_class)->src_query (decoder, query);
}

static gboolean
gst_d3d11_h264_dec_new_sequence (GstH264Decoder * decoder,
    const GstH264SPS * sps)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);
  gint crop_width, crop_height;
  gboolean modified = FALSE;
  static const GUID *supported_profiles[] = {
    &GST_GUID_D3D11_DECODER_PROFILE_H264_IDCT_FGT,
    &GST_GUID_D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
    &GST_GUID_D3D11_DECODER_PROFILE_H264_VLD_FGT
  };

  GST_LOG_OBJECT (self, "new sequence");

  if (sps->frame_cropping_flag) {
    crop_width = sps->crop_rect_width;
    crop_height = sps->crop_rect_height;
  } else {
    crop_width = sps->width;
    crop_height = sps->height;
  }

  if (self->width != crop_width || self->height != crop_height ||
      self->coded_width != sps->width || self->coded_height != sps->height) {
    GST_INFO_OBJECT (self, "resolution changed %dx%d", crop_width, crop_height);
    self->width = crop_width;
    self->height = crop_height;
    self->coded_width = sps->width;
    self->coded_height = sps->height;
    modified = TRUE;
  }

  if (self->bitdepth != sps->bit_depth_luma_minus8 + 8) {
    GST_INFO_OBJECT (self, "bitdepth changed");
    self->bitdepth = sps->bit_depth_luma_minus8 + 8;
    modified = TRUE;
  }

  if (self->chroma_format_idc != sps->chroma_format_idc) {
    GST_INFO_OBJECT (self, "chroma format changed");
    self->chroma_format_idc = sps->chroma_format_idc;
    modified = TRUE;
  }

  if (modified || !self->d3d11_decoder->opened) {
    GstVideoInfo info;

    self->out_format = GST_VIDEO_FORMAT_UNKNOWN;

    if (self->bitdepth == 8) {
      if (self->chroma_format_idc == 1)
        self->out_format = GST_VIDEO_FORMAT_NV12;
      else {
        GST_FIXME_OBJECT (self, "Could not support 8bits non-4:2:0 format");
      }
    } else if (self->bitdepth == 10) {
      if (self->chroma_format_idc == 1)
        self->out_format = GST_VIDEO_FORMAT_P010_10LE;
      else {
        GST_FIXME_OBJECT (self, "Could not support 10bits non-4:2:0 format");
      }
    }

    if (self->out_format == GST_VIDEO_FORMAT_UNKNOWN) {
      GST_ERROR_OBJECT (self, "Could not support bitdepth/chroma format");
      return FALSE;
    }

    /* allocated internal pool with coded width/height */
    gst_video_info_set_format (&info,
        self->out_format, self->coded_width, self->coded_height);

    gst_d3d11_decoder_reset (self->d3d11_decoder);
    if (!gst_d3d11_decoder_open (self->d3d11_decoder, GST_D3D11_CODEC_H264,
            &info, NUM_OUTPUT_VIEW, supported_profiles,
            G_N_ELEMENTS (supported_profiles))) {
      GST_ERROR_OBJECT (self, "Failed to create decoder");
      return FALSE;
    }

    if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (self))) {
      GST_ERROR_OBJECT (self, "Failed to negotiate with downstream");
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
gst_d3d11_h264_dec_get_bitstream_buffer (GstD3D11H264Dec * self)
{
  GST_TRACE_OBJECT (self, "Getting bitstream buffer");
  if (!gst_d3d11_decoder_get_decoder_buffer (self->d3d11_decoder,
          D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &self->bitstream_buffer_size,
          (gpointer *) & self->bitstream_buffer_bytes)) {
    GST_ERROR_OBJECT (self, "Faild to get bitstream buffer");
    return FALSE;
  }

  GST_TRACE_OBJECT (self, "Got bitstream buffer %p with size %d",
      self->bitstream_buffer_bytes, self->bitstream_buffer_size);
  self->current_offset = 0;

  return TRUE;
}

static GstD3D11DecoderOutputView *
gst_d3d11_h264_dec_get_output_view_from_picture (GstD3D11H264Dec * self,
    GstH264Picture * picture)
{
  GstBuffer *view_buffer;
  GstD3D11DecoderOutputView *view;

  view_buffer = (GstBuffer *) gst_h264_picture_get_user_data (picture);
  if (!view_buffer) {
    GST_DEBUG_OBJECT (self, "current picture does not have output view buffer");
    return NULL;
  }

  view = gst_d3d11_decoder_get_output_view_from_buffer (self->d3d11_decoder,
      view_buffer);
  if (!view) {
    GST_DEBUG_OBJECT (self, "current picture does not have output view handle");
    return NULL;
  }

  return view;
}

static gboolean
gst_d3d11_h264_dec_start_picture (GstH264Decoder * decoder,
    GstH264Picture * picture, GstH264Slice * slice, GstH264Dpb * dpb)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);
  GstD3D11H264DecPrivate *priv = self->priv;
  GstD3D11DecoderOutputView *view;
  gint i;
  GArray *dpb_array;

  view = gst_d3d11_h264_dec_get_output_view_from_picture (self, picture);
  if (!view) {
    GST_ERROR_OBJECT (self, "current picture does not have output view handle");
    return FALSE;
  }

  GST_TRACE_OBJECT (self, "Begin frame");

  if (!gst_d3d11_decoder_begin_frame (self->d3d11_decoder, view, 0, NULL)) {
    GST_ERROR_OBJECT (self, "Failed to begin frame");
    return FALSE;
  }

  for (i = 0; i < 16; i++) {
    priv->ref_frame_list[i].bPicEntry = 0xFF;
    priv->field_order_cnt_list[i][0] = 0;
    priv->field_order_cnt_list[i][1] = 0;
    priv->frame_num_list[i] = 0;
  }
  priv->used_for_reference_flags = 0;
  priv->non_existing_frame_flags = 0;

  dpb_array = gst_h264_dpb_get_pictures_all (dpb);

  for (i = 0; i < dpb_array->len; i++) {
    guint ref = 3;
    GstH264Picture *other = g_array_index (dpb_array, GstH264Picture *, i);
    GstD3D11DecoderOutputView *other_view;
    gint id = 0xff;

    if (!other->ref)
      continue;

    other_view = gst_d3d11_h264_dec_get_output_view_from_picture (self, other);

    if (other_view)
      id = other_view->view_id;

    priv->ref_frame_list[i].Index7Bits = id;
    priv->ref_frame_list[i].AssociatedFlag = other->long_term;
    priv->field_order_cnt_list[i][0] = other->top_field_order_cnt;
    priv->field_order_cnt_list[i][1] = other->bottom_field_order_cnt;
    priv->frame_num_list[i] = priv->ref_frame_list[i].AssociatedFlag
        ? other->long_term_pic_num : other->frame_num;
    priv->used_for_reference_flags |= ref << (2 * i);
    priv->non_existing_frame_flags |= (other->nonexisting) << i;
  }

  g_array_unref (dpb_array);
  g_array_set_size (self->slice_list, 0);

  return gst_d3d11_h264_dec_get_bitstream_buffer (self);
}

static gboolean
gst_d3d11_h264_dec_new_picture (GstH264Decoder * decoder,
    GstH264Picture * picture)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);
  GstBuffer *view_buffer;
  GstD3D11Memory *mem;

  view_buffer = gst_d3d11_decoder_get_output_view_buffer (self->d3d11_decoder);
  if (!view_buffer) {
    GST_ERROR_OBJECT (self, "No available output view buffer");
    return FALSE;
  }

  mem = (GstD3D11Memory *) gst_buffer_peek_memory (view_buffer, 0);

  GST_LOG_OBJECT (self, "New output view buffer %" GST_PTR_FORMAT " (index %d)",
      view_buffer, mem->subresource_index);

  gst_h264_picture_set_user_data (picture,
      view_buffer, (GDestroyNotify) gst_buffer_unref);

  GST_LOG_OBJECT (self, "New h264picture %p", picture);

  gst_h264_picture_replace (&self->current_picture, picture);

  return TRUE;
}

static GstFlowReturn
gst_d3d11_h264_dec_output_picture (GstH264Decoder * decoder,
    GstH264Picture * picture)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);
  GList *pending_frames, *iter;
  GstVideoCodecFrame *frame = NULL;
  GstBuffer *output_buffer = NULL;
  GstFlowReturn ret;
  GstBuffer *view_buffer;

  GST_LOG_OBJECT (self,
      "Outputting picture %p (poc %d)", picture, picture->pic_order_cnt);

  view_buffer = (GstBuffer *) gst_h264_picture_get_user_data (picture);

  if (!view_buffer) {
    GST_ERROR_OBJECT (self, "Could not get output view");
    return FALSE;
  }

  pending_frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (self));
  for (iter = pending_frames; iter; iter = g_list_next (iter)) {
    GstVideoCodecFrame *tmp;
    GstH264Picture *other_pic;

    tmp = (GstVideoCodecFrame *) iter->data;
    other_pic = gst_video_codec_frame_get_user_data (tmp);
    if (!other_pic) {
      /* FIXME: what should we do here? */
      GST_WARNING_OBJECT (self,
          "Codec frame %p does not have corresponding picture object", tmp);
      continue;
    }

    if (other_pic == picture) {
      frame = gst_video_codec_frame_ref (tmp);
      break;
    }
  }

  g_list_free_full (pending_frames,
      (GDestroyNotify) gst_video_codec_frame_unref);

  if (!frame) {
    GST_WARNING_OBJECT (self,
        "Failed to find codec frame for picture %p", picture);

    output_buffer =
        gst_video_decoder_allocate_output_buffer (GST_VIDEO_DECODER (self));

    if (!output_buffer) {
      GST_ERROR_OBJECT (self, "Couldn't allocate output buffer");
      return GST_FLOW_ERROR;
    }

    GST_BUFFER_PTS (output_buffer) = picture->pts;
    GST_BUFFER_DTS (output_buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION (output_buffer) = GST_CLOCK_TIME_NONE;
  } else {
    ret =
        gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER (self),
        frame);

    if (ret != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "failed to allocate output frame");
      return ret;
    }

    output_buffer = frame->output_buffer;
    GST_BUFFER_PTS (output_buffer) = GST_BUFFER_PTS (frame->input_buffer);
    GST_BUFFER_DTS (output_buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION (output_buffer) =
        GST_BUFFER_DURATION (frame->input_buffer);
  }

  if (!gst_d3d11_decoder_copy_decoder_buffer (self->d3d11_decoder,
          &self->output_state->info, view_buffer, output_buffer)) {
    GST_ERROR_OBJECT (self, "Failed to copy buffer");
    if (frame)
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
    else
      gst_buffer_unref (output_buffer);

    return GST_FLOW_ERROR;
  }

  GST_LOG_OBJECT (self, "Finish frame %" GST_TIME_FORMAT,
      GST_TIME_ARGS (GST_BUFFER_PTS (output_buffer)));

  if (frame) {
    ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
  } else {
    ret = gst_pad_push (GST_VIDEO_DECODER_SRC_PAD (self), output_buffer);
  }

  return ret;
}

static gboolean
gst_d3d11_h264_dec_submit_slice_data (GstD3D11H264Dec * self)
{
  guint buffer_size;
  gpointer buffer;
  guint8 *data;
  gsize offset = 0;
  gint i;
  D3D11_VIDEO_DECODER_BUFFER_DESC buffer_desc[4] = { 0, };
  gboolean ret;

  if (self->slice_list->len < 1) {
    GST_WARNING_OBJECT (self, "Nothing to submit");
    return FALSE;
  }

  GST_TRACE_OBJECT (self, "Getting slice control buffer");

  if (!gst_d3d11_decoder_get_decoder_buffer (self->d3d11_decoder,
          D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, &buffer_size, &buffer)) {
    GST_ERROR_OBJECT (self, "Couldn't get slice control buffer");
    return FALSE;
  }

  data = buffer;
  for (i = 0; i < self->slice_list->len; i++) {
    DXVA_Slice_H264_Short *slice_data =
        &g_array_index (self->slice_list, DXVA_Slice_H264_Short, i);

    memcpy (data + offset, slice_data, sizeof (DXVA_Slice_H264_Short));
    offset += sizeof (DXVA_Slice_H264_Short);
  }

  GST_TRACE_OBJECT (self, "Release slice control buffer");
  if (!gst_d3d11_decoder_release_decoder_buffer (self->d3d11_decoder,
          D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL)) {
    GST_ERROR_OBJECT (self, "Failed to release slice control buffer");
    return FALSE;
  }

  if (!gst_d3d11_decoder_release_decoder_buffer (self->d3d11_decoder,
          D3D11_VIDEO_DECODER_BUFFER_BITSTREAM)) {
    GST_ERROR_OBJECT (self, "Failed to release bitstream buffer");
    return FALSE;
  }

  buffer_desc[0].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
  buffer_desc[0].DataOffset = 0;
  buffer_desc[0].DataSize = sizeof (DXVA_PicParams_H264);

  buffer_desc[1].BufferType =
      D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
  buffer_desc[1].DataOffset = 0;
  buffer_desc[1].DataSize = sizeof (DXVA_Qmatrix_H264);

  buffer_desc[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
  buffer_desc[2].DataOffset = 0;
  buffer_desc[2].DataSize =
      sizeof (DXVA_Slice_H264_Short) * self->slice_list->len;

  buffer_desc[3].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
  buffer_desc[3].DataOffset = 0;
  buffer_desc[3].DataSize = self->current_offset;

  ret = gst_d3d11_decoder_submit_decoder_buffers (self->d3d11_decoder,
      4, buffer_desc);

  self->current_offset = 0;
  self->bitstream_buffer_bytes = NULL;
  self->bitstream_buffer_size = 0;
  g_array_set_size (self->slice_list, 0);

  return ret;
}

static gboolean
gst_d3d11_h264_dec_end_picture (GstH264Decoder * decoder,
    GstH264Picture * picture)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);

  GST_LOG_OBJECT (self, "end picture %p, (poc %d)",
      picture, picture->pic_order_cnt);

  if (!gst_d3d11_h264_dec_submit_slice_data (self)) {
    GST_ERROR_OBJECT (self, "Failed to submit slice data");
    return FALSE;
  }

  if (!gst_d3d11_decoder_end_frame (self->d3d11_decoder)) {
    GST_ERROR_OBJECT (self, "Failed to EndFrame");
    return FALSE;
  }

  return TRUE;
}

static void
gst_d3d11_h264_dec_picture_params_from_sps (GstD3D11H264Dec * self,
    const GstH264SPS * sps, gboolean field_pic, DXVA_PicParams_H264 * params)
{
#define COPY_FIELD(f) \
  (params)->f = (sps)->f

  params->wFrameWidthInMbsMinus1 = sps->pic_width_in_mbs_minus1;
  params->wFrameHeightInMbsMinus1 = sps->pic_height_in_map_units_minus1;
  params->residual_colour_transform_flag = sps->separate_colour_plane_flag;
  params->MbaffFrameFlag = sps->mb_adaptive_frame_field_flag && field_pic;
  params->field_pic_flag = field_pic;
  params->MinLumaBipredSize8x8Flag = sps->level_idc >= 31;

  COPY_FIELD (num_ref_frames);
  COPY_FIELD (chroma_format_idc);
  COPY_FIELD (frame_mbs_only_flag);
  COPY_FIELD (bit_depth_luma_minus8);
  COPY_FIELD (bit_depth_chroma_minus8);
  COPY_FIELD (log2_max_frame_num_minus4);
  COPY_FIELD (pic_order_cnt_type);
  COPY_FIELD (log2_max_pic_order_cnt_lsb_minus4);
  COPY_FIELD (delta_pic_order_always_zero_flag);
  COPY_FIELD (direct_8x8_inference_flag);

#undef COPY_FIELD
}

static void
gst_d3d11_h264_dec_picture_params_from_pps (GstD3D11H264Dec * self,
    const GstH264PPS * pps, DXVA_PicParams_H264 * params)
{
#define COPY_FIELD(f) \
  (params)->f = (pps)->f

  COPY_FIELD (constrained_intra_pred_flag);
  COPY_FIELD (weighted_pred_flag);
  COPY_FIELD (weighted_bipred_idc);
  COPY_FIELD (transform_8x8_mode_flag);
  COPY_FIELD (pic_init_qs_minus26);
  COPY_FIELD (chroma_qp_index_offset);
  COPY_FIELD (second_chroma_qp_index_offset);
  COPY_FIELD (pic_init_qp_minus26);
  COPY_FIELD (num_ref_idx_l0_active_minus1);
  COPY_FIELD (num_ref_idx_l1_active_minus1);
  COPY_FIELD (entropy_coding_mode_flag);
  COPY_FIELD (pic_order_present_flag);
  COPY_FIELD (deblocking_filter_control_present_flag);
  COPY_FIELD (redundant_pic_cnt_present_flag);
  COPY_FIELD (num_slice_groups_minus1);
  COPY_FIELD (slice_group_map_type);

#undef COPY_FIELD
}

static void
gst_d3d11_h264_dec_picture_params_from_slice_header (GstD3D11H264Dec *
    self, const GstH264SliceHdr * slice_header, DXVA_PicParams_H264 * params)
{
  params->sp_for_switch_flag = slice_header->sp_for_switch_flag;
  params->field_pic_flag = slice_header->field_pic_flag;
  params->CurrPic.AssociatedFlag = slice_header->bottom_field_flag;
  params->IntraPicFlag =
      GST_H264_IS_I_SLICE (slice_header) || GST_H264_IS_SI_SLICE (slice_header);
}

static gboolean
gst_d3d11_h264_dec_fill_picture_params (GstD3D11H264Dec * self,
    const GstH264SliceHdr * slice_header, DXVA_PicParams_H264 * params)
{
  const GstH264SPS *sps;
  const GstH264PPS *pps;

  g_return_val_if_fail (slice_header->pps != NULL, FALSE);
  g_return_val_if_fail (slice_header->pps->sequence != NULL, FALSE);

  pps = slice_header->pps;
  sps = pps->sequence;

  memset (params, 0, sizeof (DXVA_PicParams_H264));

  params->MbsConsecutiveFlag = 1;
  params->Reserved16Bits = 3;
  params->ContinuationFlag = 1;
  params->Reserved8BitsA = 0;
  params->Reserved8BitsB = 0;
  params->StatusReportFeedbackNumber = 1;

  gst_d3d11_h264_dec_picture_params_from_sps (self,
      sps, slice_header->field_pic_flag, params);
  gst_d3d11_h264_dec_picture_params_from_pps (self, pps, params);
  gst_d3d11_h264_dec_picture_params_from_slice_header (self,
      slice_header, params);

  return TRUE;
}

static gboolean
gst_d3d11_h264_dec_decode_slice (GstH264Decoder * decoder,
    GstH264Picture * picture, GstH264Slice * slice)
{
  GstD3D11H264Dec *self = GST_D3D11_H264_DEC (decoder);
  GstD3D11H264DecPrivate *priv = self->priv;
  GstH264SPS *sps;
  GstH264PPS *pps;
  DXVA_PicParams_H264 pic_params = { 0, };
  DXVA_Qmatrix_H264 iq_matrix = { 0, };
  guint d3d11_buffer_size = 0;
  gpointer d3d11_buffer = NULL;
  gint i, j;
  GstD3D11DecoderOutputView *view;

  pps = slice->header.pps;
  sps = pps->sequence;

  view = gst_d3d11_h264_dec_get_output_view_from_picture (self, picture);

  if (!view) {
    GST_ERROR_OBJECT (self, "current picture does not have output view");
    return FALSE;
  }

  gst_d3d11_h264_dec_fill_picture_params (self, &slice->header, &pic_params);

  pic_params.CurrPic.Index7Bits = view->view_id;
  pic_params.RefPicFlag = picture->ref;
  pic_params.frame_num = picture->frame_num;

  if (pic_params.field_pic_flag && pic_params.CurrPic.AssociatedFlag) {
    pic_params.CurrFieldOrderCnt[1] = picture->bottom_field_order_cnt;
    pic_params.CurrFieldOrderCnt[0] = 0;
  } else if (pic_params.field_pic_flag && !pic_params.CurrPic.AssociatedFlag) {
    pic_params.CurrFieldOrderCnt[0] = picture->top_field_order_cnt;
    pic_params.CurrFieldOrderCnt[1] = 0;
  } else {
    pic_params.CurrFieldOrderCnt[0] = picture->top_field_order_cnt;
    pic_params.CurrFieldOrderCnt[1] = picture->bottom_field_order_cnt;
  }

  memcpy (pic_params.RefFrameList, priv->ref_frame_list,
      sizeof (pic_params.RefFrameList));
  memcpy (pic_params.FieldOrderCntList, priv->field_order_cnt_list,
      sizeof (pic_params.FieldOrderCntList));
  memcpy (pic_params.FrameNumList, priv->frame_num_list,
      sizeof (pic_params.FrameNumList));

  pic_params.UsedForReferenceFlags = priv->used_for_reference_flags;
  pic_params.NonExistingFrameFlags = priv->non_existing_frame_flags;

  GST_TRACE_OBJECT (self, "Getting picture param decoder buffer");

  if (!gst_d3d11_decoder_get_decoder_buffer (self->d3d11_decoder,
          D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &d3d11_buffer_size,
          &d3d11_buffer)) {
    GST_ERROR_OBJECT (self,
        "Failed to get decoder buffer for picture parameters");
    return FALSE;
  }

  memcpy (d3d11_buffer, &pic_params, sizeof (DXVA_PicParams_H264));

  GST_TRACE_OBJECT (self, "Release picture param decoder buffer");

  if (!gst_d3d11_decoder_release_decoder_buffer (self->d3d11_decoder,
          D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS)) {
    GST_ERROR_OBJECT (self, "Failed to release decoder buffer");
    return FALSE;
  }

  if (pps->pic_scaling_matrix_present_flag) {
    for (i = 0; i < 6; i++) {
      for (j = 0; j < 16; j++) {
        iq_matrix.bScalingLists4x4[i][j] = pps->scaling_lists_4x4[i][j];
      }
    }

    for (i = 0; i < 2; i++) {
      for (j = 0; j < 64; j++) {
        iq_matrix.bScalingLists8x8[i][j] = pps->scaling_lists_8x8[i][j];
      }
    }
  } else {
    for (i = 0; i < 6; i++) {
      for (j = 0; j < 16; j++) {
        iq_matrix.bScalingLists4x4[i][j] = sps->scaling_lists_4x4[i][j];
      }
    }

    for (i = 0; i < 2; i++) {
      for (j = 0; j < 64; j++) {
        iq_matrix.bScalingLists8x8[i][j] = sps->scaling_lists_8x8[i][j];
      }
    }
  }

  GST_TRACE_OBJECT (self, "Getting inverse quantization maxtirx buffer");

  if (!gst_d3d11_decoder_get_decoder_buffer (self->d3d11_decoder,
          D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
          &d3d11_buffer_size, &d3d11_buffer)) {
    GST_ERROR_OBJECT (self,
        "Failed to get decoder buffer for inv. quantization matrix");
    return FALSE;
  }

  memcpy (d3d11_buffer, &iq_matrix, sizeof (DXVA_Qmatrix_H264));

  GST_TRACE_OBJECT (self, "Release inverse quantization maxtirx buffer");

  if (!gst_d3d11_decoder_release_decoder_buffer (self->d3d11_decoder,
          D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX)) {
    GST_ERROR_OBJECT (self, "Failed to release decoder buffer");
    return FALSE;
  }

  {
    guint to_write = slice->nalu.size + 3;
    gboolean is_first = TRUE;

    while (to_write > 0) {
      guint bytes_to_copy;
      gboolean is_last = TRUE;
      DXVA_Slice_H264_Short slice_short = { 0, };

      if (self->bitstream_buffer_size < to_write && self->slice_list->len > 0) {
        if (!gst_d3d11_h264_dec_submit_slice_data (self)) {
          GST_ERROR_OBJECT (self, "Failed to submit bitstream buffers");
          return FALSE;
        }

        if (!gst_d3d11_h264_dec_get_bitstream_buffer (self)) {
          GST_ERROR_OBJECT (self, "Failed to get bitstream buffer");
          return FALSE;
        }
      }

      bytes_to_copy = to_write;

      if (bytes_to_copy > self->bitstream_buffer_size) {
        bytes_to_copy = self->bitstream_buffer_size;
        is_last = FALSE;
      }

      if (bytes_to_copy >= 3 && is_first) {
        /* normal case */
        self->bitstream_buffer_bytes[0] = 0;
        self->bitstream_buffer_bytes[1] = 0;
        self->bitstream_buffer_bytes[2] = 1;
        memcpy (self->bitstream_buffer_bytes + 3,
            slice->nalu.data + slice->nalu.offset, bytes_to_copy - 3);
      } else {
        /* when this nal unit date is splitted into two buffer */
        memcpy (self->bitstream_buffer_bytes,
            slice->nalu.data + slice->nalu.offset, bytes_to_copy);
      }

      slice_short.BSNALunitDataLocation = self->current_offset;
      slice_short.SliceBytesInBuffer = bytes_to_copy;
      /* wBadSliceChopping: (dxva h264 spec.)
       * 0: All bits for the slice are located within the corresponding
       *    bitstream data buffer
       * 1: The bitstream data buffer contains the start of the slice,
       *    but not the entire slice, because the buffer is full
       * 2: The bitstream data buffer contains the end of the slice.
       *    It does not contain the start of the slice, because the start of
       *    the slice was located in the previous bitstream data buffer.
       * 3: The bitstream data buffer does not contain the start of the slice
       *    (because the start of the slice was located in the previous
       *     bitstream data buffer), and it does not contain the end of the slice
       *    (because the current bitstream data buffer is also full).
       */
      if (is_last && is_first) {
        slice_short.wBadSliceChopping = 0;
      } else if (!is_last && is_first) {
        slice_short.wBadSliceChopping = 1;
      } else if (is_last && !is_first) {
        slice_short.wBadSliceChopping = 2;
      } else {
        slice_short.wBadSliceChopping = 3;
      }

      g_array_append_val (self->slice_list, slice_short);
      self->bitstream_buffer_size -= bytes_to_copy;
      self->current_offset += bytes_to_copy;
      self->bitstream_buffer_bytes += bytes_to_copy;
      is_first = FALSE;
      to_write -= bytes_to_copy;
    }
  }

  return TRUE;
}

void
gst_d3d11_h264_dec_register (GstPlugin * plugin, GstD3D11Device * device,
    guint rank)
{
  GstD3D11Decoder *decoder;
  GstVideoInfo info;
  gboolean ret;
  static const GUID *supported_profiles[] = {
    &GST_GUID_D3D11_DECODER_PROFILE_H264_IDCT_FGT,
    &GST_GUID_D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
    &GST_GUID_D3D11_DECODER_PROFILE_H264_VLD_FGT
  };

  decoder = gst_d3d11_decoder_new (device);
  if (!decoder) {
    GST_WARNING_OBJECT (device, "decoder interface unavailable");
    return;
  }

  /* FIXME: DXVA does not provide API for query supported resolution
   * maybe we need some tries per standard resolution (e.g., HD, FullHD ...)
   * to check supported resolution */
  gst_video_info_set_format (&info, GST_VIDEO_FORMAT_NV12, 1280, 720);

  ret = gst_d3d11_decoder_open (decoder, GST_D3D11_CODEC_H264,
      &info, NUM_OUTPUT_VIEW, supported_profiles,
      G_N_ELEMENTS (supported_profiles));
  gst_object_unref (decoder);

  if (!ret) {
    GST_WARNING_OBJECT (device, "cannot open decoder device");
    return;
  }

  gst_element_register (plugin, "d3d11h264dec", rank, GST_TYPE_D3D11_H264_DEC);
}
