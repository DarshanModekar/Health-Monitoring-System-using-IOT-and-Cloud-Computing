#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h> // Add HTTP Client for ThingSpeak
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include "MAX30105.h"
#include "heartRate.h"

// Pin Definitions
#define DHT11_PIN 18
#define DS18B20_PIN 5
#define BUZZER_PIN 12  // Define the buzzer pin

// DHT11 Setup
#define DHTTYPE DHT11
DHT dht(DHT11_PIN, DHTTYPE);

// DS18B20 Setup
OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);

// MAX30105 Setup
MAX30105 particleSensor;

// Wi-Fi Credentials
const char* ssid = "motorola";
const char* password = "9606256753";

// ThingSpeak API Credentials
const char* thingSpeakServer = "http://api.thingspeak.com";
const char* apiKey = "VYIW8HX0138V70OX"; // Replace with your ThingSpeak API Key

// Web Server
WebServer server(80);

// Variables
float temperature = 0.0, humidity = 0.0, bodyTemperature = 0.0;
float heartRate = 0.0, oxygenSaturation = 0.0;
const uint32_t REPORTING_PERIOD_MS = 1000;
const uint32_t THINGSPEAK_PERIOD_MS = 20000; // Send data every 20 seconds
uint32_t lastReportTime = 0;
uint32_t lastThingSpeakTime = 0;

// Variables for Heart Rate Calculation
volatile float beatsPerMinute = 0.0;
long lastBeat = 0;

// Variables for SpO2 Calculation
float redAC = 0.0, irAC = 0.0; // AC components
float redDC = 0.0, irDC = 0.0; // DC components

void setup() {
  Serial.begin(115200);

  // Initialize Buzzer Pin
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // Initially turn off the buzzer

  // Wi-Fi Connection
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Start Web Server
  server.on("/", handle_OnConnect);
  server.onNotFound(handle_NotFound);
  server.begin();
  Serial.println("HTTP Server Started");

  // Initialize Sensors
  dht.begin();
  sensors.begin();

  // Initialize MAX30105
  if (!particleSensor.begin()) {
    Serial.println("MAX30105 not found. Please check wiring/power.");
    while (1);
  }
  particleSensor.setup(); // Default settings
  particleSensor.setPulseAmplitudeRed(0x0A);    // Red LED (low power)
  particleSensor.setPulseAmplitudeIR(0x0A);     // IR LED (low power)
  particleSensor.setPulseAmplitudeGreen(0);     // Green LED (disabled)

}

void loop() {
  server.handleClient();

  // Update DS18B20 and DHT11 readings
  sensors.requestTemperatures();
  bodyTemperature = sensors.getTempCByIndex(0);
  temperature = dht.readTemperature();
  humidity = dht.readHumidity();

  // Check for errors in readings
  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("Error reading DHT11!");
    temperature = humidity = 0.0;
  }
  if (bodyTemperature == DEVICE_DISCONNECTED_C) {
    Serial.println("Error reading DS18B20!");
    bodyTemperature = 0.0;
  }

  // Read Red and IR values
  long redValue = particleSensor.getRed();
  long irValue = particleSensor.getIR();

  // Process Heart Rate
  heartRate = processHeartRate(irValue);

  // Process SpO2
  oxygenSaturation = calculateSpO2(redValue, irValue);

  // Activate Buzzer if Oxygen Saturation is below 80%
  if (oxygenSaturation < 80.0) {
    digitalWrite(BUZZER_PIN, HIGH);  // Turn on buzzer
  } else {
    digitalWrite(BUZZER_PIN, LOW);   // Turn off buzzer
  }

  // Periodic Reporting
  if (millis() - lastReportTime > REPORTING_PERIOD_MS) {
    Serial.print("Room Temperature: ");
    Serial.print(temperature);
    Serial.println(" °C");

    Serial.print("Room Humidity: ");
    Serial.print(humidity);
    Serial.println(" %");

    Serial.print("Body Temperature: ");
    Serial.print(bodyTemperature);
    Serial.println(" °C");

    Serial.print("Heart Rate: ");
    Serial.print(heartRate);
    Serial.println(" BPM");

    Serial.print("Oxygen Saturation: ");
    Serial.print(oxygenSaturation);
    Serial.println(" %");

    Serial.println("*");
    lastReportTime = millis();
  }

  // Send Data to ThingSpeak
  if (millis() - lastThingSpeakTime > THINGSPEAK_PERIOD_MS) {
    sendToThingSpeak();
    lastThingSpeakTime = millis();
  }
}

void sendToThingSpeak() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    String url = String(thingSpeakServer) + "/update?api_key=" + apiKey + 
                 "&field1=" + String(temperature) + 
                 "&field2=" + String(humidity) + 
                 "&field3=" + String(bodyTemperature) + 
                 "&field4=" + String(heartRate) + 
                 "&field5=" + String(oxygenSaturation);

    http.begin(url); // Specify the URL
    int httpResponseCode = http.GET(); // Send the request

    if (httpResponseCode > 0) {
      Serial.print("ThingSpeak Response: ");
      Serial.println(httpResponseCode); // HTTP response code
    } else {
      Serial.print("Error sending to ThingSpeak: ");
      Serial.println(http.errorToString(httpResponseCode));
    }

    http.end(); // Free resources
  } else {
    Serial.println("WiFi Disconnected. Unable to send to ThingSpeak.");
  }
}

