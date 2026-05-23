## Summary

Research conducted on 2026-05-23 for the `libfprint` checkout on branch `unstable`. No `source.yaml` was present in this repository, so this report uses a fresh repository source inventory of C, C++, Python, Meson, documentation, test fixture, and data files. The project is a Meson-built fingerprint library exposing public `FpContext`, `FpDevice`, `FpPrint`, `FpImage`, and image-device APIs while maintaining internal `fpi_*` driver APIs. The tree contains the core library in `libfprint/`, hardware drivers in `libfprint/drivers/`, NBIS minutiae/matching code in `libfprint/nbis/`, tests in `tests/`, examples in `examples/`, GTK demo code in `demo/`, and generated udev data helpers in `data/`.

## Coding style and conventions

The top-level Meson project sets C standard `gnu99`, debug-optimized defaults, and a warning set that includes `-Wall`, format warnings, shadow warnings, and several `-Werror` checks (`meson.build:1`, `meson.build:17`). Public objects use GLib/GObject naming such as `FpContext`, `FpDevice`, and `FpPrint`, while internal APIs use the `fpi_` prefix described in `HACKING.md:33`. Public API additions and behavior changes are documented with gtk-doc comments according to `HACKING.md:45`. Driver classes follow GObject type macros with IDs such as `goodixtls52xd` (`libfprint/drivers/goodixtls/goodix52xd.c:67`, `libfprint/drivers/goodixtls/goodix52xd.c:772`). Indentation is C-style with space alignment; most core files use GLib types (`gboolean`, `guint8`, `GError`) and asynchronous callbacks.

## Detailed Findings

### Build system and driver selection

The root build defines dependencies, supported driver names, helper groups, and subdirectories (`meson.build:85`, `meson.build:126`, `meson.build:314`). Driver selection is controlled by the `drivers` Meson option (`meson_options.txt:1`). Goodix TLS drivers are part of the default driver list as `goodixtls511`, `goodixtls52xd`, and `goodixtls53xd` (`meson.build:126`). Their helper dependency group pulls in OpenSSL and threads (`meson.build:220`). The library build maps driver names to source files and shared helper files (`libfprint/meson.build:142`, `libfprint/meson.build:157`), then builds private, driver, and shared library targets (`libfprint/meson.build:255`, `libfprint/meson.build:264`, `libfprint/meson.build:274`).

### Public library layer

`FpContext` discovers and tracks devices as a GObject with private state (`libfprint/fp-context.c:46`, `libfprint/fp-context.c:60`). `FpDevice` is an abstract async-initializable GObject that exposes operations for enrollment, verification, identification, capture, and feature queries (`libfprint/fp-device.c:36`, `libfprint/fp-device.c:1284`, `libfprint/fp-device.c:1631`, `libfprint/fp-device.c:2239`). `FpPrint` stores enrolled print metadata and image data (`libfprint/fp-print.c:44`, `libfprint/fp-print.c:321`, `libfprint/fp-print.c:389`). `FpImageDevice` provides the image-device state layer used by image-producing drivers (`libfprint/fp-image-device.c:37`).

### Internal driver framework

The internal API is declared in `libfprint/drivers_api.h` and `fpi-*` headers. State-machine support is implemented by `FpiSsm`, which stores device, current state, completion callback, and cleanup data (`libfprint/fpi-ssm.c:73`). USB and SPI transfer helpers live in `fpi-usb-transfer.*` and `fpi-spi-transfer.*`; image processing and assembling helpers live in `fpi-image.*` and `fpi-assembling.*`.

### Goodix TLS drivers

The Goodix TLS implementation has a shared abstract device in `libfprint/drivers/goodixtls/goodix.c`, protocol framing in `goodix_proto.c`, TLS helper/server code in `goodixtls.c`, and product-family drivers in `goodix511.c`, `goodix52xd.c`, and `goodix53xd.c`. The shared implementation defines `FpiDeviceGoodixTls` (`libfprint/drivers/goodixtls/goodix.c:60`) and TLS handshake state handling (`libfprint/drivers/goodixtls/goodix.c:1088`, `libfprint/drivers/goodixtls/goodix.c:1130`, `libfprint/drivers/goodixtls/goodix.c:1208`). Product drivers define activation states, OTP/PSK handling, frame decoding, scan state machines, and image-device callbacks (`libfprint/drivers/goodixtls/goodix52xd.c:75`, `libfprint/drivers/goodixtls/goodix52xd.c:223`, `libfprint/drivers/goodixtls/goodix52xd.c:375`, `libfprint/drivers/goodixtls/goodix52xd.c:617`, `libfprint/drivers/goodixtls/goodix52xd.c:701`).

### Tests, examples, and data

Driver tests are listed in `tests/meson.build:24`, virtual device tests in `tests/meson.build:55`, and unit tests in `tests/meson.build:146`. `umockdev-test.py` wraps recorded USB/SPI captures and skips when `umockdev` is unavailable (`tests/umockdev-test.py:14`, `tests/umockdev-test.py:59`). Test creation is documented in `tests/README.md:1`, including use of `tests/create-driver-test.py` from the build directory (`tests/README.md:24`). Examples build simple enrollment, verification, capture, storage, and identification programs (`examples/meson.build:11`). Udev hwdb/rules and supported-device generators are built from `libfprint/meson.build:310`, `libfprint/meson.build:325`, and `libfprint/meson.build:352`.

## Code References

- `meson.build:1` - Project metadata, language selection, version, and default options.
- `meson.build:126` - Default driver list including Goodix TLS families.
- `libfprint/meson.build:142-158` - Mapping from Goodix TLS driver names to source files.
- `libfprint/fp-device.c:36` - Abstract `FpDevice` GObject definition.
- `libfprint/fpi-ssm.c:73` - Internal sequential state-machine structure.
- `libfprint/drivers/goodixtls/goodix.c:1088` - TLS handshake state enum.
- `libfprint/drivers/goodixtls/goodix52xd.c:757` - Goodix 52xd class initialization.
- `tests/meson.build:24` - Recorded driver test list.

## Architecture Documentation

The project is layered around a public GObject API, an internal driver API, and driver-specific implementations. Applications use `FpContext` to discover `FpDevice` instances and invoke async operations. Drivers subclass `FpDevice` or `FpImageDevice`, use `FpiSsm` for ordered protocol sequences, and report images or operation completion back through internal helpers. Meson composes selected drivers into a static driver library and links it into the shared `libfprint-2` library. Test fixtures replay device interactions through `umockdev`, while utility executables generate udev metadata from built-in device tables.

## Open Questions

- `source.yaml` was not present, so no definitive manifest of intended research files was available.
- This report documents the current checkout and does not distinguish fork-specific Goodix changes from upstream libfprint history.
