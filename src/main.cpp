#include <Arduino.h>

#include <mdns.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// =====================================================
// Wi-Fi configuration
// =====================================================

const char *ssid = "Thermodat";
const char *password = "ThermodatPassword1151";

#define LED_PIN 8

#define WIFI_RECONNECT_ATTEMPTS 120
#define WIFI_RECONNECT_DELAY 500

// =====================================================
// Modbus UART configuration
// =====================================================
//
// ESP32-C3:
// GPIO 4 = TX
// GPIO 5 = RX
//
// 115200 baud, 8 data bits, no parity, 1 stop bit
//
// =====================================================

#define SENSOR_RX 5
#define SENSOR_TX 4
#define SENSOR_BAUD 115200

HardwareSerial sensorSerial(1);

WebServer server(80);
WebSocketsServer webSocket(81);

// =====================================================
// Modbus request configuration
// =====================================================

const unsigned long MODBUS_TIMEOUT_MS = 1000;
const size_t MODBUS_MAX_RESPONSE = 256;

// Only one Modbus request can be active at a time because
// the controller uses a single UART connection.

struct PendingModbusRequest {
    bool active = false;
    uint8_t client = 0;
    unsigned long startedAt = 0;
    String response;
};

PendingModbusRequest pendingModbus;


// =====================================================
// PROGRAM STORAGE API
// =====================================================
//
// GET  /api/programs
//      Returns {"programs":["experiment-01", ...]}
//
// GET  /api/program?name=experiment-01
//      Returns the saved program JSON.
//
// POST /api/program
//      Saves a program JSON body to LittleFS.
//
// Files are saved as:
//      /program_experiment-01.json
//
// =====================================================


// -----------------------------------------------------
// Validate program filenames
// -----------------------------------------------------

bool isSafeProgramName(const String &name) {
    if (name.length() == 0 || name.length() > 48) {
        return false;
    }

    for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];

        bool valid =
                (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '_' ||
                c == '-';

        if (!valid) {
            return false;
        }
    }

    return true;
}


// -----------------------------------------------------
// Construct a program filepath
// -----------------------------------------------------

String programFilePath(const String &name) {
    return String("/program_") + name + ".json";
}


// -----------------------------------------------------
// Send a JSON error response
// -----------------------------------------------------

void sendJsonError(int status, const String &message) {
    StaticJsonDocument<256> doc;

    doc["error"] = message;

    String output;
    serializeJson(doc, output);

    server.send(status, "application/json", output);
}


// -----------------------------------------------------
// GET /api/programs
//
// Returns a list of saved programs.
// -----------------------------------------------------

void handleListPrograms() {
    DynamicJsonDocument doc(4096);

    JsonArray programs = doc.createNestedArray("programs");

    File root = LittleFS.open("/");

    if (!root || !root.isDirectory()) {
        sendJsonError(500, "Could not open LittleFS root");
        return;
    }

    File file = root.openNextFile();

    while (file) {
        String fileName = String(file.name());

        // Some filesystem implementations return filenames
        // without a leading slash.
        if (!fileName.startsWith("/")) {
            fileName = "/" + fileName;
        }

        if (fileName.startsWith("/program_") &&
            fileName.endsWith(".json")) {
            String name = fileName.substring(
                9,
                fileName.length() - 5
            );

            if (isSafeProgramName(name)) {
                programs.add(name);
            }
        }

        file.close();
        file = root.openNextFile();
    }

    root.close();

    String output;
    serializeJson(doc, output);

    server.send(200, "application/json", output);
}


// -----------------------------------------------------
// GET /api/program?name=experiment-01
//
// Loads a program from LittleFS.
// -----------------------------------------------------

