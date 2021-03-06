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
 */

#ifndef __GST_D3D11_VP9_DEC_H__
#define __GST_D3D11_VP9_DEC_H__

#include "gstvp9decoder.h"
#include "gstvp9picture.h"
#include "gstd3d11decoder.h"

G_BEGIN_DECLS

#define GST_TYPE_D3D11_VP9_DEC \
  (gst_d3d11_vp9_dec_get_type())
#define GST_D3D11_VP9_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_D3D11_VP9_DEC,GstD3D11Vp9Dec))
#define GST_D3D11_VP9_DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_D3D11_VP9_DEC,GstD3D11Vp9DecClass))
#define GST_D3D11_VP9_DEC_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_D3D11_VP9_DEC,GstD3D11Vp9DecClass))
#define GST_IS_D3D11_VP9_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_D3D11_VP9_DEC))
#define GST_IS_D3D11_VP9_DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_D3D11_VP9_DEC))

struct _GstD3D11Vp9Dec
{
  GstVp9Decoder parent;

  GstVideoCodecState *output_state;

  GstD3D11Device *device;
  gint adapter;

  GstD3D11Decoder *d3d11_decoder;

  GstVp9Picture *current_picture;

  guint width, height;
  GstVP9Profile profile;

  GstVideoFormat out_format;

  gboolean use_d3d11_output;
};

struct _GstD3D11Vp9DecClass
{
  GstVp9DecoderClass parent_class;
};

GType gst_d3d11_vp9_dec_get_type (void);

void  gst_d3d11_vp9_dec_register (GstPlugin * plugin,
                                  GstD3D11Device * device,
                                  guint rank);

G_END_DECLS

#endif /* __GST_D3D11_VP9_DEC_H__ */
