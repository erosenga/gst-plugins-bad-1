/* Generic video mixer plugin
 *
 * GStreamer
 * Copyright (C) 2009 Julien Isorce <julien.isorce@gmail.com>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstcollectpads.h>
#include <gst/video/video.h>

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "gstglmixer.h"

#define GST_CAT_DEFAULT gst_gl_mixer_debug
GST_DEBUG_CATEGORY (gst_gl_mixer_debug);

#define GST_GL_MIXER_GET_LOCK(mix) \
  (GST_GL_MIXER(mix)->lock)
#define GST_GL_MIXER_LOCK(mix) \
  (g_mutex_lock(&GST_GL_MIXER_GET_LOCK (mix)))
#define GST_GL_MIXER_UNLOCK(mix) \
  (g_mutex_unlock(&GST_GL_MIXER_GET_LOCK (mix)))

static void gst_gl_mixer_pad_class_init (GstGLMixerPadClass * klass);
static void gst_gl_mixer_pad_init (GstGLMixerPad * mixerpad);

static void gst_gl_mixer_pad_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_gl_mixer_pad_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);

static gboolean gst_gl_mixer_src_event (GstPad * pad, GstObject * object,
    GstEvent * event);
static gboolean gst_gl_mixer_sink_event (GstCollectPads * pads,
    GstCollectData * cdata, GstEvent * event, GstGLMixer * mix);
static gboolean gst_gl_mixer_src_setcaps (GstPad * pad, GstGLMixer * mix,
    GstCaps * caps);

enum
{
  PROP_PAD_0
};

G_DEFINE_TYPE (GstGLMixerPad, gst_gl_mixer_pad, GST_TYPE_PAD);

static void
gst_gl_mixer_pad_class_init (GstGLMixerPadClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->set_property = gst_gl_mixer_pad_set_property;
  gobject_class->get_property = gst_gl_mixer_pad_get_property;
}

static void
gst_gl_mixer_pad_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_mixer_pad_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_gl_mixer_update_src_caps (GstGLMixer * mix)
{
  GSList *l;
  gint best_width = -1, best_height = -1;
  gdouble best_fps = -1, cur_fps;
  gint best_fps_n = -1, best_fps_d = -1;
  gboolean ret = TRUE;

  GST_GL_MIXER_LOCK (mix);

  for (l = mix->sinkpads; l; l = l->next) {
    GstGLMixerPad *mpad = l->data;
    gint this_width, this_height;
    gint fps_n, fps_d;
    gint width, height;

    fps_n = GST_VIDEO_INFO_FPS_N (&mpad->info);
    fps_d = GST_VIDEO_INFO_FPS_D (&mpad->info);
    width = GST_VIDEO_INFO_WIDTH (&mpad->info);
    height = GST_VIDEO_INFO_HEIGHT (&mpad->info);

    if (fps_n == 0 || fps_d == 0 || width == 0 || height == 0)
      continue;

    this_width = width;
    this_height = height;

    if (best_width < this_width)
      best_width = this_width;
    if (best_height < this_height)
      best_height = this_height;

    if (fps_d == 0)
      cur_fps = 0.0;
    else
      gst_util_fraction_to_double (fps_n, fps_d, &cur_fps);

    if (best_fps < cur_fps) {
      best_fps = cur_fps;
      best_fps_n = fps_n;
      best_fps_d = fps_d;
    }
  }

  if (best_fps_n <= 0 && best_fps_d <= 0) {
    best_fps_n = 25;
    best_fps_d = 1;
    best_fps = 25.0;
  }

  if (best_width > 0 && best_height > 0 && best_fps > 0) {
    GstCaps *caps, *peercaps;
    GstStructure *s;
    GstVideoInfo info;

    if (GST_VIDEO_INFO_FPS_N (&mix->info) != best_fps_n ||
        GST_VIDEO_INFO_FPS_D (&mix->info) != best_fps_d) {
      if (mix->segment.position != -1) {
        mix->ts_offset = mix->segment.position - mix->segment.start;
        mix->nframes = 0;
      }
    }
    gst_video_info_set_format (&info, GST_VIDEO_INFO_FORMAT (&mix->info),
        best_width, best_height);
    info.fps_n = best_fps_n;
    info.fps_d = best_fps_d;
    info.par_n = GST_VIDEO_INFO_PAR_N (&mix->info);
    info.par_d = GST_VIDEO_INFO_PAR_D (&mix->info);

    caps = gst_video_info_to_caps (&info);

    peercaps = gst_pad_peer_query_caps (mix->srcpad, NULL);
    if (peercaps) {
      GstCaps *tmp;

      s = gst_caps_get_structure (caps, 0);
      gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT, "height",
          GST_TYPE_INT_RANGE, 1, G_MAXINT, "framerate", GST_TYPE_FRACTION_RANGE,
          0, 1, G_MAXINT, 1, NULL);

      tmp = gst_caps_intersect (caps, peercaps);
      gst_caps_unref (caps);
      gst_caps_unref (peercaps);
      caps = tmp;
      if (gst_caps_is_empty (caps)) {
        GST_DEBUG_OBJECT (mix, "empty caps");
        ret = FALSE;
        GST_GL_MIXER_UNLOCK (mix);
        goto done;
      }

      caps = gst_caps_truncate (caps);
      s = gst_caps_get_structure (caps, 0);
      gst_structure_fixate_field_nearest_int (s, "width", best_width);
      gst_structure_fixate_field_nearest_int (s, "height", best_height);
      gst_structure_fixate_field_nearest_fraction (s, "framerate", best_fps_n,
          best_fps_d);

      gst_structure_get_int (s, "width", &info.width);
      gst_structure_get_int (s, "height", &info.height);
      gst_structure_get_fraction (s, "fraction", &info.fps_n, &info.fps_d);
    }

    caps = gst_video_info_to_caps (&info);

    GST_GL_MIXER_UNLOCK (mix);
    ret = gst_gl_mixer_src_setcaps (mix->srcpad, mix, caps);
    gst_caps_unref (caps);
  } else {
    GST_GL_MIXER_UNLOCK (mix);
  }

done:
  return ret;
}

static gboolean
gst_gl_mixer_pad_sink_setcaps (GstPad * pad, GstObject * parent, GstCaps * caps)
{
  GstGLMixer *mix;
  GstGLMixerPad *mixpad;
  GstVideoInfo info;
  gboolean ret = FALSE;

  GST_INFO_OBJECT (pad, "Setting caps %" GST_PTR_FORMAT, caps);

  mix = GST_GL_MIXER (parent);
  mixpad = GST_GL_MIXER_PAD (pad);

  if (!gst_video_info_from_caps (&info, caps)) {
    GST_ERROR_OBJECT (pad, "Failed to parse caps");
    goto beach;
  }

  GST_GL_MIXER_LOCK (mix);
  if (GST_VIDEO_INFO_FORMAT (&mix->info) != GST_VIDEO_FORMAT_UNKNOWN) {
    if (GST_VIDEO_INFO_FORMAT (&mix->info) != GST_VIDEO_INFO_FORMAT (&info) ||
        GST_VIDEO_INFO_PAR_N (&mix->info) != GST_VIDEO_INFO_PAR_N (&info) ||
        GST_VIDEO_INFO_PAR_D (&mix->info) != GST_VIDEO_INFO_PAR_D (&info)) {
      GST_ERROR_OBJECT (pad, "Caps not compatible with other pads' caps");
      GST_GL_MIXER_UNLOCK (mix);
      goto beach;
    }
  }

  mix->info = info;
  mixpad->info = info;

  GST_GL_MIXER_UNLOCK (mix);

  ret = gst_gl_mixer_update_src_caps (mix);

beach:
  return ret;
}

static GstCaps *
gst_gl_mixer_pad_sink_getcaps (GstPad * pad, GstObject * parent,
    GstCaps * filter)
{
  GstGLMixer *mix;
  GstCaps *srccaps;
  GstStructure *s;
  gint i, n;

  mix = GST_GL_MIXER (parent);

  srccaps = gst_pad_get_current_caps (GST_PAD (mix->srcpad));
  if (srccaps == NULL)
    srccaps = gst_pad_get_pad_template_caps (GST_PAD (mix->srcpad));

  srccaps = gst_caps_make_writable (srccaps);

  n = gst_caps_get_size (srccaps);
  for (i = 0; i < n; i++) {
    s = gst_caps_get_structure (srccaps, i);
    gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
    if (!gst_structure_has_field (s, "pixel-aspect-ratio"))
      gst_structure_set (s, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          NULL);
  }

  GST_DEBUG_OBJECT (pad, "Returning %" GST_PTR_FORMAT, srccaps);

  return srccaps;
}

static gboolean
gst_gl_mixer_pad_sink_acceptcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps)
{
  gboolean ret;
  GstGLMixer *mix;
  GstCaps *accepted_caps;
  gint i, n;
  GstStructure *s;

  mix = GST_GL_MIXER (parent);
  GST_DEBUG_OBJECT (pad, "%" GST_PTR_FORMAT, caps);

  accepted_caps = gst_pad_get_current_caps (GST_PAD (mix->srcpad));
  if (accepted_caps == NULL)
    accepted_caps = gst_pad_get_pad_template_caps (GST_PAD (mix->srcpad));

  accepted_caps = gst_caps_make_writable (accepted_caps);
  GST_LOG_OBJECT (pad, "src caps %" GST_PTR_FORMAT, accepted_caps);

  n = gst_caps_get_size (accepted_caps);
  for (i = 0; i < n; i++) {
    s = gst_caps_get_structure (accepted_caps, i);
    gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
    if (!gst_structure_has_field (s, "pixel-aspect-ratio"))
      gst_structure_set (s, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          NULL);
  }

  ret = gst_caps_can_intersect (caps, accepted_caps);
  GST_INFO_OBJECT (pad, "%saccepted caps %" GST_PTR_FORMAT, (ret ? "" : "not "),
      caps);
  GST_INFO_OBJECT (pad, "acceptable caps are %" GST_PTR_FORMAT, accepted_caps);
  gst_caps_unref (accepted_caps);

  return ret;
}

static gboolean
gst_gl_mixer_pad_sink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *filter, *caps;

      gst_query_parse_caps (query, &filter);
      caps = gst_gl_mixer_pad_sink_getcaps (pad, parent, filter);
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      ret = TRUE;
      break;
    }
    case GST_QUERY_ACCEPT_CAPS:
    {
      GstCaps *caps;

      gst_query_parse_accept_caps (query, &caps);
      ret = gst_gl_mixer_pad_sink_acceptcaps (pad, parent, caps);
      gst_query_set_accept_caps_result (query, ret);
      ret = TRUE;
      break;
    }
    default:
      ret = gst_pad_query_default (pad, parent, query);
      break;
  }
  return ret;
}

static void
gst_gl_mixer_pad_init (GstGLMixerPad * mixerpad)
{
  gst_pad_set_query_function (GST_PAD (mixerpad), gst_gl_mixer_pad_sink_query);

  mixerpad->display = NULL;
}

/* GLMixer signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0
};

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_GL_VIDEO_CAPS)
    );

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink_%d",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS (GST_GL_VIDEO_CAPS)
    );

static void gst_gl_mixer_finalize (GObject * object);

static gboolean gst_gl_mixer_src_query (GstPad * pad, GstObject * object,
    GstQuery * query);
static GstFlowReturn gst_gl_mixer_sink_clip (GstCollectPads * pads,
    GstCollectData * data, GstBuffer * buf, GstBuffer ** outbuf,
    GstGLMixer * mix);

static GstFlowReturn gst_gl_mixer_collected (GstCollectPads * pads,
    GstGLMixer * mix);
static GstPad *gst_gl_mixer_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void gst_gl_mixer_release_pad (GstElement * element, GstPad * pad);

static void gst_gl_mixer_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gl_mixer_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static GstStateChangeReturn gst_gl_mixer_change_state (GstElement * element,
    GstStateChange transition);

static gboolean gst_gl_mixer_query_caps (GstPad * pad, GstObject * parent,
    GstQuery * query);
static gboolean gst_gl_mixer_query_duration (GstGLMixer * mix,
    GstQuery * query);
static gboolean gst_gl_mixer_query_latency (GstGLMixer * mix, GstQuery * query);

static gint64 gst_gl_mixer_do_qos (GstGLMixer * mix, GstClockTime timestamp);
static void gst_gl_mixer_update_qos (GstGLMixer * mix, gdouble proportion,
    GstClockTimeDiff diff, GstClockTime timestamp);
static void gst_gl_mixer_reset_qos (GstGLMixer * mix);
static void gst_gl_mixer_read_qos (GstGLMixer * mix, gdouble * proportion,
    GstClockTime * time);

static void gst_gl_mixer_child_proxy_init (gpointer g_iface,
    gpointer iface_data);

#define gst_gl_mixer_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstGLMixer, gst_gl_mixer, GST_TYPE_ELEMENT,
    G_IMPLEMENT_INTERFACE (GST_TYPE_CHILD_PROXY, gst_gl_mixer_child_proxy_init);
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "glmixer", 0, "opengl mixer"));
static void gst_gl_mixer_finalize (GObject * object);

static GObject *
gst_gl_mixer_child_proxy_get_child_by_index (GstChildProxy * child_proxy,
    guint index)
{
  GstGLMixer *mix = GST_GL_MIXER (child_proxy);
  GObject *obj;

  GST_GL_MIXER_LOCK (mix);
  if ((obj = g_slist_nth_data (mix->sinkpads, index)))
    g_object_ref (obj);
  GST_GL_MIXER_UNLOCK (mix);
  return obj;
}

static guint
gst_gl_mixer_child_proxy_get_children_count (GstChildProxy * child_proxy)
{
  guint count = 0;
  GstGLMixer *mix = GST_GL_MIXER (child_proxy);

  GST_GL_MIXER_LOCK (mix);
  count = mix->numpads;
  GST_GL_MIXER_UNLOCK (mix);
  GST_INFO_OBJECT (mix, "Children Count: %d", count);
  return count;
}

static void
gst_gl_mixer_child_proxy_init (gpointer g_iface, gpointer iface_data)
{
  GstChildProxyInterface *iface = g_iface;

  GST_INFO ("intializing child proxy interface");
  iface->get_child_by_index = gst_gl_mixer_child_proxy_get_child_by_index;
  iface->get_children_count = gst_gl_mixer_child_proxy_get_children_count;
}

static void
gst_gl_mixer_class_init (GstGLMixerClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_gl_mixer_finalize);

  gobject_class->get_property = gst_gl_mixer_get_property;
  gobject_class->set_property = gst_gl_mixer_set_property;

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_factory));

  element_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_gl_mixer_request_new_pad);
  element_class->release_pad = GST_DEBUG_FUNCPTR (gst_gl_mixer_release_pad);
  element_class->change_state = GST_DEBUG_FUNCPTR (gst_gl_mixer_change_state);

  /* Register the pad class */
  g_type_class_ref (GST_TYPE_GL_MIXER_PAD);

  klass->set_caps = NULL;
}

