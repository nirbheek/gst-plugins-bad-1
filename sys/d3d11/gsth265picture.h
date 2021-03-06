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

#ifndef __GST_H265_PICTURE_H__
#define __GST_H265_PICTURE_H__

#include <gst/gst.h>
#include <gst/codecparsers/gsth265parser.h>

G_BEGIN_DECLS

#define GST_TYPE_H265_PICTURE     (gst_h265_picture_get_type())
#define GST_IS_H265_PICTURE(obj)  (GST_IS_MINI_OBJECT_TYPE(obj, GST_TYPE_H265_PICTURE))
#define GST_H265_PICTURE(obj)     ((GstH265Picture *)obj)
#define GST_H265_PICTURE_CAST(obj) (GST_H265_PICTURE(obj))

typedef struct _GstH265Slice GstH265Slice;
typedef struct _GstH265Picture GstH265Picture;

#define GST_H265_DPB_MAX_SIZE 16

struct _GstH265Slice
{
  GstH265SliceHdr header;

  /* parsed nal unit (doesn't take ownership of raw data) */
  GstH265NalUnit nalu;
};

typedef enum
{
  GST_H265_PICTURE_FIELD_FRAME,
  GST_H265_PICTURE_FILED_TOP_FIELD,
  GST_H265_PICTURE_FIELD_BOTTOM_FIELD,
} GstH265PictureField;

struct _GstH265Picture
{
  GstMiniObject parent;

  GstH265SliceType type;

  GstClockTime pts;

  gint pic_order_cnt;
  gint pic_order_cnt_msb;
  gint pic_order_cnt_lsb;

  guint32 pic_latency_cnt;      /* PicLatencyCount */

  gboolean output_flag;
  gboolean NoRaslOutputFlag;
  gboolean NoOutputOfPriorPicsFlag;
  gboolean RapPicFlag;           /* nalu type between 16 and 21 */
  gboolean IntraPicFlag;         /* Intra pic (only Intra slices) */

  gboolean ref;
  gboolean long_term;
  gboolean outputted;

  GstH265PictureField field;

  gpointer user_data;
  GDestroyNotify notify;
};

G_GNUC_INTERNAL
GType gst_h265_picture_get_type (void);

G_GNUC_INTERNAL
GstH265Picture * gst_h265_picture_new (void);

G_GNUC_INTERNAL
static inline GstH265Picture *
gst_h265_picture_ref (GstH265Picture * picture)
{
  return (GstH265Picture *) gst_mini_object_ref (GST_MINI_OBJECT_CAST (picture));
}

G_GNUC_INTERNAL
static inline void
gst_h265_picture_unref (GstH265Picture * picture)
{
  gst_mini_object_unref (GST_MINI_OBJECT_CAST (picture));
}

G_GNUC_INTERNAL
static inline gboolean
gst_h265_picture_replace (GstH265Picture ** old_picture,
    GstH265Picture * new_picture)
{
  return gst_mini_object_replace ((GstMiniObject **) old_picture,
      (GstMiniObject *) new_picture);
}

G_GNUC_INTERNAL
static inline void
gst_h265_picture_clear (GstH265Picture ** picture)
{
  if (picture && *picture) {
    gst_h265_picture_unref (*picture);
    *picture = NULL;
  }
}

G_GNUC_INTERNAL
void gst_h265_picture_set_user_data (GstH265Picture * picture,
                                     gpointer user_data,
                                     GDestroyNotify notify);

G_GNUC_INTERNAL
gpointer gst_h265_picture_get_user_data (GstH265Picture * picture);

/*******************
 * GstH265Dpb *
 *******************/
typedef struct _GstH265Dpb GstH265Dpb;

G_GNUC_INTERNAL
GstH265Dpb * gst_h265_dpb_new (void);

G_GNUC_INTERNAL
void  gst_h265_dpb_set_max_num_pics (GstH265Dpb * dpb,
                                     gint max_num_pics);

G_GNUC_INTERNAL
gint gst_h265_dpb_get_max_num_pics  (GstH265Dpb * dpb);

G_GNUC_INTERNAL
void  gst_h265_dpb_free             (GstH265Dpb * dpb);

G_GNUC_INTERNAL
void  gst_h265_dpb_clear            (GstH265Dpb * dpb);

G_GNUC_INTERNAL
void  gst_h265_dpb_add              (GstH265Dpb * dpb,
                                     GstH265Picture * picture);

G_GNUC_INTERNAL
void  gst_h265_dpb_delete_unused    (GstH265Dpb * dpb);

G_GNUC_INTERNAL
void  gst_h265_dpb_delete_by_poc    (GstH265Dpb * dpb,
                                     gint poc);

G_GNUC_INTERNAL
gint  gst_h265_dpb_num_ref_pictures (GstH265Dpb * dpb);

G_GNUC_INTERNAL
void  gst_h265_dpb_mark_all_non_ref (GstH265Dpb * dpb);

G_GNUC_INTERNAL
GstH265Picture * gst_h265_dpb_get_ref_by_poc       (GstH265Dpb * dpb,
                                                    gint poc);

G_GNUC_INTERNAL
GstH265Picture * gst_h265_dpb_get_ref_by_poc_lsb   (GstH265Dpb * dpb,
                                                    gint poc_lsb);

G_GNUC_INTERNAL
GstH265Picture * gst_h265_dpb_get_short_ref_by_poc (GstH265Dpb * dpb,
                                                    gint poc);

G_GNUC_INTERNAL
GstH265Picture * gst_h265_dpb_get_long_ref_by_poc  (GstH265Dpb * dpb,
                                                    gint poc);

G_GNUC_INTERNAL
void  gst_h265_dpb_get_pictures_not_outputted  (GstH265Dpb * dpb,
                                                GList ** out);

G_GNUC_INTERNAL
GArray * gst_h265_dpb_get_pictures_all         (GstH265Dpb * dpb);

G_GNUC_INTERNAL
gint  gst_h265_dpb_get_size   (GstH265Dpb * dpb);

G_GNUC_INTERNAL
gboolean gst_h265_dpb_is_full (GstH265Dpb * dpb);

G_END_DECLS

#endif /* __GST_H265_PICTURE_H__ */
