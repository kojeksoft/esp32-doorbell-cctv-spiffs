# ESP32 Doorbell CCTV - SPIFFS Version

ESP32-based smart doorbell that captures a JPEG snapshot from an IP CCTV camera when the doorbell is pressed, temporarily stores the image in SPIFFS, and uploads it to a remote Photo Server.

This repository contains the **SPIFFS-based version** of the project.

## Architecture

```text
Doorbell
   │
   ▼
ESP32
   │
   ▼
IP CCTV Camera
   │
   │ JPEG Snapshot
   ▼
ESP32 SPIFFS
   │
   │ HTTPS / multipart
   ▼
Photo Server
   │
   ▼
WhatsApp Server
   │
   ▼
WhatsApp
```

## How It Works

1. The doorbell triggers the ESP32.
2. ESP32 requests a JPEG snapshot from the IP CCTV camera.
3. The camera is accessed using HTTP Digest Authentication.
4. The snapshot is temporarily stored in SPIFFS.
5. ESP32 uploads the image to the Photo Server using HTTPS `multipart/form-data`.
6. After a successful upload, the local image is deleted.
7. SPIFFS is cleared on ESP32 startup to prevent old temporary images from accumulating.

## Hardware

- ESP32-WROOM-32 / DOIT ESP32 DevKit V1
- Doorbell / push button
- IP CCTV camera
- Wi-Fi network

## Software

- PlatformIO
- Arduino Framework
- WiFi
- HTTPClient
- WiFiClientSecure
- SPIFFS
- mbedTLS MD5

## Project Structure

```text
esp32-doorbell-cctv-spiffs/
├── include/
│   ├── secrets.h          # Local configuration - DO NOT COMMIT
│   └── secrets.example.h
├── src/
│   └── main.cpp
├── platformio.ini
├── .gitignore
└── README.md
```

## Configuration

Copy:

```text
include/secrets.example.h
```

to:

```text
include/secrets.h
```

Configure your local settings:

```cpp
#pragma once

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define CAMERA_HOST "YOUR_CAMERA_IP"
#define CAMERA_USER "YOUR_CAMERA_USER"
#define CAMERA_PASSWORD "YOUR_CAMERA_PASSWORD"

#define PHOTO_SERVER_HOST "YOUR_PHOTO_SERVER"
#define PHOTO_SERVER_PORT 443
#define PHOTO_SERVER_PATH "/YOUR_UPLOAD_PATH"
```

`include/secrets.h` is ignored by Git and should never be committed because it contains private credentials and server configuration.

## SPIFFS

SPIFFS is used only as temporary storage.

```text
CCTV
  ↓
ESP32 RAM Buffer
  ↓
SPIFFS
  ↓
ESP32 RAM Buffer
  ↓
Photo Server
```

After a successful upload, the temporary JPEG is deleted.

SPIFFS is also cleared whenever the ESP32 boots to prevent failed or leftover snapshots from filling the filesystem.

## Current Features

- Doorbell GPIO trigger
- Wi-Fi connectivity
- CCTV snapshot capture
- HTTP Digest Authentication
- JPEG temporary storage using SPIFFS
- HTTPS upload
- Multipart/form-data upload
- Automatic temporary file cleanup
- Timestamp-based JPEG filenames

## Security

Credentials and private network/server information are stored separately in:

```text
include/secrets.h
```

This file must not be committed to the repository.

Use `secrets.example.h` as the configuration template.

## Future Development

The next version will remove SPIFFS from the image transfer path and stream the JPEG directly from the CCTV camera to the server.

```text
CCTV
  ↓
ESP32 RAM Buffer
  ↓
Photo Server
```

This will reduce flash writes and remove the need to store the complete JPEG locally before uploading.

## Status

**v1 - SPIFFS Version: Working ✅**
