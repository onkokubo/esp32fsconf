#include <WiFiClientSecure.h>
#include <ssl_client.h>
#include <PubSubClient.h>


#include <Wire.h>
#include <BME280I2C.h>

#include <ArduinoJson.h>

#include <FS.h>
#include "SPIFFS.h"

#define SERIAL_BAUD 115200


#define CONFIG_FILE "/config.json"
#define CA_CERT_FILE "/ca.pem"
#define KEY_FILE "/device.key.pem"
#define CERT_FILE "/device.crt.pem"

const char* MQTT_HOST = "";
int MQTT_PORT = 8883;
const char* MQTT_DEVICEID = "";
const char* MQTT_USER = "";
const char* MQTT_PASS = "";
const char* MQTT_TOPIC_EVT = "iot-2/evt/status/fmt/json";
const char* MQTT_TOPIC_CMD = "iot-2/evt/cmd/fmt/json";
int TZ_OFFSET = 0;
int TZ_DST = 0;

StaticJsonDocument<44000> configJson;
JsonObject networks;
StaticJsonDocument<100> jsonDoc;
JsonObject payload = jsonDoc.to<JsonObject>();
JsonObject status = payload.createNestedObject("d");
StaticJsonDocument<100> jsonReceiveDoc;
static char msg[80];

void callback(char* topic, byte* payload, unsigned int length);
//WiFiClient wifiClient;
WiFiClientSecure wifiClient;
PubSubClient mqtt;

String device_key = "";
String device_cert = "";
String ca_cert = "";


float h = 0.0;
float t = 0.0;
float p = 0.0;
boolean ignoreBmeError = true;

BME280I2C bme;

void setup() {
  SPIFFS.begin();
  // put your setup code here, to run once:
  Serial.begin(SERIAL_BAUD);
  
  Wire.begin();
  WiFi.mode(WIFI_STA);

  loadConfiguration();

  Serial.println("\nSelecting network.");
  selectNetwork();
  Serial.println("\nConnecting MQTT client.");
  mqtt = PubSubClient(MQTT_HOST, MQTT_PORT, callback, wifiClient);

  

  Serial.println("\nLoading TLS files.");
  device_key = readFile(KEY_FILE);
  device_cert = readFile(CERT_FILE);
  ca_cert = readFile(CA_CERT_FILE);
  
  Serial.println("\nSetting up security.");
  setupSecurity();

  
  Serial.println("\nStarting BME280 sensor on I2C Bus.");
  setupBME280Sensor();

  Serial.println("\nConnecting MQTT client.");
  connectMQTT();
}

void loop() {
  // put your main code here, to run repeatedly:
  
  printBME280Data(&Serial);
  
  mqttReconnect();
  
  float temp(NAN), hum(NAN), pres(NAN);
  BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
  BME280::PresUnit presUnit(BME280::PresUnit_Pa);
  bme.read(pres, temp, hum, tempUnit, presUnit);
  h = hum;
  t = temp;
  p = pres;
  if (!ignoreBmeError & (isnan(h) || isnan(t))) {
    Serial.println("Failed to read from BME280 sensor!");
  } else {

    // Send data to Watson IoT Platform
    status["temp"] = t;
    status["humidity"] = h;
    status["pressure"] = p;
    serializeJson(jsonDoc, msg, 80);
    Serial.print(MQTT_TOPIC_EVT);
    Serial.print(" ");
    Serial.println(msg);
    if (!mqtt.publish(MQTT_TOPIC_EVT, msg)) {
      Serial.println("MQTT Publish failed");
    }
  }
  // Pause - but keep polling MQTT for incoming messages
  for (int i = 0; i < 120; i++) {
    mqtt.loop();
    delay(1000);
  }
  
}

