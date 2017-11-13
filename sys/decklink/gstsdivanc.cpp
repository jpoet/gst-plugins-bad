/* GStreamer
 * Copyright (C) 2017 John Poet <jppoet@digital-nirvana.com>
 * Copyright (C) 2014 Rafaël Carré
 * Copyright (C) 2009 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (c) 2009 Baptiste Coudurier <baptiste dot coudurier at gmail dot com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

#include <assert.h>
#include <gst/gst.h>

#include "gstsdivanc.h"

GST_DEBUG_CATEGORY_STATIC (gst_decklink_video_src_debug);
#define GST_CAT_DEFAULT gst_decklink_video_src_debug

void gst_processVANC_init_log(void)
{
  GST_DEBUG_CATEGORY_INIT (gst_decklink_video_src_debug, "decklinkvideosrc",
                           0, "debug category for decklinkvideosrc element");
}

static inline unsigned (parity)(unsigned x)
{
#ifdef __GNUC__
    return __builtin_parity (x);
#else
    for (unsigned i = 4 * sizeof (x); i > 0; i /= 2)
        x ^= x >> i;
    return x & 1;
#endif
}

static inline uint32_t av_le2ne32(uint32_t val)
{
    union {
        uint32_t v;
        uint8_t b[4];
    } u;
    u.v = val;
    return (u.b[0] << 0) | (u.b[1] << 8) | (u.b[2] << 16) | (u.b[3] << 24);
}

static inline void v210_read_pixels(guint16* &a, guint16* &b, guint16* &c,
                                    guint32& val, const guint32* &src)
{
//  val  = g_htonl(*src);
  val = av_le2ne32(*src);
  ++src;
  *a++ =  val & 0x3FF;
  *b++ = (val >> 10) & 0x3FF;
  *c++ = (val >> 20) & 0x3FF;
}

static void v210_convert(guint16 *dst, const guint32 *bytes,
                         int width, int height)
{
    const int stride = ((width + 47) / 48) * 48 * 8 / 3 / 4;
    uint16_t *y = &dst[0];
    uint16_t *u = &dst[width * height * 2 / 2];
    uint16_t *v = &dst[width * height * 3 / 2];

    for (int h = 0; h < height; h++) {
        const uint32_t *src = bytes;
        uint32_t val = 0;
        int w;
        for (w = 0; w < width - 5; w += 6) {
          v210_read_pixels(u, y, v, val, src);
          v210_read_pixels(y, u, y, val, src);
          v210_read_pixels(v, y, u, val, src);
          v210_read_pixels(y, v, y, val, src);
        }
        if (w < width - 1) {
          v210_read_pixels(u, y, v, val, src);

//          val  = g_htonl(*src);
          val = av_le2ne32(*src);
          ++src;
            *y++ =  val & 0x3FF;
        }
        if (w < width - 3) {
            *u++ = (val >> 10) & 0x3FF;
            *y++ = (val >> 20) & 0x3FF;

//            val  = g_htonl(*src);
            val = av_le2ne32(*src);
            ++src;
            *v++ =  val & 0x3FF;
            *y++ = (val >> 10) & 0x3FF;
        }

        bytes += stride;
    }
}

static gboolean vanc_to_cc(GstDecklinkVideoSrc * self, guint8* &data,
                           gsize & data_size, uint16_t *vanc_buf,
                           gsize & data_count, size_t words)
{
    if (words < 3) {
        GST_ERROR_OBJECT (self, "VANC line too small (%zu words)", words);
        return FALSE;
    }

    static const uint8_t vanc_header[6] = { 0x00, 0x00, 0xff, 0x03, 0xff, 0x03 };
    if (memcmp(vanc_header, vanc_buf, 3*2)) {
        /* Does not start with the VANC header */
        return FALSE;
    }

    size_t len = (vanc_buf[5] & 0xff) + 6 + 1;
    if (len > words) {
        GST_ERROR_OBJECT (self, "Data Count (%zu) > line length (%zu)",
                          len, words);
        return FALSE;
    }

    uint16_t vanc_sum = 0;
    for (size_t i = 3; i < len - 1; i++) {
        uint16_t v = vanc_buf[i];
        int np = v >> 8;
        int p = parity(v & 0xff);
        if ((!!p ^ !!(v & 0x100)) || (np != 1 && np != 2)) {
            GST_ERROR_OBJECT (self, "Parity incorrect for word %zu", i);
            return FALSE;
        }
        vanc_sum += v;
        vanc_sum &= 0x1ff;
        vanc_buf[i] &= 0xff;
    }

    vanc_sum |= ((~vanc_sum & 0x100) << 1);
    if (vanc_buf[len - 1] != vanc_sum) {
        GST_ERROR_OBJECT (self, "VANC checksum incorrect: 0x%.4x != 0x%.4x",
                          vanc_sum, vanc_buf[len-1]);
        return FALSE;
    }

    if (vanc_buf[3] != 0x61 /* DID */ || vanc_buf[4] != 0x01 /* SDID = CEA-708 */) {
        //GST_ERROR_OBJECT (self, "Not a CEA-708 packet: DID = 0x%.2x SDID = 0x%.2x", vanc_buf[3], vanc_buf[4]);
        // XXX : what is Not a CEA-708 packet: DID = 0x61 SDID = 0x02 ?
        return FALSE;
    }

    /* CDP follows */
    uint16_t *cdp = &vanc_buf[6];
    if (cdp[0] != 0x96 || cdp[1] != 0x69) {
        GST_ERROR_OBJECT (self, "Invalid CDP header 0x%.2x 0x%.2x",
                          cdp[0], cdp[1]);
        return FALSE;
    }

    len -= 7; // remove VANC header and checksum

    if (cdp[2] != len) {
        GST_ERROR_OBJECT (self, "CDP len %d != %zu", cdp[2], len);
        return FALSE;
    }

    uint8_t cdp_sum = 0;
    for (size_t i = 0; i < len - 1; i++)
        cdp_sum += cdp[i];
    cdp_sum = cdp_sum ? 256 - cdp_sum : 0;
    if (cdp[len - 1] != cdp_sum) {
        GST_ERROR_OBJECT (self, "CDP checksum invalid 0x%.4x != 0x%.4x",
                          cdp_sum, cdp[len-1]);
        return FALSE;
    }

    uint8_t rate = cdp[3];
    if (!(rate & 0x0f)) {
        GST_ERROR_OBJECT (self, "CDP frame rate invalid (0x%.2x)", rate);
        return FALSE;
    }
    rate >>= 4;
    if (rate > 8) {
        GST_ERROR_OBJECT (self, "CDP frame rate invalid (0x%.2x)", rate);
        return FALSE;
    }

    if (!(cdp[4] & 0x43)) /* ccdata_present | caption_service_active | reserved */ {
        GST_ERROR_OBJECT (self, "CDP flags invalid (0x%.2x)", cdp[4]);
        return FALSE;
    }

    uint16_t hdr = (cdp[5] << 8) | cdp[6];
    if (cdp[7] != 0x72) /* ccdata_id */ {
        GST_ERROR_OBJECT (self, "Invalid ccdata_id 0x%.2x", cdp[7]);
        return FALSE;
    }

    unsigned cc_count = cdp[8];
    if (!(cc_count & 0xe0)) {
        GST_ERROR_OBJECT (self, "Invalid cc_count 0x%.2x", cc_count);
        return FALSE;
    }

    cc_count &= 0x1f;
    if ((len - 13) < cc_count * 3) {
        GST_ERROR_OBJECT (self, "Invalid cc_count %d (> %zu)",
                          cc_count * 3, len - 13);
        return FALSE;
    }

    if (cdp[len - 4] != 0x74) /* footer id */ {
        GST_ERROR_OBJECT (self, "Invalid footer id 0x%.2x", cdp[len-4]);
        return FALSE;
    }

    uint16_t ftr = (cdp[len - 3] << 8) | cdp[len - 2];
    if (ftr != hdr) {
        GST_ERROR_OBJECT (self, "Header 0x%.4x != Footer 0x%.4x", hdr, ftr);
        return FALSE;
    }
    else
    {
      guint16* ccP;
      guint8*  dataP;

      data = (guint8 *)g_realloc(data, data_size + (cc_count * 3));
      ccP = &cdp[9];
      dataP = &data[data_size];
      for (gsize idx = 0; idx < cc_count * 3; ++idx, ++dataP, ++ccP) {
        *dataP = *ccP;
      }
    }

    data_count += cc_count;
    data_size  += (cc_count * 3) + 1;

    return TRUE;
}

