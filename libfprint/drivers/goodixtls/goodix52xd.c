// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Michael Teuscher <michael.teuscher@pm.me>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "fp-device.h"
#include "fp-image-device.h"
#include "fp-image.h"
#include "fpi-context.h"
#include "fpi-image-device.h"
#include "fpi-image.h"
#include "fpi-ssm.h"
#include "glibconfig.h"
#include <glib/gstdio.h>
#include "gusb/gusb-device.h"
#include <stdio.h>
#include <stdlib.h>
#define FP_COMPONENT "goodixtls52xd"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix52xd_proto.h"
#include "goodix52xd.h"

#define GOODIX52XD_EMPTY_POLL_FRAMES 3
#define GOODIX52XD_IMAGE_SCALE 2
#define GOODIX52XD_IMAGE_WIDTH (GOODIX52XD_WIDTH * GOODIX52XD_IMAGE_SCALE)
#define GOODIX52XD_IMAGE_HEIGHT (GOODIX52XD_HEIGHT * GOODIX52XD_IMAGE_SCALE)
#define GOODIX52XD_WINDOWS_10034_FDT_PAYLOAD_SIZE 27
#define GOODIX52XD_WINDOWS_10034_FDT_DOWN_PAYLOAD_SIZE 40
#define GOODIX52XD_FDT_WAIT_TIMEOUT_MS 1500

static gint goodix52xd_debug_dump_seq;

struct _FpiDeviceGoodixTls52XD {
  FpiDeviceGoodixTls parent;

  guint8* otp;
  const guint8* expected_pmk_hash;
  guint16 expected_pmk_hash_len;
  gboolean firmware_10034;
  guint scan_frame_count;
  guint image_frame_count;
  gboolean finger_reported;
  gboolean waiting_finger_off;
  gboolean deactivating;

  GSList* frames;
  FpiSsm* scan_ssm;
};

G_DECLARE_FINAL_TYPE(FpiDeviceGoodixTls52XD, fpi_device_goodixtls52xd, FPI,
                     DEVICE_GOODIXTLS52XD, FpiDeviceGoodixTls);

G_DEFINE_TYPE(FpiDeviceGoodixTls52XD, fpi_device_goodixtls52xd,
              FPI_TYPE_DEVICE_GOODIXTLS);

static void goodix52xd_reset_state(FpiDeviceGoodixTls52XD* self);

static gchar *
goodix52xd_debug_dump_path(const gchar *name, const gchar *extension)
{
    const gchar *dir = g_getenv("GOODIX52XD_DUMP_DIR");
    guint seq;

    if (!dir || !*dir)
        return NULL;

    if (g_mkdir_with_parents(dir, 0700) < 0) {
        fp_warn("failed to create Goodix 52xd dump directory: %s", dir);
        return NULL;
    }

    seq = g_atomic_int_add(&goodix52xd_debug_dump_seq, 1);

    return g_strdup_printf("%s/%06u-%s.%s", dir, seq, name, extension);
}

static void
goodix52xd_debug_dump_raw_frame(const Goodix52xdPix frame[GOODIX52XD_FRAME_SIZE])
{
    g_autofree gchar *path = goodix52xd_debug_dump_path("raw-frame", "pgm");
    GString *out = NULL;
    g_autoptr(GError) error = NULL;

    if (!path)
        return;

    out = g_string_sized_new(32 + GOODIX52XD_FRAME_SIZE * sizeof(guint16));
    g_string_append_printf(out, "P5\n%d %d\n4095\n",
                           GOODIX52XD_WIDTH, GOODIX52XD_HEIGHT);

    for (guint i = 0; i != GOODIX52XD_FRAME_SIZE; ++i) {
        guint16 pix = GUINT16_TO_BE(MIN(frame[i], 4095));

        g_string_append_len(out, (const gchar *) &pix, sizeof(pix));
    }

    if (!g_file_set_contents(path, out->str, out->len, &error))
        fp_warn("failed to dump Goodix 52xd raw frame: %s", error->message);

    g_string_free(out, TRUE);
}

static void
goodix52xd_debug_dump_image(FpImage *img)
{
    g_autofree gchar *path = goodix52xd_debug_dump_path("assembled", "pgm");
    GString *out = NULL;
    g_autoptr(GError) error = NULL;

    if (!path)
        return;

    out = g_string_sized_new(32 + img->width * img->height);
    g_string_append_printf(out, "P5\n%d %d\n255\n", img->width, img->height);
    g_string_append_len(out, (const gchar *) img->data, img->width * img->height);

    if (!g_file_set_contents(path, out->str, out->len, &error))
        fp_warn("failed to dump Goodix 52xd assembled image: %s", error->message);

    g_string_free(out, TRUE);
}

// ---- ACTIVE SECTION START ----

enum activate_states {
    ACTIVATE_READ_AND_NOP,
    ACTIVATE_ENABLE_CHIP,
    ACTIVATE_NOP,
    ACTIVATE_CHECK_FW_VER,
    ACTIVATE_CHECK_PSK,
    ACTIVATE_RESET,
    ACTIVATE_OTP,
    ACTIVATE_SET_MCU_IDLE,
    ACTIVATE_SET_MCU_CONFIG,
    ACTIVATE_NUM_STATES,
};