void loadConfiguration() {
  File file = SPIFFS.open(CONFIG_FILE, "r");
  if (!file){
    Serial.println("Configuration file not found.");
  } else {
    size_t size = file.size();
    if ( size == 0 ) {
      Serial.println("Configuration file is empty.");
    } else {
      std::unique_ptr<char[]> buf (new char[size]);
      auto error =  deserializeJson(configJson, file);
      if (error) {
        Serial.println("Couldn't parse JSON from config file.");
      } else {
        Serial.println("Configuration loaded;");
      }
    }
    file.close();
  }
  MQTT_HOST = configJson["mqtt_server"].as<char*>();
  MQTT_PORT = configJson["mqtt_port"].as<int>();
  MQTT_DEVICEID = configJson["mqtt_devid"].as<char*>();
  MQTT_USER = configJson["mqtt_devid"].as<char*>();
  MQTT_PASS = configJson["mqtt_pass"].as<char*>();
  MQTT_TOPIC_EVT = configJson["mqtt_topic_evt"].as<char*>();
  MQTT_TOPIC_CMD = configJson["mqtt_topic_cmd"].as<char*>();
  TZ_OFFSET = configJson["tz_offset"].as<int>();
  TZ_DST = configJson["tz_dst"].as<int>();
  networks = configJson["networks"].as<JsonObject>();

  Serial.println("\nLoaded config:");
  Serial.println("MQTT:");
  Serial.print("  server:      ");
  Serial.println(MQTT_HOST);
  Serial.print("  port:        ");
  Serial.println(MQTT_PORT);
  Serial.print("  device ID:   ");
  Serial.println(MQTT_DEVICEID);
  Serial.print("  user:        ");
  Serial.println(MQTT_USER);
  Serial.print("  password:    ");
  Serial.println(MQTT_PASS);
  Serial.print("  event topic: ");
  Serial.println(MQTT_TOPIC_EVT);
  Serial.print("  command topic: ");
  Serial.println(MQTT_TOPIC_CMD);
  Serial.print("Timezone hours offset: ");
  Serial.println(TZ_OFFSET);
  Serial.print("Daylight saving minutes offset: ");
  Serial.println(TZ_DST);
  
}

String readFile(char * path){
  unsigned int cnt = 0;

  String pBuffer = "";                              // Declare a pointer to your buffer.
  File file = SPIFFS.open(path);                 // Open file for reading.
  if (file) {
    unsigned int fileSize = file.size();  // Get the file size.
    pBuffer = (char*)malloc(fileSize);
    if(!file || file.isDirectory()){
      Serial.println("- failed to open file for reading");
      return pBuffer;
    }
    while (file.available()){
      cnt++;
      pBuffer += char(file.read());
    }
    file.close();                         // Close the file.
  }
  return pBuffer;
}

void setupSecurity() {
  // Get cert(s) from file system
  wifiClient.setCACert(ca_cert.c_str());
  wifiClient.setCertificate(device_cert.c_str());
  wifiClient.setPrivateKey (device_key.c_str());

  // Set time from NTP servers
  configTime(TZ_OFFSET * 3600, TZ_DST * 60, "pool.ntp.org", "0.pool.ntp.org");
  Serial.println("\nWaiting for time");
  unsigned timeout = 5000;
  unsigned start = millis();
  while (millis() - start < timeout) {
    time_t now = time(nullptr);
    if (now > (2018 - 1970) * 365 * 24 * 3600) {
      break;
    }
    delay(100);
  }
  delay(1000); // Wait for time to fully sync
  Serial.println("Time sync'd");
  time_t now = time(nullptr);
  Serial.println(ctime(&now));
}


void connectMQTT() {
  mqtt.loop();
  while (! mqtt.connected()) {
    if (mqtt.connect(MQTT_DEVICEID, MQTT_USER, MQTT_PASS)) { // Token Authentication
      //    if (mqtt.connect(MQTT_DEVICEID)) { // No Token Authentication
      /*if (wifiClient.verifyCertChain(MQTT_HOST)) {
        Serial.println("certificate matches");
        } else {
        // ignore for now - but usually don't want to proceed if a valid cert not presented!
        Serial.println("certificate doesn't match");
        }*/
      Serial.println("MQTT Connected");
      mqtt.subscribe(MQTT_TOPIC_CMD);
    } else {
      Serial.println("MQTT Failed to connect! ... retrying");
      delay(5000);
    }
  }
}

