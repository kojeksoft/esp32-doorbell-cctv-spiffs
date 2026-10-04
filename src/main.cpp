#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <mbedtls/md5.h>

#include "secrets.h"

#define BELL_PIN 34

static const size_t IO_BUFFER_SIZE = 1024;
const char *host = PHOTO_SERVER_HOST;
const uint16_t port = PHOTO_SERVER_PORT;
const char *path = PHOTO_SERVER_PATH;

// -----------------------------------------------------------------------------
// Utility: SPIFFS status
// -----------------------------------------------------------------------------
void printSpiffsStatus()
{
    size_t total = SPIFFS.totalBytes();
    size_t used = SPIFFS.usedBytes();
    size_t freeBytes = total >= used ? total - used : 0;

    Serial.print("SPIFFS total : ");
    Serial.print(total);
    Serial.println(" bytes");

    Serial.print("SPIFFS used  : ");
    Serial.print(used);
    Serial.println(" bytes");

    Serial.print("SPIFFS free  : ");
    Serial.print(freeBytes);
    Serial.println(" bytes");
}

// -----------------------------------------------------------------------------
// Digest Authentication helpers
// -----------------------------------------------------------------------------
String md5Hex(const String &input)
{
    unsigned char digest[16];

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);

    mbedtls_md5_starts_ret(&ctx);
    mbedtls_md5_update_ret(
        &ctx,
        (const unsigned char *)input.c_str(),
        input.length()
    );
    mbedtls_md5_finish_ret(&ctx, digest);
    mbedtls_md5_free(&ctx);

    char output[33];

    for (int i = 0; i < 16; i++)
    {
        sprintf(&output[i * 2], "%02x", digest[i]);
    }

    output[32] = '\0';
    return String(output);
}

String getDigestValue(
    const String &header,
    const String &key
)
{
    String search = key + "=\"";
    int start = header.indexOf(search);

    if (start < 0)
    {
        return "";
    }

    start += search.length();

    int end = header.indexOf("\"", start);

    if (end < 0)
    {
        return "";
    }

    return header.substring(start, end);
}

String createDigestAuthorization(
    const String &username,
    const String &password,
    const String &method,
    const String &uri,
    const String &challenge
)
{
    String realm = getDigestValue(challenge, "realm");
    String nonce = getDigestValue(challenge, "nonce");
    String qop = "auth";
    String nc = "00000001";
    String cnonce = String((uint32_t)esp_random(), HEX);

    String ha1 = md5Hex(
        username + ":" +
        realm + ":" +
        password
    );

    String ha2 = md5Hex(
        method + ":" + uri
    );

    String response = md5Hex(
        ha1 + ":" +
        nonce + ":" +
        nc + ":" +
        cnonce + ":" +
        qop + ":" +
        ha2
    );

    String authorization =
        "Digest username=\"" + username + "\", " +
        "realm=\"" + realm + "\", " +
        "nonce=\"" + nonce + "\", " +
        "uri=\"" + uri + "\", " +
        "algorithm=MD5, " +
        "response=\"" + response + "\", " +
        "qop=" + qop + ", " +
        "nc=" + nc + ", " +
        "cnonce=\"" + cnonce + "\"";

    return authorization;
}