static void check_none(FpDevice *dev, gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static gboolean
goodix52xd_firmware_supported (const gchar *firmware)
{
  return (g_strcmp0 (firmware, GOODIX_52XD_FIRMWARE_VERSION) == 0 ||
          g_strcmp0 (firmware, GOODIX_52XD_FIRMWARE_VERSION_10034) == 0);
}

static gboolean
goodix52xd_set_expected_pmk_hash (FpiDeviceGoodixTls52XD *self,
                                  const gchar            *firmware)
{
  if (g_strcmp0 (firmware, GOODIX_52XD_FIRMWARE_VERSION_10034) == 0)
    {
      self->expected_pmk_hash = goodix_52xd_pmk_hash_10034;
      self->expected_pmk_hash_len = sizeof (goodix_52xd_pmk_hash_10034);
      self->firmware_10034 = TRUE;
      return TRUE;
    }

  if (g_strcmp0 (firmware, GOODIX_52XD_FIRMWARE_VERSION) == 0)
    {
      self->expected_pmk_hash = goodix_52xd_pmk_hash_10019;
      self->expected_pmk_hash_len = sizeof (goodix_52xd_pmk_hash_10019);
      self->firmware_10034 = FALSE;
      return TRUE;
    }

  return FALSE;
}

static const guint8 *
goodix52xd_get_tls_psk (FpDevice *dev, guint16 *length)
{
  FpiDeviceGoodixTls52XD *self = FPI_DEVICE_GOODIXTLS52XD(dev);

  if (!self->firmware_10034)
    {
      if (length)
        *length = 0;
      return NULL;
    }

  if (length)
    *length = sizeof (goodix_52xd_psk_10034);

  return goodix_52xd_psk_10034;
}

static void check_firmware_version(FpDevice *dev, gchar *firmware,
                                   gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device firmware: \"%s\"", firmware);

  if (!goodix52xd_firmware_supported(firmware)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device firmware: \"%s\"", firmware);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  goodix52xd_set_expected_pmk_hash(FPI_DEVICE_GOODIXTLS52XD(dev), firmware);

  fpi_ssm_next_state(user_data);
}

static void check_reset(FpDevice *dev, gboolean success, guint16 number,
                        gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!success) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to reset device");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device reset number: %d", number);

  if (number != GOODIX_52XD_RESET_NUMBER) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device reset number: %d", number);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_preset_psk_read(FpDevice *dev, gboolean success,
                                  guint32 flags, guint8 *psk, guint16 length,
                                  gpointer user_data, GError *error) {
  FpiDeviceGoodixTls52XD *self = FPI_DEVICE_GOODIXTLS52XD(dev);

  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!success) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to read PSK from device");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device PSK flags: 0x%08x", flags);
  fp_dbg("Device PSK hash length: %d", length);

  if (flags != GOODIX_52XD_PSK_FLAGS) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK flags: 0x%08x", flags);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!self->expected_pmk_hash ||
      length != self->expected_pmk_hash_len) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK hash length: %d", length);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (memcmp(psk, self->expected_pmk_hash, self->expected_pmk_hash_len)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Unsupported device PSK hash");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}
static void check_idle(FpDevice* dev, gpointer user_data, GError* err)
{

    if (err) {
        fpi_ssm_mark_failed(user_data, err);
        return;
    }
    fpi_ssm_next_state(user_data);
}
static void check_config_upload(FpDevice* dev, gboolean success,
                                gpointer user_data, GError* error)
{
    if (error) {
        fpi_ssm_mark_failed(user_data, error);
    }
    else if (!success) {
        fpi_ssm_mark_failed(user_data,
                            g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                        "failed to upload mcu config"));
    }
    else {
        fpi_ssm_next_state(user_data);
    }
}
static void read_otp_callback(FpDevice* dev, guint8* data, guint16 len,
                              gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }
    if (len < 64) {
        fpi_ssm_mark_failed(ssm, g_error_new(FP_DEVICE_ERROR,
                                             FP_DEVICE_ERROR_DATA_INVALID,
                                             "OTP is invalid (len: %d)", len));
        return;
    }
    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(dev);
    g_free(self->otp);
    self->otp = g_memdup2(data, 64);
    fpi_ssm_next_state(ssm);
}

static void goodix52xd_send_upload_config(FpDevice *dev,
                                          GoodixSuccessCallback callback,
                                          gpointer user_data)
{
    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(dev);
    guint8 config[sizeof(goodix_52xd_config)];

    memcpy(config, goodix_52xd_config, sizeof(config));

    if (self->firmware_10034) {
        /* Captured Windows 10034 config adjusts two parameters and checksum. */
        config[175] = 0xef;
        config[235] = 0xe2;
        config[255] = 0x0f;
    }

    goodix_send_upload_config_mcu(dev, config, sizeof(config), NULL,
                                  callback, user_data);
}