static void
gst_gl_mixer_collect_free (GstGLMixerCollect * mixcol)
{
  if (mixcol->buffer) {
    gst_buffer_unref (mixcol->buffer);
    mixcol->buffer = NULL;
  }
}

static void
gst_gl_mixer_reset (GstGLMixer * mix)
{
  GSList *l;

  gst_video_info_init (&mix->info);
  mix->ts_offset = 0;
  mix->nframes = 0;

  gst_segment_init (&mix->segment, GST_FORMAT_TIME);
  mix->segment.position = -1;

  /* clean up collect data */
  for (l = mix->sinkpads; l; l = l->next) {
    GstGLMixerPad *p = l->data;
    GstGLMixerCollect *mixcol = p->mixcol;

    gst_buffer_replace (&mixcol->buffer, NULL);
    mixcol->start_time = -1;
    mixcol->end_time = -1;

    gst_video_info_init (&p->info);
  }

  mix->newseg_pending = TRUE;
  mix->flush_stop_pending = FALSE;
}

static void
gst_gl_mixer_init (GstGLMixer * mix)
{
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (mix);

  mix->srcpad =
      gst_pad_new_from_template (gst_element_class_get_pad_template (klass,
          "src"), "src");
  gst_pad_set_query_function (GST_PAD (mix->srcpad),
      GST_DEBUG_FUNCPTR (gst_gl_mixer_src_query));
  gst_pad_set_event_function (GST_PAD (mix->srcpad),
      GST_DEBUG_FUNCPTR (gst_gl_mixer_src_event));
  gst_element_add_pad (GST_ELEMENT (mix), mix->srcpad);

  mix->collect = gst_collect_pads_new ();

  gst_collect_pads_set_function (mix->collect,
      (GstCollectPadsFunction) GST_DEBUG_FUNCPTR (gst_gl_mixer_collected), mix);
  gst_collect_pads_set_event_function (mix->collect,
      (GstCollectPadsEventFunction) gst_gl_mixer_sink_event, mix);
  gst_collect_pads_set_clip_function (mix->collect,
      (GstCollectPadsClipFunction) gst_gl_mixer_sink_clip, mix);

  g_mutex_init (&mix->lock);

  mix->array_buffers = 0;
  mix->display = NULL;
  mix->fbo = 0;
  mix->depthbuffer = 0;

  /* initialize variables */
  gst_gl_mixer_reset (mix);
}

