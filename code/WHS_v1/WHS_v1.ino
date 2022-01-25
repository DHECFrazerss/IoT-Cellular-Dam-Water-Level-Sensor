/*  This example sketch allows your device to collect GPS, temperature, and battery
    data and send those values via MQTT to Adafruit IO. You can connect, publish, and
    subsribe to MQTT topics. Works great on the Botletics SIM7000 shield! Just make
    sure to replace Adafruit IO credentials with your own, and change the names of the
    feeds you want to use to publish and subscribe.

    Author: Timothy Woo (www.botletics.com)
    Github: https://github.com/botletics/SIM7000-LTE-Shield
    Last Updated: 1/7/2021
    License: GNU GPL v3.0
*/

#include "Adafruit_FONA.h"            // from botletics: https://github.com/botletics/SIM7000-LTE-Shield/tree/master/Code
#include "Adafruit_MQTT.h"            // from adafruit:  https://github.com/adafruit/Adafruit_MQTT_Library
#include "Adafruit_MQTT_FONA.h"

#include <Adafruit_BMP280.h>          // BMP280 SENSOR LIBRARY
#include <NewPing.h>                  // JSN-SR04 LIBRARY

#define SIMCOM_7000                   // cellular MCU we are using

// FONA PINS -----------------------------------------------------------------------------------------
#define FONA_PWRKEY 6
#define FONA_RST 7
#define FONA_TX 10 
#define FONA_RX 11 

#define LED 13
int sampling_rate = 15;                // initialize the delay between loops. This can be changed with subscribe

// send AT commands via fona's software serial
#include <SoftwareSerial.h>
SoftwareSerial fonaSS = SoftwareSerial(FONA_TX, FONA_RX);
SoftwareSerial *fonaSerial = &fonaSS;

// construct an instance of the LTE fona
Adafruit_FONA_LTE fona = Adafruit_FONA_LTE();

// MQTT SETUP WITH ADAFRUIT IO -----------------------------------------------------------------------
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883
#define AIO_USERNAME    "smitty444"
#define AIO_KEY         "aio_UtgK29tCMxzEDaRMLQeGj6wY7P0B"

// pass in fona class and server details to mqtt class
Adafruit_MQTT_FONA mqtt(&fona, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

// THE PUBLISHING FEEDS ------------------------------------------------------------------------------
Adafruit_MQTT_Publish feed_location = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME, "/feeds/location/csv");
Adafruit_MQTT_Publish feed_temp = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/temperature");
Adafruit_MQTT_Publish feed_pressure = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/pressure");
Adafruit_MQTT_Publish feed_stage = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/stage");

// THE SUBSCRIBING FEEDS -----------------------------------------------------------------------------
Adafruit_MQTT_Subscribe feed_led_toggle = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/led-toggle");
//Adafruit_MQTT_Subscribe feed_led_selection = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/led-selection");
//Adafruit_MQTT_Subscribe feed_deployment_toggle = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME, "/feeds/deployment-toggle");
Adafruit_MQTT_Subscribe feed_sampling_rate = Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/sampling-rate");

// construct the BMP280
Adafruit_BMP280 bmp;

// construct the JSN-SR04
#define trigPin 3
#define echoPin 4
NewPing sonar(trigPin, echoPin);

// some global variables
uint8_t readline(char *buff, uint8_t maxbuff, uint16_t timeout = 0);
char imei[16] = {0}; 
bool deployed = false;
bool pin_state = true;
float initial_distance = 0;
float sea_level = 200;

