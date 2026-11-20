#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <drm_fourcc.h>
#include <gst/gst.h>
#define GST_USE_UNSTABLE_API
#include <gst/va/gstva.h>
#include <gst/va/gstvadisplay_drm.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/allocators/gstdmabuf.h>

#ifndef I915_FORMAT_MOD_4_TILED
#define I915_FORMAT_MOD_4_TILED fourcc_mod_code(INTEL, 9)
#endif

#define TILE4_MOD_STR ":0x0100000000000009"

#define SINK_CAPS "video/x-raw(memory:DMABuf), format=(string)DMA_DRM, " \
  "drm-format=(string){ UYVY, NV12, YUYV }"
#define SRC_CAPS "video/x-raw(memory:DMABuf), format=(string)DMA_DRM, " \
  "drm-format=(string){ UYVY" TILE4_MOD_STR ", NV12" TILE4_MOD_STR \
  ", YUYV" TILE4_MOD_STR " }"

GST_DEBUG_CATEGORY_STATIC (gst_icam_tile4_debug);
#define GST_CAT_DEFAULT gst_icam_tile4_debug

#define GST_TYPE_ICAM_TILE4CONV (gst_icam_tile4conv_get_type ())
G_DECLARE_FINAL_TYPE (GstIcamTile4Conv, gst_icam_tile4conv, GST,
    ICAM_TILE4CONV, GstBaseTransform)

struct _GstIcamTile4Conv
{
  GstBaseTransform parent;

  GstVideoInfoDmaDrm in_drm, out_drm;
  GstVideoInfo in_info;
  GstAllocator *allocator;
  GstVaDisplay *display;
  VASurfaceID surf;             /* surface of the buffer being produced */
  gchar *device;
};

G_DEFINE_TYPE (GstIcamTile4Conv, gst_icam_tile4conv, GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate sink_t = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS (SINK_CAPS));
static GstStaticPadTemplate src_t = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS (SRC_CAPS));

/* ---------- caps ---------- */

static void
convert_value (const GValue * in, GValue * out, gboolean to_tiled)
{
  if (GST_VALUE_HOLDS_LIST (in)) {
    g_value_init (out, GST_TYPE_LIST);
    for (guint i = 0; i < gst_value_list_get_size (in); i++) {
      GValue tmp = G_VALUE_INIT;
      convert_value (gst_value_list_get_value (in, i), &tmp, to_tiled);
      gst_value_list_append_and_take_value (out, &tmp);
    }
    return;
  }

  gchar **parts = g_strsplit (g_value_get_string (in), ":", 2);
  gchar *res = to_tiled ? g_strconcat (parts[0], TILE4_MOD_STR, NULL)
      : g_strdup (parts[0]);
  g_strfreev (parts);
  g_value_init (out, G_TYPE_STRING);
  g_value_take_string (out, res);
}

static GstCaps *
gst_icam_tile4conv_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  const gboolean to_tiled = (direction == GST_PAD_SINK);
  GstCaps *ret = gst_caps_new_empty ();

  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *st = gst_structure_copy (gst_caps_get_structure (caps, i));
    GstCapsFeatures *f = gst_caps_get_features (caps, i);
    const GValue *v = gst_structure_get_value (st, "drm-format");

    if (v && (G_VALUE_HOLDS_STRING (v) || GST_VALUE_HOLDS_LIST (v))) {
      GValue out = G_VALUE_INIT;
      convert_value (v, &out, to_tiled);
      gst_structure_take_value (st, "drm-format", &out);
    } else {
      GstCaps *t = gst_static_caps_get (to_tiled ? &src_t.static_caps
          : &sink_t.static_caps);
      gst_structure_set_value (st, "drm-format",
          gst_structure_get_value (gst_caps_get_structure (t, 0),
              "drm-format"));
      gst_caps_unref (t);
    }
    gst_caps_append_structure_full (ret, st,
        f ? gst_caps_features_copy (f) : NULL);
  }

  if (filter) {
    GstCaps *tmp = gst_caps_intersect_full (filter, ret,
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (ret);
    ret = tmp;
  }
  GST_DEBUG_OBJECT (trans, "transform caps (%s): %" GST_PTR_FORMAT
      " -> %" GST_PTR_FORMAT, to_tiled ? "sink->src" : "src->sink", caps, ret);
  return ret;
}