static void
gst_gl_mixer_finalize (GObject * object)
{
  GstGLMixer *mix = GST_GL_MIXER (object);

  gst_object_unref (mix->collect);
  g_mutex_clear (&mix->lock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_gl_mixer_query_duration (GstGLMixer * mix, GstQuery * query)
{
  GValue item = { 0 };
  gint64 max;
  gboolean res;
  GstFormat format;
  GstIterator *it;
  gboolean done;

  /* parse format */
  gst_query_parse_duration (query, &format, NULL);

  max = -1;
  res = TRUE;
  done = FALSE;

  /* Take maximum of all durations */
  it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (mix));
  while (!done) {
    GstIteratorResult ires;

    ires = gst_iterator_next (it, &item);
    switch (ires) {
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
      case GST_ITERATOR_OK:
      {
        GstPad *pad;
        gint64 duration;

        pad = g_value_get_object (&item);

        /* ask sink peer for duration */
        res &= gst_pad_peer_query_duration (pad, format, &duration);
        /* take max from all valid return values */
        if (res) {
          /* valid unknown length, stop searching */
          if (duration == -1) {
            max = duration;
            done = TRUE;
          }
          /* else see if bigger than current max */
          else if (duration > max)
            max = duration;
        }
        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        max = -1;
        res = TRUE;
        gst_iterator_resync (it);
        break;
      default:
        res = FALSE;
        done = TRUE;
        break;
    }
  }
  g_value_reset (&item);
  gst_iterator_free (it);

  if (res) {
    /* and store the max */
    GST_DEBUG_OBJECT (mix, "Total duration in format %s: %"
        GST_TIME_FORMAT, gst_format_get_name (format), GST_TIME_ARGS (max));
    gst_query_set_duration (query, format, max);
  }

  return res;
}

static gboolean
gst_gl_mixer_query_caps (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstCaps *filter, *caps;
  GstGLMixer *mix = GST_GL_MIXER (parent);
  GstStructure *s;
  gint n;

  gst_query_parse_caps (query, &filter);

  if (GST_VIDEO_INFO_FORMAT (&mix->info) != GST_VIDEO_FORMAT_UNKNOWN) {
    caps = gst_pad_get_current_caps (mix->srcpad);
  } else {
    caps = gst_pad_get_pad_template_caps (mix->srcpad);
  }

  caps = gst_caps_make_writable (caps);

  n = gst_caps_get_size (caps) - 1;
  for (; n >= 0; n--) {
    s = gst_caps_get_structure (caps, n);
    gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);
    if (GST_VIDEO_INFO_FPS_D (&mix->info) != 0) {
      gst_structure_set (s,
          "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
    }
  }
  gst_query_set_caps_result (query, caps);

  return TRUE;
}

static gboolean
gst_gl_mixer_query_latency (GstGLMixer * mix, GstQuery * query)
{
  GValue item = { 0 };
  GstClockTime min, max;
  gboolean live;
  gboolean res;
  GstIterator *it;
  gboolean done;

  res = TRUE;
  done = FALSE;
  live = FALSE;
  min = 0;
  max = GST_CLOCK_TIME_NONE;

  /* Take maximum of all latency values */
  it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (mix));
  while (!done) {
    GstIteratorResult ires;

    ires = gst_iterator_next (it, &item);
    switch (ires) {
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
      case GST_ITERATOR_OK:
      {
        GstPad *pad;
        GstQuery *peerquery;
        GstClockTime min_cur, max_cur;
        gboolean live_cur;

        pad = g_value_get_object (&item);
        peerquery = gst_query_new_latency ();

        /* Ask peer for latency */
        res &= gst_pad_peer_query (pad, peerquery);

        /* take max from all valid return values */
        if (res) {
          gst_query_parse_latency (peerquery, &live_cur, &min_cur, &max_cur);

          if (min_cur > min)
            min = min_cur;

          if (max_cur != GST_CLOCK_TIME_NONE &&
              ((max != GST_CLOCK_TIME_NONE && max_cur > max) ||
                  (max == GST_CLOCK_TIME_NONE)))
            max = max_cur;

          live = live || live_cur;
        }

        gst_query_unref (peerquery);
        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        live = FALSE;
        min = 0;
        max = GST_CLOCK_TIME_NONE;
        res = TRUE;
        gst_iterator_resync (it);
        break;
      default:
        res = FALSE;
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (it);

  if (res) {
    /* store the results */
    GST_DEBUG_OBJECT (mix, "Calculated total latency: live %s, min %"
        GST_TIME_FORMAT ", max %" GST_TIME_FORMAT,
        (live ? "yes" : "no"), GST_TIME_ARGS (min), GST_TIME_ARGS (max));
    gst_query_set_latency (query, live, min, max);
  }

  return res;
}

static void
gst_gl_mixer_update_qos (GstGLMixer * mix, gdouble proportion,
    GstClockTimeDiff diff, GstClockTime timestamp)
{
  GST_DEBUG_OBJECT (mix,
      "Updating QoS: proportion %lf, diff %s%" GST_TIME_FORMAT ", timestamp %"
      GST_TIME_FORMAT, proportion, (diff < 0) ? "-" : "",
      GST_TIME_ARGS (ABS (diff)), GST_TIME_ARGS (timestamp));

  GST_OBJECT_LOCK (mix);
  mix->proportion = proportion;
  if (G_LIKELY (timestamp != GST_CLOCK_TIME_NONE)) {
    if (G_UNLIKELY (diff > 0))
      mix->earliest_time =
          timestamp + 2 * diff + gst_util_uint64_scale_int (GST_SECOND,
          GST_VIDEO_INFO_FPS_D (&mix->info), GST_VIDEO_INFO_FPS_N (&mix->info));
    else
      mix->earliest_time = timestamp + diff;
  } else {
    mix->earliest_time = GST_CLOCK_TIME_NONE;
  }
  GST_OBJECT_UNLOCK (mix);
}

static void
gst_gl_mixer_reset_qos (GstGLMixer * mix)
{
  gst_gl_mixer_update_qos (mix, 0.5, 0, GST_CLOCK_TIME_NONE);
  mix->qos_processed = mix->qos_dropped = 0;
}

static void
gst_gl_mixer_read_qos (GstGLMixer * mix, gdouble * proportion,
    GstClockTime * time)
{
  GST_OBJECT_LOCK (mix);
  *proportion = mix->proportion;
  *time = mix->earliest_time;
  GST_OBJECT_UNLOCK (mix);
}

static gboolean
gst_gl_mixer_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstGLMixer *mix = GST_GL_MIXER (parent);
  gboolean res = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
    {
      GstFormat format;

      gst_query_parse_position (query, &format, NULL);

      switch (format) {
        case GST_FORMAT_TIME:
          gst_query_set_position (query, format,
              gst_segment_to_stream_time (&mix->segment, GST_FORMAT_TIME,
                  mix->segment.position));
          res = TRUE;
          break;
        default:
          break;
      }
      break;
    }
    case GST_QUERY_DURATION:
      res = gst_gl_mixer_query_duration (mix, query);
      break;
    case GST_QUERY_LATENCY:
      res = gst_gl_mixer_query_latency (mix, query);
      break;
    case GST_QUERY_CAPS:
      res = gst_gl_mixer_query_caps (pad, parent, query);
      break;
    case GST_QUERY_CUSTOM:
    {
      /* mix is a sink in terms of gl chain, so we are sharing the gldisplay that
       * comes from src pad with every display of the sink pads */
      GSList *walk = mix->sinkpads;
      GstStructure *structure = gst_query_writable_structure (query);

      res =
          g_strcmp0 (gst_element_get_name (mix),
          gst_structure_get_name (structure)) == 0;

      if (!res) {
        GstGLDisplay *foreign_display = NULL;
        gulong foreign_gl_context = 0;

        if (mix->display) {
          /* this gl filter is a sink in terms of the gl chain */
          foreign_display = mix->display;
        } else {
          /* at least one gl element is after in our gl chain */
          /* id_value is set by upstream element of itself when going
           * to paused state */
          const GValue *id_value =
              gst_structure_get_value (structure, "gstgldisplay");
          foreign_display = GST_GL_DISPLAY (g_value_get_pointer (id_value));
        }

        foreign_gl_context =
            gst_gl_display_get_internal_gl_context (foreign_display);

        /* iterate on each sink pad until reaching the gl element
         * that requested the query */
        while (!res && walk) {
          GstGLMixerPad *sink_pad = GST_GL_MIXER_PAD (walk->data);
          GstPad *peer = gst_pad_get_peer (GST_PAD_CAST (sink_pad));
          walk = g_slist_next (walk);

          g_return_val_if_fail (sink_pad->display != NULL, FALSE);

          gst_gl_display_activate_gl_context (foreign_display, FALSE);

          res =
              gst_gl_display_create_context (sink_pad->display,
              foreign_gl_context);

          gst_gl_display_activate_gl_context (foreign_display, TRUE);

          if (res)
            gst_structure_set (structure, "gstgldisplay", G_TYPE_POINTER,
                sink_pad->display, NULL);
          else
            GST_ELEMENT_ERROR (mix, RESOURCE, NOT_FOUND,
                GST_GL_DISPLAY_ERR_MSG (sink_pad->display), (NULL));

          /* does not work:
           * res = gst_pad_query_default (GST_PAD_CAST (sink_pad), query);*/
          res = gst_pad_query (peer, query);
          gst_object_unref (peer);
        }
      }
      break;
    }
    default:
      /* FIXME, needs a custom query handler because we have multiple
       * sinkpads, send to the master pad until then */
      res = FALSE;
      gst_query_unref (query);
      break;
  }

  return res;
}

static gboolean
gst_gl_mixer_src_setcaps (GstPad * pad, GstGLMixer * mix, GstCaps * caps)
{
  GstGLMixerClass *mixer_class = GST_GL_MIXER_GET_CLASS (mix);
  GstVideoInfo info;
  gboolean ret;

  GST_INFO_OBJECT (mix, "set src caps: %" GST_PTR_FORMAT, caps);

  if (!gst_video_info_from_caps (&info, caps)) {
    ret = TRUE;
    goto done;
  }

  GST_GL_MIXER_LOCK (mix);

  if (GST_VIDEO_INFO_FPS_N (&mix->info) != GST_VIDEO_INFO_FPS_N (&info) ||
      GST_VIDEO_INFO_FPS_D (&mix->info) != GST_VIDEO_INFO_FPS_D (&info)) {
    if (mix->segment.position != -1) {
      mix->ts_offset = mix->segment.position - mix->segment.start;
      mix->nframes = 0;
    }
    gst_gl_mixer_reset_qos (mix);
  }

  mix->info = info;

  GST_GL_MIXER_UNLOCK (mix);

  if (!gst_gl_display_gen_fbo (mix->display, GST_VIDEO_INFO_WIDTH (&mix->info),
          GST_VIDEO_INFO_HEIGHT (&mix->info), &mix->fbo, &mix->depthbuffer)) {
    GST_ELEMENT_ERROR (mix, RESOURCE, NOT_FOUND,
        GST_GL_DISPLAY_ERR_MSG (mix->display), (NULL));
    ret = FALSE;
    goto done;
  }

  if (mixer_class->set_caps)
    mixer_class->set_caps (mix, caps);

  ret = TRUE;
done:

  return ret;
}

static GstPad *
gst_gl_mixer_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * req_name, const GstCaps * caps)
{
  GstGLMixer *mix;
  GstGLMixerPad *mixpad;
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (element);

  mix = GST_GL_MIXER (element);

  if (templ == gst_element_class_get_pad_template (klass, "sink_%d")) {
    gint serial = 0;
    gchar *name = NULL;
    GstGLMixerCollect *mixcol = NULL;

    GST_GL_MIXER_LOCK (mix);
    if (req_name == NULL || strlen (req_name) < 6
        || !g_str_has_prefix (req_name, "sink_")) {
      /* no name given when requesting the pad, use next available int */
      serial = mix->next_sinkpad++;
    } else {
      /* parse serial number from requested padname */
      serial = g_ascii_strtoull (&req_name[5], NULL, 10);
      if (serial >= mix->next_sinkpad)
        mix->next_sinkpad = serial + 1;
    }
    /* create new pad with the name */
    name = g_strdup_printf ("sink_%d", serial);
    mixpad = g_object_new (GST_TYPE_GL_MIXER_PAD, "name", name, "direction",
        templ->direction, "template", templ, NULL);
    g_free (name);

    mixcol = (GstGLMixerCollect *)
        gst_collect_pads_add_pad_full (mix->collect, GST_PAD (mixpad),
        sizeof (GstGLMixerCollect),
        (GstCollectDataDestroyNotify) gst_gl_mixer_collect_free, TRUE);

    /* Keep track of each other */
    mixcol->mixpad = mixpad;
    mixpad->mixcol = mixcol;

    mixcol->start_time = -1;
    mixcol->end_time = -1;

    /* Keep an internal list of mixpads for zordering */
    mix->sinkpads = g_slist_append (mix->sinkpads, mixpad);
    mix->numpads++;
    GST_GL_MIXER_UNLOCK (mix);
  } else {
    return NULL;
  }

  GST_DEBUG_OBJECT (element, "Adding pad %s", GST_PAD_NAME (mixpad));

  /* add the pad to the element */
  gst_element_add_pad (element, GST_PAD (mixpad));
  gst_child_proxy_child_added (GST_CHILD_PROXY (mix), G_OBJECT (mixpad),
      GST_OBJECT_NAME (mixpad));

  return GST_PAD (mixpad);
}

static void
gst_gl_mixer_release_pad (GstElement * element, GstPad * pad)
{
  GstGLMixer *mix;
  GstGLMixerPad *mixpad;
  gboolean update_caps;

  mix = GST_GL_MIXER (element);

  GST_GL_MIXER_LOCK (mix);
  if (G_UNLIKELY (g_slist_find (mix->sinkpads, pad) == NULL)) {
    g_warning ("Unknown pad %s", GST_PAD_NAME (pad));
    goto error;
  }

  mixpad = GST_GL_MIXER_PAD (pad);

  mix->sinkpads = g_slist_remove (mix->sinkpads, pad);
  gst_child_proxy_child_removed (GST_CHILD_PROXY (mix), G_OBJECT (mixpad),
      GST_OBJECT_NAME (mixpad));
  mix->numpads--;

  update_caps = GST_VIDEO_INFO_FORMAT (&mix->info) != GST_VIDEO_FORMAT_UNKNOWN;
  GST_GL_MIXER_UNLOCK (mix);

  gst_collect_pads_remove_pad (mix->collect, pad);

  if (update_caps)
    gst_gl_mixer_update_src_caps (mix);

  gst_element_remove_pad (element, pad);
  return;
error:
  GST_GL_MIXER_UNLOCK (mix);
}

/* try to get a buffer on all pads. As long as the queued value is
 * negative, we skip buffers */
static gint
gst_gl_mixer_fill_queues (GstGLMixer * mix,
    GstClockTime output_start_time, GstClockTime output_end_time)
{
  GSList *l;
  gboolean eos = TRUE;
  gboolean need_more_data = FALSE;

  for (l = mix->sinkpads; l; l = l->next) {
    GstGLMixerPad *pad = l->data;
    GstGLMixerCollect *mixcol = pad->mixcol;
    GstSegment *segment = &pad->mixcol->collect.segment;
    GstBuffer *buf;

    buf = gst_collect_pads_peek (mix->collect, &mixcol->collect);
    if (buf) {
      GstClockTime start_time, end_time;

      start_time = GST_BUFFER_TIMESTAMP (buf);
      if (start_time == -1) {
        gst_buffer_unref (buf);
        GST_ERROR_OBJECT (pad, "Need timestamped buffers!");
        return -2;
      }

      /* FIXME: Make all this work with negative rates */

      if ((mixcol->buffer && start_time < GST_BUFFER_TIMESTAMP (mixcol->buffer))
          || (mixcol->queued
              && start_time < GST_BUFFER_TIMESTAMP (mixcol->queued))) {
        GST_WARNING_OBJECT (pad, "Buffer from the past, dropping");
        gst_buffer_unref (buf);
        buf = gst_collect_pads_pop (mix->collect, &mixcol->collect);
        gst_buffer_unref (buf);
        need_more_data = TRUE;
        continue;
      }

      if (mixcol->queued) {
        end_time = start_time - GST_BUFFER_TIMESTAMP (mixcol->queued);
        start_time = GST_BUFFER_TIMESTAMP (mixcol->queued);
        gst_buffer_unref (buf);
        buf = gst_buffer_ref (mixcol->queued);
      } else {
        end_time = GST_BUFFER_DURATION (buf);

        if (end_time == -1) {
          mixcol->queued = buf;
          need_more_data = TRUE;
          continue;
        }
      }

      g_assert (start_time != -1 && end_time != -1);
      end_time += start_time;   /* convert from duration to position */

      if (mixcol->end_time != -1 && mixcol->end_time > end_time) {
        GST_WARNING_OBJECT (pad, "Buffer from the past, dropping");
        if (buf == mixcol->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&mixcol->queued, NULL);
        } else {
          gst_buffer_unref (buf);
          buf = gst_collect_pads_pop (mix->collect, &mixcol->collect);
          gst_buffer_unref (buf);
        }

        need_more_data = TRUE;
        continue;
      }

      /* Check if it's inside the segment */
      if (start_time >= segment->stop || end_time < segment->start) {
        GST_DEBUG_OBJECT (pad, "Buffer outside the segment");

        if (buf == mixcol->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&mixcol->queued, NULL);
        } else {
          gst_buffer_unref (buf);
          buf = gst_collect_pads_pop (mix->collect, &mixcol->collect);
          gst_buffer_unref (buf);
        }

        need_more_data = TRUE;
        continue;
      }

      /* Clip to segment and convert to running time */
      start_time = MAX (start_time, segment->start);
      if (segment->stop != -1)
        end_time = MIN (end_time, segment->stop);
      start_time =
          gst_segment_to_running_time (segment, GST_FORMAT_TIME, start_time);
      end_time =
          gst_segment_to_running_time (segment, GST_FORMAT_TIME, end_time);
      g_assert (start_time != -1 && end_time != -1);

      /* Convert to the output segment rate */
      if (ABS (mix->segment.rate) != 1.0) {
        start_time *= ABS (mix->segment.rate);
        end_time *= ABS (mix->segment.rate);
      }

      if (end_time >= output_start_time && start_time < output_end_time) {
        GST_DEBUG_OBJECT (pad,
            "Taking new buffer with start time %" GST_TIME_FORMAT,
            GST_TIME_ARGS (start_time));
        gst_buffer_replace (&mixcol->buffer, buf);
        mixcol->start_time = start_time;
        mixcol->end_time = end_time;

        if (buf == mixcol->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&mixcol->queued, NULL);
        } else {
          gst_buffer_unref (buf);
          buf = gst_collect_pads_pop (mix->collect, &mixcol->collect);
          gst_buffer_unref (buf);
        }
        eos = FALSE;
      } else if (start_time >= output_end_time) {
        GST_DEBUG_OBJECT (pad, "Keeping buffer until %" GST_TIME_FORMAT,
            GST_TIME_ARGS (start_time));
        gst_buffer_unref (buf);
        eos = FALSE;
      } else {
        GST_DEBUG_OBJECT (pad, "Too old buffer -- dropping");
        if (buf == mixcol->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&mixcol->queued, NULL);
        } else {
          gst_buffer_unref (buf);
          buf = gst_collect_pads_pop (mix->collect, &mixcol->collect);
          gst_buffer_unref (buf);
        }

        need_more_data = TRUE;
        continue;
      }
    } else {
      if (mixcol->end_time != -1) {
        if (mixcol->end_time < output_start_time) {
          gst_buffer_replace (&mixcol->buffer, NULL);
          mixcol->start_time = mixcol->end_time = -1;
          if (!GST_COLLECT_PADS_STATE_IS_SET (mixcol,
                  GST_COLLECT_PADS_STATE_EOS))
            need_more_data = TRUE;
        } else {
          eos = FALSE;
        }
      }
    }
  }

  if (need_more_data)
    return 0;
  if (eos)
    return -1;

  return 1;
}