void handleGetProgram() {
    if (!server.hasArg("name")) {
        sendJsonError(400, "Missing name query parameter");
        return;
    }

    String name = server.arg("name");

    if (!isSafeProgramName(name)) {
        sendJsonError(400, "Invalid program name");
        return;
    }

    String path = programFilePath(name);

    File file = LittleFS.open(path, "r");

    if (!file) {
        sendJsonError(404, "Program not found");
        return;
    }

    String output;
    output.reserve(file.size() + 1);

    while (file.available()) {
        output += static_cast<char>(file.read());
    }

    file.close();

    // Make sure the stored file contains valid JSON and
    // has the expected steps array before returning it.
    DynamicJsonDocument checkDoc(4096);

    DeserializationError error =
            deserializeJson(checkDoc, output);

    if (error || !checkDoc["steps"].is<JsonArray>()) {
        sendJsonError(500, "Saved program file is invalid");
        return;
    }

    server.send(200, "application/json", output);
}


// -----------------------------------------------------
// POST /api/program
//
// Saves a program to LittleFS.
//
// Expected JSON structure:
//
// {
//   "name": "experiment-01",
//   "programNumber": 1,
//   "steps": [
//     {
//       "type": 0,
//       "value": 100,
//       "target": 250,
//       "transition": 0,
//       "active": true
//     }
//   ]
// }
//
// The steps array must contain exactly 10 entries.
// -----------------------------------------------------

void handleSaveProgram() {
    if (!server.hasArg("plain") ||
        server.arg("plain").length() == 0) {
        sendJsonError(400, "Missing JSON request body");
        return;
    }

    DynamicJsonDocument input(4096);

    DeserializationError error =
            deserializeJson(input, server.arg("plain"));

    if (error) {
        sendJsonError(400, "Invalid JSON");
        return;
    }

    String name = input["name"] | "";

    int programNumber = input["programNumber"] | 0;

    JsonArrayConst steps =
            input["steps"].as<JsonArrayConst>();

    if (!isSafeProgramName(name)) {
        sendJsonError(
            400,
            "Name must use 1-48 letters, digits, underscores or hyphens"
        );
        return;
    }

    if (programNumber < 1 || programNumber > 80) {
        sendJsonError(
            400,
            "programNumber must be from 1 to 80"
        );
        return;
    }

    if (steps.isNull() || steps.size() != 10) {
        sendJsonError(
            400,
            "steps must contain exactly 10 entries"
        );
        return;
    }

    // Build a normalized document rather than saving
    // arbitrary fields sent by the browser.
    DynamicJsonDocument saved(4096);

    saved["name"] = name;
    saved["programNumber"] = programNumber;

    JsonArray savedSteps = saved.createNestedArray("steps");

    for (JsonVariantConst step: steps) {
        int type = step["type"] | -1;

        long value = step["value"] | -1L;

        long target = step["target"] | 999999L;

        int transition = step["transition"] | -1;

        bool active = step["active"] | false;

        if (type < 0 || type > 6 ||
            value < 0 || value > 65535 ||
            target < -2700 || target > 25000 ||
            transition < 0 || transition > 2) {
            sendJsonError(
                400,
                "One or more step values are invalid"
            );

            return;
        }

        JsonObject savedStep =
                savedSteps.createNestedObject();

        savedStep["type"] = type;
        savedStep["value"] = value;
        savedStep["target"] = target;
        savedStep["transition"] = transition;
        savedStep["active"] = active;
    }

    String path = programFilePath(name);

    File file = LittleFS.open(path, "w");

    if (!file) {
        sendJsonError(
            500,
            "Could not open program file for writing"
        );

        return;
    }

    size_t bytesWritten = serializeJson(saved, file);

    file.close();

    if (bytesWritten == 0) {
        LittleFS.remove(path);

        sendJsonError(
            500,
            "Could not write program file"
        );

        return;
    }

    StaticJsonDocument<256> responseDoc;

    responseDoc["ok"] = true;
    responseDoc["name"] = name;
    responseDoc["bytes"] = bytesWritten;

    String response;

    serializeJson(responseDoc, response);

    server.send(200, "application/json", response);

    Serial.printf(
        "Saved program '%s' (%u bytes)\n",
        name.c_str(),
        static_cast<unsigned>(bytesWritten)
    );
}


