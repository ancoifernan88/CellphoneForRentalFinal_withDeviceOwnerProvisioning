/*
  ANGKOY CELLPHONE FOR RENT
  ESP8266 / ESPino ESP-12 MODULE

  WIFI:
    SSID: AngkoyCellphoneForRent
    PASSWORD: 00000000
    IP: 192.168.1.110

  UDP PORT: 8888

  ADMIN:
    USERNAME: admin
    PASSWORD: 2580

  HARDWARE:
    D5 / GPIO14 = COIN INPUT (ACTIVE HIGH)
    D1 / GPIO5  = RELAY (ACTIVE LOW)

  PAYMENT:
    REGISTER|phoneID|token
    PAY|phoneID|token

  RESPONSES:
    REGISTERED|phoneID
    PAYMENT_GRANTED|transaction
    COIN_UPDATE|transaction|pulses|15
    ADD_PULSES|phoneID|pulses|transaction
    PAYMENT_COMPLETE|transaction|pulses
    PAYMENT_CANCELLED|transaction

  Android calculates rental minutes.
  ESP8266 only counts and reports coin pulses.
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <stdlib.h>

// ==================================================
// HARDWARE
// ==================================================

#define COIN_PIN 14
#define RELAY_PIN 5

#define RELAY_ON LOW
#define RELAY_OFF HIGH

// ==================================================
// NETWORK
// ==================================================

IPAddress localIP(192, 168, 1, 110);
IPAddress gateway(192, 168, 1, 110);
IPAddress subnet(255, 255, 255, 0);

const unsigned int UDP_PORT = 8888;

WiFiUDP udp;
WiFiServer portalServer(80);

const char DEVICE_TOKEN[] = "ANGKOY-CHANGE-ME";

// ==================================================
// DEFAULT SETTINGS
// ==================================================

const char DEFAULT_SSID[] = "AngkoyCellphoneForRent";
const char DEFAULT_WIFI_PASSWORD[] = "00000000";
const char DEFAULT_ADMIN_PASSWORD[] = "2580";
const char ADMIN_USERNAME[] = "admin";

// ==================================================
// PAYMENT SETTINGS
// ==================================================

const byte MAX_DEVICES = 8;

const unsigned long PAYMENT_TIMEOUT = 15000UL;
const unsigned long REGISTRATION_TIMEOUT = 30000UL;
const unsigned long PAYMENT_COOLDOWN = 5000UL;

// ==================================================
// EEPROM
// ==================================================

#define EEPROM_SIZE 512
#define CONFIG_MAGIC 0xA67B

struct ConfigData {
  unsigned int magic;
  char ssid[33];
  char wifiPassword[64];
  char adminPassword[33];
};

ConfigData config;

// ==================================================
// REGISTERED PHONES
// ==================================================

struct PhoneDevice {
  String phoneID;
  IPAddress ip;
  unsigned int port;
  unsigned long lastRequest;
  bool active;
};

PhoneDevice devices[MAX_DEVICES];

// ==================================================
// PAYMENT STATE
// ==================================================

bool paymentActive = false;

String paymentOwner = "";
String paymentTransaction = "";

IPAddress paymentIP;
unsigned int paymentPort = 0;

unsigned long lastCoinAt = 0;
unsigned long lastPaymentAt = 0;

bool paymentCooldownStarted = false;

// ==================================================
// COIN COUNTER
// ==================================================

// The ISR only increments the counter.
// No micros(), no noInterrupts(), and no string operations
// inside the interrupt routine.

volatile unsigned long paymentPulseCount = 0;
volatile bool coinCountingEnabled = false;

unsigned long lastReportedPulses = 0;

// ==================================================
// PORTAL RESTART
// ==================================================

bool restartPending = false;
unsigned long restartAt = 0;

// ==================================================
// COIN INTERRUPT
// ==================================================

void ICACHE_RAM_ATTR coinPulseISR() {
  if (coinCountingEnabled) {
    paymentPulseCount++;
  }
}

// ==================================================
// EEPROM FUNCTIONS
// ==================================================

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, config);

  if (config.magic != CONFIG_MAGIC) {

    config.magic = CONFIG_MAGIC;

    memset(config.ssid, 0, sizeof(config.ssid));
    memset(config.wifiPassword, 0, sizeof(config.wifiPassword));
    memset(config.adminPassword, 0, sizeof(config.adminPassword));

    strncpy(config.ssid, DEFAULT_SSID, 32);

    strncpy(config.wifiPassword,
            DEFAULT_WIFI_PASSWORD, 63);

    strncpy(config.adminPassword,
            DEFAULT_ADMIN_PASSWORD, 32);

    EEPROM.put(0, config);
    EEPROM.commit();
  }

  config.ssid[32] = '\0';
  config.wifiPassword[63] = '\0';
  config.adminPassword[32] = '\0';
}

void saveConfig() {
  config.magic = CONFIG_MAGIC;
  EEPROM.put(0, config);
  EEPROM.commit();
}

// ==================================================
// UDP FUNCTIONS
// ==================================================

void sendUDP(IPAddress ip, unsigned int port, String message) {
  udp.beginPacket(ip, port);
  udp.write((const uint8_t*)message.c_str(), message.length());
  udp.endPacket();
}

void sendToPaymentOwner(String message) {
  if (paymentOwner.length() == 0) return;

  sendUDP(paymentIP, paymentPort, message);
}

// ==================================================
// PHONE REGISTRATION
// ==================================================

int findDevice(String phoneID) {

  for (byte i = 0; i < MAX_DEVICES; i++) {

    if (devices[i].active &&
        devices[i].phoneID == phoneID) {
      return i;
    }
  }

  return -1;
}

void removeDevice(String phoneID) {

  int index = findDevice(phoneID);

  if (index < 0) return;

  devices[index].phoneID = "";
  devices[index].ip = IPAddress(0, 0, 0, 0);
  devices[index].port = 0;
  devices[index].lastRequest = 0;
  devices[index].active = false;

  Serial.println("Registration removed: " + phoneID);
}

void cleanupExpiredRegistrations() {

  unsigned long now = millis();

  for (byte i = 0; i < MAX_DEVICES; i++) {

    if (!devices[i].active) continue;

    if (paymentActive &&
        devices[i].phoneID == paymentOwner) {
      continue;
    }

    if (now - devices[i].lastRequest >=
        REGISTRATION_TIMEOUT) {

      String expiredID = devices[i].phoneID;
      removeDevice(expiredID);
    }
  }
}

void registerPhone(
  String phoneID,
  String token,
  IPAddress senderIP,
  unsigned int senderPort
) {

  if (token != DEVICE_TOKEN) {
    sendUDP(senderIP, senderPort, "INVALID_TOKEN");
    return;
  }

  if (phoneID.length() == 0 ||
      phoneID.length() > 32) {

    sendUDP(senderIP, senderPort, "INVALID_PHONE_ID");
    return;
  }

  int index = findDevice(phoneID);

  if (index >= 0) {

    devices[index].ip = senderIP;
    devices[index].port = senderPort;
    devices[index].lastRequest = millis();

    sendUDP(senderIP, senderPort,
            "REGISTERED|" + phoneID);

    return;
  }

  for (byte i = 0; i < MAX_DEVICES; i++) {

    if (!devices[i].active) {

      devices[i].phoneID = phoneID;
      devices[i].ip = senderIP;
      devices[i].port = senderPort;
      devices[i].lastRequest = millis();
      devices[i].active = true;

      sendUDP(senderIP, senderPort,
              "REGISTERED|" + phoneID);

      Serial.println("Phone registered: " + phoneID);

      return;
    }
  }

  sendUDP(senderIP, senderPort, "REGISTRATION_FULL");
}

// ==================================================
// START PAYMENT
// ==================================================

void startPayment(
  String phoneID,
  String token,
  IPAddress senderIP,
  unsigned int senderPort
) {

  if (token != DEVICE_TOKEN) {
    sendUDP(senderIP, senderPort, "INVALID_TOKEN");
    return;
  }

  int index = findDevice(phoneID);

  if (index < 0) {
    sendUDP(senderIP, senderPort, "NOT_REGISTERED");
    return;
  }

  if (devices[index].ip != senderIP) {
    sendUDP(senderIP, senderPort, "IP_MISMATCH");
    return;
  }

  // Duplicate PAY request from current payer.
  if (paymentActive && phoneID == paymentOwner) {

    sendUDP(senderIP, senderPort,
            "PAYMENT_GRANTED|" + paymentTransaction);

    return;
  }

  // Another phone is paying.
  if (paymentActive) {

    sendUDP(senderIP, senderPort,
            "PAYMENT_BUSY|" + paymentOwner);

    removeDevice(phoneID);

    return;
  }

  // Cooldown.
  if (paymentCooldownStarted &&
      millis() - lastPaymentAt < PAYMENT_COOLDOWN) {

    sendUDP(senderIP, senderPort, "REQUEST_COOLDOWN");

    removeDevice(phoneID);

    return;
  }

  devices[index].port = senderPort;
  devices[index].lastRequest = millis();

  paymentOwner = phoneID;
  paymentIP = senderIP;
  paymentPort = senderPort;

  paymentTransaction = "TX" + String(millis());

  lastCoinAt = millis();

  paymentPulseCount = 0;
  lastReportedPulses = 0;

  coinCountingEnabled = true;
  paymentActive = true;

  // Unlock coin slot.
  digitalWrite(RELAY_PIN, RELAY_ON);

  sendToPaymentOwner(
    "PAYMENT_GRANTED|" + paymentTransaction
  );

  Serial.println("==============================");
  Serial.println("PAYMENT STARTED");
  Serial.println("PHONE: " + paymentOwner);
  Serial.println("TX: " + paymentTransaction);
  Serial.println("==============================");
}

// ==================================================
// FINISH PAYMENT
// ==================================================

void finishPayment() {

  if (!paymentActive) return;

  // Stop counting before reading the final count.
  coinCountingEnabled = false;

  unsigned long finalPulses = paymentPulseCount;

  String owner = paymentOwner;
  String transaction = paymentTransaction;

  Serial.println("==============================");
  Serial.println("PAYMENT FINISHED");
  Serial.println("PHONE: " + owner);
  Serial.println("PULSES: " + String(finalPulses));
  Serial.println("==============================");

  if (finalPulses > 0) {

    sendToPaymentOwner(
      "ADD_PULSES|" +
      owner + "|" +
      String(finalPulses) + "|" +
      transaction
    );

    sendToPaymentOwner(
      "PAYMENT_COMPLETE|" +
      transaction + "|" +
      String(finalPulses)
    );

  } else {

    sendToPaymentOwner(
      "PAYMENT_CANCELLED|" + transaction
    );
  }

  lastPaymentAt = millis();
  paymentCooldownStarted = true;

  paymentActive = false;

  // Automatically remove registration.
  removeDevice(owner);

  paymentOwner = "";
  paymentTransaction = "";
  paymentIP = IPAddress(0, 0, 0, 0);
  paymentPort = 0;

  lastReportedPulses = 0;

  // Lock coin slot.
  digitalWrite(RELAY_PIN, RELAY_OFF);
}

// ==================================================
// MONITOR COIN PULSES
// ==================================================

void checkCoinSlot() {

  if (!paymentActive) return;

  unsigned long currentPulses = paymentPulseCount;

  if (currentPulses != lastReportedPulses) {

    lastReportedPulses = currentPulses;

    lastCoinAt = millis();

    sendToPaymentOwner(
      "COIN_UPDATE|" +
      paymentTransaction + "|" +
      String(currentPulses) + "|15"
    );

    Serial.println(
      "COIN PULSES: " + String(currentPulses)
    );
  }
}

// ==================================================
// PAYMENT TIMEOUT
// ==================================================

void checkPaymentTimeout() {

  if (!paymentActive) return;

  if (millis() - lastCoinAt >= PAYMENT_TIMEOUT) {
    finishPayment();
  }
}

// ==================================================
// UDP COMMAND PARSER
// ==================================================

String getField(String data, byte fieldNumber) {

  int start = 0;

  for (byte i = 0; i < fieldNumber; i++) {

    int separator = data.indexOf('|', start);

    if (separator < 0) return "";

    start = separator + 1;
  }

  int end = data.indexOf('|', start);

  if (end < 0) {
    return data.substring(start);
  }

  return data.substring(start, end);
}

void receiveCommands() {

  int packetSize = udp.parsePacket();

  if (packetSize <= 0) return;

  char packetBuffer[256];

  int len = udp.read(
    packetBuffer,
    sizeof(packetBuffer) - 1
  );

  if (len <= 0) return;

  packetBuffer[len] = '\0';

  String command = String(packetBuffer);
  command.trim();

  IPAddress senderIP = udp.remoteIP();
  unsigned int senderPort = udp.remotePort();

  String action = getField(command, 0);
  String phoneID = getField(command, 1);
  String token = getField(command, 2);

  Serial.println("UDP: " + command);

  if (action == "REGISTER") {

    registerPhone(
      phoneID,
      token,
      senderIP,
      senderPort
    );

  } else if (action == "PAY") {

    startPayment(
      phoneID,
      token,
      senderIP,
      senderPort
    );
  }
}

// ==================================================
// BASE64 DECODER
// No strtol() or strtoul() used.
// ==================================================

int base64Value(char c) {

  if (c >= 'A' && c <= 'Z') return c - 'A';

  if (c >= 'a' && c <= 'z') return c - 'a' + 26;

  if (c >= '0' && c <= '9') return c - '0' + 52;

  if (c == '+') return 62;

  if (c == '/') return 63;

  return -1;
}

String base64Decode(String input) {

  String output = "";

  int value = 0;
  int bits = -8;

  for (unsigned int i = 0; i < input.length(); i++) {

    int decoded = base64Value(input[i]);

    if (decoded < 0) continue;

    value = (value << 6) + decoded;
    bits += 6;

    if (bits >= 0) {

      output += (char)((value >> bits) & 0xFF);

      bits -= 8;
    }
  }

  return output;
}

bool checkAdminAuth(String authorization) {

  String prefix = "Basic ";

  if (!authorization.startsWith(prefix)) {
    return false;
  }

  String encoded = authorization.substring(prefix.length());

  String decoded = base64Decode(encoded);

  String expected =
    String(ADMIN_USERNAME) + ":" +
    String(config.adminPassword);

  return decoded == expected;
}

// ==================================================
// MANUAL HEX CONVERSION
// ==================================================

int hexToValue(char c) {

  if (c >= '0' && c <= '9') return c - '0';

  if (c >= 'A' && c <= 'F') return c - 'A' + 10;

  if (c >= 'a' && c <= 'f') return c - 'a' + 10;

  return -1;
}

String urlDecode(String input) {

  String output = "";

  for (unsigned int i = 0; i < input.length(); i++) {

    char c = input[i];

    if (c == '+') {

      output += ' ';

    } else if (c == '%' && i + 2 < input.length()) {

      int high = hexToValue(input[i + 1]);
      int low = hexToValue(input[i + 2]);

      if (high >= 0 && low >= 0) {

        output += (char)(high * 16 + low);

        i += 2;

      } else {

        output += c;
      }

    } else {

      output += c;
    }
  }

  return output;
}

// ==================================================
// FORM VALUE READER
// ==================================================

String getFormValue(String body, String key) {

  String search = key + "=";

  int start = body.indexOf(search);

  if (start < 0) return "";

  start += search.length();

  int end = body.indexOf('&', start);

  if (end < 0) {
    end = body.length();
  }

  return urlDecode(body.substring(start, end));
}

// ==================================================
// HOME PAGE
// ==================================================

String homePage() {

  String page = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Angkoy Cellphone For Rent</title>
<style>
body {
  background:#101827;
  color:white;
  font-family:Arial;
  text-align:center;
  padding:25px;
}
.card {
  background:#1d2b42;
  padding:25px;
  border-radius:15px;
  max-width:400px;
  margin:auto;
}
a {
  color:#62c5ff;
  text-decoration:none;
}
</style>
</head>
<body>
<div class="card">
<h2>ANGKOY CELLPHONE FOR RENT</h2>
<p>Welcome!</p>
<p>Connect your rental application to begin.</p>
<p><a href="/admin">Administrator Login</a></p>
</div>
</body>
</html>
)HTML";

  return page;
}

// ==================================================
// ADMIN PAGE
// ==================================================

String adminPage() {

  String page = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Angkoy Admin</title>
<style>
body {
  background:#101827;
  color:white;
  font-family:Arial;
  padding:20px;
}
.card {
  background:#1d2b42;
  padding:20px;
  border-radius:15px;
  max-width:430px;
  margin:auto;
}
input {
  width:100%;
  box-sizing:border-box;
  padding:12px;
  margin:8px 0 18px;
  border-radius:6px;
  border:0;
}
button {
  width:100%;
  padding:13px;
  background:#1689d8;
  color:white;
  border:0;
  border-radius:6px;
  font-size:16px;
}
label {
  display:block;
  text-align:left;
}
</style>
</head>
<body>
<div class="card">
<h2>ANGKOY ADMIN SETTINGS</h2>

<form method="POST" action="/save">

<label>Wi-Fi SSID</label>
<input name="ssid" maxlength="32" required value="
)HTML";

  page += String(config.ssid);

  page += R"HTML(">

<label>Wi-Fi Password</label>
<input name="wifi" type="password" minlength="8"
maxlength="63" required value="
)HTML";

  page += String(config.wifiPassword);

  page += R"HTML(">

<label>New Admin Password</label>
<input name="admin" type="password" minlength="4"
maxlength="32" placeholder="Leave blank to keep current password">

<button type="submit">SAVE SETTINGS</button>
</form>

<p>ESP IP: 192.168.1.110</p>
<p>UDP Port: 8888</p>
<p>Maximum temporary registrations: 8</p>
<p>Unused registrations expire after 30 seconds.</p>

</div>
</body>
</html>
)HTML";

  return page;
}

// ==================================================
// HTTP RESPONSE
// ==================================================

void sendHttpResponse(
  WiFiClient &client,
  int status,
  String contentType,
  String body
) {

  String statusText = "OK";

  if (status == 400) statusText = "Bad Request";

  if (status == 401) statusText = "Unauthorized";

  if (status == 404) statusText = "Not Found";

  if (status == 409) statusText = "Conflict";

  client.print("HTTP/1.1 ");
  client.print(status);
  client.print(" ");
  client.println(statusText);

  client.println("Connection: close");
  client.println("Cache-Control: no-store");

  client.println("Content-Type: " + contentType);

  client.print("Content-Length: ");
  client.println(body.length());

  client.println();

  client.print(body);
}

// ==================================================
// SAVE ADMIN SETTINGS
// ==================================================

void handleSave(
  WiFiClient &client,
  String authorization,
  String body
) {

  if (!checkAdminAuth(authorization)) {

    client.println("HTTP/1.1 401 Unauthorized");
    client.println("WWW-Authenticate: Basic realm=\"Angkoy Admin\"");
    client.println("Connection: close");
    client.println();

    return;
  }

  if (paymentActive) {

    sendHttpResponse(
      client,
      409,
      "text/plain",
      "Payment is active. Try again later."
    );

    return;
  }

  String newSSID = getFormValue(body, "ssid");

  String newWiFi = getFormValue(body, "wifi");

  String newAdmin = getFormValue(body, "admin");

  if (newSSID.length() < 1 ||
      newSSID.length() > 32) {

    sendHttpResponse(
      client, 400, "text/plain",
      "SSID must contain 1 to 32 characters."
    );

    return;
  }

  if (newWiFi.length() < 8 ||
      newWiFi.length() > 63) {

    sendHttpResponse(
      client, 400, "text/plain",
      "Wi-Fi password must contain 8 to 63 characters."
    );

    return;
  }

  if (newAdmin.length() > 0 &&
      (newAdmin.length() < 4 ||
       newAdmin.length() > 32)) {

    sendHttpResponse(
      client, 400, "text/plain",
      "Admin password must contain 4 to 32 characters."
    );

    return;
  }

  memset(config.ssid, 0, sizeof(config.ssid));

  memset(config.wifiPassword, 0,
         sizeof(config.wifiPassword));

  strncpy(config.ssid, newSSID.c_str(), 32);

  strncpy(config.wifiPassword, newWiFi.c_str(), 63);

  if (newAdmin.length() > 0) {

    memset(config.adminPassword, 0,
           sizeof(config.adminPassword));

    strncpy(config.adminPassword, newAdmin.c_str(), 32);
  }

  saveConfig();

  String response = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
</head>
<body style="font-family:Arial;text-align:center;padding:30px">
<h2>Settings Saved</h2>
<p>ESP8266 is restarting.</p>
<p>Reconnect using the new Wi-Fi name and password.</p>
</body>
</html>
)HTML";

  sendHttpResponse(
    client, 200, "text/html", response
  );

  restartPending = true;
  restartAt = millis() + 1500UL;
}

// ==================================================
// WEB PORTAL
// ==================================================

void handlePortal() {

  WiFiClient client = portalServer.available();

  if (!client) return;

  client.setTimeout(1500);

  String requestLine = client.readStringUntil('\n');
  requestLine.trim();

  if (requestLine.length() == 0) {
    client.stop();
    return;
  }

  String method = "";
  String path = "";

  int firstSpace = requestLine.indexOf(' ');

  int secondSpace =
    requestLine.indexOf(' ', firstSpace + 1);

  if (firstSpace > 0 && secondSpace > firstSpace) {

    method = requestLine.substring(0, firstSpace);

    path = requestLine.substring(
      firstSpace + 1,
      secondSpace
    );
  }

  String authorization = "";

  int contentLength = 0;

  // Read HTTP headers.
  while (client.connected()) {

    String header = client.readStringUntil('\n');

    header.trim();

    if (header.length() == 0) break;

    String lowerHeader = header;
    lowerHeader.toLowerCase();

    if (lowerHeader.startsWith("authorization:")) {

      authorization = header.substring(14);
      authorization.trim();
    }

    if (lowerHeader.startsWith("content-length:")) {

      int colon = header.indexOf(':');

      contentLength =
        header.substring(colon + 1).toInt();
    }
  }

  if (contentLength < 0 || contentLength > 512) {

    sendHttpResponse(
      client, 400, "text/plain", "Invalid request size."
    );

    client.stop();
    return;
  }

  // Read POST body.
  String body = "";

  if (method == "POST" && contentLength > 0) {

    unsigned long startTime = millis();

    while ((int)body.length() < contentLength &&
           millis() - startTime < 2000UL) {

      while (client.available() &&
             (int)body.length() < contentLength) {

        body += (char)client.read();
      }

      yield();
    }
  }

  // Remove URL query parameters.
  int queryPosition = path.indexOf('?');

  if (queryPosition >= 0) {
    path = path.substring(0, queryPosition);
  }

  // HOME PAGE.
  if (method == "GET" && path == "/") {

    sendHttpResponse(
      client, 200, "text/html", homePage()
    );
  }

  // ADMIN PAGE.
  else if (method == "GET" && path == "/admin") {

    if (!checkAdminAuth(authorization)) {

      client.println("HTTP/1.1 401 Unauthorized");
      client.println("WWW-Authenticate: Basic realm=\"Angkoy Admin\"");
      client.println("Content-Type: text/plain");
      client.println("Connection: close");
      client.println();
      client.println("Administrator login required.");

      client.stop();
      return;
    }

    sendHttpResponse(
      client, 200, "text/html", adminPage()
    );
  }

  // SAVE SETTINGS.
  else if (method == "POST" && path == "/save") {

    handleSave(
      client,
      authorization,
      body
    );
  }

  // UNKNOWN PAGE.
  else {

    sendHttpResponse(
      client, 404, "text/plain", "Page not found."
    );
  }

  client.stop();
}

// ==================================================
// SETUP
// ==================================================

void setup() {

  Serial.begin(115200);

  delay(200);

  Serial.println();
  Serial.println("================================");
  Serial.println("ANGKOY CELLPHONE FOR RENT");
  Serial.println("ESP8266 STARTING");
  Serial.println("================================");

  // Relay locked at startup.
  pinMode(RELAY_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, RELAY_OFF);

  // Coin input active HIGH.
  pinMode(COIN_PIN, INPUT);

  // Count rising edges.
  attachInterrupt(
    digitalPinToInterrupt(COIN_PIN),
    coinPulseISR,
    RISING
  );

  coinCountingEnabled = false;
  paymentPulseCount = 0;

  // Clear phone registrations.
  for (byte i = 0; i < MAX_DEVICES; i++) {

    devices[i].phoneID = "";
    devices[i].ip = IPAddress(0, 0, 0, 0);
    devices[i].port = 0;
    devices[i].lastRequest = 0;
    devices[i].active = false;
  }

  // Load saved settings.
  loadConfig();

  // Start standalone Wi-Fi.
  WiFi.mode(WIFI_AP);

  WiFi.softAPConfig(
    localIP,
    gateway,
    subnet
  );

  bool apStarted = WiFi.softAP(
    config.ssid,
    config.wifiPassword
  );

  if (apStarted) {
    Serial.println("Wi-Fi AP started.");
  } else {
    Serial.println("Wi-Fi AP FAILED!");
  }

  Serial.print("SSID: ");
  Serial.println(config.ssid);

  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  // Start UDP.
  udp.begin(UDP_PORT);

  Serial.print("UDP PORT: ");
  Serial.println(UDP_PORT);

  // Start web portal.
  portalServer.begin();

  Serial.println("WEB PORTAL STARTED");
  Serial.println("ADMIN USERNAME: admin");
  Serial.println("ADMIN PASSWORD: 2580");
  Serial.println("================================");
}

// ==================================================
// MAIN LOOP
// ==================================================

void loop() {

  checkCoinSlot();

  receiveCommands();

  checkPaymentTimeout();

  cleanupExpiredRegistrations();

  handlePortal();

  if (restartPending &&
      (long)(millis() - restartAt) >= 0) {

    ESP.restart();
  }

  yield();
}