static void activate_run_state(FpiSsm* ssm, FpDevice* dev)
{

    switch (fpi_ssm_get_cur_state(ssm)) {
    case ACTIVATE_READ_AND_NOP: {
        GError* error = NULL;
        if (!goodix_send_nop_wakeup(dev, &error)) {
            fpi_ssm_mark_failed(ssm, error);
            break;
        }
        goodix_start_read_loop(dev);
        fpi_ssm_next_state_delayed(ssm, 50);
        break;
    }

    case ACTIVATE_ENABLE_CHIP:
      goodix_send_enable_chip(dev, TRUE, check_none, ssm);
      break;

    case ACTIVATE_NOP:
      goodix_send_nop(dev, check_none, ssm);
      break;

    case ACTIVATE_CHECK_FW_VER:
      goodix_send_firmware_version(dev, check_firmware_version, ssm);
      break;

    case ACTIVATE_CHECK_PSK:
      goodix_send_preset_psk_read(dev, GOODIX_52XD_PSK_FLAGS, 32,
                                  check_preset_psk_read, ssm);
      break;

    case ACTIVATE_RESET:
      goodix_send_reset(dev, TRUE, 20, check_reset, ssm);
      break;

    case ACTIVATE_OTP:
      goodix_send_read_otp(dev, read_otp_callback, ssm);
      break;

    case ACTIVATE_SET_MCU_IDLE:
        goodix_send_mcu_switch_to_idle_mode(dev, 20, check_idle, ssm);
        break;

    case ACTIVATE_SET_MCU_CONFIG:
        goodix52xd_send_upload_config(dev, check_config_upload, ssm);
        break;
    }
}

static void tls_activation_complete(FpDevice* dev, gpointer user_data,
                                    GError* error)
{
    if (error) {
        fp_err("failed to complete tls activation: %s", error->message);
        fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
        return;
    }
    FpImageDevice* image_dev = FP_IMAGE_DEVICE(dev);

    fpi_image_device_activate_complete(image_dev, error);
}

static void activate_complete(FpiSsm* ssm, FpDevice* dev, GError* error)
{
    G_DEBUG_HERE();
    if (!error)
        goodix_tls(dev, tls_activation_complete, NULL);
    else {
        fp_err("failed during activation: %s (code: %d)", error->message,
               error->code);
        fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
    }
}

// ---- ACTIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- SCAN SECTION START ----

enum SCAN_STAGES {
    SCAN_STAGE_WINDOWS_10034_UPLOAD_CONFIG,
    SCAN_STAGE_WINDOWS_10034_SET_DRV_STATE,
    SCAN_STAGE_WINDOWS_10034_PRIME,
    SCAN_STAGE_SWITCH_TO_FDT_DOWN,
    SCAN_STAGE_SWITCH_TO_FDT_MODE,
    SCAN_STAGE_GET_IMG,

    SCAN_STAGE_NUM,
};

static void goodix52xd_check_scan_preamble(FpDevice* dev, gboolean success,
                                           gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }

    if (!success) {
        fpi_ssm_mark_failed(
            ssm, fpi_device_error_new(FP_DEVICE_ERROR_GENERAL));
        return;
    }

    fpi_ssm_next_state(ssm);
}

static void goodix52xd_check_none_scan_preamble(FpDevice* dev,
                                                gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }

    fpi_ssm_next_state(ssm);
}

static void goodix52xd_windows_register_reset_complete(FpDevice* dev,
                                                       gpointer user_data,
                                                       GError* error);
static void write_sensor_complete(FpDevice* dev, gpointer user_data,
                                  GError* error);

static void check_none_cmd(FpDevice* dev, guint8* data, guint16 len,
                           gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }
    fpi_ssm_next_state(ssm);
}

static void goodix52xd_set_u16(guint8* payload, guint16 value)
{
    payload[0] = value & 0xff;
    payload[1] = value >> 8;
}

static void goodix52xd_fill_windows_10034_image_payload(
    FpiDeviceGoodixTls52XD* self, guint8 payload[10])
{
    guint8 image_flags = self->image_frame_count == 0 ? 0x45 : 0xc5;

    payload[0] = image_flags;
    payload[1] = 0x03;
    goodix52xd_set_u16(payload + 2, self->otp[26] + 6);
    goodix52xd_set_u16(payload + 4, self->otp[26]);
    goodix52xd_set_u16(payload + 6, self->otp[45] + 6);
    goodix52xd_set_u16(payload + 8, self->otp[45]);
}

static void goodix52xd_fill_image_payload(FpiDeviceGoodixTls52XD* self,
                                          guint8 payload[10])
{
    if (self->firmware_10034) {
        goodix52xd_fill_windows_10034_image_payload(self, payload);
        return;
    }

    payload[0] = 0x43;
    payload[1] = 0x03;
    payload[2] = self->otp[26] + 6;
    payload[3] = 0x00;
    payload[4] = self->otp[26];
    payload[5] = 0x00;
    payload[6] = self->otp[45] + 6;
    payload[7] = 0x00;
    payload[8] = self->otp[45];
    payload[9] = 0x00;
}

