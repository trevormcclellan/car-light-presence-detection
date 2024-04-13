#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEBeacon.h>
#include <vector>
#include <map>

const gpio_num_t wakePin = GPIO_NUM_4;
const int accPin = GPIO_NUM_1;
const int lightOutput = GPIO_NUM_9;

int lightBrightness = 0;
const int dimDelay = 5;

bool accOn = true;

int scanTime = 5;  // In seconds
BLEScan *pBLEScan;
bool foundBeacon = false;
unsigned long lastBeaconTime = 0;  // Variable to store the last time the beacon was seen
unsigned long powerOffStartTime = 0;

// Replace with your desired AP credentials
const char *ssid = "arduino";
const char *password = "YourPassword";

AsyncWebServer server(80);

// Define the SPIFFS file path to store the text
const char *filePath = "/trackedUUIDs.txt";
const int maxTextLength = 100;  // Adjust the maximum text length as needed

String storedText;
String uuidListHTML;     // HTML to display the list of found UUIDs
String trackedListHTML;  // HTML to display the list of tracked UUIDs

std::vector<String> trackedBeacons;  // Vector to store tracked beacons
std::map<String, int> foundUUIDs;    // Map to store found UUIDs and their RSSI

int minRSSI = -100; // Default minimum RSSI

void fadeLights(bool on) {
  // Increase brightness until it reaches 255
  if (on) {
    while (lightBrightness < 255) {
      lightBrightness++;
      analogWrite(lightOutput, lightBrightness);
      delay(dimDelay);  // Adjust the delay for the desired fading speed
    }
  }

  // Decrease brightness until it reaches 0
  else {
    while (lightBrightness > 0) {
      lightBrightness--;
      analogWrite(lightOutput, lightBrightness);
      delay(dimDelay);  // Adjust the delay for the desired fading speed
    }
  }
}


class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveManufacturerData() == true) {
      std::string strManufacturerData = advertisedDevice.getManufacturerData();
      int rssi = advertisedDevice.getRSSI();

      uint8_t cManufacturerData[100];
      strManufacturerData.copy((char *)cManufacturerData, strManufacturerData.length(), 0);

      if (strManufacturerData.length() == 25 && cManufacturerData[0] == 0x4C && cManufacturerData[1] == 0x00) {
        BLEBeacon oBeacon = BLEBeacon();
        oBeacon.setData(strManufacturerData);
        String UUID = oBeacon.getProximityUUID().toString().c_str();

        // Check if the UUID is already in the map
        auto it = foundUUIDs.find(UUID);
        if (it != foundUUIDs.end()) {
          // Update the RSSI for the existing UUID
          it->second = rssi;
        } else {
          // Add the new UUID to the map and the list
          foundUUIDs[UUID] = rssi;
          uuidListHTML += "<li>" + UUID + " (RSSI: " + String(rssi) + ") <button onclick=\"addToTracked('" + UUID + "')\">Add to Tracked</button></li>";
        }

        // Check if the UUID is in the list of tracked beacons and RSSI is above the minimum threshold
        if (std::find(trackedBeacons.begin(), trackedBeacons.end(), UUID) != trackedBeacons.end() && rssi >= minRSSI) {
          if (!foundBeacon) {  // if the beacon wasn't already found
            fadeLights(true);
          }
          foundBeacon = true;                          // set foundBeacon flag to true
          lastBeaconTime = millis();                   // Update the last beacon seen time
        }
      }
    }
  }
};

void saveMinRSSI() {
  File file = SPIFFS.open("/minRSSI.txt", "w");
  if (file) {
    file.println(minRSSI);
    file.close();
  }
}

void loadMinRSSI() {
  File file = SPIFFS.open("/minRSSI.txt", "r");
  if (file) {
    minRSSI = file.readStringUntil('\n').toInt();
    file.close();
  }
}

void setup() {
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();  // create a new scan
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);  // active scan uses more power, but gets results faster

  // Start AP
  WiFi.softAP(ssid, password);

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    return;
  }

  // Load tracked UUIDs from SPIFFS
  loadTrackedUUIDs();

  
  // Load minimum RSSI from SPIFFS
  loadMinRSSI();

  // Define web server routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    handleRoot(request);
  });

  server.on("/addToTracked", HTTP_GET, [](AsyncWebServerRequest *request) {
    handleAddToTracked(request);
  });

  server.on("/removeFromTracked", HTTP_GET, [](AsyncWebServerRequest *request) {
    handleRemoveFromTracked(request);
  });

  server.on("/setMinRSSI", HTTP_POST, [](AsyncWebServerRequest *request) {
    handleSetMinRSSI(request);
  });

  server.begin();

  pinMode(accPin, INPUT);
  pinMode(lightOutput, OUTPUT);
  esp_sleep_enable_ext0_wakeup(wakePin, LOW);
}

