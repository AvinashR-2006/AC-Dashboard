# AC Control Dashboard

This is a new ESP32 project for AC monitoring and control, built from the existing local server pattern but kept in a separate folder.

## What it does

- Hosts a light-themed mobile dashboard from the ESP32 using `WebServer` + `LittleFS`
- Opens directly to the dashboard when a QR code points to the device URL
- Keeps Wi-Fi and HiveMQ/MQTT setup embedded in the dashboard instead of using a separate login page
- Publishes live status to MQTT so the AC can be viewed and controlled through internet access
- Redirects to a hosted GitHub Pages dashboard after first-time setup
- Reads 4 thermostat probes from DS18B20 sensors on one OneWire bus
- Reads 1 ambient temperature and humidity value from a DHT22
- Shows two readings at a time on a `20x4` I2C LCD and rotates every 30 seconds
- Controls fan motor speed with ESP32 PWM
- Controls main AC power and compressor with two relay outputs
- Uses an edge-triggered timer for exact ON/OFF minute events

## MQTT topics

Use the configured base topic, for example `home/ac1`.

- `home/ac1/status`: retained JSON status from ESP32
- `home/ac1/availability`: retained `online` / `offline`
- `home/ac1/status/get`: publish anything here to request fresh status
- `home/ac1/control`: publish `{"power_on":true,"compressor_on":false}`
- `home/ac1/fan/set`: publish `{"fan_speed_percent":55}`
- `home/ac1/timer/set`: publish `{"on_time":"08:00","off_time":"18:00"}`

## GitHub Pages flow

1. Host `remote-dashboard.html` on GitHub Pages.
2. Print the product QR code with the ESP32 local URL, usually `http://192.168.4.1/`.
3. On first boot, connect the phone to `AC_Controller_Setup`.
4. Open the QR URL and enter:
   - Wi-Fi SSID and password
   - HiveMQ MQTT host, port, username, password, and base topic
   - GitHub Pages dashboard URL
5. Press `Save Wi-Fi`, then `Connect Device`.
6. ESP32 joins Wi-Fi, connects to HiveMQ, and redirects the phone to the GitHub Pages dashboard.
7. The GitHub Pages dashboard controls the ESP32 through HiveMQ WebSockets.

## Default pin mapping

- `GPIO4`  : OneWire bus for 4 DS18B20 probes
- `GPIO16` : DHT22 data
- `GPIO25` : PWM fan output
- `GPIO26` : Main power relay
- `GPIO27` : Compressor relay
- `I2C`    : LCD `20x4` at address `0x27`

## Notes

- The firmware assumes all 4 thermostat sensors are DS18B20 devices connected to the same OneWire bus.
- Compressor output is forced OFF if main power is OFF.
- ESP32 MQTT uses TLS port `8883` with `setInsecure()` for HiveMQ-style username/password auth.
- `remote-dashboard.html` connects to HiveMQ over WebSockets, typically `wss://your-cluster:8884/mqtt`.
- Because GitHub Pages is static, MQTT WebSocket credentials are entered in the browser and saved in local storage. A backend would be more secure for a commercial deployment.
- Setup AP defaults:
  - SSID: `AC_Controller_Setup`
  - Password: `12345678`