void mqttReconnect() {
  mqtt.loop();
  while (!mqtt.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (mqtt.connect(MQTT_DEVICEID, MQTT_USER, MQTT_PASS)) { // Token Authentication
      //    if (mqtt.connect(MQTT_DEVICEID)) { // No Token Authentication
      Serial.println("MQTT Connected");
      // Should verify the certificates here - like in the startup function
      mqtt.subscribe(MQTT_TOPIC_CMD);
      mqtt.loop();
    } else {
      Serial.println("MQTT Failed to connect!");
      delay(5000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  // handle message arrived
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] : ");

  payload[length] = 0; // ensure valid content is zero terminated so can treat as c-string
  Serial.println((char *)payload);
  DeserializationError err = deserializeJson(jsonReceiveDoc, (char *)payload);
  if (err) {
    Serial.print(F("deserializeJson() failed with code "));
    Serial.println(err.c_str());
  } else {
    JsonObject cmdData = jsonReceiveDoc.as<JsonObject>();
    if (0 == strcmp(topic, MQTT_TOPIC_CMD)) {
      //valid message received
      //ReportingInterval = cmdData["Interval"].as<int32_t>();
      // this form allows you specify the type of the data you want from the JSON object
      jsonReceiveDoc.clear();
    }
    else {
      Serial.println("Unknown command received");
    }
  }
}


void selectNetwork() {
  String ssid = "";
  String pass = "";
  WiFi.disconnect();
  int n = WiFi.scanNetworks();
  Serial.println("scan done");
  if (n == 0) {
    Serial.println("no networks found");
  }
  else {
    Serial.print(n);
    Serial.println(" networks found");
    for (int i = 0; i < n; ++i) {
      // Print SSID and RSSI for each network found
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.print(")");
      Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " " : "*");
      delay(10);
      //for (int j = 0; j < sizeof(ssids); j++) {
      for (JsonObject::iterator it=networks.begin(); it!=networks.end(); ++it) {
      //for (JsonPair p : configJson["networks"]) {
        String key = it->key().c_str();
        if (WiFi.SSID(i) == key) {
          ssid = key;
          pass = it->value().as<char*>();
          Serial.println("Using network: " + ssid);
          WiFi.begin(ssid.c_str(), pass.c_str());
          while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
          }
          Serial.println("");
          Serial.println("WiFi Connected");
          break;
        }
      }
      if (WiFi.status() == WL_CONNECTED) {

        Serial.println("Connection details: ");
        //Serial.println("IP Address: " + WiFi.localIP());
        //Serial.println("MAC Address: " + WiFi.macAddress());
        break;
      }
    }
  }
}  

void setupBME280Sensor() {
  int retries = 0;
  while (!bme.begin() && retries < 10) {
    Serial.println("Could not find BME280 sensor!");
    retries++;
    delay(1000);
  }
  switch (bme.chipModel()) {
    case BME280::ChipModel_BME280:
      Serial.println("Found BME280 sensor! Success.");
      break;
    case BME280::ChipModel_BMP280:
      Serial.println("Found BMP280 sensor! No Humidity available.");
      break;
    default:
      Serial.println("Found UNKNOWN sensor! Error!");
  }
}


void printBME280Data(Stream* client) {
  float temp(NAN), hum(NAN), pres(NAN);
  BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
  BME280::PresUnit presUnit(BME280::PresUnit_Pa);
  bme.read(pres, temp, hum, tempUnit, presUnit);
  client->print("Temp: ");
  client->print(temp);
  client->print("°" + String(tempUnit == BME280::TempUnit_Celsius ? 'C' : 'F'));
  client->print("\t\tHumidity: ");
  client->print(hum);
  client->print("% RH");
  client->print("\t\tPressure: ");
  client->print(pres);
  client->println(" Pa");
}