static int getANCTableIndex(BMDDisplayMode dm)
{
  switch (dm) {
    case bmdModeNTSC:
    case bmdModeNTSC2398:
      return 0;

    case bmdModePAL:
      return 2;
    case bmdModeNTSCp:
      return 1;
    case bmdModePALp:
      return 3;

    case bmdModeHD1080p2398:
    case bmdModeHD1080p24:
    case bmdModeHD1080p25:
    case bmdModeHD1080p2997:
    case bmdModeHD1080p30:
      return 6;

    case bmdModeHD1080i50:
    case bmdModeHD1080i5994:
    case bmdModeHD1080i6000:
      return 5;

    case bmdModeHD1080p50:
    case bmdModeHD1080p5994:
    case bmdModeHD1080p6000:
      return 6;

    case bmdModeHD720p50:
    case bmdModeHD720p5994:
    case bmdModeHD720p60:
      return 4;

    case bmdMode2k2398:
    case bmdMode2k24:
    case bmdMode2k25:
      return 7;
  };

  return -1;
}

static int vancTable[][7] = {
  { 1, 22, 263, 285, 526, 528, 0 }, // 480i
  { 1, 44, 525, 528, 0 }, // 480p
  { 1, 22, 311, 335, 624, 626, 0 }, // 576i
  { 1, 44, 621, 626, 0 }, // 576p
  { 1, 25, 746, 750, 0 }, // 720p
  { 1, 20, 561, 583, 1124, 1125, 0 }, // 1080i
  { 1, 41, 1122, 1125, 0 }, // 1080p
  { 1, 2000, 0 } // 2k
};