static gboolean
gst_icam_tile4conv_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (trans);

  GST_INFO_OBJECT (self, "set caps: in %" GST_PTR_FORMAT ", out %"
      GST_PTR_FORMAT, incaps, outcaps);

  if (!gst_video_info_dma_drm_from_caps (&self->in_drm, incaps) ||
      !gst_video_info_dma_drm_from_caps (&self->out_drm, outcaps)) {
    GST_ERROR_OBJECT (self, "Invalid DMA_DRM caps");
    return FALSE;
  }
  if (self->in_drm.drm_modifier != DRM_FORMAT_MOD_LINEAR ||
      self->out_drm.drm_modifier != I915_FORMAT_MOD_4_TILED ||
      self->in_drm.drm_fourcc != self->out_drm.drm_fourcc) {
    GST_ERROR_OBJECT (self, "Unsupported modifier/fourcc combination");
    return FALSE;
  }
  if (!gst_video_info_dma_drm_to_video_info (&self->in_drm, &self->in_info)) {
    GST_ERROR_OBJECT (self, "Cannot derive video info");
    return FALSE;
  }
  GST_INFO_OBJECT (self, "negotiated %s (%" GST_FOURCC_FORMAT ") %dx%d, "
      "%u planes, modifier 0x%016" G_GINT64_MODIFIER "x -> 0x%016"
      G_GINT64_MODIFIER "x",
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&self->in_info)),
      GST_FOURCC_ARGS (self->in_drm.drm_fourcc),
      GST_VIDEO_INFO_WIDTH (&self->in_info),
      GST_VIDEO_INFO_HEIGHT (&self->in_info),
      GST_VIDEO_INFO_N_PLANES (&self->in_info),
      (guint64) self->in_drm.drm_modifier,
      (guint64) self->out_drm.drm_modifier);
  return TRUE;
}

/* ---------- output allocation ---------- */

static void
drop_surface (GstIcamTile4Conv * self)
{
  if (self->surf != VA_INVALID_SURFACE && self->display) {
    GST_DEBUG_OBJECT (self, "destroying VA surface 0x%x", self->surf);
    vaDestroySurfaces (gst_va_display_get_va_dpy (self->display),
        &self->surf, 1);
  }
  self->surf = VA_INVALID_SURFACE;
}

static gboolean
ensure_display (GstIcamTile4Conv * self)
{
  if (self->display)
    return TRUE;
  GST_INFO_OBJECT (self, "creating VA display on %s", self->device);
  if (!self->display) {
    GST_ERROR_OBJECT (self, "Cannot create VA display on %s", self->device);
    return FALSE;
  }
  GST_INFO_OBJECT (self, "VA display ready: %s",
      vaQueryVendorString (gst_va_display_get_va_dpy (self->display)));
  return TRUE;
}

/* Create a Tile4 VA surface and export it as DRM PRIME 2. */
static gboolean
export_tile4_surface (GstIcamTile4Conv * self, VADRMPRIMESurfaceDescriptor * d)
{
  VADisplay dpy = gst_va_display_get_va_dpy (self->display);
  const GstVideoInfo *vi = &self->in_info;
  const guint32 fourcc = self->in_drm.drm_fourcc;
  const guint rt = (fourcc == DRM_FORMAT_NV12) ? VA_RT_FORMAT_YUV420
      : VA_RT_FORMAT_YUV422;
  uint64_t mods[1] = { I915_FORMAT_MOD_4_TILED };
  VADRMFormatModifierList ml = { };
  ml.num_modifiers = 1;
  ml.modifiers = mods;

  VASurfaceAttrib attrs[3] = { };
  attrs[0].type = VASurfaceAttribPixelFormat;
  attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
  attrs[0].value.type = VAGenericValueTypeInteger;
  attrs[0].value.value.i = (int) fourcc;
  attrs[1].type = VASurfaceAttribUsageHint;
  attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
  attrs[1].value.type = VAGenericValueTypeInteger;
  attrs[1].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC;
  attrs[2].type = VASurfaceAttribDRMFormatModifiers;
  attrs[2].flags = VA_SURFACE_ATTRIB_SETTABLE;
  attrs[2].value.type = VAGenericValueTypePointer;
  attrs[2].value.value.p = &ml;

  drop_surface (self);
  GST_DEBUG_OBJECT (self, "creating Tile4 VA surface %" GST_FOURCC_FORMAT
      " %dx%d, rt format 0x%x", GST_FOURCC_ARGS (fourcc),
      GST_VIDEO_INFO_WIDTH (vi), GST_VIDEO_INFO_HEIGHT (vi), rt);
  VASurfaceID surf = VA_INVALID_SURFACE;
  VAStatus s = vaCreateSurfaces (dpy, rt, GST_VIDEO_INFO_WIDTH (vi),
      GST_VIDEO_INFO_HEIGHT (vi), &surf, 1, attrs, 3);
  if (s == VA_STATUS_SUCCESS) {
    s = vaExportSurfaceHandle (dpy, surf,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS, d);
    if (s == VA_STATUS_SUCCESS)
      self->surf = surf;        /* kept until transform() has uploaded */
    else
      vaDestroySurfaces (dpy, &surf, 1);
  }

  if (s != VA_STATUS_SUCCESS) {
    GST_ERROR_OBJECT (self, "VA Tile4 surface failed: %s", vaErrorStr (s));
    return FALSE;
  }
  GST_DEBUG_OBJECT (self, "VA surface 0x%x created and exported", self->surf);
  return TRUE;
}

