# Licensing

All of AmbiBottom - the WLED usermod (`firmware/wled-usermod/`), the shared core
(`shared/ambi_core/`), the Linux bridge (`src/`, `include/`), the tools, tests and
documentation - is licensed under the **European Union Public Licence v. 1.2**
(EUPL-1.2, [LICENSE](../LICENSE)).

## Why EUPL-1.2

The firmware is WLED with this usermod compiled in, and the usermod uses WLED's
internals, so the firmware is a derivative work of WLED, which is "EUPL v. 1.2 or
later". Using the same licence for the whole repository keeps it simple: one licence,
compatible with WLED in every direction.

If you distribute a built firmware (`.bin`), the EUPL asks you to make the source
available too: WLED's source at the version you built, plus this repository.

## Third-party code

Nothing third-party is stored in this repository. What gets linked:

| Library | Licence | Where |
|---|---|---|
| WLED and its dependencies | EUPL-1.2 and others | the firmware build; fetched by PlatformIO |
| Mbed TLS | Apache-2.0 (or GPL-2.0-or-later) | Linux: system library; ESP32: part of ESP-IDF |
| cJSON | MIT | Linux: system library; ESP32: part of ESP-IDF |

Protocol knowledge (not code) came from public projects: diyHue (Apache-2.0),
relume-tv (MIT), ha-hue-entertainment (MIT).

## What is deliberately not in the repository

- No code or files from Philips / TP Vision / Signify.
- No pairing data, keys, passwords or logs: they live on the device (NVS) or in the
  Linux bridge's state directory, and `.gitignore` plus `tools/check_publish.sh` keep
  them out.

## Trademarks

"Philips", "Ambilight" and "Hue" are trademarks of their owners. They are used only to
describe what AmbiBottom works with. AmbiBottom is not affiliated with or endorsed by
them.