static void
gst_gl_mixer_process_buffers (GstGLMixer * mix, GstBuffer * outbuf)
{
  GstGLMixerClass *mix_class = GST_GL_MIXER_GET_CLASS (mix);
  GSList *walk = mix->sinkpads;
  gint array_index = 0;

  while (walk) {                /* We walk with this list because it's ordered */
    GstGLMixerPad *pad = GST_GL_MIXER_PAD (walk->data);
    GstGLMixerCollect *mixcol = pad->mixcol;

    walk = g_slist_next (walk);

    if (mixcol->buffer != NULL) {
      GstClockTime timestamp;
      gint64 stream_time;
      GstSegment *seg;

      seg = &mixcol->collect.segment;

      timestamp = GST_BUFFER_TIMESTAMP (mixcol->buffer);

      stream_time =
          gst_segment_to_stream_time (seg, GST_FORMAT_TIME, timestamp);

      /* sync object properties on stream time */
      if (GST_CLOCK_TIME_IS_VALID (stream_time))
        gst_object_sync_values (GST_OBJECT (pad), stream_time);

      /* put buffer into array */
      mix->array_buffers->pdata[array_index] = mixcol->buffer;

      /*if (pad == mix->master) {
         gint64 running_time;

         running_time =
         gst_segment_to_running_time (seg, GST_FORMAT_TIME, timestamp); */

      /* outgoing buffers need the running_time */
      /*GST_BUFFER_TIMESTAMP (outbuf) = running_time;
         GST_BUFFER_DURATION (outbuf) = GST_BUFFER_DURATION (mixcol->buffer);

         mix->last_ts = running_time;
         if (GST_BUFFER_DURATION_IS_VALID (outbuf))
         mix->last_ts += GST_BUFFER_DURATION (outbuf);
         } */
    }
    ++array_index;
  }

  mix_class->process_buffers (mix, mix->array_buffers, outbuf);
}