static void goodix52xd_fill_windows_10034_fdt_payload(
    FpiDeviceGoodixTls52XD* self,
    guint8 payload[GOODIX52XD_WINDOWS_10034_FDT_PAYLOAD_SIZE])
{
    static const guint8 fdt_thresholds[] = {
        0x96, 0x96, 0x8e, 0x8e, 0x98, 0x98, 0x8f, 0x8f,
        0x95, 0x95, 0x8d, 0x8d, 0x91, 0x91, 0x88, 0x88,
    };

    memset(payload, 0, GOODIX52XD_WINDOWS_10034_FDT_PAYLOAD_SIZE);
    payload[0] = 0x0d;
    payload[1] = 0x01;
    goodix52xd_set_u16(payload + 2, 0x0100 | self->otp[33]);
    goodix52xd_set_u16(payload + 4, 0x0100 | self->otp[41]);
    goodix52xd_set_u16(payload + 6, 0x0100 | self->otp[42]);
    goodix52xd_set_u16(payload + 8, 0x0100 | self->otp[43]);
    memcpy(payload + 10, fdt_thresholds, sizeof(fdt_thresholds));
}

static void goodix52xd_fill_windows_10034_fdt_down_payload(
    FpiDeviceGoodixTls52XD* self,
    guint8 payload[GOODIX52XD_WINDOWS_10034_FDT_DOWN_PAYLOAD_SIZE],
    gboolean wait_for_finger)
{
    static const guint8 fdt_thresholds[] = {
        0x96, 0x96, 0x8e, 0x8e, 0x98, 0x98, 0x8f, 0x8f,
        0x95, 0x95, 0x8d, 0x8d, 0x91, 0x91, 0x88, 0x88,
    };

    memset(payload, 0, GOODIX52XD_WINDOWS_10034_FDT_DOWN_PAYLOAD_SIZE);
    payload[0] = 0x9c;
    payload[1] = 0x01;
    goodix52xd_set_u16(payload + 2, 0x0100 | self->otp[33]);
    goodix52xd_set_u16(payload + 4, 0x0100 | self->otp[41]);
    goodix52xd_set_u16(payload + 6, 0x0100 | self->otp[42]);
    goodix52xd_set_u16(payload + 8, 0x0100 | self->otp[43]);
    memcpy(payload + 10, fdt_thresholds, sizeof(fdt_thresholds));
    payload[26] = wait_for_finger ? 0x01 : 0x00;
    payload[28] = 0x05;
    payload[29] = 0x03;
    goodix52xd_set_u16(payload + 30, self->otp[26] + 6);
    goodix52xd_set_u16(payload + 32, self->otp[26]);
    goodix52xd_set_u16(payload + 34, self->otp[45] + 6);
    goodix52xd_set_u16(payload + 36, self->otp[45]);
    payload[38] = 0x03;
    payload[39] = 0x00;
}

/**
 * @brief Squashes the 12 bit pixels of a raw frame into the 4 bit pixels used
 * by libfprint.
 * @details Borrowed from the elan driver. We reduce frames to
 * within the max and min.
 *
 * @param frame
 * @param squashed
 */
static void squash_frame_linear(const Goodix52xdPix* frame, guint8* squashed)
{
    Goodix52xdPix min = 0xffff;
    Goodix52xdPix max = 0;

    for (int i = 0; i != GOODIX52XD_FRAME_SIZE; ++i) {
        const Goodix52xdPix pix = frame[i];
        if (pix < min) {
            min = pix;
        }
        if (pix > max) {
            max = pix;
        }
    }

    for (int i = 0; i != GOODIX52XD_FRAME_SIZE; ++i) {
        const Goodix52xdPix pix = frame[i];
        if (pix - min == 0 || max - min == 0) {
            squashed[i] = 0;
        }
        else {
            squashed[i] = (pix - min) * 0xff / (max - min);
        }
    }
}

static FpImage*
goodix52xd_image_from_frame(const Goodix52xdPix* raw_frame)
{
    g_autofree guint8* squashed = g_malloc(GOODIX52XD_FRAME_SIZE);
    FpImage* img = fp_image_new(GOODIX52XD_IMAGE_WIDTH,
                                GOODIX52XD_IMAGE_HEIGHT);

    squash_frame_linear(raw_frame, squashed);

    for (guint y = 0; y != GOODIX52XD_HEIGHT; ++y) {
        for (guint x = 0; x != GOODIX52XD_WIDTH; ++x) {
            guint8 pix = squashed[x + y * GOODIX52XD_WIDTH];

            for (guint yy = 0; yy != GOODIX52XD_IMAGE_SCALE; ++yy) {
                guint out_y = y * GOODIX52XD_IMAGE_SCALE + yy;

                for (guint xx = 0; xx != GOODIX52XD_IMAGE_SCALE; ++xx) {
                    guint out_x = x * GOODIX52XD_IMAGE_SCALE + xx;

                    img->data[out_x + out_y * img->width] = pix;
                }
            }
        }
    }

    return img;
}