void loop() {
  if (digitalRead(accPin) == HIGH) {
    if (accOn) {  // If the acc power was just turned off
      powerOffStartTime = millis();
      // Disable the web server and AP
      server.end();
      WiFi.softAPdisconnect(true);
    }
    accOn = false;
  } else {
    digitalWrite(LED_BUILTIN, LOW);
    if (!accOn) { // If the acc power was just turned on
      // Re-enable the web server and AP
      server.begin();
      WiFi.softAP(ssid, password);
    }
    accOn = true;
    powerOffStartTime = 0;
  }

  // If acc power has been off for 5 minutes, go into deep sleep
  if (powerOffStartTime && millis() - powerOffStartTime > 5 * 60 * 1000) {
    powerOffStartTime = 0;
    delay(100);
    esp_deep_sleep_start();
  }

  foundUUIDs.clear();

  BLEDevice::getScan()->start(scanTime);

  // Check if the beacon hasn't been seen in 0.5 minutes
  if (foundBeacon && millis() - lastBeaconTime > 0.5 * 60 * 1000) {
    foundBeacon = false;
    fadeLights(false);
  }

  delay(15000);  // 15 seconds delay
}

void handleRoot(AsyncWebServerRequest *request) {
  String html = "<html><head><meta http-equiv='refresh' content='10'></head><body>";

  // Display the list of found UUIDs with their RSSI and a button to add to tracked
  html += "<h2>Found UUIDs:</h2><ul>";
  for (const auto &entry : foundUUIDs) {
    html += "<li>" + entry.first + " (RSSI: " + String(entry.second) + ") <button onclick=\"addToTracked('" + entry.first + "')\">Add to Tracked</button></li>";
  }
  html += "</ul>";

  // Display the list of tracked UUIDs with a button to remove each
  html += "<h2>Tracked UUIDs:</h2><ul>";
  for (const auto &uuid : trackedBeacons) {
    html += "<li>" + uuid + " <button onclick=\"removeFromTracked('" + uuid + "')\">Remove from Tracked</button></li>";
  }
  html += "</ul>";

  // Input field to set the minimum RSSI
  html += "<h2>Set Minimum RSSI:</h2>";
  html += "<form action='/setMinRSSI' method='post'>";
  html += "<input type='number' name='minRSSI' value='" + String(minRSSI) + "'><br>";
  html += "<input type='submit' value='Set Minimum RSSI'>";
  html += "</form>";

  // JavaScript function to add a UUID to the tracked list
  html += "<script>"
          "function addToTracked(uuid) {"
          "   fetch('/addToTracked?uuid=' + uuid);"
          "   location.reload();"
          "}"
          "</script>";

  // JavaScript function to remove a UUID from the tracked list
  html += "<script>"
          "function removeFromTracked(uuid) {"
          "   fetch('/removeFromTracked?uuid=' + uuid);"
          "   location.reload();"
          "}"
          "</script>";

  html += "</body></html>";

  request->send(200, "text/html", html);
}

void handleAddToTracked(AsyncWebServerRequest *request) {
  if (request->hasArg("uuid")) {
    String uuid = request->arg("uuid");
    // Check if the UUID is not already in the list
    auto it = std::find(trackedBeacons.begin(), trackedBeacons.end(), uuid);
    if (it == trackedBeacons.end()) {
      // Add the UUID to the list
      trackedBeacons.push_back(uuid);
    }
    // Save the tracked UUIDs to SPIFFS
    saveTrackedUUIDs();
    request->send(200, "text/plain", "UUID added to tracked list.");
  } else {
    request->send(400, "text/plain", "Bad Request");
  }
}

void handleRemoveFromTracked(AsyncWebServerRequest *request) {
  if (request->hasArg("uuid")) {
    String uuid = request->arg("uuid");
    // Check if the UUID is in the list
    auto it = std::find(trackedBeacons.begin(), trackedBeacons.end(), uuid);
    if (it != trackedBeacons.end()) {
      // Remove the UUID from the list
      trackedBeacons.erase(it);
    }
    // Save the tracked UUIDs to SPIFFS
    saveTrackedUUIDs();
    request->send(200, "text/plain", "UUID removed from tracked list.");
  } else {
    request->send(400, "text/plain", "Bad Request");
  }
}

void handleSetMinRSSI(AsyncWebServerRequest *request) {
  if (request->hasArg("minRSSI")) {
    minRSSI = request->arg("minRSSI").toInt();
    saveMinRSSI();
    request->send(200, "text/plain", "Minimum RSSI set.");
  } else {
    request->send(400, "text/plain", "Bad Request");
  }
}

void saveTrackedUUIDs() {
  File file = SPIFFS.open(filePath, "w");
  if (file) {
    for (const auto &uuid : trackedBeacons) {
      file.println(uuid);
    }
    file.close();
  }
}

void loadTrackedUUIDs() {
  File file = SPIFFS.open(filePath, "r");
  if (file) {
    while (file.available()) {
      String uuid = file.readStringUntil('\n');
      uuid.trim();
      if (!uuid.isEmpty()) {
        trackedBeacons.push_back(uuid);
      }
    }
    file.close();
  }
}