// -----------------------------------------------------------------------------
// Camera -> SPIFFS
// -----------------------------------------------------------------------------
bool downloadCameraImage(const String &localPath)
{
    const String uri = "/cgi-bin/snapshot.cgi?channel=1";
    const String url = String("http://") + CAMERA_HOST + uri;

    Serial.println();
    Serial.println("Mengambil gambar dari kamera...");
    Serial.println(url);

    // Request pertama tanpa Authorization untuk memperoleh Digest challenge.
    WiFiClient client1;
    HTTPClient http1;

    if (!http1.begin(client1, url))
    {
        Serial.println("HTTP begin gagal");
        return false;
    }

    const char *headerKeys[] = {
        "WWW-Authenticate"
    };

    http1.collectHeaders(headerKeys, 1);

    int firstCode = http1.GET();

    Serial.print("First HTTP status: ");
    Serial.println(firstCode);

    if (firstCode != HTTP_CODE_UNAUTHORIZED)
    {
        Serial.println("Kamera tidak memberikan Digest challenge");
        http1.end();
        return false;
    }

    String challenge = http1.header("WWW-Authenticate");

    Serial.print("Digest challenge: ");
    Serial.println(challenge);

    http1.end();

    // Buat header Digest Authorization.
    String authorization = createDigestAuthorization(
        CAMERA_USER,
        CAMERA_PASSWORD,
        "GET",
        uri,
        challenge
    );

    // Request kedua menggunakan Digest Authorization.
    WiFiClient client2;
    HTTPClient http2;

    if (!http2.begin(client2, url))
    {
        Serial.println("HTTP begin kedua gagal");
        return false;
    }

    http2.addHeader("Authorization", authorization);

    int httpCode = http2.GET();

    Serial.print("Second HTTP status: ");
    Serial.println(httpCode);

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.println("Digest authentication gagal");
        http2.end();
        return false;
    }

    WiFiClient *stream = http2.getStreamPtr();
    int contentLength = http2.getSize();

    Serial.print("Content-Length: ");
    Serial.println(contentLength);

    if (contentLength > 0)
    {
        size_t total = SPIFFS.totalBytes();
        size_t used = SPIFFS.usedBytes();
        size_t freeBytes = total >= used ? total - used : 0;

        if ((size_t)contentLength > freeBytes)
        {
            Serial.println("Ruang SPIFFS tidak cukup untuk snapshot");
            printSpiffsStatus();
            http2.end();
            return false;
        }
    }

    File file = SPIFFS.open(localPath, FILE_WRITE);

    if (!file)
    {
        Serial.println("Gagal membuka file SPIFFS");
        printSpiffsStatus();
        http2.end();
        return false;
    }

    uint8_t buffer[IO_BUFFER_SIZE];
    int totalBytes = 0;

    while (
        http2.connected() &&
        (contentLength > 0 || contentLength == -1)
    )
    {
        size_t available = stream->available();

        if (available)
        {
            size_t readSize = min(available, sizeof(buffer));

            int bytesRead = stream->readBytes(
                buffer,
                readSize
            );

            if (bytesRead <= 0)
            {
                break;
            }

            size_t bytesWritten = file.write(
                buffer,
                bytesRead
            );

            if (bytesWritten != (size_t)bytesRead)
            {
                Serial.println("Gagal menulis seluruh data ke SPIFFS");
                file.close();
                http2.end();
                return false;
            }

            totalBytes += bytesRead;

            if (contentLength > 0)
            {
                contentLength -= bytesRead;
            }
        }

        delay(1);
    }

    file.close();
    http2.end();

    Serial.print("Total gambar tersimpan: ");
    Serial.print(totalBytes);
    Serial.println(" bytes");

    return totalBytes > 0;
}

// -----------------------------------------------------------------------------
// Reliable write helper
// -----------------------------------------------------------------------------
bool writeAll(
    WiFiClientSecure &client,
    const uint8_t *data,
    size_t length
)
{
    size_t totalSent = 0;

    while (totalSent < length)
    {
        size_t sent = client.write(
            data + totalSent,
            length - totalSent
        );

        if (sent == 0)
        {
            Serial.println("Gagal mengirim data ke server");
            return false;
        }

        totalSent += sent;
        delay(1);
    }

    return true;
}