/* Perform qos calculations before processing the next frame. Returns TRUE if
 * the frame should be processed, FALSE if the frame can be dropped entirely */
static gint64
gst_gl_mixer_do_qos (GstGLMixer * mix, GstClockTime timestamp)
{
  GstClockTime qostime, earliest_time;
  gdouble proportion;
  gint64 jitter;

  /* no timestamp, can't do QoS => process frame */
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (timestamp))) {
    GST_LOG_OBJECT (mix, "invalid timestamp, can't do QoS, process frame");
    return -1;
  }

  /* get latest QoS observation values */
  gst_gl_mixer_read_qos (mix, &proportion, &earliest_time);

  /* skip qos if we have no observation (yet) => process frame */
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (earliest_time))) {
    GST_LOG_OBJECT (mix, "no observation yet, process frame");
    return -1;
  }

  /* qos is done on running time */
  qostime =
      gst_segment_to_running_time (&mix->segment, GST_FORMAT_TIME, timestamp);

  /* see how our next timestamp relates to the latest qos timestamp */
  GST_LOG_OBJECT (mix, "qostime %" GST_TIME_FORMAT ", earliest %"
      GST_TIME_FORMAT, GST_TIME_ARGS (qostime), GST_TIME_ARGS (earliest_time));

  jitter = GST_CLOCK_DIFF (qostime, earliest_time);
  if (qostime != GST_CLOCK_TIME_NONE && jitter > 0) {
    GST_DEBUG_OBJECT (mix, "we are late, drop frame");
    return jitter;
  }

  GST_LOG_OBJECT (mix, "process frame");
  return jitter;
}

