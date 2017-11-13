/* GStreamer
 * Copyright (C) 2012 Daniel Kristjansson <danielk@digital-nirvana.com>
 * Copyright (C) 2015 Gavin Hurlbut <gavin@digital-nirvana.com>
 * Copyright (C) 2016 John Poet <jppoet@digital-nirvana.com>
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

#ifndef _Decklink_VANC_H_
#define _Decklink_VANC_H_

#include "gstdecklinkvideosrc.h"

void gst_processVANC_init_log(void);
bool gst_processVANC(GstDecklinkVideoSrc *self,
                     IDeckLinkVideoFrameAncillary *vanc_frame,
                     guint width, gsize row_bytes, GstBuffer * buffer);

#endif