static GstFlowReturn
gst_icam_tile4conv_prepare_output_buffer (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer ** outbuf)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (trans);
  GstVideoInfo *vi = &self->in_info;
  const guint w = GST_VIDEO_INFO_WIDTH (vi), h = GST_VIDEO_INFO_HEIGHT (vi);
  const guint np = GST_VIDEO_INFO_N_PLANES (vi);
  gsize offs[GST_VIDEO_MAX_PLANES] = { 0 };
  gint strides[GST_VIDEO_MAX_PLANES] = { 0 };
  VADRMPRIMESurfaceDescriptor d = { };

  GST_DEBUG_OBJECT (self, "prepare output for input %" GST_PTR_FORMAT, inbuf);
  if (!ensure_display (self))
    return GST_FLOW_ERROR;
  if (!self->allocator)
    self->allocator = gst_dmabuf_allocator_new ();
  if (!export_tile4_surface (self, &d))
    return GST_FLOW_ERROR;

  GST_DEBUG_OBJECT (self, "export: fourcc %" GST_FOURCC_FORMAT ", %ux%u, "
      "%u objects, %u layers, fd %d, size %u, modifier 0x%016"
      G_GINT64_MODIFIER "x", GST_FOURCC_ARGS (d.fourcc), d.width, d.height,
      d.num_objects, d.num_layers, d.objects[0].fd, d.objects[0].size,
      (guint64) d.objects[0].drm_format_modifier);

  if (d.num_layers != 1 || d.num_objects != 1 ||
      d.layers[0].num_planes != np ||
      d.objects[0].drm_format_modifier != I915_FORMAT_MOD_4_TILED) {
    GST_ERROR_OBJECT (self, "Unexpected export: %u layers, %u objects, "
        "%u planes, modifier 0x%" G_GINT64_MODIFIER "x", d.num_layers,
        d.num_objects, d.layers[0].num_planes,
        (guint64) d.objects[0].drm_format_modifier);
    for (guint i = 0; i < d.num_objects; i++)
      close (d.objects[i].fd);
    drop_surface (self);
    return GST_FLOW_ERROR;
  }
  for (guint p = 0; p < np; p++) {
    offs[p] = d.layers[0].offset[p];
    strides[p] = d.layers[0].pitch[p];
    GST_DEBUG_OBJECT (self, "output plane %u: offset %" G_GSIZE_FORMAT
        ", stride %d", p, offs[p], strides[p]);
  }

  GstBuffer *out = gst_buffer_new ();
  gst_buffer_append_memory (out,
      gst_dmabuf_allocator_alloc (self->allocator, d.objects[0].fd,
          d.objects[0].size));
  gst_buffer_add_video_meta_full (out, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_INFO_FORMAT (vi), w, h, np, offs, strides);

  gst_buffer_copy_into (out, inbuf,
      static_cast<GstBufferCopyFlags> (GST_BUFFER_COPY_METADATA |
          GST_BUFFER_COPY_TIMESTAMPS), 0, -1);
  *outbuf = out;
  GST_DEBUG_OBJECT (self, "output buffer ready: %" GST_PTR_FORMAT, out);
  return GST_FLOW_OK;
}

/* ---------- transform ---------- */

typedef struct
{
  void *base;
  gsize len;
  guint8 *data;
  gsize size;
  int fd;
  guint64 sync;
} DmaMap;

/* mmap the dmabuf fd directly, bypassing the allocator's map function */
static gboolean
dma_map (GstBuffer * buf, gboolean write, DmaMap * m)
{
  GstMemory *mem = gst_buffer_peek_memory (buf, 0);
  gsize off = 0, max = 0;

  if (gst_buffer_n_memory (buf) != 1 || !gst_is_dmabuf_memory (mem))
    return FALSE;
  gst_memory_get_sizes (mem, &off, &max);
  m->fd = gst_dmabuf_memory_get_fd (mem);
  m->len = max;
  m->base = mmap (NULL, m->len, PROT_READ | (write ? PROT_WRITE : 0),
      MAP_SHARED, m->fd, 0);
  if (m->base == MAP_FAILED)
    return FALSE;
  m->data = (guint8 *) m->base + off;
  m->size = m->len - off;
  m->sync = write ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ;

  struct dma_buf_sync s = { DMA_BUF_SYNC_START | m->sync };
  ioctl (m->fd, DMA_BUF_IOCTL_SYNC, &s);
  GST_DEBUG ("mmapped dmabuf fd %d (%s): len %" G_GSIZE_FORMAT ", offset %"
      G_GSIZE_FORMAT, m->fd, write ? "rw" : "ro", m->len, off);
  return TRUE;
}

