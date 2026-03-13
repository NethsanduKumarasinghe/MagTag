#include <Wire.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define MPU_ADDR 0x68
#define LIMIT_SWITCH_PIN 27
#define BUZZER_PIN 14

HardwareSerial gpsSerial(1);
TinyGPSPlus gps;

int16_t AcX, AcY;

const char* gpsUrl = "https://magtic-default-rtdb.firebaseio.com/devices/default.json";
const char* statusUrl = "https://magtic-default-rtdb.firebaseio.com/statusUpdates/default.json";
const char* deviceStatusUrl = "https://magtic-default-rtdb.firebaseio.com/statusUpdates/default.json";

WebServer server(80);
String incomingSSID = "", incomingPassword = "";

bool isWiFiConnected = false;
bool statusSent = false;
bool systemReady = false;

bool armed = true;
bool motionSpikeDetected = false;
bool sustainedMotionDetected = false;
bool limitSwitchTriggered = false;

unsigned long motionMonitoringStart = 0;
unsigned long lastSpikeTime = 0;
unsigned long lastSendTime = 0;
unsigned long lastLimitSendTime = 0;
const unsigned long motionThreshold = 8000;
const unsigned long sendCooldown = 3000;

void startAPMode() {
    const char* apSSID = "ESP32-Setup";
    const char* apPassword = "12345678";

    WiFi.softAP(apSSID, apPassword);
    IPAddress IP = WiFi.softAPIP();

    Serial.println("AP Mode Started");
    Serial.print("Open browser at http://");
    Serial.println(IP);

server.on("/", []() {
    String html = "<h2>ESP32 WiFi Setup</h2>"
                  "<form action=\"/wifi\" method=\"POST\">"
                  "SSID: <input name=\"ssid\"><br>"
                  "Password: <input name=\"password\" type=\"password\"><br><br>"
                  "<input type=\"submit\" value=\"Connect\">"
                  "</form>";

    server.send(200, "text/html", html);
});

server.on("/wifi", HTTP_POST, []() {
    if (server.hasArg("ssid") && server.hasArg("password")) {

        incomingSSID = server.arg("ssid");
        incomingPassword = server.arg("password");

        Serial.println("Received WiFi credentials");
        server.send(200, "text/html", "Connecting to WiFi...");

        WiFi.softAPDisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(incomingSSID.c_str(), incomingPassword.c_str());

    } else {
        server.send(400, "text/html", "Missing SSID or password!");
    }
});

server.begin();
}


void sendConnectionNotification() {

    HTTPClient http;
    WiFiClientSecure client;

    client.setInsecure();

    String payload =
        "{\"deviceStatus\":\"connected\",\"timestamp\":"
        + String(millis() + 1751175280000) + "}";

    http.begin(client, statusUrl);
    http.addHeader("Content-Type", "application/json");

    int code1 = http.PUT(payload);
    http.end();


    http.begin(client, deviceStatusUrl);
    http.addHeader("Content-Type", "application/json");

    int code2 = http.PUT(payload);
    http.end();

    Serial.printf("Status sent to Firebase. Codes: %d, %d\n", code1, code2);
}



void sendGPSData(bool motionDetected, bool limitTriggered) {

    if (!gps.location.isValid()) {
        Serial.println("Cannot send GPS: not connected or invalid GPS");
        return;
    }

    HTTPClient http;
    WiFiClientSecure client;

    client.setInsecure();

    http.begin(client, gpsUrl);
    http.addHeader("Content-Type", "application/json");
    String payload = "{";
payload += "\"device\":\"default\",";
payload += "\"lat\":" + String(gps.location.lat(), 6) + ",";
payload += "\"lng\":" + String(gps.location.lng(), 6) + ",";
payload += "\"motion\":" + String(motionDetected ? "true" : "false") + ",";
payload += "\"limit\":" + String(limitTriggered ? "true" : "false") + ",";
payload += "\"timestamp\":" + String(millis() + 1751175280000);
payload += "}";

int code = http.PUT(payload);

if (code > 0)
    Serial.printf("GPS Data sent. Code: %d\n", code);
else
    Serial.printf("GPS Send failed. Code: %d\n", code);

http.end();
}