void setup() {
  Serial.begin(9600);
  Serial.println(F("*** Executing WHS_v1.ino ***"));

  // configure the led
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);

  // configure the JSN-SR04 pins
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  pinMode(FONA_RST, OUTPUT);
  digitalWrite(FONA_RST, HIGH);             // reset is default high

  fona.powerOn(FONA_PWRKEY);                // power on fona by pulsing power key 
  
  moduleSetup();                            // establish serial communication, find fona, determine device IMEI
  
  fona.setFunctionality(1);                 // set fona to its full functionality (AT+CFUN=1)

  fona.setNetworkSettings(F("hologram"));   // sets APN as 'hologram', used with Hologram SIM card

  //fona.set_eDRX()

  // enable GPS
  while (!fona.enableGPS(true)) {
    Serial.println(F("Failed to turn on GPS, retrying..."));
    delay(2000); // Retry every 2s
  }
  Serial.println(F("Turned on GPS!")); 

  // first disable data
  if (!fona.enableGPRS(false)) Serial.println(F("Failed to disable data!"));

  // enable data
  while (!fona.enableGPRS(true)) {
    Serial.println(F("Failed to enable data, retrying..."));
    delay(2000); // Retry every 2s
  }
  Serial.println(F("Enabled data!"));


  // subscribe to the subscription feeds
  mqtt.subscribe(&feed_led_toggle); // Only if you're using MQTT
  //mqtt.subscribe(&feed_led_selection);
  //mqtt.subscribe(&feed_deployment_toggle);
  mqtt.subscribe(&feed_sampling_rate);

  // begin the BMP280
  bmp.begin(0x76);      // bmp.begin(I2C_address)
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     /* Operating Mode. */
                  Adafruit_BMP280::SAMPLING_X2,     /* Temp. oversampling */
                  Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                  Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                  Adafruit_BMP280::STANDBY_MS_500); /* Standby time. */

  // find the initial distance that will correspond to sea level
  while(initial_distance <= 0) {
    initial_distance = sonar.ping_in();
    delay(50);
  }
  initial_distance = initial_distance/12;
  Serial.print("initial distance: "); Serial.print(initial_distance); Serial.println(" ft");
  delay(50);
}