static void goodix52xd_continue_image_poll(FpDevice* dev, gpointer ssm)
{
    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(dev);

    if (self->firmware_10034)
        goodix_send_write_sensor_register(dev, 556, 0x020a,
                                          goodix52xd_windows_register_reset_complete,
                                          ssm);
    else
        fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_MODE);
}

static void scan_on_read_img(FpDevice* dev, guint8* data, guint16 len,
                             gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }

    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(dev);
    GError* frame_error = NULL;
    Goodix52xdPix* frame = g_malloc(GOODIX52XD_FRAME_SIZE * sizeof(Goodix52xdPix));

    if (!goodix52xd_decode_frame(frame, data, len, &frame_error)) {
        g_free(frame);
        fpi_ssm_mark_failed(ssm, frame_error);
        return;
    }

    self->image_frame_count++;
    goodix52xd_debug_dump_raw_frame(frame);

    if (self->waiting_finger_off) {
        Goodix52xdFrameStats stats = { 0 };
        gboolean empty = goodix52xd_frame_is_empty(frame, &stats);

        fp_dbg("awaiting-finger-off frame stats nonzero=%u min=%u max=%u mean=%u high=%u empty=%d",
               stats.nonzero, stats.min, stats.max, stats.mean,
               stats.high_pixels, empty);

        g_free(frame);

        if (empty) {
            gboolean report_finger_off = self->finger_reported;

            g_slist_free_full(g_steal_pointer(&self->frames), g_free);
            self->scan_frame_count = 0;
            self->image_frame_count = 0;
            self->waiting_finger_off = FALSE;
            self->finger_reported = FALSE;

            fpi_ssm_next_state(ssm);

            if (report_finger_off)
                fpi_image_device_report_finger_status(FP_IMAGE_DEVICE(dev), FALSE);

            return;
        }

        fp_dbg("finger still present on Goodix 52xd sensor; continuing finger-off wait");
        self->scan_frame_count = 0;
        self->image_frame_count = 0;
        fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_DOWN);
        return;
    }

    if (!self->finger_reported) {
        Goodix52xdFrameStats stats = { 0 };
        gboolean empty = goodix52xd_frame_is_empty(frame, &stats);

        fp_dbg("awaiting-finger frame stats nonzero=%u min=%u max=%u mean=%u high=%u empty=%d",
               stats.nonzero, stats.min, stats.max, stats.mean,
               stats.high_pixels, empty);

        if (empty) {
            g_free(frame);
            g_slist_free_full(g_steal_pointer(&self->frames), g_free);
            self->scan_frame_count = 0;
            if (self->image_frame_count < GOODIX52XD_EMPTY_POLL_FRAMES) {
                fp_dbg("empty Goodix 52xd frame while awaiting finger; continuing image poll burst");
                goodix52xd_continue_image_poll(dev, ssm);
                return;
            }

            self->image_frame_count = 0;
            fp_dbg("empty Goodix 52xd poll burst while awaiting finger; continuing wait");
            fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_DOWN);
            return;
        }
    }

    if (!self->finger_reported) {
        FpImageDevice* img_dev = FP_IMAGE_DEVICE(dev);
        FpImage* img;

        fpi_image_device_report_finger_status(FP_IMAGE_DEVICE(dev), TRUE);
        self->finger_reported = TRUE;

        img = goodix52xd_image_from_frame(frame);
        goodix52xd_debug_dump_image(img);

        g_free(frame);
        self->scan_frame_count = 0;
        self->image_frame_count = 0;

        fpi_ssm_next_state(ssm);

        fpi_image_device_image_captured(img_dev, img);
        return;
    }

    g_free(frame);
    self->scan_frame_count = 0;
    self->image_frame_count = 0;
    fpi_ssm_mark_failed(
        ssm,
        g_error_new(G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Unexpected extra Goodix 52xd image after finger report"));
}

static void scan_get_img(FpDevice* dev, FpiSsm* ssm)
{
    FpImageDevice* img_dev = FP_IMAGE_DEVICE(dev);
    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(img_dev);
    guint8 payload[10];
    goodix52xd_fill_image_payload(self, payload);
    goodix_tls_read_image(dev, payload, sizeof(payload), scan_on_read_img, ssm);
}

const guint8 fdt_switch_state_mode_52xd[] = {
    0x0d, 0x01, 0x27, 0x01, 0x21, 0x01, 0x27,
    0x01, 0x23, 0x01, 0x8d, 0x8d, 0x86, 0x86,
    0x97, 0x97, 0x8f, 0x8f, 0x9b, 0x9b, 0x92,
    0x92, 0x96, 0x96, 0x8c, 0x8c, 0x01
};

guint8 fdt_switch_state_down_52xd[] = {
    0x9c, 0x01, 0x27, 0x01, 0x21, 0x01, 0x21,
    0x01, 0x23, 0x01, 0x8d, 0x8d, 0x86, 0x86,
    0x97, 0x97, 0x8f, 0x8f, 0x9b, 0x9b, 0x92,
    0x92, 0x96, 0x96, 0x8c, 0x8c, 0x00, 0x00,
    0x05, 0x03, 0xa7, 0x00, 0xa1, 0x00, 0xa7,
    0x00, 0xa3, 0x00, 0x00
};

static void goodix52xd_windows_prime_done(FpDevice* dev, guint8* data,
                                          guint16 len, gpointer ssm,
                                          GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }

    if (len == 1) {
        fp_dbg("Windows 10034 prime status: 0x%02x", data[0]);
        fpi_ssm_next_state(ssm);
        return;
    }

    if (len != 1 &&
        len != GOODIX52XD_PACKED_FRAME_SIZE &&
        len != GOODIX52XD_PACKED_FRAME_WITH_TRAILER_SIZE &&
        len != GOODIX52XD_LE16_FRAME_SIZE)
        fp_warn("Windows 10034 prime returned unexpected length: %u",
                (guint) len);

    fpi_ssm_next_state(ssm);
}