static void
dma_unmap (DmaMap * m)
{
  struct dma_buf_sync s = { DMA_BUF_SYNC_END | m->sync };
  ioctl (m->fd, DMA_BUF_IOCTL_SYNC, &s);
  munmap (m->base, m->len);
  GST_DEBUG ("unmapped dmabuf fd %d", m->fd);
}

static GstFlowReturn
gst_icam_tile4conv_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (trans);
  GstVideoInfo *vi = &self->in_info;
  const GstVideoFormatInfo *fi = vi->finfo;
  GstVideoMeta *im = gst_buffer_get_video_meta (inbuf);
  const guint w = GST_VIDEO_INFO_WIDTH (vi), h = GST_VIDEO_INFO_HEIGHT (vi);
  DmaMap imap;

  GST_DEBUG_OBJECT (self, "transform: pts %" GST_TIME_FORMAT ", surface 0x%x, "
      "input video meta %s", GST_TIME_ARGS (GST_BUFFER_PTS (inbuf)),
      self->surf, im ? "present" : "missing");

  if (self->surf == VA_INVALID_SURFACE)
    return GST_FLOW_ERROR;
  if (!dma_map (inbuf, FALSE, &imap)) {
    GST_ERROR_OBJECT (self, "Cannot mmap input dmabuf: %s", g_strerror (errno));
    drop_surface (self);
    return GST_FLOW_ERROR;
  }

  VADisplay dpy = gst_va_display_get_va_dpy (self->display);
  VAImage img = { };
  void *dst = NULL;
  GstFlowReturn ret = GST_FLOW_ERROR;
  gboolean have_img = FALSE;

  VAStatus s = vaDeriveImage (dpy, self->surf, &img);
  if (s == VA_STATUS_SUCCESS) {
    have_img = TRUE;
    s = vaMapBuffer (dpy, img.buf, &dst);
  }

  if (s != VA_STATUS_SUCCESS) {
    GST_ERROR_OBJECT (self, "VA map of Tile4 surface failed: %s",
        vaErrorStr (s));
    dst = NULL;
  } else {
    GST_DEBUG_OBJECT (self, "VA image: %" GST_FOURCC_FORMAT " %ux%u, "
        "%u planes, data size %u", GST_FOURCC_ARGS (img.format.fourcc),
        img.width, img.height, img.num_planes, img.data_size);
    ret = GST_FLOW_OK;
    for (guint p = 0; p < GST_VIDEO_INFO_N_PLANES (vi); p++) {
      const gsize soff = im ? im->offset[p]
          : GST_VIDEO_INFO_PLANE_OFFSET (vi, p);
      const gint sstr = im ? im->stride[p]
          : GST_VIDEO_INFO_PLANE_STRIDE (vi, p);
      const gsize row_bytes = GST_VIDEO_FORMAT_INFO_PSTRIDE (fi, p) *
          GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (fi, p, w);
      const guint rows = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (fi, p, h);

      GST_DEBUG_OBJECT (self, "plane %u: src offset %" G_GSIZE_FORMAT
          " stride %d, dst offset %u pitch %u, %" G_GSIZE_FORMAT
          " bytes x %u rows", p, soff, sstr,
          p < img.num_planes ? img.offsets[p] : 0,
          p < img.num_planes ? img.pitches[p] : 0, row_bytes, rows);

          if (sstr <= 0 || rows == 0 ||
          soff + (gsize) sstr * (rows - 1) + row_bytes > imap.size) {
        GST_ERROR_OBJECT (self, "Input plane %u out of bounds", p);
        ret = GST_FLOW_ERROR;
        break;
      }
      if (p >= img.num_planes || img.pitches[p] < row_bytes ||
          img.offsets[p] + (gsize) img.pitches[p] * (rows - 1) + row_bytes
          > img.data_size) {
        GST_ERROR_OBJECT (self, "VA image plane %u layout invalid", p);
        ret = GST_FLOW_ERROR;
        break;
      }
      /* linear copy; the VA driver tiles on unmap */
      for (guint y = 0; y < rows; y++)
        std::memcpy ((guint8 *) dst + img.offsets[p]
            + (gsize) y * img.pitches[p],
            imap.data + soff + (gsize) y * sstr, row_bytes);
    }
  }

  if (dst)
    vaUnmapBuffer (dpy, img.buf);
  if (have_img)
    vaDestroyImage (dpy, img.image_id);
  if (ret == GST_FLOW_OK)
    vaSyncSurface (dpy, self->surf);

  dma_unmap (&imap);
  drop_surface (self);          /* the exported fd keeps the memory alive */
  GST_DEBUG_OBJECT (self, "transform done: %s", gst_flow_get_name (ret));
  return ret;
}