void sendMotionOrLimitStatus(bool motionDetected, bool limitTriggered) {

    HTTPClient http;
    WiFiClientSecure client;

    client.setInsecure();

    http.begin(client, gpsUrl);
    http.addHeader("Content-Type", "application/json");

    String payload = "{";
    payload += "\"device\":\"default\",";
    payload += "\"motion\":" + String(motionDetected ? "true" : "false") + ",";
    payload += "\"limit\":" + String(limitTriggered ? "true" : "false") + ",";
    payload += "\"timestamp\":" + String(millis() + 1751175280000);
    payload += "}";

    int code = http.PUT(payload);

    if (code > 0)
        Serial.printf("Status sent. Code: %d\n", code);
    else
        Serial.printf("Status send failed. Code: %d\n", code);

    http.end();
}



void initSensors() {

    Wire.begin(19, 18);

    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x6B);
    Wire.write(0);
    Wire.endTransmission(true);

    gpsSerial.begin(9600, SERIAL_8N1, 25, 26);

    pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
    pinMode(BUZZER_PIN, OUTPUT);

    digitalWrite(BUZZER_PIN, LOW);

    Serial.println("Sensors Initialized");
}



void setup() {

    Serial.begin(115200);
    delay(1000);

    WiFi.mode(WIFI_AP_STA);

    startAPMode();

    Serial.println("Waiting for WiFi connection...");
}
void loop() {

    server.handleClient();

    unsigned long currentTime = millis();


    if (WiFi.status() == WL_CONNECTED && !isWiFiConnected) {

        isWiFiConnected = true;

        sendConnectionNotification();

        statusSent = true;

        initSensors();

        systemReady = true;

        Serial.println("System Ready");
    }


    if (!systemReady) return;


    while (gpsSerial.available()) {
        gps.encode(gpsSerial.read());
    }


    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B);
    Wire.endTransmission(false);

    Wire.requestFrom(MPU_ADDR, 6, true);

    AcX = (Wire.read() << 8) | Wire.read();
    AcY = (Wire.read() << 8) | Wire.read();


    bool motionNow =
        abs(AcX) > motionThreshold ||
        abs(AcY) > motionThreshold;


    bool limitSwitchState =
        digitalRead(LIMIT_SWITCH_PIN);


    if (armed &&
        motionNow &&
        !motionSpikeDetected &&
        limitSwitchState == LOW &&
        !limitSwitchTriggered) {

        Serial.println("Motion spike detected and motion detected");

        motionSpikeDetected = true;
        limitSwitchTriggered = true;

        sendMotionOrLimitStatus(true, true);
        sendGPSData(true, true);

        lastSendTime = currentTime;
    }



    if (armed &&
        motionNow &&
        !motionSpikeDetected) {

        Serial.println("Motion spike detected.");

        motionSpikeDetected = true;

        sendMotionOrLimitStatus(true, false);
        sendGPSData(true, false);

        lastSendTime = currentTime;
    }



    if (limitSwitchState == LOW &&
        !limitSwitchTriggered) {

        Serial.println("Limit switch triggered");

        limitSwitchTriggered = true;

        digitalWrite(BUZZER_PIN, HIGH);

        sendMotionOrLimitStatus(false, true);
        sendGPSData(false, true);

        lastLimitSendTime = currentTime;
    }



    if (motionSpikeDetected &&
        limitSwitchTriggered &&
        (currentTime - lastSendTime > sendCooldown)) {

        sendGPSData(true, true);

        lastSendTime = currentTime;
    }
        if (motionSpikeDetected &&
        (currentTime - lastSendTime > sendCooldown)) {

        sendGPSData(true, false);

        lastSendTime = currentTime;
    }



    if (limitSwitchTriggered &&
        (currentTime - lastLimitSendTime > sendCooldown)) {

        sendGPSData(false, true);

        lastLimitSendTime = currentTime;
    }


    delay(100);
}