float processHeartRate(long irValue) {
  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();

    beatsPerMinute = 60 / (delta / 1000.0);
    return beatsPerMinute;
  }
  return beatsPerMinute;
}

float calculateSpO2(long redValue, long irValue) {
  // Extract AC and DC components
  redDC = 0.99 * redDC + 0.01 * redValue; // Moving average for DC
  irDC = 0.99 * irDC + 0.01 * irValue;    // Moving average for DC
  redAC = redValue - redDC;               // AC is the fluctuating part
  irAC = irValue - irDC;

  if (irAC <= 0 || redAC <= 0) {
    return 0.0; // Invalid readings
  }

  // Calculate the ratio of Red/IR
  float ratio = (redAC / redDC) / (irAC / irDC);

  // Convert ratio to SpO2 using a standard formula
  float spo2 = 110.0 - (25.0 * ratio);

  // Ensure SpO2 is in a realistic range
  return constrain(spo2, 0.0, 100.0);
}

void handle_OnConnect() {
  server.send(200, "text/html", SendHTML(temperature, humidity, bodyTemperature, heartRate, oxygenSaturation));
}

void handle_NotFound() {
  server.send(404, "text/plain", "Not found");
}

String SendHTML(float temperature, float humidity, float bodyTemperature, float heartRate, float oxygenSaturation) {
  
  String ptr = "<!DOCTYPE html>";
  ptr += "<html>";
  ptr += "<head>";
  ptr += "<title>ESP32 Patient Health Monitoring</title>";
  ptr += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  ptr += "<link href='https://fonts.googleapis.com/css?family=Open+Sans:300,400,600' rel='stylesheet'>";
  ptr += "<style>";
  ptr += "html { font-family: 'Open Sans', sans-serif; display: block; margin: 0px auto; text-align: center;color: #444444;}";
  ptr += "body{margin: 0px;} ";
  ptr += "h1 {margin: 50px auto 30px;} ";
  ptr += ".side-by-side{display: table-cell;vertical-align: middle;position: relative;}";
  ptr += ".text{font-weight: 600;font-size: 19px;width: 200px;}";
  ptr += ".reading{font-weight: 300;font-size: 50px;padding-right: 25px;}";
  ptr += ".temperature .reading{color: #F29C1F;}";
  ptr += ".humidity .reading{color: #3B97D3;}";
  ptr += ".heartRate .reading{color: #FF0000;}";
  ptr += ".oxygenSaturation .reading{color: #955BA5;}";
  ptr += ".bodyTemperature .reading{color: #F29C1F;}";
  ptr += ".superscript{font-size: 17px;font-weight: 600;position: absolute;top: 10px;}";
  ptr += ".data{padding: 10px;}";
  ptr += ".container{display: table;margin: 0 auto;}";
  ptr += ".icon{width:65px}";

  // Add CSS for alert
  ptr += ".alert {color: red; font-size: 20px; font-weight: bold; margin-top: 20px;}";

  ptr += "</style>";
  ptr += "</head>";
  ptr += "<body>";
  ptr += "<h1>ESP32 Patient Health Monitoring</h1>";
  ptr += "<h3>HOSPI-15</h3>";
  ptr += "<div class='container'>";

  // Display Alert if Oxygen Saturation is below 80%
  if (oxygenSaturation < 80.0) {
    ptr += "<div class='alert'>ALERT: Oxygen Saturation is below 80%!</div>";
  }

  // Temperature
  ptr += "<div class='data temperature'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg enable-background='new 0 0 19.438 54.003' height=54.003px id=Layer_1 version=1.1 viewBox='0 0 19.438 54.003' width=19.438px><g><path d='M11.976,8.82v-2h4.084V6.063C16.06,2.715,13.345,0,9.996,0H9.313C5.965,0,3.252,2.715,3.252,6.063v30.982C1.261,38.825,0,41.403,0,44.286c0,5.367,4.351,9.718,9.719,9.718c5.368,0,9.719-4.351,9.719-9.718c0-2.943-1.312-5.574-3.378-7.355V18.436h-3.914v-2h3.914v-2.808h-4.084v-2h4.084V8.82H11.976z M15.302,44.833c0,3.083-2.5,5.583-5.583,5.583s-5.583-2.5-5.583-5.583c0-2.279,1.368-4.236,3.326-5.104V24.257C7.462,23.01,8.472,22,9.719,22s2.257,1.01,2.257,2.257V39.73C13.934,40.597,15.302,42.554,15.302,44.833z' fill=#F29C21 /></g></svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Room Temperature</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += (int)temperature;
  ptr += "<span class='superscript'>&deg;C</span></div>";
  ptr += "</div>";

  // Humidity
  ptr += "<div class='data humidity'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg enable-background='new 0 0 29.235 40.64' height=40.64px id=Layer_1 version=1.1 viewBox='0 0 29.235 40.64' width=29.235px><path d='M14.618,0C14.618,0,0,17.95,0,26.022C0,34.096,6.544,40.64,14.618,40.64s14.617-6.544,14.617-14.617C29.235,17.95,14.618,0,14.618,0z M13.667,37.135c-5.604,0-10.162-4.56-10.162-10.162c0-0.787,0.638-1.426,1.426-1.426c0.787,0,1.425,0.639,1.425,1.426c0,4.031,3.28,7.312,7.311,7.312c0.787,0,1.425,0.638,1.425,1.425C15.093,36.497,14.455,37.135,13.667,37.135z' fill=#3C97D3 /></svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Room Humidity</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += (int)humidity;
  ptr += "<span class='superscript'>%</span></div>";
  ptr += "</div>";

  // Body Temperature
  ptr += "<div class='data bodyTemperature'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg enable-background='new 0 0 19.438 54.003' height=54.003px id=Layer_1 version=1.1 viewBox='0 0 19.438 54.003' width=19.438px><g><path d='M11.976,8.82v-2h4.084V6.063C16.06,2.715,13.345,0,9.996,0H9.313C5.965,0,3.252,2.715,3.252,6.063v30.982C1.261,38.825,0,41.403,0,44.286c0,5.367,4.351,9.718,9.719,9.718c5.368,0,9.719-4.351,9.719-9.718c0-2.943-1.312-5.574-3.378-7.355V18.436h-3.914v-2h3.914v-2.808h-4.084v-2h4.084V8.82H11.976z M15.302,44.833c0,3.083-2.5,5.583-5.583,5.583s-5.583-2.5-5.583-5.583c0-2.279,1.368-4.236,3.326-5.104V24.257C7.462,23.01,8.472,22,9.719,22s2.257,1.01,2.257,2.257V39.73C13.934,40.597,15.302,42.554,15.302,44.833z' fill=#F29C21 /></g></svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Body Temperature</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += (int)bodyTemperature;
  ptr += "<span class='superscript'>&deg;C</span></div>";
  ptr += "</div>";

  // Heart Rate
  ptr += "<div class='data heartRate'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg enable-background='new 0 0 19.438 54.003' height=54.003px id=Layer_1 version=1.1 viewBox='0 0 19.438 54.003' width=19.438px><g><path d='M11.976,8.82v-2h4.084V6.063C16.06,2.715,13.345,0,9.996,0H9.313C5.965,0,3.252,2.715,3.252,6.063v30.982C1.261,38.825,0,41.403,0,44.286c0,5.367,4.351,9.718,9.719,9.718c5.368,0,9.719-4.351,9.719-9.718c0-2.943-1.312-5.574-3.378-7.355V18.436h-3.914v-2h3.914v-2.808h-4.084v-2h4.084V8.82H11.976z M15.302,44.833c0,3.083-2.5,5.583-5.583,5.583s-5.583-2.5-5.583-5.583c0-2.279,1.368-4.236,3.326-5.104V24.257C7.462,23.01,8.472,22,9.719,22s2.257,1.01,2.257,2.257V39.73C13.934,40.597,15.302,42.554,15.302,44.833z' fill=#F29C21 /></g></svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Heart Rate</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += (int)heartRate;
  ptr += "<span class='superscript'>bpm</span></div>";
  ptr += "</div>";

  // Oxygen Saturation
  ptr += "<div class='data oxygenSaturation'>";
  ptr += "<div class='side-by-side icon'>";
  ptr += "<svg enable-background='new 0 0 19.438 54.003' height=54.003px id=Layer_1 version=1.1 viewBox='0 0 19.438 54.003' width=19.438px><g><path d='M11.976,8.82v-2h4.084V6.063C16.06,2.715,13.345,0,9.996,0H9.313C5.965,0,3.252,2.715,3.252,6.063v30.982C1.261,38.825,0,41.403,0,44.286c0,5.367,4.351,9.718,9.719,9.718c5.368,0,9.719-4.351,9.719-9.718c0-2.943-1.312-5.574-3.378-7.355V18.436h-3.914v-2h3.914v-2.808h-4.084v-2h4.084V8.82H11.976z M15.302,44.833c0,3.083-2.5,5.583-5.583,5.583s-5.583-2.5-5.583-5.583c0-2.279,1.368-4.236,3.326-5.104V24.257C7.462,23.01,8.472,22,9.719,22s2.257,1.01,2.257,2.257V39.73C13.934,40.597,15.302,42.554,15.302,44.833z' fill=#F29C21 /></g></svg>";
  ptr += "</div>";
  ptr += "<div class='side-by-side text'>Oxygen Saturation</div>";
  ptr += "<div class='side-by-side reading'>";
  ptr += (int)oxygenSaturation;
  ptr += "<span class='superscript'>%</span></div>";
  ptr += "</div>";

  ptr += "</div>";
  ptr += "</body>";
  ptr += "</html>";

  return ptr;
}
