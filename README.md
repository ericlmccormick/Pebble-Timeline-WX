# Timeline WX — hourly weather for Pebble

**Current source version:** 2.5.0. **Platform in the current manifest:** Emery (Pebble Time 2).

Timeline WX displays a scrollable **48-hour** hourly forecast with weather conditions, temperatures, precipitation, wind and optional gust information. It supports day/night appearance and watch/phone configuration.

## Weather source migration

**New installs default to the [Weather+ plugin](https://github.com/ericlmccormick/pebble-weather-plus).** Install and configure Weather+ in the Pebble phone app for its shared provider, location and alert settings. If Weather+ is missing, Timeline WX indicates the dependency and can use a built-in backup weather provider.

Existing users can retain built-in weather settings; the app recommends moving to Weather+ for ongoing features. Built-in providers in the phone settings include automatic, Open-Meteo, NWS, MET Norway and OpenWeather OneCall (API key required). Provider availability and fields vary by location.

## Settings

Phone settings include weather source, units (temperature F/C/K/R; wind mph/km/h/m/s/knots), 12/24-hour time, auto/day/night appearance, gusts, notification level, location (phone GPS or manually entered city/ZIP), and refresh interval. Long-press **Select** for the on-watch settings menu. Weather+ maintains its own provider and measurement-unit configuration.

## Development and support

Use the [direct PBW build workflow](.github/workflows/direct-pbw.yml) or a compatible Pebble SDK. `package.json` defines the current source version, permissions and build platform. Report issues with the selected weather source, version and watch model; remove API keys from logs.

Support development: https://ko-fi.com/ericmccormick
