// Goodix TLS 52xd protocol helpers for libfprint

// Copyright (C) 2026

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

#pragma once

#include <glib.h>

#define GOODIX52XD_WIDTH 64
#define GOODIX52XD_HEIGHT 80
#define GOODIX52XD_SCAN_WIDTH 64
#define GOODIX52XD_FRAME_SIZE (GOODIX52XD_WIDTH * GOODIX52XD_HEIGHT)
#define GOODIX52XD_PACKED_FRAME_SIZE                                            \
  ((GOODIX52XD_HEIGHT * GOODIX52XD_SCAN_WIDTH) / 4 * 6)
#define GOODIX52XD_PACKED_FRAME_WITH_TRAILER_SIZE                               \
  (GOODIX52XD_PACKED_FRAME_SIZE + 4)
#define GOODIX52XD_LE16_FRAME_SIZE                                              \
  (GOODIX52XD_FRAME_SIZE * sizeof (Goodix52xdPix))
#define GOODIX52XD_EMPTY_MEAN_MIN 1900
#define GOODIX52XD_EMPTY_MEAN_MAX 2200
#define GOODIX52XD_EMPTY_HIGH_PIXEL_THRESHOLD 2800
#define GOODIX52XD_EMPTY_HIGH_PIXEL_MAX 16
#define GOODIX52XD_EMPTY_SATURATED_MEAN_MIN 4000
#define GOODIX52XD_EMPTY_SATURATED_HIGH_PIXEL_MIN 5100

typedef guint16 Goodix52xdPix;

typedef struct
{
  guint      nonzero;
  Goodix52xdPix min;
  Goodix52xdPix max;
  guint      mean;
  guint      high_pixels;
} Goodix52xdFrameStats;

gboolean goodix52xd_decode_frame (Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE],
                                  const guint8 *raw_frame,
                                  guint16       length,
                                  GError      **error);

gboolean goodix52xd_frame_is_empty (const Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE],
                                    Goodix52xdFrameStats *stats);