// -----------------------------------------------------------------------------
// SPIFFS -> Server Photo
// -----------------------------------------------------------------------------
bool uploadImage(
    const String &localPath,
    const String &uploadFilename
)
{
    File file = SPIFFS.open(localPath, FILE_READ);

    if (!file)
    {
        Serial.println("File upload tidak ditemukan");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure(); // TODO: gunakan CA certificate untuk production.

    Serial.println();
    Serial.println("Connecting to upload server...");

    if (!client.connect(host, port))
    {
        Serial.println("HTTPS connection gagal");
        file.close();
        return false;
    }

    String boundary = "----ESP32DoorbellBoundary";

    String head =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; "
        "name=\"upfile\"; "
        "filename=\"" + uploadFilename + "\"\r\n"
        "Content-Type: image/jpeg\r\n"
        "\r\n";

    String tail =
        "\r\n--" + boundary + "--\r\n";

    size_t fileSize = file.size();
    size_t contentLength =
        head.length() +
        fileSize +
        tail.length();

    client.print(
        String("POST ") + path + " HTTP/1.1\r\n"
    );

    client.print(
        String("Host: ") + host + "\r\n"
    );

    client.print(
        String("Content-Type: multipart/form-data; boundary=") +
        boundary + "\r\n"
    );

    client.print(
        String("Content-Length: ") +
        contentLength + "\r\n"
    );

    client.print("Connection: close\r\n\r\n");
    client.print(head);

    uint8_t buffer[IO_BUFFER_SIZE];
    size_t totalUploaded = 0;

    Serial.print("File size: ");
    Serial.print(fileSize);
    Serial.println(" bytes");

    while (file.available())
    {
        size_t bytesRead = file.read(
            buffer,
            sizeof(buffer)
        );

        if (bytesRead == 0)
        {
            break;
        }

        if (!writeAll(client, buffer, bytesRead))
        {
            Serial.println("Upload file terputus");
            file.close();
            client.stop();
            return false;
        }

        totalUploaded += bytesRead;
    }

    Serial.print("Total file bytes sent: ");
    Serial.println(totalUploaded);

    file.close();
    client.print(tail);

    Serial.println("Upload data terkirim");

    unsigned long timeout = millis();

    while (
        client.connected() &&
        !client.available()
    )
    {
        if (millis() - timeout > 15000)
        {
            Serial.println("Response timeout");
            client.stop();
            return false;
        }

        delay(10);
    }

    String statusLine = client.readStringUntil('\n');
    statusLine.trim();

    Serial.print("Server response: ");
    Serial.println(statusLine);

    bool success = statusLine.indexOf("200") >= 0;

    while (client.available())
    {
        String line = client.readStringUntil('\n');

        if (line == "\r")
        {
            break;
        }
    }

    String response;

    while (
        client.connected() ||
        client.available()
    )
    {
        while (client.available())
        {
            response += (char)client.read();
        }

        delay(1);
    }

    client.stop();

    Serial.print("Response body: ");
    Serial.println(response);

    return success;
}

// -----------------------------------------------------------------------------
// Filename / doorbell
// -----------------------------------------------------------------------------
String makeFilename()
{
    struct tm timeinfo;

    if (!getLocalTime(&timeinfo, 10000))
    {
        Serial.println("Gagal mendapatkan waktu NTP");
        return "doorbell.jpg";
    }

    char buffer[32];

    strftime(
        buffer,
        sizeof(buffer),
        "%Y%m%d%H%M%S.jpg",
        &timeinfo
    );

    return String(buffer);
}

void processDoorbell()
{
    String filename = makeFilename();
    String localPath = "/" + filename;

    Serial.println();
    Serial.println("=== BELL TRIGGERED ===");
    Serial.print("Filename: ");
    Serial.println(filename);

    if (!downloadCameraImage(localPath))
    {
        Serial.println("GAGAL MENGAMBIL GAMBAR");
        return;
    }

    Serial.println("GAMBAR BERHASIL DIAMBIL");

    if (!uploadImage(localPath, filename))
    {
        Serial.println("UPLOAD GAGAL");
        Serial.println("File lokal dipertahankan");
        return;
    }

    Serial.println("UPLOAD BERHASIL");

    if (SPIFFS.remove(localPath))
    {
        Serial.println("File lokal berhasil dihapus");
    }
    else
    {
        Serial.println("Peringatan: file lokal gagal dihapus");
    }
}

// -----------------------------------------------------------------------------
// Arduino setup / loop
// -----------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    pinMode(BELL_PIN, INPUT);

    delay(1000);

    Serial.println();
    Serial.println("ESP32 Doorbell 2026 - SPIFFS Version");

    if (!SPIFFS.begin(true))
    {
        Serial.println("SPIFFS gagal");
        return;
    }

    // SEMENTARA: bersihkan seluruh SPIFFS
    Serial.println("Formatting SPIFFS...");
    SPIFFS.format();
    Serial.println("SPIFFS selesai diformat");

    Serial.println("SPIFFS mounted");
    printSpiffsStatus();

    Serial.print("Connecting WiFi");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startAttemptTime = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");

        if (millis() - startAttemptTime > 20000)
        {
            Serial.println();
            Serial.println("WiFi gagal, retry...");

            WiFi.disconnect();
            delay(1000);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            startAttemptTime = millis();
        }
    }

    Serial.println();
    Serial.println("WiFi connected");

    configTime(
        7 * 3600,
        0,
        "pool.ntp.org"
    );

    delay(2000);

    Serial.print("IP ESP32: ");
    Serial.println(WiFi.localIP());
}

void loop()
{
    if (digitalRead(BELL_PIN) == HIGH)
    {
        processDoorbell();

        // Tunggu tombol dilepas agar tidak trigger berkali-kali.
        while (digitalRead(BELL_PIN) == HIGH)
        {
            delay(50);
        }

        delay(300);
    }

    delay(10);
}