// =====================================================
// MODBUS ASCII
// =====================================================


// -----------------------------------------------------
// Validate a hexadecimal command.
//
// Accepts hex characters only, with an even number
// of characters.
// -----------------------------------------------------

bool isValidHexCommand(String command) {
    command.trim();
    command.toUpperCase();
    command.replace(" ", "");

    if (command.startsWith(":")) {
        command.remove(0, 1);
    }

    if (command.length() == 0 ||
        command.length() % 2 != 0) {
        return false;
    }

    for (size_t i = 0; i < command.length(); i++) {
        char c = command[i];

        bool valid =
                (c >= '0' && c <= '9') ||
                (c >= 'A' && c <= 'F');

        if (!valid) {
            return false;
        }
    }

    return true;
}


// -----------------------------------------------------
// Calculate Modbus ASCII LRC.
//
// Input example:
// 040300000001
//
// Output:
//  F7
// -----------------------------------------------------

String calculateLRC(const String &hexData) {
    uint8_t sum = 0;

    for (size_t i = 0; i + 1 < hexData.length(); i += 2) {
        String byteString = hexData.substring(i, i + 2);

        uint8_t value =
                static_cast<uint8_t>(
                    strtoul(byteString.c_str(), nullptr, 16)
                );

        sum += value;
    }

    uint8_t lrc = static_cast<uint8_t>(-sum);

    char result[3];

    snprintf(result, sizeof(result), "%02X", lrc);

    return String(result);
}


// -----------------------------------------------------
// Validate a complete Modbus ASCII response.
//
// Example:
// :0403020107EF\r\n
//
// Checks:
// - Colon at the beginning
// - CRLF at the end
// - Even-length hexadecimal payload
// - Valid LRC checksum
// -----------------------------------------------------

bool validateModbusAsciiResponse(
    const String &line,
    String &reason
) {
    if (line.length() < 9 ||
        line[0] != ':' ||
        !line.endsWith("\r\n")) {
        reason = "bad frame shape";
        return false;
    }

    // Remove ':' and CRLF.
    String hex = line.substring(1, line.length() - 2);

    if (hex.length() < 6 ||
        hex.length() % 2 != 0) {
        reason = "bad hexadecimal length";
        return false;
    }

    uint8_t sum = 0;

    for (size_t i = 0; i < hex.length(); i += 2) {
        char a = hex[i];
        char b = hex[i + 1];

        bool aValid =
                (a >= '0' && a <= '9') ||
                (a >= 'A' && a <= 'F') ||
                (a >= 'a' && a <= 'f');

        bool bValid =
                (b >= '0' && b <= '9') ||
                (b >= 'A' && b <= 'F') ||
                (b >= 'a' && b <= 'f');

        if (!aValid || !bValid) {
            reason = "non-hexadecimal character";
            return false;
        }

        char byteText[3] = {a, b, '\0'};

        sum += static_cast<uint8_t>(
            strtoul(byteText, nullptr, 16)
        );
    }

    // All decoded bytes including the LRC must sum to zero.
    if (sum != 0) {
        reason = "LRC checksum mismatch";
        return false;
    }

    return true;
}


// -----------------------------------------------------
// Start a Modbus request without waiting for a response.
//
// Browser sends:
// CMD:040300000001
//
// ESP32 transmits:
// :040300000001F7\r\n
//
// The response is handled later by serviceModbusRequest()
// in loop(), allowing HTTP and WebSocket processing to
// continue while the controller is responding.
// -----------------------------------------------------

