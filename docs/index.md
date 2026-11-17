---
layout: home
title: Davis VP2 WiFi Gateway

# Everything on the front page is edited here. The design lives in _layouts/home.html.

eyebrow: ESP32-S3 firmware for the Davis Vantage Pro2
headline: Your Vantage Pro2 online, without a PC
lead: A small ESP32-S3 board plugs into the WeatherLink USB logger, reads the console every few seconds and sends the data over WiFi to Weather Underground, Windy, MQTT or your own server.
pills:
  - Arduino core 3.x
  - No extra libraries
  - Set up from your phone

reading:
  host: davis-vp2.local
  temp: "12.4"
  sub: Dew point 9.2 °C
  tiles:
    - { label: Humidity,    value: "81",     unit: "%" }
    - { label: Wind,        value: "3.1",    unit: "m/s SW" }
    - { label: Gust 10 min, value: "6.7",    unit: "m/s" }
    - { label: Barometer,   value: "1016.2", unit: "hPa" }
    - { label: Rain today,  value: "1.2",    unit: "mm" }
    - { label: Solar,       value: "214",    unit: "W/m²" }

features_title: What it does
features_lead: The board takes the place of the PC the logger was designed for, and adds a few things the PC software never did.
features:
  - icon: live
    title: Live data every 5 seconds
    text: Temperature, humidity, wind with its 2 and 10 minute averages and the gust, pressure, rain, solar and UV, straight from the console.
  - icon: upload
    title: Four places to send it
    text: Weather Underground, Windy, an MQTT broker and any web address that takes JSON. Each one has its own interval and its own switch.
  - icon: home
    title: Fits into your home setup
    text: A JSON feed and retained MQTT topics make the data easy to use in Home Assistant, Node-RED or a script of your own.
  - icon: sensors
    title: Optional sensors
    text: Solar, UV, up to seven extra temperature and humidity stations and the soil and leaf station. Tick the ones you have.
  - icon: thermo
    title: Tenths of a degree
    text: Wind chill, heat index and dew point can be worked out on the board instead of taking the console's whole degrees Fahrenheit.
  - icon: wifi
    title: Looks after its own WiFi
    text: It reconnects by itself after an outage. If it can't, it opens a setup network so you can always reach it.
  - icon: clock
    title: Console clock kept right
    text: The console time is set from the internet once a day, so the daily rain total resets at the right midnight.
  - icon: update
    title: Updates from the browser
    text: After the first flash over USB, new firmware goes in through the web page, and your settings stay as they are.

how_lead: The logger was always meant to talk to a computer. The ESP32-S3 simply pretends to be one.
how:
  - title: The logger becomes a USB device
    text: The WeatherLink logger turns the console's data port into a USB serial device, exactly what a PC would see.
  - title: The board reads it as a USB host
    text: On every poll the ESP32-S3 wakes the console, asks for a data packet and checks it before using it.
  - title: Each service gets its own schedule
    text: Values are converted to metric and sent where you asked. If the console goes quiet, nothing old is sent.

parts_lead: Nothing on the console side changes. The rest is a handful of parts and an evening of work.
parts:
  - name: Vantage Pro2 console
    note: With the WeatherLink USB data logger fitted. A Vantage Vue should work too, but hasn't been tried.
  - name: ESP32-S3-DevKitC-1
    note: Written for the N16R8 version. Other S3 boards with a native USB port can work with small changes.
  - name: USB-A female breakout
    note: The board doesn't power its own USB port, so the logger gets its 5 V through this.
  - name: 5 V USB power supply
    note: 1 A is plenty.
  - name: Jumper wires
    note: Four of them, for VBUS, D-, D+ and ground.
  - name: Console AC adapter
    note: With the logger being read all day, batteries alone don't last.

start_lead: From a bare board to data on the internet in six steps. The setup guide has the details for each one.
steps:
  - title: Wire it
    text: Connect the breakout to the board. VBUS to `5V`, D- to `GPIO19`, D+ to `GPIO20`, ground to `GND`.
  - title: Flash it
    text: Upload the sketch from the Arduino IDE through the UART connector, with the board settings listed in the guide.
  - title: Power it
    text: Plug the logger into the breakout and power the board. The LED blinks blue while the setup network is open.
  - title: Join the setup network
    text: On your phone, join `DavisVP2-XXXXXX` with the password `davisvp2`. The setup page opens by itself.
  - title: Fill in the settings
    text: Pick your WiFi, switch on the services you want and save. The board restarts and joins your network.
  - title: Check it
    text: Open `davis-vp2.local`, log in as `admin` and change the passwords. A green LED means data is flowing.

cta_title: Ready to build one?
cta_text: The setup guide covers wiring, flashing, every setting and what to do when something doesn't work.
---

## Project status

Version 1.1.0 adds support for the optional sensors and lets you choose where wind chill, heat index and dew point come from. Bug reports and pull requests are welcome. If something doesn't work, the serial log at 115200 baud usually shows what went wrong, so please attach it to your report.

## Credits

The packet decoding follows the Davis serial communication reference manual for the Vantage consoles, and was cross-checked against the Vantage driver in WeeWX.