static void goodix52xd_windows_fdt_mode_done(FpDevice* dev, guint8* data,
                                             guint16 len, gpointer ssm,
                                             GError* err)
{
    (void) dev;
    (void) data;
    (void) len;

    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }

    fpi_ssm_next_state(ssm);
}

static void goodix52xd_windows_fdt_mode_ack(FpDevice* dev, guint8* data,
                                            guint16 len, gpointer ssm,
                                            GError* err)
{
    (void) data;
    (void) len;

    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }

    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(dev);
    guint8 payload[GOODIX52XD_WINDOWS_10034_FDT_PAYLOAD_SIZE];

    goodix52xd_fill_windows_10034_fdt_payload(self, payload);
    payload[GOODIX52XD_WINDOWS_10034_FDT_PAYLOAD_SIZE - 1] = 0x01;
    goodix_send_mcu_switch_to_fdt_mode(
        dev, payload, sizeof(payload), TRUE, NULL,
        goodix52xd_windows_fdt_mode_done, ssm);
}

static void scan_run_state(FpiSsm* ssm, FpDevice* dev)
{
    FpImageDevice* img_dev = FP_IMAGE_DEVICE(dev);
    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(img_dev);

    switch (fpi_ssm_get_cur_state(ssm)) {

    case SCAN_STAGE_WINDOWS_10034_UPLOAD_CONFIG:
        if (self->firmware_10034) {
            goodix52xd_send_upload_config(dev, goodix52xd_check_scan_preamble,
                                          ssm);
            break;
        }

        fpi_ssm_next_state(ssm);
        break;

    case SCAN_STAGE_WINDOWS_10034_SET_DRV_STATE:
        if (self->firmware_10034) {
            goodix_send_set_drv_state(dev, 1,
                                      goodix52xd_check_none_scan_preamble,
                                      ssm);
            break;
        }

        fpi_ssm_next_state(ssm);
        break;

    case SCAN_STAGE_WINDOWS_10034_PRIME:
        if (self->firmware_10034) {
            goodix_send_tls_image_or_data(dev, goodix52xd_windows_prime_done,
                                          ssm);
            break;
        }

        fpi_ssm_next_state(ssm);
        break;

    case SCAN_STAGE_SWITCH_TO_FDT_MODE:
        if (self->firmware_10034) {
            guint8 payload[GOODIX52XD_WINDOWS_10034_FDT_PAYLOAD_SIZE];

            goodix52xd_fill_windows_10034_fdt_payload(self, payload);
            goodix_send_mcu_switch_to_fdt_mode(
                dev, payload, sizeof(payload), FALSE, NULL,
                goodix52xd_windows_fdt_mode_ack, ssm);
            break;
        }

        goodix_send_mcu_switch_to_fdt_mode(dev, (guint8*) fdt_switch_state_mode_52xd,
                                           sizeof(fdt_switch_state_mode_52xd), FALSE, NULL,
                                           check_none_cmd, ssm);
        break;

    case SCAN_STAGE_SWITCH_TO_FDT_DOWN:
        if (self->firmware_10034) {
            guint8 payload[GOODIX52XD_WINDOWS_10034_FDT_DOWN_PAYLOAD_SIZE];

            goodix52xd_fill_windows_10034_fdt_down_payload(self, payload,
                                                           FALSE);
            goodix_send_mcu_switch_to_fdt_down(
                dev, payload, sizeof(payload), FALSE, NULL,
                receive_fdt_down_ack, ssm);
            break;
        }

        // FDT Down Cali
        fdt_switch_state_down_52xd[2] = self->otp[33];
        fdt_switch_state_down_52xd[4] = self->otp[41];
        fdt_switch_state_down_52xd[6] = self->otp[42];
        fdt_switch_state_down_52xd[8] = self->otp[43];

        // Image Cali
        fdt_switch_state_down_52xd[32] = self->otp[26];
        fdt_switch_state_down_52xd[36] = self->otp[45];
        fdt_switch_state_down_52xd[30] = fdt_switch_state_down_52xd[32] + 6;
        fdt_switch_state_down_52xd[34] = fdt_switch_state_down_52xd[36] + 6;



        fdt_switch_state_down_52xd[26] = 0x00;

        goodix_send_mcu_switch_to_fdt_down(dev, (guint8*) fdt_switch_state_down_52xd,
                                           sizeof(fdt_switch_state_down_52xd), FALSE, NULL,
                                           receive_fdt_down_ack, ssm);
        break;
    case SCAN_STAGE_GET_IMG:
        guint16 payload = self->firmware_10034 ? 0x030a : 0x0305;
        goodix_send_write_sensor_register(dev, 556, payload, write_sensor_complete, ssm);
        break;
    }
}