void startModbusRequest(uint8_t client, String command) {
    command.trim();
    command.toUpperCase();
    command.replace(" ", "");

    if (command.startsWith(":")) {
        command.remove(0, 1);
    }

    if (!isValidHexCommand(command)) {
        webSocket.sendTXT(
            client,
            "ERROR: Invalid hexadecimal command"
        );

        return;
    }

    if (pendingModbus.active) {
        webSocket.sendTXT(
            client,
            "ERROR: Modbus busy; try again"
        );

        return;
    }

    // Remove old UART bytes before beginning a new request.
    while (sensorSerial.available()) {
        sensorSerial.read();
    }

    String frame =
            ":" + command + calculateLRC(command) + "\r\n";

    Serial.println();
    Serial.println("========== MODBUS ASCII ==========");

    Serial.print("Command: ");
    Serial.println(command);

    Serial.print("Frame: ");
    Serial.print(frame);

    Serial.println("==================================");

    pendingModbus.active = true;
    pendingModbus.client = client;
    pendingModbus.startedAt = millis();
    pendingModbus.response = "";

    sensorSerial.print(frame);
    sensorSerial.flush();
}


// -----------------------------------------------------
// Process incoming UART bytes without blocking.
//
// When a full response arrives:
// - Validate the Modbus ASCII frame.
// - Check the LRC.
// - Return the response to the requesting browser.
//
// A timeout is also handled here.
// -----------------------------------------------------

void serviceModbusRequest() {
    if (!pendingModbus.active) {
        return;
    }

    while (sensorSerial.available()) {
        char c = static_cast<char>(sensorSerial.read());

        if (pendingModbus.response.length() >= MODBUS_MAX_RESPONSE) {
            uint8_t client = pendingModbus.client;

            pendingModbus.active = false;
            pendingModbus.response = "";

            webSocket.sendTXT(
                client,
                "ERROR: Modbus response too long"
            );

            return;
        }

        pendingModbus.response += c;

        if (pendingModbus.response.endsWith("\r\n")) {
            uint8_t client = pendingModbus.client;

            String reason;

            if (validateModbusAsciiResponse(
                pendingModbus.response,
                reason)) {
                Serial.println("Validated Modbus response:");
                Serial.print(pendingModbus.response);

                webSocket.sendTXT(
                    client,
                    pendingModbus.response
                );
            } else {
                Serial.print("Invalid Modbus ASCII response: ");
                Serial.println(reason);

                webSocket.sendTXT(
                    client,
                    String("ERROR: Invalid Modbus ASCII response: ") +
                    reason
                );
            }

            pendingModbus.active = false;
            pendingModbus.response = "";

            return;
        }
    }

    if (millis() - pendingModbus.startedAt >= MODBUS_TIMEOUT_MS) {
        uint8_t client = pendingModbus.client;
        String partial = pendingModbus.response;

        pendingModbus.active = false;
        pendingModbus.response = "";

        if (partial.length() == 0) {
            webSocket.sendTXT(
                client,
                "ERROR: Modbus timeout - no response"
            );
        } else {
            webSocket.sendTXT(
                client,
                String("ERROR: Modbus response timeout - partial data: ") +
                partial
            );
        }
    }
}


// =====================================================
// WEBSOCKET
// =====================================================

void webSocketEvent(
    uint8_t num,
    WStype_t type,
    uint8_t *payload,
    size_t length
) {
    if (type == WStype_CONNECTED) {
        Serial.printf(
            "\nBrowser connected via WebSocket. Client: %u\n",
            num
        );

        return;
    }

    if (type == WStype_DISCONNECTED) {
        Serial.printf(
            "\nBrowser disconnected. Client: %u\n",
            num
        );

        // If this client owned the active request, cancel it.
        if (pendingModbus.active &&
            pendingModbus.client == num) {
            pendingModbus.active = false;
            pendingModbus.response = "";
        }

        return;
    }

    if (type != WStype_TEXT) {
        return;
    }

    // Use the provided payload length instead of assuming that
    // the WebSocket payload is null-terminated.
    String message;
    message.reserve(length + 1);

    for (size_t i = 0; i < length; i++) {
        message += static_cast<char>(payload[i]);
    }

    Serial.print("\nWebSocket received: ");
    Serial.println(message);

    // Browser sends commands as CMD:<hex bytes>.
    if (message.startsWith("CMD:")) {
        startModbusRequest(
            num,
            message.substring(4)
        );
    }
}

