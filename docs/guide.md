---
title: Setup guide
lead: Everything from wiring to troubleshooting. If you just want to get going, the quick start on the front page is enough. Come back here when you need the details.
---

**On this page**

- [Wiring](#wiring)
- [Building and flashing](#building-and-flashing)
- [Arduino IDE settings](#arduino-ide-settings)
- [First setup](#first-setup)
- [The web interface](#the-web-interface)
- [Settings reference](#settings-reference)
- [Upload services](#upload-services)
- [JSON data](#json-data)
- [When WiFi goes away](#when-wifi-goes-away)
- [LED and BOOT button](#led-and-boot-button)
- [Updating the firmware](#updating-the-firmware)
- [Troubleshooting](#troubleshooting)
- [Security notes](#security-notes)
- [Known limitations](#known-limitations)

## Wiring

The ESP32-S3-DevKitC-1 has two USB connectors. The one marked UART goes through a USB-to-serial chip: that's the one for power, flashing and the serial log. The one marked USB is wired straight to GPIO19 and GPIO20 of the chip, and this firmware uses those two pins as a USB host for the logger.

The catch is that the board doesn't put 5 V on the VBUS pin of that connector, so it can't power anything plugged into it. A normal OTG adapter isn't enough and the logger would never show up. The simplest fix is a USB-A female breakout wired to the header pins:

| Breakout pin | Usual wire colour | DevKitC-1 pin |
|---|---|---|
| VBUS | red | 5V |
| D- | white | GPIO19 |
| D+ | green | GPIO20 |
| GND | black | GND |

Plug the logger's cable into the breakout, then power the board from its UART connector or from a 5 V supply on the 5V pin. Leave the board's own USB connector empty, since it shares GPIO19 and GPIO20 with the breakout.

Keep the console on its AC adapter. With the logger fitted and being read every few seconds, the batteries run down quickly.

## Building and flashing

1. Install the Arduino IDE, version 2.x.
2. In File > Preferences, add this address to "Additional boards manager URLs":
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Open the Boards Manager, search for "esp32" and install "esp32 by Espressif Systems", version 3.x.
4. Put `DavisVP2_Gateway.ino` in a folder called `DavisVP2_Gateway` and open it.
5. Set the board options from the table below.
6. Connect the board through its UART connector, select the port and press Upload. If the upload doesn't start, hold BOOT, tap RST, let go of BOOT and try again.
7. Open the Serial Monitor at 115200 baud to watch the log.

If you have the v1.1 revision of the board, change `RGB_LED_PIN` near the top of the sketch from 48 to 38, otherwise the status LED stays dark. Everything else works the same on both revisions.

## Arduino IDE settings

| Option | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Disabled |
| Upload Mode | UART0 / Hardware CDC |
| Flash Size | 16MB (128Mb) |
| PSRAM | OPI PSRAM |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |

The two USB options are the ones that matter. The native USB port has to stay free for the logger, and the log has to go out through the UART connector. If either one is wrong, the build stops with a message telling you which option to change.

Any partition scheme with two app slots will do. You need two slots to update the firmware over the network.

## First setup

When no WiFi network is saved, the board starts its own network called `DavisVP2-XXXXXX`, where the last six characters come from the board's MAC address. The password is `davisvp2`.

Join it from a phone or laptop. Most devices open the setup page automatically, the same way a hotel WiFi login page appears. If yours doesn't, go to `http://192.168.4.1/config`. Nothing asks for a login on this network, because the network itself is already protected by its password.

Press Scan to list the networks nearby, pick yours, type its password and fill in anything else you need. The upload services can wait until later. Press "Save and reboot".

The board restarts and joins your network. From then on you can reach it at `http://davis-vp2.local/`. If your phone doesn't understand `.local` names (some Android phones don't), use the IP address shown in your router's list of connected devices. Log in as `admin` with the password `davisvp2`, then change that password and the setup network password on the settings page.

## The web interface

| Address | What you get | Login |
|---|---|---|
| `/` | Status page with current readings, the state of the console and logger, network details and upload counters. Refreshes every 10 seconds. | yes |
| `/config` | All the settings | yes |
| `/update` | Firmware upload | yes |
| `/api/data` | The latest reading as JSON | no |
| `/api/status` | A short health summary as JSON | yes |

Pages opened through the setup network never ask for a login.

## Settings reference

Password fields show dots when a value is saved. Leave the field alone to keep the saved value, or empty it to remove the value, for example for an open WiFi network. The setup network password has to be 8 to 63 characters long and the admin password at least 4. Shorter values are ignored and the old ones stay.

Changes take effect after the automatic restart that follows Save.

### WiFi

| Setting | Default | Notes |
|---|---|---|
| SSID and password | empty | Your home network. |
| Hostname | davis-vp2 | Used for the `.local` address and shown in your router's client list. |
| Static IP, gateway, netmask, DNS | off | Leave it off to get an address from the router. If DNS is empty, the gateway is used. |
| Failed attempts before opening the setup network | 10 | Each attempt lasts 15 seconds. |
| Retry interval while the setup network is open | 60 s | See [When WiFi goes away](#when-wifi-goes-away). |
| Setup network password | davisvp2 | |
| Admin password | davisvp2 | The user name is always `admin`. |

### Davis console

| Setting | Default | Notes |
|---|---|---|
| Console baud rate | 19200 | This is the console's own default. Only change it if you changed it on the console. |
| Rain collector | Auto | Read from the console. Set it by hand only if the detected value is wrong. |
| Poll interval | 5 s | The minimum is 3 s. The outdoor transmitter sends new data about every 2.5 seconds, so polling faster gains nothing. |
| LOOP packet every N polls | 12 | This second packet carries battery status, the month and year rain totals and the optional stations. |
| Set console clock from NTP | on | Shortly after start-up, then once a day. |
| Time zone | CET-1CEST,M3.5.0,M10.5.0/3 | POSIX format. This one is right for Italy and most of central Europe. |
| NTP server | pool.ntp.org | |

### Derived values

The console works out wind chill, heat index and dew point by itself, but it reports them in whole degrees Fahrenheit, which means steps of about 0.6 °C. The board can calculate them instead from the temperature, which the console gives to a tenth of a degree. Whatever you choose here, a value the console doesn't report (for example on an old console that only sends the basic packet) is always calculated.

| Setting | Default | Choices |
|---|---|---|
| Wind chill | From the console | From the console, or calculated with the current wind, the 2 minute average or the 10 minute average |
| Heat index | From the console | From the console, or calculated with the US National Weather Service formula |
| Dew point | From the console | From the console, or calculated with the Magnus formula |

The calculated wind chill uses the 2001 formula adopted by the US and Canadian weather services. It only applies below 10 °C and above 4.8 km/h of wind; outside that range wind chill equals the air temperature. The current wind is what the formula was designed for, while the averages give a calmer value that jumps around less.

### Optional sensors

A Vantage Pro2 can be fitted with extra sensors, and many stations don't have all of them. Untick the ones yours doesn't have: an unticked sensor is never shown on the status page and never uploaded, even if the console reports a stray value for it.

| Setting | Default | Notes |
|---|---|---|
| Solar radiation sensor | on | The THSW index and evapotranspiration are worked out from solar radiation, so they disappear too when this is off. |
| UV sensor | on | |
| Extra temperature / humidity stations | off | Up to 7 extra stations. Channels with no station attached are skipped. |
| Soil / leaf station | off | Up to 4 soil temperatures, 4 soil moistures, 4 leaf temperatures and 2 leaf wetness sensors. |

Solar and UV are on by default because a sensor that isn't there simply reports nothing, so leaving them on does no harm. The extra and soil/leaf stations are read from the second packet, so they update at the "LOOP packet every N polls" rate, once a minute with the default settings.

### Upload intervals

| Service | Default | Notes |
|---|---|---|
| Weather Underground | 60 s | |
| Windy | 300 s | Windy takes one reading every 5 minutes, so shorter intervals aren't allowed. |
| MQTT | 10 s | |
| HTTP POST | 60 s | |

The other fields for each service are explained in the next section.

## Upload services

Nothing is uploaded until the console has answered at least once, and a reading older than three poll intervals plus 10 seconds is never sent. If the console goes quiet, the uploads simply wait for it.

### Weather Underground

Register your station on wunderground.com and copy the station ID and key from the My Devices page. Tick Enabled, paste both values and save.

The board sends temperature, humidity, dew point, wind speed and direction, the gust, the 2 minute average wind, barometer, rain in the last hour and today, solar radiation, UV, and the inside temperature and humidity. Values are converted to the imperial units Weather Underground expects. Sensors you unticked under [Optional sensors](#optional-sensors) are left out.

Other services that accept the Weather Underground upload format can use this same section. Replace the upload URL with the one they give you and use their station ID and key.

### Windy

Create a station at stations.windy.com, then copy its station ID and station password into the Windy section. The board uses Windy's v2 upload API and sends temperature, dew point, humidity, wind, gust and direction, sea-level pressure, rain in the last hour, UV and solar radiation.

### MQTT

Fill in the broker address, port, user name and password. You can also put a full address in the host field, for example `mqtt://192.168.1.10:1883`. TLS is not supported.

The board publishes two topics under the base topic, which is `davis/vp2` by default:

| Topic | Content |
|---|---|
| `davis/vp2/state` | The latest reading as JSON (see [JSON data](#json-data)), retained. |
| `davis/vp2/status` | `online` while the board is connected and `offline` when it drops off, retained. |

### HTTP POST

Any web address that accepts a JSON POST. The body is the same JSON that `/api/data` returns. If you fill in a token, it's sent as `Authorization: Bearer <token>`. Any 2xx response counts as success, and both `http://` and `https://` addresses work.

## JSON data

`/api/data`, the MQTT state topic and the HTTP POST all use the same object. Values the console doesn't have, such as solar radiation on a station without the sensor, come through as `null`.

| Field | Unit | Meaning |
|---|---|---|
| `valid` | | `true` once at least one packet has been received |
| `ts` | s | Unix time of the reading, 0 if the clock hasn't been set yet |
| `age_s` | s | Seconds since the reading was taken |
| `temp_out_c`, `temp_in_c` | °C | Outside and inside temperature |
| `hum_out`, `hum_in` | % | Outside and inside humidity |
| `dewpoint_c` | °C | Dew point |
| `heat_index_c`, `wind_chill_c`, `thsw_c` | °C | Heat index, wind chill and THSW index |
| `wind_ms` | m/s | Current wind speed |
| `wind_dir` | ° | Current wind direction |
| `wind_avg2_ms`, `wind_avg10_ms` | m/s | 2 and 10 minute average wind speed |
| `gust10_ms`, `gust10_dir` | m/s, ° | Strongest gust of the last 10 minutes and its direction |
| `baro_hpa` | hPa | Barometer, reduced to sea level |
| `press_abs_hpa` | hPa | Station pressure |
| `altimeter_hpa` | hPa | Altimeter setting |
| `bar_trend` | | Davis trend code: -60 falling fast, -20 falling slowly, 0 steady, 20 rising slowly, 60 rising fast |
| `rain_rate_mmh` | mm/h | Rain rate |
| `rain_day_mm`, `rain_15m_mm`, `rain_1h_mm`, `rain_24h_mm` | mm | Rain today and over the last 15 minutes, hour and 24 hours |
| `rain_storm_mm`, `rain_month_mm`, `rain_year_mm` | mm | Storm, month and year totals |
| `et_day_mm` | mm | Evapotranspiration today |
| `uv` | index | UV index |
| `solar_wm2` | W/m² | Solar radiation |
| `console_batt_v` | V | Console battery voltage |
| `tx_batt_status` | | 0 when all transmitter batteries are fine, otherwise one bit per transmitter with a low battery |
| `loop2` | | `true` when the console supports the newer LOOP2 packet |

The optional stations add fields only for the channels that actually have a sensor, and only when the station is ticked in the settings. The number in the name is the channel, as set on the station.

| Field | Unit | Meaning |
|---|---|---|
| `extra_temp1_c` to `extra_temp7_c` | °C | Extra temperature stations |
| `extra_hum1` to `extra_hum7` | % | Extra humidity |
| `soil_temp1_c` to `soil_temp4_c` | °C | Soil temperature |
| `soil_moist1_cb` to `soil_moist4_cb` | cb | Soil moisture in centibar (0 is wet, higher is drier) |
| `leaf_temp1_c` to `leaf_temp4_c` | °C | Leaf temperature |
| `leaf_wet1`, `leaf_wet2` | 0 to 15 | Leaf wetness, 0 is dry and 15 is soaking wet |

A shortened example:

```json
{
  "valid": true,
  "ts": 1794925200,
  "age_s": 2,
  "temp_out_c": 12.4,
  "hum_out": 81,
  "wind_ms": 3.1,
  "wind_dir": 225,
  "gust10_ms": 6.7,
  "baro_hpa": 1016.2,
  "rain_day_mm": 1.2
}
```

## When WiFi goes away

When the connection drops, the board starts reconnecting straight away. It makes up to 10 attempts of 15 seconds each, so about two and a half minutes. A router that restarts in a minute or two won't trigger anything else. If none of the attempts work, the board opens its setup network so you can still reach it and check the settings.

While the setup network is open, the board keeps trying your network every 60 seconds, but only when nobody is connected to the setup network, because each search briefly moves the radio off the setup network's channel. As soon as your router is back, the board reconnects, waits until the setup network has been idle for 30 seconds, closes it and carries on as usual.

The console keeps being read the whole time, and uploads resume as soon as the board is back online.

If you opened the setup network on purpose with the BOOT button, it stays open for 5 minutes after the last device disconnects from it.

## LED and BOOT button

| LED | Meaning |
|---|---|
| Green | On your network, and the console is answering |
| Blue, blinking | The setup network is open |
| Orange, blinking | Connecting to your WiFi |
| Magenta | The USB logger isn't detected |
| Red | The logger is there but the console isn't answering |

The BOOT button sits next to the USB connectors. The action happens when you let go:

| Hold BOOT for | What happens |
|---|---|
| 3 seconds | The setup network opens. The LED turns cyan once you've held it long enough. |
| 10 seconds | All settings are erased and the board restarts. The LED turns red once you've held it long enough. |

Don't hold BOOT while you power the board up, because that puts the chip into flashing mode instead.

## Updating the firmware

In the Arduino IDE, use Sketch > Export Compiled Binary. Of the files it creates, take `DavisVP2_Gateway.ino.bin`, not the ones with "merged" or "bootloader" in their names. Open `/update` on the board, choose the file and press Upload. The board restarts with the new firmware and keeps all its settings.

## Troubleshooting

### The status page says "USB logger not detected"

- Measure the voltage on the breakout's VBUS pin. Without 5 V there, the logger never appears.
- Check that D+ and D- aren't swapped: GPIO20 is D+ and GPIO19 is D-.
- Make sure nothing is plugged into the board's own USB connector.
- Look at the serial log. When the logger is found it prints a line starting with `[USB] device 10C4:`. If a device appears with a different first number, the firmware ignores it. Please open an issue and include that line.

### The console isn't answering

- Make sure the logger is pushed fully into its socket in the console.
- The console must not be in setup mode. Press and hold DONE until the normal screen comes back.
- The baud rate in the settings has to match the console, which is 19200 unless you changed it.
- An occasional timeout in the log is normal. A steady stream of them isn't.

### The board never joins my WiFi

- The ESP32 only works on 2.4 GHz. A router that uses one name for both 2.4 and 5 GHz usually works fine, but a network that's 5 GHz only won't.
- For a hidden network, type the name by hand instead of using Scan.
- The serial log shows a reason code every time the connection fails. 15 or 202 usually means a wrong password, and 201 means the network wasn't found.

### The rain figures look wrong

If the rain totals are off by a constant factor, the collector size is wrong. The status page shows which size is in use and whether it was detected or set by hand. Pick the right one in the settings: 0.2 mm for the metric collector sold in Europe, 0.01 in for the US version.

### I can't open davis-vp2.local

Use the IP address from your router's client list, or give the board a static IP. Some Android phones and some office networks don't resolve `.local` names.

### Windy or Weather Underground show errors

The status page shows the last HTTP code for each service. 401 or 403 almost always means the station ID or the key is wrong. The serial log also shows the beginning of the server's reply.

### I'm locked out

If you only forgot the admin password, hold BOOT for 3 seconds and change it through the setup network, which doesn't ask for a login. To start completely fresh, hold BOOT for 10 seconds instead.

## Security notes

- Anyone who joins the setup network can change the settings without logging in, so give it a proper password.
- On your home network the pages use HTTP basic authentication, which isn't encrypted. That's fine on a home network, but don't expose the board to the internet.
- `/api/data` can be read by anyone on your network.
- Uploads over HTTPS are encrypted, but the board doesn't check server certificates.
- Passwords are stored in plain text in the board's flash memory.

## Known limitations

- Readings taken while the internet is down aren't sent afterwards. The logger keeps its own archive, but this version doesn't read it back yet.
- MQTT works without TLS only.
- Readings from the extra and soil/leaf stations go to the JSON feed, MQTT and HTTP POST, but not to Weather Underground or Windy.
- The firmware is written for the ESP32-S3-DevKitC-1. Other S3 boards may need different pins for the LED and the button.

[Back to the front page](index.md)
