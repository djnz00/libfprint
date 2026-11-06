# Goodix 521d (27c6:521d) with firmware 10019 support

This fork adds support for the Goodix 521d fingerprint sensor
(USB ID `27c6:521d`) running firmware `GFUSB_GM168SEC_APP_10019`.

## The Problem

The stock `goodixtls52xd` driver targets firmware `10034`. Sensors
shipped with or downgraded to `10019` fail with:

- `Invalid device firmware`
- `Invalid OTP (0 bytes)`
- `Goodix TLS PSK is not configured`
- `TLS server rejected device handshake`
- an `awaiting-finger-off` hang

## The Fix

1. **Firmware 10019 accepted** (already in-tree)
2. **OTP fallback**: when OTP < 64 bytes, fill required fields with
   hardcoded values so image/FDT config proceeds
3. **TLS PSK = 32 zero bytes for 10019**: the device's session key is
   zeros, not the PMK readback or the 10034 constant (discovered
   from https://github.com/knauth/goodix-521d-explanation)
4. **Threshold calibration**: `EMPTY_SATURATED_MEAN_MIN` 4000→3800,
   `EMPTY_SATURATED_HIGH_PIXEL_MIN` 5100→4750. The 521d idle frame
   is bright (~mean 3895, 94% pixels >2800); these classify idle as
   empty so finger-on/off works.

## Tested

- Hardware: Goodix 27c6:521d
- Firmware: GFUSB_GM168SEC_APP_10019
- OS: Arch Linux
- libfprint: 1.94.10
- Result: enrollment completes, verify works (~60% match rate;
  tuning thresholds may improve)

## Tuning

Override thresholds via systemd:

```ini
[Service]
Environment=GOODIX52XD_EMPTY_SATURATED_MEAN_MIN=3800
Environment=GOODIX52XD_EMPTY_SATURATED_HIGH_PIXEL_MIN=4750
```

## License

LGPL-2.1-or-later, same as upstream. See COPYING.

## Credits

- Upstream: https://gitlab.freedesktop.org/libfprint/libfprint
- djnz00 fork: https://github.com/djnz00/libfprint
- Python reference: https://github.com/knauth/goodix-521d-explanation
- Goodix Linux Discord

## Files touched

- `libfprint/drivers/goodixtls/goodix52xd.c`
- `libfprint/drivers/goodixtls/goodix52xd_proto.c`
- `libfprint/drivers/goodixtls/goodix52xd_proto.h`