static GstFlowReturn
gst_gl_mixer_collected (GstCollectPads * pads, GstGLMixer * mix)
{
  GstFlowReturn ret;
  GstClockTime output_start_time, output_end_time;
  GstBuffer *outbuf = NULL;
  gint res;
  gint64 jitter;

  g_return_val_if_fail (GST_IS_GL_MIXER (mix), GST_FLOW_ERROR);

  /* If we're not negotiated yet... */
  if (GST_VIDEO_INFO_FORMAT (&mix->info) == GST_VIDEO_FORMAT_UNKNOWN)
    return GST_FLOW_NOT_NEGOTIATED;

  if (g_atomic_int_compare_and_exchange (&mix->flush_stop_pending, TRUE, FALSE)) {
    GST_DEBUG_OBJECT (mix, "pending flush stop");
    gst_pad_push_event (mix->srcpad, gst_event_new_flush_stop (TRUE));
  }

  GST_GL_MIXER_LOCK (mix);

  if (mix->newseg_pending) {
    GST_DEBUG_OBJECT (mix, "Sending NEWSEGMENT event");
    if (!gst_pad_push_event (mix->srcpad,
            gst_event_new_segment (&mix->segment))) {
      ret = GST_FLOW_ERROR;
      goto error;
    }
    mix->newseg_pending = FALSE;
  }

  if (mix->segment.position == -1)
    output_start_time = mix->segment.start;
  else
    output_start_time = mix->segment.position;

  if (output_start_time >= mix->segment.stop) {
    GST_DEBUG_OBJECT (mix, "Segment done");
    gst_pad_push_event (mix->srcpad, gst_event_new_eos ());
    ret = GST_FLOW_EOS;
    goto error;
  }

  output_end_time =
      mix->ts_offset + gst_util_uint64_scale (mix->nframes + 1,
      GST_SECOND * GST_VIDEO_INFO_FPS_D (&mix->info),
      GST_VIDEO_INFO_FPS_N (&mix->info));
  if (mix->segment.stop != -1)
    output_end_time = MIN (output_end_time, mix->segment.stop);

  res = gst_gl_mixer_fill_queues (mix, output_start_time, output_end_time);

  if (res == 0) {
    GST_DEBUG_OBJECT (mix, "Need more data for decisions");
    ret = GST_FLOW_OK;
    goto error;
  } else if (res == -1) {
    GST_DEBUG_OBJECT (mix, "All sinkpads are EOS -- forwarding");
    gst_pad_push_event (mix->srcpad, gst_event_new_eos ());
    ret = GST_FLOW_EOS;
    goto error;
  } else if (res == -2) {
    GST_ERROR_OBJECT (mix, "Error collecting buffers");
    ret = GST_FLOW_ERROR;
    goto error;
  }

  jitter = gst_gl_mixer_do_qos (mix, output_start_time);
  if (jitter <= 0) {
    /* FIXME: Use GstGLMeta */
    /* Calculating out buffer size from input size */
    /*ret = gst_gl_buffer_parse_caps (GST_PAD_CAPS (mix->srcpad), &width, &height);

       gl_outbuf = gst_gl_buffer_new (mix->display, mix->width, mix->height);

       outbuf = GST_BUFFER (gl_outbuf);
       gst_buffer_set_caps (outbuf, GST_PAD_CAPS (mix->srcpad)); */

    gst_gl_mixer_process_buffers (mix, outbuf);
    mix->qos_processed++;
  } else {
    GstMessage *msg;

    mix->qos_dropped++;

    /* TODO: live */
    msg =
        gst_message_new_qos (GST_OBJECT_CAST (mix), FALSE,
        gst_segment_to_running_time (&mix->segment, GST_FORMAT_TIME,
            output_start_time), gst_segment_to_stream_time (&mix->segment,
            GST_FORMAT_TIME, output_start_time), output_start_time,
        output_end_time - output_start_time);
    gst_message_set_qos_values (msg, jitter, mix->proportion, 1000000);
    gst_message_set_qos_stats (msg, GST_FORMAT_BUFFERS, mix->qos_processed,
        mix->qos_dropped);
    gst_element_post_message (GST_ELEMENT_CAST (mix), msg);

    ret = GST_FLOW_OK;
  }

  mix->segment.position = output_end_time;
  mix->nframes++;

  GST_GL_MIXER_UNLOCK (mix);
  if (outbuf) {
    GST_LOG_OBJECT (mix,
        "Pushing buffer with ts %" GST_TIME_FORMAT " and duration %"
        GST_TIME_FORMAT, GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)),
        GST_TIME_ARGS (GST_BUFFER_DURATION (outbuf)));
    ret = gst_pad_push (mix->srcpad, outbuf);
  }