void loop() {
  // connect to cell network
  while (!netStatus()) {
    Serial.println(F("Failed to connect to cell network, retrying..."));
    delay(2000); // Retry every 2s
  }
  Serial.println(F("Connected to cell network!"));
  readRSSI();
  
//  // first ensure that we are finished configuring everything in Adafruit IO and want the package to start collecting data
//  while (! deployed) {
//    
//    MQTT_connect();
//    // subscription packet subloop, this runs and waits for the toggle switch in Adafruit IO to turn on
//    Adafruit_MQTT_Subscribe *subscription4;
//    while ((subscription4 = mqtt.readSubscription(5000))) {
//      if (subscription4 == &feed_deployment_toggle) {
//        Serial.print(F("***Received: ")); Serial.println((char *)feed_deployment_toggle.lastread);
//      }
//    }
//    // based on the received packet, either break from the loop or repeat again every five seconds
//    if (strcmp(feed_deployment_toggle.lastread, "ON") == 0) {
//      Serial.println(F("***Package is deployed"));
//      deployed = true;
//    }
//    else if (strcmp(feed_deployment_toggle.lastread, "OFF") == 0) {
//      Serial.println(F("***Package not ready"));
//      delay(5000);    // wait five seconds to not spam the serial monitor
//    }
//    else {
//      Serial.println("***No packet received");
//    }
//    delay(3000);
//  }

  // take gps data
  float latitude, longitude, speed_kph, heading, altitude;
  char latBuff[12], longBuff[12], locBuff[50], speedBuff[12], headBuff[12], altBuff[12];
  int gps_fails = 0;
  
  while (!fona.getGPS(&latitude, &longitude, &speed_kph, &heading, &altitude) && gps_fails < 5) {
    Serial.println(F("Failed to get GPS location, retrying..."));
    delay(2000); // Retry every 2s
    gps_fails++;
    if(gps_fails == 4) {
      Serial.println("Giving up on GPS");
    }
  }
  if(gps_fails < 5) {
     Serial.println(F("Found 'eeeeem!"));
    Serial.println(F("---------------------"));
    Serial.print(F("Latitude: ")); Serial.println(latitude, 6);
    Serial.print(F("Longitude: ")); Serial.println(longitude, 6);
    Serial.print(F("Speed: ")); Serial.println(speed_kph);
    Serial.print(F("Heading: ")); Serial.println(heading);
    Serial.print(F("Altitude: ")); Serial.println(altitude);
  
     dtostrf(latitude, 1, 6, latBuff); // float_val, min_width, digits_after_decimal, char_buffer
    dtostrf(longitude, 1, 6, longBuff);
    dtostrf(speed_kph, 1, 0, speedBuff);
    dtostrf(heading, 1, 0, headBuff);
    dtostrf(altitude, 1, 1, altBuff);
  
    sprintf(locBuff, "%s,%s,%s,%s", speedBuff, latBuff, longBuff, altBuff);
  }
  
  // take temperature data
  float celsius = bmp.readTemperature();
  Serial.print(F("Temperature = "));
  Serial.print(celsius);
  Serial.println(" *C");
  char tempBuff[6];

  dtostrf(celsius, 1, 2, tempBuff);

  // take pressure data
  float pressure = bmp.readPressure();
  Serial.print(F("Pressure = "));
  Serial.print(pressure);
  Serial.println(" Pa");
  char pressBuff[10];
  dtostrf(pressure, 1, 2, pressBuff);

  // take stage data
  float distance = 0;
  while(distance <= 0) {
    distance = sonar.ping_in();
    delay(50);
  }
  distance = distance/12;
  Serial.print("distance: "); Serial.print(distance); Serial.println(" ft");
  distance = sea_level + (initial_distance - distance);
  Serial.print("above sea level: "); Serial.print(distance); Serial.println(" ft");
  char stageBuff[7];
  dtostrf(distance, 1, 2, stageBuff);

  Serial.println(F("---------------------"));

  bool new_time = false;

  // connect to MQTT
  MQTT_connect();

  // This is our 'wait for incoming subscription packets' busy subloop
  Adafruit_MQTT_Subscribe *subscription;
  while ((subscription = mqtt.readSubscription(5000))) {
    // checks if led is on or off
    if (subscription == &feed_led_toggle) {
      Serial.print(F("*** Got: "));
      Serial.println((char *)feed_led_toggle.lastread);
      delay(100);
    }
//    if (subscription == &feed_led_selection) {
//      Serial.print(F("*** Got: "));
//      Serial.println((char *)feed_led_selection.lastread);
//      delay(100);
//    }
    if (subscription == &feed_sampling_rate) {
      Serial.print(F("*** Got: "));
      Serial.println((char *)feed_sampling_rate.lastread);
      delay(100);
      new_time = true;
    }
//    // this checks if the deployment switch is ever turned off
//    if (subscription == &feed_deployment_toggle) {
//      Serial.print(F("***Deployed: ")); Serial.print((char *)feed_deployment_toggle.lastread);
//    }
  }

  // publish data to Adafruit IO
//  if(gps_fails < 5) {
//    MQTT_publish_checkSuccess(feed_location, locBuff);
//  }
  MQTT_publish_checkSuccess(feed_stage, stageBuff);
  MQTT_publish_checkSuccess(feed_temp, tempBuff);
  MQTT_publish_checkSuccess(feed_pressure, pressBuff);
  
  // reassign the number
//  int number = atoi((char *)feed_led_selection.lastread);
//  delay(100);
//  Serial.print("Number: "); Serial.print((char *)feed_led_selection.lastread); Serial.print(", "); Serial.println(number);

  // reassign the sampling rate
  if(new_time == true) {
    sampling_rate = atoi((char *)feed_sampling_rate.lastread);
    delay(100);
    Serial.println(F("New sampling rate: ")); Serial.print(sampling_rate);
  }
   

  // toggle led assigned to the number
//  pinMode(number, OUTPUT);
//  delay(100);
//  digitalWrite(number, HIGH);

//  // check if deployment was turned off
//  if (strcmp(feed_deployment_toggle.lastread, "OFF") == 0) {
//    Serial.println(F("Deployment turned off"));
//    deployed = false;
//  }

  // check if LED was turned on or off
  if (strcmp(feed_led_toggle.lastread, "ON") == 0) {
    Serial.print(F("*** Commanded to turn on ")); Serial.println(LED);
    digitalWrite(LED, HIGH);
  }
  else if (strcmp(feed_led_toggle.lastread, "OFF") == 0) {
    Serial.print(F("*** Commanded to turn off ")); Serial.println(LED);
    digitalWrite(LED, LOW);
  }

  // Delay until next post
  Serial.print(F("Waiting for ")); Serial.print(sampling_rate); Serial.println(F(" seconds\r\n"));
  delay(sampling_rate * 1000UL); // Delay
}