void startMDNS() {
    esp_err_t err = mdns_init();

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("mDNS init failed: %s\n", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set("thermodat");
    mdns_instance_name_set("Thermodat ESP32");

    mdns_service_add(
        "Thermodat Web Server",
        "_http",
        "_tcp",
        80,
        nullptr,
        0
    );

    Serial.println("mDNS available at http://termodat.local");
}

// =====================================================
// SETUP
// =====================================================

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
        SENSOR_TX
    );

    Serial.printf("Modbus RX GPIO: %d\n", SENSOR_RX);
    Serial.printf("Modbus TX GPIO: %d\n", SENSOR_TX);
    Serial.printf("Modbus baud: %d\n", SENSOR_BAUD);
    Serial.println("Modbus format: SERIAL_8N1");

    // -------------------------------------------------
    // LittleFS
    // -------------------------------------------------

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed!");
        return;
    }

    Serial.println("LittleFS mounted.");
    Serial.println("Files:");

    File root = LittleFS.open("/");
    File file = root.openNextFile();

    while (file) {
        Serial.printf(
            "  %s  %u bytes\n",
            file.name(),
            static_cast<unsigned>(file.size())
        );

        file.close();
        file = root.openNextFile();
    }

    root.close();

    // -------------------------------------------------
    // Wi-Fi
    // -------------------------------------------------

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
    WiFi.mode(WIFI_AP);
    if (WiFi.softAP(ssid, password)) {
        Serial.println("Wi-Fi access point started!");
        Serial.print("Network: ");
        Serial.println(ssid);
        Serial.print("Password: ");
        Serial.println(password);
        Serial.print("IP address: ");
        Serial.println(WiFi.softAPIP());

        // Start mDNS
        startMDNS();

        // AP is running
        digitalWrite(LED_PIN, LOW);
    } else {
        Serial.println("Failed to start Wi-Fi access point!");
        digitalWrite(LED_PIN, HIGH);
        return;
    }

    // -------------------------------------------------
    // HTTP routes
    //
    // Register API routes before server.begin().
    // -------------------------------------------------

    server.on(
        "/api/programs",
        HTTP_GET,
        handleListPrograms
    );

    server.on(
        "/api/program",
        HTTP_GET,
        handleGetProgram
    );

    server.on(
        "/api/program",
        HTTP_POST,
        handleSaveProgram
    );

    server.onNotFound([]() {
        if (server.uri().startsWith("/api/")) {
            sendJsonError(404, "Unknown API endpoint");
        } else {
            server.send(404, "text/plain", "Not found");
        }
    });

    // Serve the web interface and Chart.js from LittleFS.
    server.serveStatic(
        "/",
        LittleFS,
        "/index.html"
    );

    server.serveStatic(
        "/chart.js",
        LittleFS,
        "/chart.js"
    );

    server.begin();

    Serial.println("HTTP server started");

    // -------------------------------------------------
    // WebSocket
    // -------------------------------------------------

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);

    Serial.println("WebSocket server started");

    Serial.println();
    Serial.println("ESP32-C3 is ready.");
    Serial.println();
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
    // Keep HTTP and WebSocket handlers running
    server.handleClient();
    webSocket.loop();

    // Process Modbus responses
    serviceModbusRequest();

    // -------------------------------------------------
    // Wi-Fi client status LED
    // -------------------------------------------------

    static unsigned long lastBlink = 0;

    int connectedDevices = WiFi.softAPgetStationNum();

    if (connectedDevices > 0) {
        digitalWrite(LED_PIN, LOW);  // LED steady on (if active-low)
    } else {
        if (millis() - lastBlink >= 500) {
            lastBlink = millis();

            int currentLedValue = digitalRead(LED_PIN);
            digitalWrite(LED_PIN, !currentLedValue);
        }
    }
}
