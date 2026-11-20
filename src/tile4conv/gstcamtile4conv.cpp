#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include "libdrm/intel_bufmgr.h"
#include <drm_fourcc.h>
#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/allocators/gstdmabuf.h>

#ifndef I915_FORMAT_MOD_4_TILED
#define I915_FORMAT_MOD_4_TILED fourcc_mod_code(INTEL, 9)
#endif

#ifndef I915_TILING_NONE
#define I915_TILING_NONE 0
#endif

#define ALIGN_UP(v, a) (((v) + (a) - 1) / (a) * (a))

#define TILE4_MOD_STR ":0x0100000000000009"
#define TILE_W 128
#define TILE_H 32
#define TILE_SIZE 4096

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
  drm_intel_bufmgr *bufmgr;
  int drm_fd;
  gchar *device;
  guint16 swz[TILE_H][TILE_W / 16];     /* [row][16B chunk] -> offset in tile */
};

G_DEFINE_TYPE (GstIcamTile4Conv, gst_icam_tile4conv, GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate sink_t = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS (SINK_CAPS));
static GstStaticPadTemplate src_t = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS (SRC_CAPS));

/* In-tile byte offset bits (4KB tile = 128B x 32 rows):
 * [3:0]=x0-3 [5:4]=y0-1 [6]=x4 [7]=y2 [8]=x5 [9]=y3 [10]=x6 [11]=y4
 * NOTE: verify against Mesa isl before production use. */
static void
init_swizzle (GstIcamTile4Conv * self)
{
  for (guint y = 0; y < TILE_H; y++)
    for (guint c = 0; c < TILE_W / 16; c++)
      self->swz[y][c] = (guint16) (((y & 3) << 4) | ((c & 1) << 6)
          | (((y >> 2) & 1) << 7) | (((c >> 1) & 1) << 8)
          | (((y >> 3) & 1) << 9) | (((c >> 2) & 1) << 10)
          | (((y >> 4) & 1) << 11));
}

static void
copy_plane_to_tile4 (GstIcamTile4Conv * self, const guint8 * src,
    gsize src_stride, guint8 * dst, gsize dst_stride, gsize row_bytes,
    guint rows)
{
  const gsize tiles_per_row = dst_stride / TILE_W;

  for (guint y = 0; y < rows; y++) {
    const gsize ty = y / TILE_H, iy = y % TILE_H;
    const guint8 *s = src + y * src_stride;

    for (gsize x = 0; x < row_bytes; x += 16) {
      const gsize tx = x / TILE_W, c = (x % TILE_W) / 16;
      const gsize n = std::min<gsize> (16, row_bytes - x);
      std::memcpy (dst + (ty * tiles_per_row + tx) * TILE_SIZE
          + self->swz[iy][c], s + x, n);
    }
  }
}

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
  return ret;
}

static gboolean
gst_icam_tile4conv_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (trans);

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
  return TRUE;
}

/* ---------- output allocation ---------- */

static gboolean
ensure_bufmgr (GstIcamTile4Conv * self)
{
  if (self->bufmgr)
    return TRUE;
  self->drm_fd = open (self->device, O_RDWR | O_CLOEXEC);
  if (self->drm_fd < 0) {
    GST_ERROR_OBJECT (self, "Cannot open %s: %s", self->device,
        g_strerror (errno));
    return FALSE;
  }
  self->bufmgr = drm_intel_bufmgr_gem_init (self->drm_fd, 4096);
  if (!self->bufmgr) {
    GST_ERROR_OBJECT (self, "drm_intel_bufmgr_gem_init failed");
    close (self->drm_fd);
    self->drm_fd = -1;
    return FALSE;
  }
  return TRUE;
}