static void receive_fdt_down_wait_done(FpDevice* dev, guint8* data, guint16 len,
                                       gpointer ssm, GError* err)
{
    (void) data;
    (void) len;

    if (err) {
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) &&
            !fpi_device_action_is_cancelled(dev)) {
            fp_dbg("Goodix 52xd FDT wait timed out; polling image");
            g_error_free(err);
            fpi_ssm_jump_to_state(ssm, SCAN_STAGE_GET_IMG);
            return;
        }

        fpi_ssm_mark_failed(ssm, err);
        return;
    }

    fpi_ssm_next_state(ssm);
}

static void receive_fdt_down_ack(FpDevice* dev, guint8* data, guint16 len,
                           gpointer ssm, GError* err)
{
    (void) data;
    (void) len;

    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }

    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(dev);

    if (self->firmware_10034) {
        guint8 payload[GOODIX52XD_WINDOWS_10034_FDT_DOWN_PAYLOAD_SIZE];

        goodix52xd_fill_windows_10034_fdt_down_payload(self, payload, TRUE);
        goodix_send_mcu_switch_to_fdt_down_timeout(
            dev, payload, sizeof(payload), TRUE, NULL,
            GOODIX52XD_FDT_WAIT_TIMEOUT_MS, receive_fdt_down_wait_done, ssm);
        return;
    }

    fdt_switch_state_down_52xd[26] = 0x01;
    goodix_send_mcu_switch_to_fdt_down_timeout(
        dev, (guint8*) fdt_switch_state_down_52xd,
        sizeof(fdt_switch_state_down_52xd), TRUE, NULL,
        GOODIX52XD_FDT_WAIT_TIMEOUT_MS, receive_fdt_down_wait_done, ssm);
}

static void goodix52xd_windows_register_reset_complete(FpDevice* dev,
                                                       gpointer user_data,
                                                       GError* error)
{
    if (error) {
        fp_err("failed to reset scan register: %s (code: %d)",
               error->message, error->code);
        fpi_ssm_mark_failed(user_data, error);
        return;
    }

    goodix_send_write_sensor_register(dev, 556, 0x030a,
                                      write_sensor_complete, user_data);
}

static void write_sensor_complete(FpDevice *dev, gpointer user_data, GError *error) 
{
    if (error) {
        fp_err("failed to scan: %s (code: %d)", error->message, error->code);
        fpi_ssm_mark_failed(user_data, error);
        return;
    }
    scan_get_img(dev, user_data);
}

static void scan_complete(FpiSsm* ssm, FpDevice* dev, GError* error)
{
    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(dev);

    self->scan_ssm = NULL;
    g_slist_free_full(g_steal_pointer(&self->frames), g_free);
    self->scan_frame_count = 0;
    self->image_frame_count = 0;

    if (error) {
        gboolean cancelled = g_error_matches(error, G_IO_ERROR,
                                             G_IO_ERROR_CANCELLED);

        self->waiting_finger_off = FALSE;
        self->finger_reported = FALSE;

        if (cancelled && (self->deactivating ||
                          fpi_device_action_is_cancelled(dev))) {
            fp_dbg("scan cancelled");
            g_clear_error(&error);
            return;
        }

        if (!cancelled)
            fp_err("failed to scan: %s (code: %d)", error->message, error->code);

        fpi_image_device_session_error(FP_IMAGE_DEVICE(dev), error);
        return;
    }
    fp_dbg("finished scan");
}

static void scan_start(FpiDeviceGoodixTls52XD* dev)
{
    FpiSsm* ssm;

    g_slist_free_full(g_steal_pointer(&dev->frames), g_free);
    dev->scan_frame_count = 0;
    dev->image_frame_count = 0;
    dev->finger_reported = FALSE;
    dev->waiting_finger_off = FALSE;
    dev->deactivating = FALSE;
    ssm = fpi_ssm_new(FP_DEVICE(dev), scan_run_state, SCAN_STAGE_NUM);
    dev->scan_ssm = ssm;
    fpi_ssm_start(ssm, scan_complete);
}