#if 0
static void dump_user_data(GstDecklinkVideoSrc * self, guint8* &data,
                           gsize & data_size, gsize & data_count)
{
  guint    idx, len;
  GString *result = g_string_sized_new(32);

  g_string_printf (result, "CC Size %4lu count %3lu : ", data_size, data_count);

  len = (data_size > 25) ? 25 : data_size;
  if (len > 0)
    g_string_append_printf (result, "0x ");

  for (idx = 0; idx < len; ++idx)
    g_string_append_printf (result, "%2x ", data[idx]);

  if (data_size > 25) {
    idx = data_size - 25;
    if (idx < 25)
      idx = 25;

    g_string_append_printf(result,  " ... 0x ");
    for (; idx < data_size; ++idx)
      g_string_append_printf (result, "%2x ", data[idx]);
  }

  GST_WARNING_OBJECT (self, "%s", result->str);
  g_string_free (result, TRUE);
}
#endif

bool gst_processVANC(GstDecklinkVideoSrc *self,
                     IDeckLinkVideoFrameAncillary *vanc_frame,
                     guint width, gsize row_bytes, GstBuffer * buffer)
{
  int vanc_start, vanc_end;

  BMDPixelFormat fmt = vanc_frame->GetPixelFormat();
  if (fmt != bmdFormat10BitYUV) {
    GST_WARNING_OBJECT (self, "VANC - data for HD resolutions is only "
                        "available in 10BitYUV mode. "
                        "SD is not currently supported.");
    return false;
  }

  BMDDisplayMode dm  = vanc_frame->GetDisplayMode();
  int vanc_index = getANCTableIndex(dm);

  if (vanc_index < 0) {
    GST_WARNING_OBJECT (self, "VANC - bad index");
    return false;
  }

  /* Process each range of vertical blanking line ranges for the current mode */
  for (int idx = 0; vancTable[vanc_index][idx]; idx += 2) {
    vanc_start = vancTable[vanc_index][idx];
    vanc_end   = vancTable[vanc_index][idx + 1];

    for (int j = vanc_start; j <= vanc_end; ++j) {
      uint32_t *buf = NULL;

      if (vanc_frame->GetBufferForVerticalBlankingLine
          (j, (void**) &buf) == S_OK) {

        guint8 *cc_data = NULL;
        gsize   cc_data_count = 0;
        gsize   cc_data_size  = 0;
        guint16 dec[row_bytes];

        v210_convert(&dec[0], buf, width, 1);

        cc_data_size += 2; /* 2 byte header */
        vanc_to_cc(self, cc_data, cc_data_size, dec, cc_data_count, row_bytes);
        if (cc_data_count) {
          cc_data[0] = 0x80/*process_em_data*/ |
                       0x40/*process_cc_data*/ |
                       0x00/*process_addl_data*/|
                       (cc_data_count);
          cc_data[1] = 0x00; /* reserved */

          gst_buffer_add_mpeg_user_data_meta (buffer,
                                      GST_VIDEO_USER_DATA_IDENTIFIER_ATSC,
                                      GST_VIDEO_USER_DATA_TYPE_CC,
                                      &cc_data[0], cc_data_size);
          g_free(cc_data);
        }
      }
    }
  }

  return true;
}