static GstFlowReturn
gst_icam_tile4conv_prepare_output_buffer (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer ** outbuf)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (trans);
  GstVideoInfo *vi = &self->in_info;
  const GstVideoFormatInfo *fi = vi->finfo;
  const guint w = GST_VIDEO_INFO_WIDTH (vi), h = GST_VIDEO_INFO_HEIGHT (vi);
  const guint np = GST_VIDEO_INFO_N_PLANES (vi);
  gsize offs[GST_VIDEO_MAX_PLANES] = { 0 };
  gint strides[GST_VIDEO_MAX_PLANES] = { 0 };
  gsize total = 0;

  if (!ensure_bufmgr (self))
    return GST_FLOW_ERROR;
  if (!self->allocator)
    self->allocator = gst_dmabuf_allocator_new ();

  /* Tile4 layout: stride multiple of 128B, rows padded to 32, 4KB-aligned planes */
  for (guint p = 0; p < np; p++) {
    const gsize row_bytes = GST_VIDEO_FORMAT_INFO_PSTRIDE (fi, p) *
        GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (fi, p, w);
    const guint rows = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (fi, p, h);

    strides[p] = (gint) ALIGN_UP (row_bytes, TILE_W);
    offs[p] = total;
    total += ALIGN_UP ((gsize) rows, TILE_H) * strides[p];
    total = ALIGN_UP (total, TILE_SIZE);
  }

  /* Linear GEM object: one 4KB "row" per tile, 1 byte per pixel */
  uint32_t tiling = I915_TILING_NONE;
  unsigned long pitch = 0;
  drm_intel_bo *bo = drm_intel_bo_alloc_tiled (self->bufmgr, "ICAM_TILE4",
      TILE_SIZE, (int) (total / TILE_SIZE), 1, &tiling, &pitch, 0);
  if (!bo) {
    GST_ERROR_OBJECT (self, "drm_intel_bo_alloc_tiled failed for %zu bytes",
        (size_t) total);
    return GST_FLOW_ERROR;
  }

  int fd = -1;
  drm_intel_bo_gem_export_to_prime (bo, &fd);
  const gsize bo_size = bo->size;
  /* the dmabuf fd keeps the buffer alive */
  drm_intel_bo_unreference (bo);
  if (fd < 0 || bo_size < total) {
    GST_ERROR_OBJECT (self, "PRIME export failed (fd %d, size %zu < %zu)",
        fd, (size_t) bo_size, (size_t) total);
    if (fd >= 0)
      close (fd);
    return GST_FLOW_ERROR;
  }

  GstBuffer *out = gst_buffer_new ();
  gst_buffer_append_memory (out,
      gst_dmabuf_allocator_alloc (self->allocator, fd, bo_size));
  gst_buffer_add_video_meta_full (out, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_INFO_FORMAT (vi), w, h, np, offs, strides);

  gst_buffer_copy_into (out, inbuf,
      static_cast<GstBufferCopyFlags> (GST_BUFFER_COPY_METADATA |
          GST_BUFFER_COPY_TIMESTAMPS), 0, -1);
  *outbuf = out;
  return GST_FLOW_OK;
}

/* ---------- transform ---------- */

static GstFlowReturn
gst_icam_tile4conv_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (trans);
  GstVideoInfo *vi = &self->in_info;
  const GstVideoFormatInfo *fi = vi->finfo;
  GstVideoMeta *im = gst_buffer_get_video_meta (inbuf);
  GstVideoMeta *om = gst_buffer_get_video_meta (outbuf);
  GstMapInfo imap, omap;
  const guint w = GST_VIDEO_INFO_WIDTH (vi), h = GST_VIDEO_INFO_HEIGHT (vi);

  if (!om)
    return GST_FLOW_ERROR;
  if (!gst_buffer_map (inbuf, &imap, GST_MAP_READ))
    return GST_FLOW_ERROR;
  if (!gst_buffer_map (outbuf, &omap, GST_MAP_WRITE)) {
    gst_buffer_unmap (inbuf, &imap);
    return GST_FLOW_ERROR;
  }

  GstFlowReturn ret = GST_FLOW_OK;
  for (guint p = 0; p < GST_VIDEO_INFO_N_PLANES (vi); p++) {
    const gsize soff = im ? im->offset[p] : GST_VIDEO_INFO_PLANE_OFFSET (vi, p);
    const gint sstr = im ? im->stride[p] : GST_VIDEO_INFO_PLANE_STRIDE (vi, p);
    const gsize row_bytes = GST_VIDEO_FORMAT_INFO_PSTRIDE (fi, p) *
        GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (fi, p, w);
    const guint rows = GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (fi, p, h);
    const gsize need = (gsize) ((rows + TILE_H - 1) / TILE_H) * om->stride[p]
        * TILE_H + om->offset[p];

    if (om->stride[p] % TILE_W || need > omap.size) {
      GST_ERROR_OBJECT (self, "Output plane %u layout invalid", p);
      ret = GST_FLOW_ERROR;
      break;
    }
    copy_plane_to_tile4 (self, imap.data + soff, sstr,
        omap.data + om->offset[p], om->stride[p], row_bytes, rows);
  }

  gst_buffer_unmap (outbuf, &omap);
  gst_buffer_unmap (inbuf, &imap);
  return ret;
}

static gboolean
gst_icam_tile4conv_stop (GstBaseTransform * trans)
{
  GstIcamTile4Conv *self = GST_ICAM_TILE4CONV (trans);

  if (self->bufmgr)
    drm_intel_bufmgr_destroy (self->bufmgr);
  self->bufmgr = NULL;
  if (self->drm_fd >= 0)
    close (self->drm_fd);
  self->drm_fd = -1;
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
  bt->stop = gst_icam_tile4conv_stop;
  bt->passthrough_on_same_caps = FALSE;
}

static void
gst_icam_tile4conv_init (GstIcamTile4Conv * self)
{
  self->drm_fd = -1;
  self->device = g_strdup ("/dev/dri/renderD128");
  init_swizzle (self);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_icam_tile4_debug, "icamtile4conv", 0,
      "iCamera Tile4 converter");
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