static void finger_off_start(FpiDeviceGoodixTls52XD* dev)
{
    FpiSsm* ssm;

    g_slist_free_full(g_steal_pointer(&dev->frames), g_free);
    dev->scan_frame_count = 0;
    dev->image_frame_count = 0;
    dev->waiting_finger_off = TRUE;
    dev->deactivating = FALSE;
    ssm = fpi_ssm_new(FP_DEVICE(dev), scan_run_state, SCAN_STAGE_NUM);
    dev->scan_ssm = ssm;
    fpi_ssm_start(ssm, scan_complete);
}

static void goodix52xd_cancel_scan(FpiDeviceGoodixTls52XD* self)
{
    FpiSsm* ssm = g_steal_pointer(&self->scan_ssm);

    if (!ssm)
        return;

    fpi_ssm_mark_failed(
        ssm,
        g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Operation was cancelled"));
}

// ---- SCAN SECTION END ----

// ---- DEV SECTION START ----

static void dev_init(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  if (!goodix_dev_init(dev, &error)) {
    fpi_image_device_open_complete(img_dev, error);
    return;
  }

  fpi_image_device_open_complete(img_dev, NULL);
}

static void dev_deinit(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  goodix52xd_cancel_scan(FPI_DEVICE_GOODIXTLS52XD(img_dev));
  goodix52xd_reset_state(FPI_DEVICE_GOODIXTLS52XD(img_dev));

  if (!goodix_dev_deinit(dev, &error)) {
    fpi_image_device_close_complete(img_dev, error);
    return;
  }

  fpi_image_device_close_complete(img_dev, NULL);
}

static void dev_activate(FpImageDevice *img_dev) {
    FpDevice* dev = FP_DEVICE(img_dev);

    fpi_ssm_start(fpi_ssm_new(dev, activate_run_state, ACTIVATE_NUM_STATES),
                  activate_complete);
}



static void dev_change_state(FpImageDevice* img_dev, FpiImageDeviceState state)
{
    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(img_dev);
    G_DEBUG_HERE();

    if (fpi_device_action_is_cancelled(FP_DEVICE(img_dev))) {
        fp_dbg("ignoring Goodix 52xd image-device state change while action is cancelled");
        return;
    }

    if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON) {
        scan_start(self);
    }
    else if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF) {
        if (!self->finger_reported) {
            fp_dbg("ignoring Goodix 52xd finger-off wait without a reported finger");
            return;
        }

        finger_off_start(self);
    }
}

static void goodix52xd_reset_state(FpiDeviceGoodixTls52XD* self)
{
    g_slist_free_full(g_steal_pointer(&self->frames), g_free);
    self->scan_frame_count = 0;
    self->image_frame_count = 0;
    self->finger_reported = FALSE;
    self->waiting_finger_off = FALSE;
    self->deactivating = FALSE;
}

static void dev_deactivate(FpImageDevice *img_dev) {
    FpDevice* dev = FP_DEVICE(img_dev);
    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(img_dev);
    self->deactivating = TRUE;
    goodix52xd_cancel_scan(self);
    goodix_reset_state(dev);
    GError* error = NULL;
    goodix_shutdown_tls(dev, &error);
    goodix52xd_reset_state(self);
    fpi_image_device_deactivate_complete(img_dev, error);
}

// ---- DEV SECTION END ----

static void fpi_device_goodixtls52xd_init(FpiDeviceGoodixTls52XD* self)
{
    self->frames = NULL;
    self->finger_reported = FALSE;
    self->waiting_finger_off = FALSE;
    self->image_frame_count = 0;
    self->scan_ssm = NULL;
    self->deactivating = FALSE;
}

static void
fpi_device_goodixtls52xd_finalize(GObject* object)
{
    FpiDeviceGoodixTls52XD* self = FPI_DEVICE_GOODIXTLS52XD(object);

    goodix52xd_reset_state(self);
    g_clear_pointer(&self->otp, g_free);

    G_OBJECT_CLASS(fpi_device_goodixtls52xd_parent_class)->finalize(object);
}

static void fpi_device_goodixtls52xd_class_init(
    FpiDeviceGoodixTls52XDClass *class) {
  GObjectClass *object_class = G_OBJECT_CLASS(class);
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS(class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS(class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS(class);

  object_class->finalize = fpi_device_goodixtls52xd_finalize;

  gx_class->interface = GOODIX_52XD_INTERFACE;
  gx_class->ep_in = GOODIX_52XD_EP_IN;
  gx_class->ep_out = GOODIX_52XD_EP_OUT;
  gx_class->get_tls_psk = goodix52xd_get_tls_psk;

  dev_class->id = "goodixtls52xd";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 52XD";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;

  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_dev_class->bz3_threshold = 24;
  img_dev_class->img_width = GOODIX52XD_IMAGE_WIDTH;
  img_dev_class->img_height = GOODIX52XD_IMAGE_HEIGHT;

  img_dev_class->img_open = dev_init;
  img_dev_class->img_close = dev_deinit;
  img_dev_class->activate = dev_activate;
  img_dev_class->change_state = dev_change_state;
  img_dev_class->deactivate = dev_deactivate;

  fpi_device_class_auto_initialize_features(dev_class);
}