void moduleSetup() {
  // this should all take about 3s for the SIMCOM 7000 module
  fonaSS.begin(115200); // Default SIM7000 shield baud rate
  Serial.println(F("Configuring to 9600 baud"));
  fonaSS.println("AT+IPR=9600"); // Set baud rate
  delay(100); // Short pause to let the command run
  fonaSS.begin(9600);
  if (! fona.begin(fonaSS)) {
    Serial.println(F("FONA not found"));
    while (1); // Don't proceed if it couldn't find the device
  }

  uint8_t type = fona.type();
  Serial.println(F("FONA is OK"));
  Serial.print(F("Found "));
  switch (type) {
    case SIM800L:
      Serial.println(F("SIM800L")); break;
    case SIM800H:
      Serial.println(F("SIM800H")); break;
    case SIM808_V1:
      Serial.println(F("SIM808 (v1)")); break;
    case SIM808_V2:
      Serial.println(F("SIM808 (v2)")); break;
    case SIM5320A:
      Serial.println(F("SIM5320A (American)")); break;
    case SIM5320E:
      Serial.println(F("SIM5320E (European)")); break;
    case SIM7000:
      Serial.println(F("SIM7000")); break;
    case SIM7070:
      Serial.println(F("SIM7070")); break;
    case SIM7500:
      Serial.println(F("SIM7500")); break;
    case SIM7600:
      Serial.println(F("SIM7600")); break;
    default:
      Serial.println(F("???")); break;
  }

  // Print module IMEI number.
  uint8_t imeiLen = fona.getIMEI(imei);
  if (imeiLen > 0) {
    Serial.print("Module IMEI: "); Serial.println(imei);
  }
}

bool netStatus() {
  int n = fona.getNetworkStatus();

  Serial.print(F("Network status ")); Serial.print(n); Serial.print(F(": "));
  if (n == 0) Serial.println(F("Not registered"));
  if (n == 1) Serial.println(F("Registered (home)"));
  if (n == 2) Serial.println(F("Not registered (searching)"));
  if (n == 3) Serial.println(F("Denied"));
  if (n == 4) Serial.println(F("Unknown"));
  if (n == 5) Serial.println(F("Registered roaming"));

  if (!(n == 1 || n == 5)) return false;
  else return true;
}

void MQTT_connect() {
  int8_t ret;

  // Stop if already connected.
  if (mqtt.connected()) {
    return;
  }

  Serial.println("Connecting to MQTT... ");

  while ((ret = mqtt.connect()) != 0) { // connect will return 0 for connected
    Serial.println(mqtt.connectErrorString(ret));
    Serial.println("Retrying MQTT connection in 5 seconds...");
    mqtt.disconnect();
    delay(5000);  // wait 5 seconds
  }
  Serial.println("MQTT Connected!");
}

void MQTT_publish_checkSuccess(Adafruit_MQTT_Publish &feed, const char *feedContent) {
  Serial.println(F("Sending data..."));
  uint8_t txfailures = 0;
  if (! feed.publish(feedContent)) {
    Serial.println(F("Failed"));
    txfailures++;
  }
  else {
    Serial.println(F("OK!"));
    txfailures = 0;
  }
}

void readRSSI() {
  uint8_t n = fona.getRSSI();
  int8_t r;

  Serial.print(F("RSSI = ")); Serial.print(n); Serial.print(": ");
  if (n == 0) r = -115;
  if (n == 1) r = -111;
  if (n == 31) r = -52;
  if ((n >= 2) && (n <= 30)) {
    r = map(n, 2, 30, -110, -54);
  }
  Serial.print(r); Serial.println(F(" dBm"));
}
