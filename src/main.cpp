#include <Arduino.h>

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <cmath>
#include <LittleFS.h>

const char *ssid = "TECNO CAMON 30S Pro";
const char *password = "2c6urddtnaeq56i";

#define LED_PIN 8

#define WIFI_RECONNECT_ATTEMPTS 120
#define WIFI_RECONNECT_DELAY 500


// =====================================================
// Modbus UART
// =====================================================
//
// ESP32-C3:
//
// GPIO 4 = TX
// GPIO 5 = RX
//
// UART settings:
//
// 115200 baud
// 8 data bits
// No parity
// 1 stop bit
//
// =====================================================

#define SENSOR_RX 5
#define SENSOR_TX 4
#define SENSOR_BAUD 115200

HardwareSerial sensorSerial(1);

WebServer server(80);

WebSocketsServer webSocket = WebSocketsServer(81);

float temperature = 25.0;

unsigned long lastTemperatureUpdate = 0;

const unsigned long TEMPERATURE_INTERVAL = 200;


// =====================================================
// Calculate Modbus ASCII LRC
// =====================================================
//
// Example:
//
// Input:
//     010300000004
//
// Bytes:
//
//     01 03 00 00 00 04
//
// Sum:
//
//     01 + 03 + 00 + 00 + 00 + 04 = 08
//
// Two's complement:
//
//     F8
//
// Result:
//
//     F8
//
// Complete frame:
//
//     :010300000004F8\r\n
//
// =====================================================

String calculateLRC(String hexData) {
    uint8_t sum = 0;

    for (int i = 0; i < hexData.length(); i += 2) {
        String byteString = hexData.substring(i, i + 2);
        uint8_t value = strtoul(byteString.c_str(), nullptr, 16);
        sum += value;
    }

    // Two's complement
    uint8_t lrc = (uint8_t) (-sum);

    char result[3];

    sprintf(result, "%02X", lrc);

    return String(result);
}