done:
  return ret;

  /* ERRORS */
error:
  {
    GST_GL_MIXER_UNLOCK (mix);
    goto done;
  }
}

static gboolean
forward_event_func (GstPad * pad, GValue * ret, GstEvent * event)
{
  gst_event_ref (event);
  GST_LOG_OBJECT (pad, "About to send event %s", GST_EVENT_TYPE_NAME (event));
  if (!gst_pad_push_event (pad, event)) {
    g_value_set_boolean (ret, FALSE);
    GST_WARNING_OBJECT (pad, "Sending event  %p (%s) failed.",
        event, GST_EVENT_TYPE_NAME (event));
  } else {
    GST_LOG_OBJECT (pad, "Sent event  %p (%s).",
        event, GST_EVENT_TYPE_NAME (event));
  }
  gst_object_unref (pad);
  return TRUE;
}

/* forwards the event to all sinkpads, takes ownership of the
 * event
 *
 * Returns: TRUE if the event could be forwarded on all
 * sinkpads.
 */
static gboolean
gst_gl_mixer_push_sink_event (GstGLMixer * mix, GstEvent * event)
{
  GstIterator *it;
  GValue vret = { 0 };

  GST_LOG_OBJECT (mix, "Forwarding event %p (%s)", event,
      GST_EVENT_TYPE_NAME (event));

  g_value_init (&vret, G_TYPE_BOOLEAN);
  g_value_set_boolean (&vret, TRUE);
  it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (mix));
  gst_iterator_fold (it, (GstIteratorFoldFunction) forward_event_func, &vret,
      event);
  gst_iterator_free (it);
  gst_event_unref (event);

  return g_value_get_boolean (&vret);
}

static GstFlowReturn
gst_gl_mixer_sink_clip (GstCollectPads * pads,
    GstCollectData * data, GstBuffer * buf, GstBuffer ** outbuf,
    GstGLMixer * mix)
{
  GstGLMixerPad *pad = GST_GL_MIXER_PAD (data->pad);
  GstGLMixerCollect *mixcol = pad->mixcol;
  GstClockTime start_time, end_time;

  start_time = GST_BUFFER_TIMESTAMP (buf);
  if (start_time == -1) {
    GST_ERROR_OBJECT (pad, "Timestamped buffers required!");
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }

  end_time = GST_BUFFER_DURATION (buf);
  if (end_time == -1)
    end_time =
        gst_util_uint64_scale_int (GST_SECOND,
        GST_VIDEO_INFO_FPS_D (&pad->info), GST_VIDEO_INFO_FPS_N (&pad->info));
  if (end_time == -1) {
    *outbuf = buf;
    return GST_FLOW_OK;
  }

  start_time = MAX (start_time, mixcol->collect.segment.start);
  start_time =
      gst_segment_to_running_time (&mixcol->collect.segment,
      GST_FORMAT_TIME, start_time);

  end_time += GST_BUFFER_TIMESTAMP (buf);
  if (mixcol->collect.segment.stop != -1)
    end_time = MIN (end_time, mixcol->collect.segment.stop);
  end_time =
      gst_segment_to_running_time (&mixcol->collect.segment,
      GST_FORMAT_TIME, end_time);

  /* Convert to the output segment rate */
  if (ABS (mix->segment.rate) != 1.0) {
    start_time *= ABS (mix->segment.rate);
    end_time *= ABS (mix->segment.rate);
  }

  if (mixcol->buffer != NULL && end_time < mixcol->end_time) {
    gst_buffer_unref (buf);
    *outbuf = NULL;
    return GST_FLOW_OK;
  }

  *outbuf = buf;
  return GST_FLOW_OK;
}

static gboolean
gst_gl_mixer_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstGLMixer *mix = GST_GL_MIXER (parent);
  gboolean result;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_QOS:
    {
      GstQOSType type;
      GstClockTimeDiff diff;
      GstClockTime timestamp;
      gdouble proportion;

      gst_event_parse_qos (event, &type, &proportion, &diff, &timestamp);

      gst_gl_mixer_update_qos (mix, proportion, diff, timestamp);

      result = gst_gl_mixer_push_sink_event (mix, event);
      break;
    }
    case GST_EVENT_SEEK:
    {
      gdouble rate;
      GstFormat fmt;
      GstSeekFlags flags;
      GstSeekType start_type, stop_type;
      gint64 start, stop;
      GSList *l;
      gdouble abs_rate;

      /* parse the seek parameters */
      gst_event_parse_seek (event, &rate, &fmt, &flags, &start_type,
          &start, &stop_type, &stop);

      if (rate <= 0.0) {
        GST_ERROR_OBJECT (mix, "Negative rates not supported yet");
        result = FALSE;
        gst_event_unref (event);
        break;
      }

      GST_DEBUG_OBJECT (mix, "Handling SEEK event");

      /* check if we are flushing */
      if (flags & GST_SEEK_FLAG_FLUSH) {
        /* flushing seek, start flush downstream, the flush will be done
         * when all pads received a FLUSH_STOP. */
        gst_pad_push_event (mix->srcpad, gst_event_new_flush_start ());

        /* make sure we accept nothing anymore and return WRONG_STATE */
        gst_collect_pads_set_flushing (mix->collect, TRUE);
      }

      /* now wait for the collected to be finished and mark a new
       * segment */
      GST_COLLECT_PADS_STREAM_LOCK (mix->collect);

      abs_rate = ABS (rate);

      GST_GL_MIXER_LOCK (mix);
      for (l = mix->sinkpads; l; l = l->next) {
        GstGLMixerPad *p = l->data;

        if (flags & GST_SEEK_FLAG_FLUSH) {
          gst_buffer_replace (&p->mixcol->buffer, NULL);
          p->mixcol->start_time = p->mixcol->end_time = -1;
          continue;
        }

        /* Convert to the output segment rate */
        if (ABS (mix->segment.rate) != abs_rate) {
          if (ABS (mix->segment.rate) != 1.0 && p->mixcol->buffer) {
            p->mixcol->start_time /= ABS (mix->segment.rate);
            p->mixcol->end_time /= ABS (mix->segment.rate);
          }
          if (abs_rate != 1.0 && p->mixcol->buffer) {
            p->mixcol->start_time *= abs_rate;
            p->mixcol->end_time *= abs_rate;
          }
        }
      }
      GST_GL_MIXER_UNLOCK (mix);

      gst_segment_do_seek (&mix->segment, rate, fmt, flags, start_type, start,
          stop_type, stop, NULL);
      mix->segment.position = -1;
      mix->ts_offset = 0;
      mix->nframes = 0;
      mix->newseg_pending = TRUE;

      if (flags & GST_SEEK_FLAG_FLUSH) {
        gst_collect_pads_set_flushing (mix->collect, FALSE);

        /* we can't send FLUSH_STOP here since upstream could start pushing data
         * after we unlock mix->collect.
         * We set flush_stop_pending to TRUE instead and send FLUSH_STOP after
         * forwarding the seek upstream or from gst_gl_mixer_collected,
         * whichever happens first.
         */
        mix->flush_stop_pending = TRUE;
      }

      GST_COLLECT_PADS_STREAM_UNLOCK (mix->collect);

      gst_gl_mixer_reset_qos (mix);

      result = gst_gl_mixer_push_sink_event (mix, event);

      if (g_atomic_int_compare_and_exchange (&mix->flush_stop_pending, TRUE,
              FALSE)) {
        GST_DEBUG_OBJECT (mix, "pending flush stop");
        gst_pad_push_event (mix->srcpad, gst_event_new_flush_stop (TRUE));
      }

      break;
    }
    case GST_EVENT_NAVIGATION:
      /* navigation is rather pointless. */
      result = FALSE;
      gst_event_unref (event);
      break;
    default:
      /* just forward the rest for now */
      result = gst_gl_mixer_push_sink_event (mix, event);
      break;
  }

  return result;
}