static gboolean
gst_icam_tile4conv_start (GstBaseTransform * trans)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (trans);

  GST_INFO_OBJECT (self, "start, device %s", self->device);
  return TRUE;
}

static gboolean
gst_icam_tile4conv_stop (GstBaseTransform * trans)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (trans);

  GST_INFO_OBJECT (self, "stop, releasing VA display and allocator");

  drop_surface (self);
  gst_clear_object (&self->display);
  gst_clear_object (&self->allocator);
  return TRUE;
}

/* ---------- GObject ---------- */

enum { PROP_0, PROP_DEVICE };

static void
gst_icam_tile4conv_finalize (GObject * obj)
{
  g_free (GST_ICAM_TILE4CONV (obj)->device);
  G_OBJECT_CLASS (gst_icam_tile4conv_parent_class)->finalize (obj);
}

static void
gst_icam_tile4conv_set_property (GObject * o, guint id, const GValue * v,
    GParamSpec * ps)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (o);
  if (id == PROP_DEVICE) {
    g_free (self->device);
    self->device = g_value_dup_string (v);
    GST_INFO_OBJECT (self, "device set to %s", self->device);
  } else {
    G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
  }
}

static void
gst_icam_tile4conv_get_property (GObject * o, guint id, GValue * v,
    GParamSpec * ps)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (o);
  if (id == PROP_DEVICE)
    g_value_set_string (v, self->device);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, ps);
}

static void
gst_icam_tile4conv_class_init (GstIcamTile4ConvClass * klass)
{
  GObjectClass *go = G_OBJECT_CLASS (klass);
  GstElementClass *ec = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *bt = GST_BASE_TRANSFORM_CLASS (klass);

  go->finalize = gst_icam_tile4conv_finalize;
  go->set_property = gst_icam_tile4conv_set_property;
  go->get_property = gst_icam_tile4conv_get_property;
  g_object_class_install_property (go, PROP_DEVICE,
      g_param_spec_string ("device", "DRM device",
          "DRM render node used to allocate Tile4 buffers",
          "/dev/dri/renderD128",
          static_cast<GParamFlags> (G_PARAM_READWRITE |
              G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  gst_element_class_add_static_pad_template (ec, &sink_t);
  gst_element_class_add_static_pad_template (ec, &src_t);
  gst_element_class_set_static_metadata (ec,
      "iCamera Linear to Tile4 converter", "Filter/Converter/Video",
      "Converts DMABuf DMA_DRM linear frames to Intel I915_FORMAT_MOD_4_TILED",
      "icamera");

  bt->transform_caps = gst_icam_tile4conv_transform_caps;
  bt->set_caps = gst_icam_tile4conv_set_caps;
  bt->prepare_output_buffer = gst_icam_tile4conv_prepare_output_buffer;
  bt->transform = gst_icam_tile4conv_transform;
  bt->start = gst_icam_tile4conv_start;
  bt->stop = gst_icam_tile4conv_stop;
  bt->passthrough_on_same_caps = FALSE;
}

static void
gst_icam_tile4conv_init (GstIcamTile4Conv * self)
{
  self->device = g_strdup ("/dev/dri/renderD128");
  self->surf = VA_INVALID_SURFACE;
  GST_DEBUG_OBJECT (self, "initialized, default device %s", self->device);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_icam_tile4_debug, "icamtile4conv", 0,
      "iCamera Tile4 converter");
  GST_INFO ("registering icamtile4conv");
  return gst_element_register (plugin, "icamtile4conv", GST_RANK_NONE,
      GST_TYPE_ICAM_TILE4CONV);
}

#ifndef PACKAGE
#define PACKAGE "gst-plugins-icamera"
#endif
#ifndef VERSION
#define VERSION "1.0"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, icamtile4conv,
    "Linear to Intel Tile4 DMABuf converter", plugin_init, VERSION, "LGPL",
    "gst-plugins-icamera", "https://github.com/intel/icamerasrc")