bool isValidHexCommand(String command) {
    if (command.length() == 0) {
        return false;
    }

    if (command.length() % 2 != 0) {
        return false;
    }

    for (int i = 0; i < command.length(); i++) {
        char c = command.charAt(i);

        bool valid =
                (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');

        if (!valid) {
            return false;
        }
    }

    return true;
}

String sendModbusASCII(String hexCommand) {
    // Remove leading/trailing whitespace
    hexCommand.trim();

    // Convert to uppercase
    hexCommand.toUpperCase();

    // Remove spaces
    hexCommand.replace(" ", "");

    // Allow user to enter a leading colon
    if (hexCommand.startsWith(":")) {
        hexCommand = hexCommand.substring(1);
    }

    if (!isValidHexCommand(hexCommand)) {
        return "ERROR: Invalid hexadecimal command";
    }

    String lrc = calculateLRC(hexCommand);

    String frame = ":" + hexCommand + lrc + "\r\n";

    Serial.println();
    Serial.println("========== MODBUS ASCII ==========");

    Serial.print("Command: ");
    Serial.println(hexCommand);

    Serial.print("LRC: ");
    Serial.println(lrc);

    Serial.print("Frame: ");
    Serial.print(frame);

    Serial.println("==================================");

    // ---------------------------------------------------
    // Clear old data from UART RX buffer
    // ---------------------------------------------------

    while (sensorSerial.available()) {
        sensorSerial.read();
    }

    sensorSerial.print(frame);
    sensorSerial.flush();

    String response = "";

    const unsigned long RESPONSE_TIMEOUT = 1000;

    unsigned long startTime = millis();


    while (millis() - startTime < RESPONSE_TIMEOUT) {
        while (sensorSerial.available()) {
            char c = sensorSerial.read();

            response += c;
            if (response.length() >= 2 && response.endsWith("\r\n")) {
                Serial.println();
                Serial.println("Modbus response:");
                Serial.println(response);

                return response;
            }
        }

        delay(1);
    }

    if (response.length() == 0) {
        Serial.println();
        Serial.println("Modbus timeout - no response");

        return "ERROR: Modbus timeout - no response";
    }

    Serial.println();
    Serial.println("Modbus response timeout - partial data:");
    Serial.println(response);

    return response;
}

void webSocketEvent(
    uint8_t num,
    WStype_t type,
    uint8_t *payload,
    size_t length) {
    if (type == WStype_CONNECTED) {
        Serial.println();
        Serial.print("Browser connected via WebSocket. Client: ");
        Serial.println(num);

        return;
    }

    if (type == WStype_DISCONNECTED) {
        Serial.println();
        Serial.print("Browser disconnected. Client: ");
        Serial.println(num);

        return;
    }

    if (type == WStype_TEXT) {
        String message = String((char *) payload);
        Serial.println();
        Serial.print("WebSocket received: ");
        Serial.println(message);


        // =================================================
        // Modbus command
        // =================================================
        //
        // Browser sends:
        //
        //     CMD:010300000004
        //
        // =================================================

        if (message.startsWith("CMD:")) {
            String command = message.substring(4);
            String response = sendModbusASCII(command);
            webSocket.sendTXT(num, response);

            return;
        }
    }
}

void setup() {
    Serial.begin(115200);

    delay(2000);

    Serial.println();
    Serial.println("Starting ESP32-C3...");

    Serial.println("Starting Modbus UART...");

    sensorSerial.begin(
        SENSOR_BAUD,
        SERIAL_8N1,
        SENSOR_RX,
        SENSOR_TX);


    Serial.print("Modbus RX GPIO: ");

    Serial.println(SENSOR_RX);

    Serial.print("Modbus TX GPIO: ");

    Serial.println(SENSOR_TX);

    Serial.print("Modbus baud: ");

    Serial.println(SENSOR_BAUD);

    Serial.println("Modbus format: SERIAL_8N1");

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed!");

        return;
    }

    Serial.println("LittleFS mounted.");

    Serial.println("Files:");

    File root = LittleFS.open("/");

    File file = root.openNextFile();

    while (file) {
        Serial.print("  ");
        Serial.print(file.name());
        Serial.print("  ");
        Serial.print(file.size());
        Serial.println(" bytes");

        file = root.openNextFile();
    }

    WiFi.mode(WIFI_STA);

    WiFi.begin(ssid, password);


    Serial.print("Connecting to WiFi");

    int counter = 0;

    pinMode(LED_PIN, OUTPUT);

    while (counter < WIFI_RECONNECT_ATTEMPTS && WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(WIFI_RECONNECT_DELAY);
        Serial.print(".");

        counter++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED_PIN, HIGH);
        Serial.println();
        Serial.println("WiFi connection failed!");
        Serial.println("Please restart ESP32-C3 after fixing the WiFi");

        return;
    }

    digitalWrite(LED_PIN, LOW);
    Serial.println();

    Serial.println("WiFi connected!");

    Serial.print("ESP32 IP address: ");

    Serial.println(WiFi.localIP());

    server.serveStatic(
        "/",
        LittleFS,
        "/index.html");

    server.serveStatic(
        "/chart.js",
        LittleFS,
        "/chart.js");

    server.begin();

    Serial.println("HTTP server started");

    webSocket.begin();

    webSocket.onEvent(webSocketEvent);


    Serial.println("WebSocket server started");


    Serial.println();
    Serial.println("ESP32-C3 is ready.");

    Serial.println();
}

void loop() {
    bool connectionLostFlag = false;

    wl_status_t WiFiStatus = WiFi.status();

    int counter = 0;


    if (WiFiStatus != WL_CONNECTED) {
        connectionLostFlag = true;
        Serial.println("WiFi connection lost. Attempting to reconnect");
    }


    while (counter < WIFI_RECONNECT_ATTEMPTS && WiFiStatus != WL_CONNECTED) {
        WiFiStatus = WiFi.status();
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(WIFI_RECONNECT_DELAY);
        Serial.print(".");

        counter++;
    }

    if (WiFiStatus != WL_CONNECTED) {
        digitalWrite(LED_PIN, HIGH);
        Serial.println();
        Serial.println("WiFi reconnect failed!");

        return;
    }

    if (connectionLostFlag == true) {
        connectionLostFlag = false;
        digitalWrite(LED_PIN, LOW);

        Serial.println();
        Serial.println("WiFi connection reestablished!");

        Serial.print("ESP32 IP address: ");
        Serial.println(WiFi.localIP());
    }

    server.handleClient();

    webSocket.loop();
}