static gboolean
gst_gl_mixer_sink_event (GstCollectPads * pads, GstCollectData * cdata,
    GstEvent * event, GstGLMixer * mix)
{
  GstGLMixerPad *pad = GST_GL_MIXER_PAD (cdata->pad);
  gboolean ret = TRUE;

  GST_DEBUG_OBJECT (pad, "Got %s event on pad %s:%s",
      GST_EVENT_TYPE_NAME (event), GST_DEBUG_PAD_NAME (pad));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret =
          gst_gl_mixer_pad_sink_setcaps (GST_PAD (pad), GST_OBJECT (mix), caps);
      gst_event_unref (event);
      event = NULL;
      break;
    }
    case GST_EVENT_SEGMENT:{
      GstSegment seg;
      gst_event_copy_segment (event, &seg);

      g_assert (seg.format == GST_FORMAT_TIME);
      break;
    }
    case GST_EVENT_FLUSH_STOP:
      mix->newseg_pending = TRUE;
      mix->flush_stop_pending = FALSE;
      gst_gl_mixer_reset_qos (mix);
      gst_buffer_replace (&pad->mixcol->buffer, NULL);
      pad->mixcol->start_time = -1;
      pad->mixcol->end_time = -1;

      gst_segment_init (&mix->segment, GST_FORMAT_TIME);
      mix->segment.position = -1;
      mix->ts_offset = 0;
      mix->nframes = 0;
      break;
    default:
      break;
  }

  if (event != NULL)
    return gst_collect_pads_event_default (pads, cdata, event, FALSE);

  return ret;
}

static void
gst_gl_mixer_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  //GstGLMixer *mix = GST_GL_MIXER (object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_mixer_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  //GstGLMixer *mix = GST_GL_MIXER (object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstStateChangeReturn
gst_gl_mixer_change_state (GstElement * element, GstStateChange transition)
{
  GstGLMixer *mix;
  GstStateChangeReturn ret;
  GstGLMixerClass *mixer_class;

  g_return_val_if_fail (GST_IS_GL_MIXER (element), GST_STATE_CHANGE_FAILURE);

  mix = GST_GL_MIXER (element);
  mixer_class = GST_GL_MIXER_GET_CLASS (mix);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
    {
      GSList *walk = mix->sinkpads;
      gint i = 0;

      GstElement *parent = GST_ELEMENT (gst_element_get_parent (mix));
      GstStructure *structure = NULL;
      GstQuery *query = NULL;
      gboolean isPerformed = FALSE;

      if (!parent) {
        GST_ELEMENT_ERROR (mix, CORE, STATE_CHANGE, (NULL),
            ("A parent bin is required"));
        return FALSE;
      }

      structure = gst_structure_new_empty (gst_element_get_name (mix));
      query = gst_query_new_custom (GST_QUERY_CUSTOM, structure);

      /* retrieve the gldisplay that is owned by gl elements after the gl mixer */
      isPerformed = gst_element_query (parent, query);

      if (isPerformed) {
        const GValue *id_value =
            gst_structure_get_value (structure, "gstgldisplay");
        if (G_VALUE_HOLDS_POINTER (id_value))
          /* at least one gl element is after in our gl chain */
          mix->display =
              g_object_ref (GST_GL_DISPLAY (g_value_get_pointer (id_value)));
        else {
          /* this gl filter is a sink in terms of the gl chain */
          mix->display = gst_gl_display_new ();
          gst_gl_display_create_context (mix->display, 0);
        }
      }

      gst_query_unref (query);
      gst_object_unref (GST_OBJECT (parent));

      /* instanciate a gldisplay for each sink pad */
      while (walk) {
        GstGLMixerPad *sink_pad = GST_GL_MIXER_PAD (walk->data);
        walk = g_slist_next (walk);
        sink_pad->display = gst_gl_display_new ();
      }
      mix->array_buffers = g_ptr_array_sized_new (mix->next_sinkpad);
      for (i = 0; i < mix->next_sinkpad; ++i) {
        g_ptr_array_add (mix->array_buffers, NULL);
      }
      GST_LOG_OBJECT (mix, "starting collectpads");
      gst_collect_pads_start (mix->collect);
      break;
    }
    case GST_STATE_CHANGE_PAUSED_TO_READY:
    {
      GSList *walk = mix->sinkpads;
      GST_LOG_OBJECT (mix, "stopping collectpads");
      gst_collect_pads_stop (mix->collect);
      g_ptr_array_free (mix->array_buffers, TRUE);
      while (walk) {
        GstGLMixerPad *sink_pad = GST_GL_MIXER_PAD (walk->data);
        walk = g_slist_next (walk);
        if (sink_pad->display)
          gst_gl_display_activate_gl_context (sink_pad->display, FALSE);
      }
      if (mixer_class->reset)
        mixer_class->reset (mix);
      if (mix->fbo) {
        gst_gl_display_del_fbo (mix->display, mix->fbo, mix->depthbuffer);
        mix->fbo = 0;
        mix->depthbuffer = 0;
      }
      if (mix->display) {
        g_object_unref (mix->display);
        mix->display = NULL;
      }
      while (walk) {
        GstGLMixerPad *sink_pad = GST_GL_MIXER_PAD (walk->data);
        walk = g_slist_next (walk);
        if (sink_pad->display) {
          gst_gl_display_activate_gl_context (sink_pad->display, TRUE);
          g_object_unref (sink_pad->display);
          sink_pad->display = NULL;
        }
      }
      break;
    }
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_gl_mixer_reset (mix);
      break;
    default:
      break;
  }

  return ret;
}