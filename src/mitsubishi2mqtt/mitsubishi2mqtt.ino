#include <ESP8266WiFi.h>      // WIFI for ESP8266
#include <ESP8266mDNS.h>      // mDNS for ESP8266
#include <ESP8266WebServer.h> // webServer for ESP8266
#include <ArduinoJson.h>      // json to process MQTT: ArduinoJson 6.11.4
#include <PubSubClient.h>     // MQTT: PubSubClient 2.7.0
#include <DNSServer.h>        // DNS for captive portal
#include "FS.h"               // SPIFFS for store config 
#include <math.h>             // for rounding to Fahrenheit values

#include <ArduinoOTA.h>   // for OTA
#include <HeatPump.h>     // Swiacago library: https://github.com/SwiCago/HeatPump
//#include <Ticker.h>     // for LED status (Using a Wemos D1-Mini)
#include "config.h"       // config file
#include "html_common.h"  // common code HTML (like header, footer)
#include "html_init.h"    // code html for initial config
#include "html_menu.h"    // code html for menu
#include "html_pages.h"   // code html for pages

//Ticker ticker;

// wifi, mqtt and heatpump client instances
WiFiClient espClient;
PubSubClient mqtt_client(espClient);

//Captive portal variables, only used for config page
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 1, 1);
IPAddress netMsk(255, 255, 255, 0);
DNSServer dnsServer;
ESP8266WebServer server(80);
boolean captive = false;
boolean mqtt_config = false;
boolean wifi_config = false;

//HVAC
HeatPump hp;
unsigned long lastTempSend;
unsigned long lastMqttRetry;

//Web OTA
int uploaderror = 0;

void setup() {
  // Start serial for debug before HVAC connect to serial
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("Starting Mitsubishi2MQTT"));
  // Mount SPIFFS filesystem
  if (!SPIFFS.begin()) {
    Serial.println(F("An Error has occurred while mounting SPIFFS"));
  }

  //set led pin as output
  pinMode(blueLedPin, OUTPUT);
  /*
    ticker.attach(0.6, tick);
  */

  //Define hostname
  hostname += hostnamePrefix;
  hostname += String(ESP.getChipId(), HEX);
  mqtt_client_id = hostname;
  WiFi.hostname(hostname);
  setDefaults();
  loadWifi();
  loadAdvance();
  if (initWifi()) {
    SPIFFS.remove(console_file);
    //write_log("Starting Mitsubishi2MQTT");
    //Web interface
    server.on("/", handleRoot);
    server.on("/control", handleControl);
    server.on("/setup", handleSetup);
    server.on("/mqtt", handleMqtt);
    server.on("/wifi", handleWifi);
    server.on("/advance", handleAdvance);
    server.on("/status", handleStatus);
    server.on("/others", handleOthers);
    server.onNotFound(handleNotFound);
    if (login_password.length() > 0) {
      server.on("/login", handleLogin);
      //here the list of headers to be recorded, use for authentication
      const char * headerkeys[] = {"User-Agent", "Cookie"} ;
      size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
      //ask server to track these headers
      server.collectHeaders(headerkeys, headerkeyssize);
    }
    server.on("/upgrade", handleUpgrade);
    server.on("/upload", HTTP_POST, handleUploadDone, handleUploadLoop);

    server.begin();
    lastMqttRetry = 0;
    if (loadMqtt()) {
      //write_log("Starting MQTT");
      // setup topics
      heatpump_topic              = mqtt_topic + "/" + mqtt_fn;
      
      heatpump_set_topic          = heatpump_topic + "/set";
      heatpump_status_topic       = heatpump_topic + "/status";
      heatpump_timers_topic       = heatpump_topic + "/timers";

      heatpump_debug_topic        = heatpump_topic + "/debug";
      heatpump_debug_set_topic    = heatpump_topic + "/debug/set";
      // startup mqtt connection
      initMqtt();
    }
    else {
      //write_log("Not found MQTT config go to configuration page");
    }
    Serial.println(F("Connection to HVAC. Stop serial log."));
    //write_log("Connection to HVAC");
    hp.setSettingsChangedCallback(hpSettingsChanged);
    hp.setStatusChangedCallback(hpStatusChanged);
    hp.setPacketCallback(hpPacketDebug);
    hp.connect(&Serial);
    lastTempSend = millis();
  }
  else {
    dnsServer.start(DNS_PORT, "*", apIP);
    initCaptivePortal();
  }
  initOTA();
}

/*
  void tick()
  {
  //toggle state
  int state = digitalRead(blueLedPin);  // get the current state of GPIO2 pin
  digitalWrite(blueLedPin, !state);     // set pin to the opposite state
  }*/

bool loadWifi() {
  ap_ssid = "";
  ap_pwd  = "";
  File configFile = SPIFFS.open(wifi_conf, "r");
  if (!configFile) {
    Serial.println(F("Failed to open wifi config file"));
    return false;
  }
  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println(F("Wifi config file size is too large"));
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  const size_t capacity = JSON_OBJECT_SIZE(4) + 130;
  DynamicJsonDocument doc(capacity);
  deserializeJson(doc, buf.get());
  hostname = doc["hostname"].as<String>();
  ap_ssid  = doc["ap_ssid"].as<String>();
  ap_pwd   = doc["ap_pwd"].as<String>();
  //prevent ota password is "null" if not exist key
  if (doc.containsKey("ota_pwd")) {
    ota_pwd  = doc["ota_pwd"].as<String>();
  } else {
    ota_pwd = "";
  }
  return true;
}


void saveMqtt(String mqttFn, String mqttHost, String mqttPort, String mqttUser,
              String mqttPwd, String mqttTopic) {

  const size_t capacity = JSON_OBJECT_SIZE(6) + 400;
  DynamicJsonDocument doc(capacity);
  // if mqtt port is empty, we use default port
  if (mqttPort[0] == '\0') mqttPort = "1883";
  doc["mqtt_fn"]   = mqttFn;
  doc["mqtt_host"] = mqttHost;
  doc["mqtt_port"] = mqttPort;
  doc["mqtt_user"] = mqttUser;
  doc["mqtt_pwd"] = mqttPwd;
  doc["mqtt_topic"] = mqttTopic;
  File configFile = SPIFFS.open(mqtt_conf, "w");
  if (!configFile) {
    Serial.println(F("Failed to open config file for writing"));
  }
  serializeJson(doc, Serial);
  serializeJson(doc, configFile);
  configFile.close();
}

void saveAdvance(String tempUnit, String supportMode, String loginPassword) {
  const size_t capacity = JSON_OBJECT_SIZE(3) + 200;
  DynamicJsonDocument doc(capacity);
  // if temp unit is empty, we use default celcius
  if (tempUnit == '\0') tempUnit = "cel";
  doc["unit_tempUnit"]   = tempUnit;
  // if support mode is empty, we use default all mode
  if (supportMode == '\0') supportMode = "all";
  doc["support_mode"]   = supportMode;
  // if login password is empty, we use empty
  if (loginPassword == '\0') loginPassword = "";
  doc["login_password"]   = loginPassword;
  File configFile = SPIFFS.open(advance_conf, "w");
  if (!configFile) {
    Serial.println(F("Failed to open config file for writing"));
  }
  serializeJson(doc, Serial);
  serializeJson(doc, configFile);
  configFile.close();
}

void saveWifi(String apSsid, String apPwd, String hostName, String otaPwd) {
  const size_t capacity = JSON_OBJECT_SIZE(4) + 130;
  DynamicJsonDocument doc(capacity);
  doc["ap_ssid"] = apSsid;
  doc["ap_pwd"] = apPwd;
  doc["hostname"] = hostName;
  doc["ota_pwd"] = otaPwd;
  File configFile = SPIFFS.open(wifi_conf, "w");
  if (!configFile) {
    Serial.println(F("Failed to open wifi file for writing"));
  }
  serializeJson(doc, Serial);
  serializeJson(doc, configFile);
  delay(10);
  configFile.close();
}

void saveOthers(String haa, String haat, String debug) {
  const size_t capacity = JSON_OBJECT_SIZE(3) + 130;
  DynamicJsonDocument doc(capacity);
  doc["haa"] = haa;
  doc["haat"] = haat;
  doc["debug"] = debug;
  File configFile = SPIFFS.open(others_conf, "w");
  if (!configFile) {
    Serial.println(F("Failed to open wifi file for writing"));
  }
  serializeJson(doc, configFile);
  delay(10);
  configFile.close();
}

// Initialize captive portal page
void initCaptivePortal() {
  Serial.println(F("Starting captive portal"));
  server.on("/", handleInitSetup);
  server.on("/save", handleSaveWifi);
  server.on("/reboot", handleReboot);
  server.onNotFound(handleNotFound);
  server.begin();
  captive = true;
}

void initMqtt() {
  mqtt_client.setServer(mqtt_server.c_str(), atoi(mqtt_port.c_str()));
  mqtt_client.setCallback(mqttCallback);
  mqttConnect();
}

// Enable OTA only when connected as a client.
void initOTA() {
  //write_log("Start OTA Listener");
  ArduinoOTA.setHostname(hostname.c_str());
  if (ota_pwd.length() > 0) {
    ArduinoOTA.setPassword(ota_pwd.c_str());
  }
  ArduinoOTA.onStart([]() {
    //write_log("Start");
  });
  ArduinoOTA.onEnd([]() {
    //write_log("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    //    write_log("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    //    write_log("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
    else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
    else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
    else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
    else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
  });
  ArduinoOTA.begin();
}

bool loadMqtt() {
  //write_log("Loading MQTT configuration");
  File configFile = SPIFFS.open(mqtt_conf, "r");
  if (!configFile) {
    //write_log("Failed to open MQTT config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    //write_log("Config file size is too large");
    return false;
  }
  std::unique_ptr<char[]> buf(new char[size]);

  configFile.readBytes(buf.get(), size);
  const size_t capacity = JSON_OBJECT_SIZE(6) + 400;
  DynamicJsonDocument doc(capacity);
  deserializeJson(doc, buf.get());
  mqtt_fn             = doc["mqtt_fn"].as<String>();
  mqtt_server         = doc["mqtt_host"].as<String>();
  mqtt_port           = doc["mqtt_port"].as<String>();
  mqtt_username       = doc["mqtt_user"].as<String>();
  mqtt_password       = doc["mqtt_pwd"].as<String>();
  mqtt_topic          = doc["mqtt_topic"].as<String>();

  //write_log("=== START DEBUG MQTT ===");
  //write_log("Friendly Name" + mqtt_fn);
  //write_log("IP Server " + mqtt_server);
  //write_log("IP Port " + mqtt_port);
  //write_log("Username " + mqtt_username);
  //write_log("Password " + mqtt_password);
  //write_log("Topic " + mqtt_topic);
  //write_log("=== END DEBUG MQTT ===");

  mqtt_config = true;
  return true;
}

bool loadAdvance() {
  File configFile = SPIFFS.open(advance_conf, "r");
  if (!configFile) {
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    return false;
  }
  std::unique_ptr<char[]> buf(new char[size]);

  configFile.readBytes(buf.get(), size);
  const size_t capacity = JSON_OBJECT_SIZE(3) + 200;
  DynamicJsonDocument doc(capacity);
  deserializeJson(doc, buf.get());
  //unit
  String unit_tempUnit            = doc["unit_tempUnit"].as<String>();
  if (unit_tempUnit == "fah") useFahrenheit = true;
  //mode
  String supportMode = doc["support_mode"].as<String>();
  if (supportMode == "nht") supportHeatMode = false;
  //prevent login password is "null" if not exist key
  if (doc.containsKey("login_password")) {
    login_password = doc["login_password"].as<String>();
  } else {
    login_password = "";
  }
  return true;
}

void setDefaults() {
  ap_ssid = "";
  ap_pwd  = "";
  others_haa = "ON";
  others_haa_topic = "homeassistant";
  others_debug = "OFF";

}

boolean initWifi() {
  bool connectWifiSuccess = true;
  if (ap_ssid[0] != '\0') {
    connectWifiSuccess = wifi_config = connectWifi();
    if (connectWifiSuccess) {
      return true;
    }
    else
    {
      // reset hostname back to default before starting AP mode for privacy
      hostname = hostnamePrefix;
      hostname += String(ESP.getChipId(), HEX);
    }
  }

  Serial.println(F("\n\r \n\rStarting in AP mode"));
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  if (!connectWifiSuccess)
    // Set AP password when falling back to AP on fail
    WiFi.softAP(hostname.c_str(), hostname.c_str());
  else
    // First time setup does not require password
    WiFi.softAP(hostname.c_str());
  Serial.print(F("IP address: "));
  Serial.println(WiFi.softAPIP());
  //ticker.attach(0.2, tick); // Start LED to flash rapidly to indicate we are ready for setting up the wifi-connection (entered captive portal).
  wifi_config = false;
  return false;
}

// Handler webserver response

void handleNotFound() {
  if (captive) {
    String initSetupContent = FPSTR(html_init_setup);
    server.send(200, "text/html", initSetupContent);
  }
  else {
    String headerContent = FPSTR(html_common_header);
    String menuRootPage =  FPSTR(html_menu_root);
    menuRootPage.replace("_SHOW_LOGOUT_", (String)(login_password.length() > 0));
    //not show control button if hp not connected
    menuRootPage.replace("_SHOW_CONTROL_", (String)(hp.isConnected()));
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + menuRootPage + footerContent;
    server.send(200, "text/html", toSend);
  }
}

void handleSaveWifi() {
  checkLogin();
  Serial.println(F("Saving wifi config"));
  if (server.hasArg("submit")) {
    saveWifi(server.arg("ssid"), server.arg("psk"), server.arg("hn"), server.arg("otapwd"));
  }
  String headerContent = FPSTR(html_common_header);
  String initSavePage =  FPSTR(html_init_save);
  String footerContent = FPSTR(html_common_footer);
  String toSend = headerContent + initSavePage + footerContent;
  toSend.replace(F("_UNIT_NAME_"), hostname);
  toSend.replace(F("_VERSION_"), m2mqtt_version);
  server.send(200, F("text/html"), toSend);
  delay(100);
  ESP.restart();
}

void handleReboot() {
  Serial.println(F("Rebooting"));
  String headerContent = FPSTR(html_common_header);
  String initRebootPage =  FPSTR(html_init_reboot);
  String footerContent = FPSTR(html_common_footer);
  String toSend = headerContent + initRebootPage + footerContent;
  toSend.replace(F("_UNIT_NAME_"), hostname);
  toSend.replace(F("_VERSION_"), m2mqtt_version);
  server.send(200, F("text/html"), toSend);
  delay(100);
  ESP.restart();
}

void handleRoot() {
  checkLogin();
  if (server.hasArg("REBOOT")) {
    String headerContent = FPSTR(html_common_header);
    String rebootPage =  FPSTR(html_page_reboot);
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + rebootPage + footerContent;
    toSend.replace(F("_UNIT_NAME_"), hostname);
    toSend.replace(F("_VERSION_"), m2mqtt_version);
    server.send(200, F("text/html"), toSend);
    delay(10);
    ESP.reset();
  }
  else {
    String headerContent = FPSTR(html_common_header);
    String menuRootPage =  FPSTR(html_menu_root);
    menuRootPage.replace("_SHOW_LOGOUT_", (String)(login_password.length() > 0));
    //not show control button if hp not connected
    menuRootPage.replace("_SHOW_CONTROL_", (String)(hp.isConnected()));
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + menuRootPage + footerContent;
    toSend.replace(F("_UNIT_NAME_"), hostname);
    toSend.replace(F("_VERSION_"), m2mqtt_version);
    server.send(200, F("text/html"), toSend);
  }
}

void handleInitSetup() {
  String headerContent = FPSTR(html_common_header);
  String initSetupPage =  FPSTR(html_init_setup);
  String footerContent = FPSTR(html_common_footer);
  String toSend = headerContent + initSetupPage + footerContent;
  toSend.replace(F("_UNIT_NAME_"), hostname);
  toSend.replace(F("_VERSION_"), m2mqtt_version);
  server.send(200, F("text/html"), toSend);
}

void handleSetup() {
  checkLogin();
  if (server.hasArg("RESET")) {
    String headerContent = FPSTR(html_common_header);
    String resetPage =  FPSTR(html_page_reset);
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + resetPage + footerContent;
    toSend.replace(F("_UNIT_NAME_"), hostname);
    toSend.replace(F("_VERSION_"), m2mqtt_version);
    server.send(200, F("text/html"), toSend);
    SPIFFS.format();
    delay(10);
    ESP.reset();
  }
  else {
    String headerContent = FPSTR(html_common_header);
    String menuSetupPage =  FPSTR(html_menu_setup);
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + menuSetupPage + footerContent;
    toSend.replace(F("_UNIT_NAME_"), hostname);
    toSend.replace(F("_VERSION_"), m2mqtt_version);
    server.send(200, F("text/html"), toSend);
  }

}

void handleOthers() {
  checkLogin();
  if (server.hasArg("save")) {

  }
  else {
    String headerContent = FPSTR(html_common_header);
    String othersPage =  FPSTR(html_page_others);
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + othersPage + footerContent;
    toSend.replace("_UNIT_NAME_", mqtt_fn);
    toSend.replace("_VERSION_", m2mqtt_version);
    toSend.replace("_HAA_TOPIC_", others_haa_topic);
    if (strcmp(others_haa.c_str(), "ON") == 0) {
      toSend.replace("_HAA_ON_", "selected");
    }
    else {
      toSend.replace("_HAA_OFF_", "selected");
    }
    if (strcmp(others_debug.c_str(), "ON") == 0) {
      toSend.replace("_DEBUG_ON_", "selected");
    }
    else {
      toSend.replace("_DEBUG_OFF_", "selected");
    }
    server.send(200, "text/html", toSend);
  }
}

void handleMqtt() {
  checkLogin();
  if (server.hasArg("save")) {
    saveMqtt(server.arg("fn"), server.arg("mh"), server.arg("ml"), server.arg("mu"), server.arg("mp"), server.arg("mt"));
    String headerContent = FPSTR(html_common_header);
    String saveRebootPage =  FPSTR(html_page_save_reboot);
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + saveRebootPage + footerContent;
    toSend.replace(F("_UNIT_NAME_"), hostname);
    toSend.replace(F("_VERSION_"), m2mqtt_version);
    server.send(200, F("text/html"), toSend);
    delay(10);
    ESP.reset();
  }
  else {
    String headerContent = FPSTR(html_common_header);
    String mqttPage =  FPSTR(html_page_mqtt);
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + mqttPage + footerContent;
    toSend.replace(F("_UNIT_NAME_"), mqtt_fn);
    toSend.replace(F("_MQTT_HOST_"), mqtt_server);
    toSend.replace(F("_MQTT_PORT_"), String(mqtt_port));
    toSend.replace(F("_MQTT_USER_"), mqtt_username);
    toSend.replace(F("_MQTT_PASSWORD_"), mqtt_password);
    toSend.replace(F("_MQTT_TOPIC_"), mqtt_topic);
    toSend.replace(F("_VERSION_"), m2mqtt_version);
    server.send(200, F("text/html"), toSend);
  }
}

void handleAdvance() {
  checkLogin();
  if (server.hasArg("save")) {
    saveAdvance(server.arg("tu"), server.arg("md"), server.arg("lpw"));
    String headerContent = FPSTR(html_common_header);
    String saveRebootPage =  FPSTR(html_page_save_reboot);
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + saveRebootPage + footerContent;
    toSend.replace(F("_UNIT_NAME_"), hostname);
    toSend.replace(F("_VERSION_"), m2mqtt_version);
    server.send(200, F("text/html"), toSend);
    delay(10);
    ESP.reset();
  }
  else {
    String headerContent = FPSTR(html_common_header);
    String advancePage =  FPSTR(html_page_advance);
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + advancePage + footerContent;
    toSend.replace(F("_UNIT_NAME_"), mqtt_fn);
    toSend.replace(F("_VERSION_"), m2mqtt_version);
    //temp
    if (useFahrenheit) toSend.replace(F("_TU_FAH_"), F("selected"));
    else toSend.replace(F("_TU_CEL_"), F("selected"));
    //mode
    if (supportHeatMode) toSend.replace(F("_MD_ALL_"), F("selected"));
    else toSend.replace(F("_MD_NONHEAT_"), F("selected"));
    toSend.replace(F("_LOGIN_PASSWORD_"), login_password);
    server.send(200, F("text/html"), toSend);
  }
}

void handleWifi() {
  checkLogin();
  if (server.hasArg("save")) {
    saveWifi(server.arg("ssid"), server.arg("psk"), server.arg("hn"), server.arg("otapwd"));
    String headerContent = FPSTR(html_common_header);
    String rebootPage =  FPSTR(html_page_save_reboot);
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + rebootPage + footerContent;
    toSend.replace(F("_UNIT_NAME_"), hostname);
    toSend.replace(F("_VERSION_"), m2mqtt_version);
    server.send(200, F("text/html"), toSend);
    delay(10);
    ESP.reset();
  }
  else {
    String headerContent = FPSTR(html_common_header);
    String wifiPage =  FPSTR(html_page_wifi);
    String footerContent = FPSTR(html_common_footer);
    String toSend = headerContent + wifiPage + footerContent;
    toSend.replace(F("_UNIT_NAME_"), hostname);
    toSend.replace(F("_SSID_"), ap_ssid);
    toSend.replace(F("_PSK_"), ap_pwd);
    toSend.replace(F("_OTA_PWD_"), ota_pwd);
    toSend.replace(F("_VERSION_"), m2mqtt_version);
    server.send(200, F("text/html"), toSend);
  }

}

void handleStatus() {
  String headerContent = FPSTR(html_common_header);
  String statusPage =  FPSTR(html_page_status);
  String footerContent = FPSTR(html_common_footer);
  String toSend = headerContent + statusPage + footerContent;
  if (server.hasArg("mrconn")) mqttConnect();
  toSend.replace(F("_UNIT_NAME_"), hostname);
  toSend.replace(F("_VERSION_"), m2mqtt_version);
  String connected = F("<font color='green'><b>CONNECTED</b></font>");
  String disconnected = F("<font color='red'><b>DISCONNECTED</b></font>");
  if ((Serial) and hp.isConnected()) toSend.replace(F("_HVAC_STATUS_"), connected);
  else  toSend.replace(F("_HVAC_STATUS_"), disconnected);
  if (mqtt_client.connected()) toSend.replace(F("_MQTT_STATUS_"), connected);
  else toSend.replace(F("_MQTT_STATUS_"), disconnected);
  toSend.replace(F("_MQTT_REASON_"), String(mqtt_client.state()));
  toSend.replace(F("_WIFI_STATUS_"), String(WiFi.RSSI()));
  server.send(200, F("text/html"), toSend);
}



void handleControl() {
  checkLogin();
  //not connected to hp, redirect to status page
  if (!hp.isConnected()) {
    server.sendHeader("Location", "/status");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  heatpumpSettings settings = hp.getSettings();
  settings = change_states(settings);
  String controlPage =  FPSTR(html_page_control);
  String headerContent = FPSTR(html_common_header);
  String footerContent = FPSTR(html_common_footer);
  //write_log("Enter HVAC control");
  headerContent.replace("_UNIT_NAME_", hostname);
  footerContent.replace("_VERSION_", m2mqtt_version);
  controlPage.replace("_UNIT_NAME_", hostname);
  controlPage.replace("_RATE_", "60");
  controlPage.replace("_ROOMTEMP_", String(getTemperature(hp.getRoomTemperature(), useFahrenheit)));
  controlPage.replace("_USE_FAHRENHEIT_", (String)useFahrenheit);
  controlPage.replace("_TEMP_SCALE_", getTemperatureScale());
  controlPage.replace("_HEAT_MODE_SUPPORT_", (String)supportHeatMode);

  if (strcmp(settings.power, "ON") == 0) {
    controlPage.replace("_POWER_ON_", "selected");
  }
  else if (strcmp(settings.power, "OFF") == 0) {
    controlPage.replace("_POWER_OFF_", "selected");
  }

  if (strcmp(settings.mode, "HEAT") == 0) {
    controlPage.replace("_MODE_H_", "selected");
  }
  else if (strcmp(settings.mode, "DRY") == 0) {
    controlPage.replace("_MODE_D_", "selected");
  }
  else if (strcmp(settings.mode, "COOL") == 0) {
    controlPage.replace("_MODE_C_", "selected");
  }
  else if (strcmp(settings.mode, "FAN") == 0) {
    controlPage.replace("_MODE_F_", "selected");
  }
  else if (strcmp(settings.mode, "AUTO") == 0) {
    controlPage.replace("_MODE_A_", "selected");
  }

  if (strcmp(settings.fan, "AUTO") == 0) {
    controlPage.replace("_FAN_A_", "selected");
  }
  else if (strcmp(settings.fan, "QUIET") == 0) {
    controlPage.replace("_FAN_Q_", "selected");
  }
  else if (strcmp(settings.fan, "1") == 0) {
    controlPage.replace("_FAN_1_", "selected");
  }
  else if (strcmp(settings.fan, "2") == 0) {
    controlPage.replace("_FAN_2_", "selected");
  }
  else if (strcmp(settings.fan, "3") == 0) {
    controlPage.replace("_FAN_3_", "selected");
  }
  else if (strcmp(settings.fan, "4") == 0) {
    controlPage.replace("_FAN_4_", "selected");
  }

  controlPage.replace("_VANE_V_", settings.vane);
  if (strcmp(settings.vane, "AUTO") == 0) {
    controlPage.replace("_VANE_A_", "selected");
  }
  else if (strcmp(settings.vane, "1") == 0) {
    controlPage.replace("_VANE_1_", "selected");
  }
  else if (strcmp(settings.vane, "2") == 0) {
    controlPage.replace("_VANE_2_", "selected");
  }
  else if (strcmp(settings.vane, "3") == 0) {
    controlPage.replace("_VANE_3_", "selected");
  }
  else if (strcmp(settings.vane, "4") == 0) {
    controlPage.replace("_VANE_4_", "selected");
  }
  else if (strcmp(settings.vane, "5") == 0) {
    controlPage.replace("_VANE_5_", "selected");
  }
  else if (strcmp(settings.vane, "SWING") == 0) {
    controlPage.replace("_VANE_S_", "selected");
  }

  controlPage.replace("_WIDEVANE_V_", settings.wideVane);
  if (strcmp(settings.wideVane, "<<") == 0) {
    controlPage.replace("_WVANE_1_", "selected");
  }
  else if (strcmp(settings.wideVane, "<") == 0) {
    controlPage.replace("_WVANE_2_", "selected");
  }
  else if (strcmp(settings.wideVane, "|") == 0) {
    controlPage.replace("_WVANE_3_", "selected");
  }
  else if (strcmp(settings.wideVane, ">") == 0) {
    controlPage.replace("_WVANE_4_", "selected");
  }
  else if (strcmp(settings.wideVane, ">>") == 0) {
    controlPage.replace("_WVANE_5_", "selected");
  }
  else if (strcmp(settings.wideVane, "SWING") == 0) {
    controlPage.replace("_WVANE_S_", "selected");
  }

  controlPage.replace("_TEMP_", String(getTemperature(hp.getTemperature(), useFahrenheit)));

  // We need to send the page content in chunks to overcome
  // a limitation on the maximum size we can send at one
  // time (approx 6k).
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", headerContent);
  server.sendContent(controlPage);
  server.sendContent(footerContent);
  // Signal the end of the content
  server.sendContent("");
  //delay(100);
}

//login page, also called for logout
void handleLogin() {
  bool loginSuccess = false;
  String msg;
  String loginPage =  FPSTR(html_page_login);
  if (server.hasHeader("Cookie")) {
    //Found cookie;
    String cookie = server.header("Cookie");
  }
  if (server.hasArg("USERNAME") || server.hasArg("PASSWORD") || server.hasArg("LOGOUT")) {
    if (server.hasArg("LOGOUT")) {
      //logout
      server.sendHeader("Cache-Control", "no-cache");
      server.sendHeader("Set-Cookie", "M2MSESSIONID=0");
      loginSuccess = false;
    }
    if (server.hasArg("USERNAME") && server.hasArg("PASSWORD")) {
      if (server.arg("USERNAME") == "admin" &&  server.arg("PASSWORD") == login_password) {
        server.sendHeader("Cache-Control", "no-cache");
        server.sendHeader("Set-Cookie", "M2MSESSIONID=1");
        loginSuccess = true;
        msg = F("<b><font color='red'>Login successful, you will be redirect in few seconds.</font></b>");
        loginPage += F("<script>");
        loginPage += F("setTimeout(function () {");
        loginPage += F("window.location.href= '/';");
        loginPage += F("}, 3000);");
        loginPage += F("</script>");
        //Log in Successful;
      } else {
        msg = F("<b><font color='red'>Wrong username/password! try again.</font></b>");
        //Log in Failed;
      }
    }
  }
  else {
    if (is_authenticated() or login_password.length() == 0) {
      server.sendHeader("Location", "/");
      server.sendHeader("Cache-Control", "no-cache");
      //use javascript in the case browser disable redirect
      String redirectPage = F("<html lang=\"en\" class=\"\"><head><meta charset='utf-8'>");
      redirectPage += F("<script>");
      redirectPage += F("setTimeout(function () {");
      redirectPage += F("window.location.href= '/';");
      redirectPage += F("}, 1000);");
      redirectPage += F("</script>");
      redirectPage += F("</body></html>");
      server.send(301, F("text/html"), redirectPage);
      return;
    }
  }
  String headerContent = FPSTR(html_common_header);
  String footerContent = FPSTR(html_common_footer);
  String toSend = headerContent + loginPage + footerContent;
  toSend.replace(F("_UNIT_NAME_"), hostname);
  toSend.replace(F("_VERSION_"), m2mqtt_version);
  toSend.replace(F("_LOGIN_SUCCESS_"), (String) loginSuccess);
  toSend.replace(F("_LOGIN_MSG_"), msg);
  server.send(200, F("text/html"), toSend);
}

void handleUpgrade()
{
  uploaderror = 0;
  String headerContent = FPSTR(html_common_header);
  String upgradePage =  FPSTR(html_page_upgrade);
  String footerContent = FPSTR(html_common_footer);
  String toSend = headerContent + upgradePage + footerContent;
  toSend.replace(F("_UNIT_NAME_"), hostname);
  toSend.replace(F("_VERSION_"), m2mqtt_version);
  server.send(200, F("text/html"), toSend);
}

void handleUploadDone()
{
  //Serial.printl(PSTR("HTTP: Firmware upload done"));
  bool restartflag = false;
  String headerContent = FPSTR(html_common_header);
  String uploadDonePage = FPSTR(html_page_upload);
  String content = F("<div style='text-align:center;'><b>Upload ");
  if (uploaderror) {
    content += F("<font color='red'>failed</font></b>");
    if (uploaderror == 1) {
      content += F("<br/><br/>No file selected");
    } else if (uploaderror == 2) {
      content += F("<br/><br/>File size is larger than available free space");
    } else if (uploaderror == 3) {
      content += F("<br/><br/>File magic header does not start with 0xE9");
    } else if (uploaderror == 4) {
      content += F("<br/><br/>File flash size is larger than device flash size");
    } else if (uploaderror == 5) {
      content += F("<br/><br/>File upload buffer miscompare");
    } else if (uploaderror == 6) {
      content += F("<br/><br/>Upload failed. Enable logging option 3 for more information");
    } else if (uploaderror == 7) {
      content += F("<br/><br/>Upload aborted");
    } else {
      content += F("<br/><br/>Upload error code ");
      content += String(uploaderror);
    }
    if (Update.hasError()) {
      content += F("<br/><br/>Update error code (see Updater.cpp) ");
      content += String(Update.getError());
    }
  } else {
    content += F("<b><font color='green'>successful</font></b><br/><br/>Device will restart in a few seconds");
    content += F("<script>");
    content += F("setTimeout(function () {");
    content += F("window.location.href= '/';");
    content += F("}, 10000);");
    content += F("</script>");
    restartflag = true;
  }
  content += F("</div><br/>");
  uploadDonePage.replace("_UPLOAD_MSG_", content);
  String footerContent = FPSTR(html_common_footer);
  String toSend = headerContent + uploadDonePage + footerContent;
  toSend.replace("_UNIT_NAME_", hostname);
  toSend.replace("_VERSION_", m2mqtt_version);
  server.send(200, "text/html", toSend);
  if (restartflag) {
    delay(10);
    ESP.reset();
  }
}

void handleUploadLoop()
{
  // Based on ESP8266HTTPUpdateServer.cpp uses ESP8266WebServer Parsing.cpp and Cores Updater.cpp (Update)
  //char log[200];
  if (uploaderror) {
    Update.end();
    return;
  }
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (upload.filename.c_str()[0] == 0)
    {
      uploaderror = 1;
      return;
    }
    //save cpu by disconnect/stop retry mqtt server
    if (mqtt_client.state() == MQTT_CONNECTED) {
      mqtt_client.disconnect();
      lastMqttRetry = millis();
    }
    //snprintf_P(log, sizeof(log), PSTR("Upload: File %s ..."), upload.filename.c_str());
    //Serial.printl(log);
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketchSpace)) {         //start with max available size
      //Update.printError(Serial);
      uploaderror = 2;
      return;
    }
  } else if (!uploaderror && (upload.status == UPLOAD_FILE_WRITE)) {
    if (upload.totalSize == 0)
    {
      if (upload.buf[0] != 0xE9) {
        //Serial.println(PSTR("Upload: File magic header does not start with 0xE9"));
        uploaderror = 3;
        return;
      }
      uint32_t bin_flash_size = ESP.magicFlashChipSize((upload.buf[3] & 0xf0) >> 4);
      if (bin_flash_size > ESP.getFlashChipRealSize()) {
        //Serial.printl(PSTR("Upload: File flash size is larger than device flash size"));
        uploaderror = 4;
        return;
      }
      if (ESP.getFlashChipMode() == 3) {
        upload.buf[2] = 3; // DOUT - ESP8285
      } else {
        upload.buf[2] = 2; // DIO - ESP8266
      }
    }
    if (!uploaderror && (Update.write(upload.buf, upload.currentSize) != upload.currentSize)) {
      //Update.printError(Serial);
      uploaderror = 5;
      return;
    }
  } else if (!uploaderror && (upload.status == UPLOAD_FILE_END)) {
    if (Update.end(true)) { // true to set the size to the current progress
      //snprintf_P(log, sizeof(log), PSTR("Upload: Successful %u bytes. Restarting"), upload.totalSize);
      //Serial.printl(log)
    } else {
      //Update.printError(Serial);
      uploaderror = 6;
      return;
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    //Serial.println(PSTR("Upload: Update was aborted"));
    uploaderror = 7;
    Update.end();
  }
  delay(0);
}

void write_log(String log) {
  File logFile = SPIFFS.open(console_file, "a");
  logFile.println(log);
  logFile.close();
}

heatpumpSettings change_states(heatpumpSettings settings) {
  if (server.hasArg("CONNECT")) {
    hp.connect(&Serial);
  }
  else {
    bool update = false;
    if (server.hasArg("POWER")) {
      settings.power = server.arg("POWER").c_str();
      update = true;
    }
    if (server.hasArg("MODE")) {
      settings.mode = server.arg("MODE").c_str();
      update = true;
    }
    if (server.hasArg("TEMP")) {
      settings.temperature = setTemperature(server.arg("TEMP").toInt(), useFahrenheit);
      update = true;
    }
    if (server.hasArg("FAN")) {
      settings.fan = server.arg("FAN").c_str();
      update = true;
    }
    if (server.hasArg("VANE")) {
      settings.vane = server.arg("VANE").c_str();
      update = true;
    }
    if (server.hasArg("WIDEVANE")) {
      settings.wideVane = server.arg("WIDEVANE").c_str();
      update = true;
    }
    if (update) {
      hp.setSettings(settings);
      hp.update();
    }
  }
  return settings;
}

void hpSettingsChanged() {
  // send room temp, operating info and all information
  heatpumpSettings currentSettings = hp.getSettings();
  
  const size_t bufferSizeInfo = JSON_OBJECT_SIZE(6);
  StaticJsonDocument<bufferSizeInfo> rootInfo;

  
  rootInfo["power"]       = currentSettings.power;
  rootInfo["mode"]        = currentSettings.mode;
  rootInfo["temperature"] = currentSettings.temperature;
  rootInfo["fan"]         = currentSettings.fan;
  rootInfo["vane"]        = currentSettings.vane;
  rootInfo["wideVane"]    = currentSettings.wideVane;
  //root["iSee"]        = currentSettings.iSee;

  String mqttOutput;
  serializeJson(rootInfo, mqttOutput);

  if (!mqtt_client.publish_P(heatpump_topic.c_str(), mqttOutput.c_str(), true)) {
    mqtt_client.publish(heatpump_debug_topic.c_str(), "failed to publish to heatpump topic");
  }
}


void hpStatusChanged(heatpumpStatus currentStatus) {

  // send room temp and operating info
  const size_t bufferSizeInfo = JSON_OBJECT_SIZE(2);
  StaticJsonDocument<bufferSizeInfo> rootInfo;

  rootInfo["roomTemperature"] = currentStatus.roomTemperature;
  rootInfo["operating"]       = currentStatus.operating;

  String mqttInfo;
  serializeJson(rootInfo, mqttInfo);

  if (!mqtt_client.publish_P(heatpump_status_topic.c_str(), mqttInfo.c_str(), false)) {
    mqtt_client.publish(heatpump_debug_topic.c_str(), (char*)(F("Failed to publish hp status change")));
  }

  // send the timer info
  const size_t bufferSizeTimers = JSON_OBJECT_SIZE(5);
  StaticJsonDocument<bufferSizeTimers> rootTimers;

  rootTimers["mode"]          = currentStatus.timers.mode;
  rootTimers["onMins"]        = currentStatus.timers.onMinutesSet;
  rootTimers["onRemainMins"]  = currentStatus.timers.onMinutesRemaining;
  rootTimers["offMins"]       = currentStatus.timers.offMinutesSet;
  rootTimers["offRemainMins"] = currentStatus.timers.offMinutesRemaining;

  String mqttTimers;
  serializeJson(rootTimers, mqttTimers);

  if (!mqtt_client.publish_P(heatpump_timers_topic.c_str(), mqttTimers.c_str(), true)) {
    mqtt_client.publish(heatpump_debug_topic.c_str(), (char*)(F("failed to publish timer info to heatpump/status topic")));
  }

}

void hpPacketDebug(byte* packet, unsigned int length, char* packetDirection) {
  if (_debugMode) {
    String message;
    for (int idx = 0; idx < length; idx++) {
      if (packet[idx] < 16) {
        message += "0"; // pad single hex digits with a 0
      }
      message += String(packet[idx], HEX) + " ";
    }

    const size_t bufferSize = JSON_OBJECT_SIZE(1);
    StaticJsonDocument<bufferSize> root;

    root[packetDirection] = message;
    String mqttOutput;
    serializeJson(root, mqttOutput);
    if (!mqtt_client.publish(heatpump_debug_topic.c_str(), mqttOutput.c_str())) {
      mqtt_client.publish(heatpump_debug_topic.c_str(), (char*)(F("Failed to publish to heatpump/debug topic")));
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {

  // Copy payload into message buffer
  char message[length + 1];
  for (int i = 0; i < length; i++) {
    message[i] = (char)payload[i];
  }
  message[length] = '\0';

  if (strcmp(topic, heatpump_set_topic.c_str()) == 0) { //if the incoming message is on the heatpump_set_topic topic...
    // Parse message into JSON
    const size_t bufferSize = JSON_OBJECT_SIZE(6);
    DynamicJsonDocument root(bufferSize);
    DeserializationError error = deserializeJson(root, message);

    if (error) {
      mqtt_client.publish(heatpump_debug_topic.c_str(), (char*)(F("!root.success(): invalid JSON on heatpump_set_topic...")));
      return;
    }

    // Step 3: Retrieve the values
    if (root.containsKey("power")) {
      const char* power = root["power"];
      hp.setPowerSetting(power);
    }

    if (root.containsKey("mode")) {
      const char* mode = root["mode"];
      hp.setModeSetting(mode);
    }

    if (root.containsKey("temperature")) {
      float temperature = root["temperature"];
      hp.setTemperature(temperature);
    }

    if (root.containsKey("fan")) {
      const char* fan = root["fan"];
      hp.setFanSpeed(fan);
    }

    if (root.containsKey("vane")) {
      const char* vane = root["vane"];
      hp.setVaneSetting(vane);
    }

    if (root.containsKey("wideVane")) {
      const char* wideVane = root["wideVane"];
      hp.setWideVaneSetting(wideVane);
    }

    if (root.containsKey("remoteTemp")) {
      float remoteTemp = root["remoteTemp"];
      hp.setRemoteTemperature(remoteTemp);
    }
    else if (root.containsKey("custom")) {
      String custom = root["custom"];

      // copy custom packet to char array
      char buffer[(custom.length() + 1)]; // +1 for the NULL at the end
      custom.toCharArray(buffer, (custom.length() + 1));

      byte bytes[20]; // max custom packet bytes is 20
      int byteCount = 0;
      char *nextByte;

      // loop over the byte string, breaking it up by spaces (or at the end of the line - \n)
      nextByte = strtok(buffer, " ");
      while (nextByte != NULL && byteCount < 20) {
        bytes[byteCount] = strtol(nextByte, NULL, 16); // convert from hex string
        nextByte = strtok(NULL, "   ");
        byteCount++;
      }

      // dump the packet so we can see what it is. handy because you can run the code without connecting the ESP to the heatpump, and test sending custom packets
      hpPacketDebug(bytes, byteCount, "customPacket");

      hp.sendCustomPacket(bytes, byteCount);
    }
    else {
      bool result = hp.update();

      if (!result) {
        mqtt_client.publish(heatpump_debug_topic.c_str(), (char*)(F("heatpump: update() failed")));
      }
    }

  } else if (strcmp(topic, heatpump_debug_set_topic.c_str()) == 0) { //if the incoming message is on the heatpump_debug_set_topic topic...
    if (strcmp(message, "on") == 0) {
      _debugMode = true;
      mqtt_client.publish(heatpump_debug_topic.c_str(), (char*)(F("debug mode enabled")));
    } else if (strcmp(message, "off") == 0) {
      _debugMode = false;
      mqtt_client.publish(heatpump_debug_topic.c_str(), (char*)(F("debug mode disabled")));
    }
  } else {//should never get called, as that would mean something went wrong with subscribe
    mqtt_client.publish(heatpump_debug_topic.c_str(), (char*)(F("heatpump: wrong topic received")));
  }
}

void mqttConnect() {
  // Loop until we're reconnected
  int attempts = 0;
  while (!mqtt_client.connected()) {
    // Attempt to connect
    mqtt_client.connect(mqtt_client_id.c_str(), mqtt_username.c_str(), mqtt_password.c_str());
    // If state < 0 (MQTT_CONNECTED) => network problem we retry 5 times and then waiting for MQTT_RETRY_INTERVAL_MS and retry reapeatly
    if (mqtt_client.state() < MQTT_CONNECTED) {
      if (attempts == 5) {
        lastMqttRetry = millis();
        return;
      }
      else {
        delay(10);
        attempts++;
      }
    }
    // If state > 0 (MQTT_CONNECTED) => config or server problem we stop retry
    else if (mqtt_client.state() > MQTT_CONNECTED) {
      return;
    }
    // We are connected
    else    {
      mqtt_client.subscribe(heatpump_set_topic.c_str());
      mqtt_client.subscribe(heatpump_debug_set_topic.c_str());
    }
  }
}

bool connectWifi() {
  WiFi.hostname(hostname);
  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
    delay(10);
  }
  WiFi.begin(ap_ssid, ap_pwd);
  Serial.println("Connecting to " + ap_ssid);
  unsigned long wifiStartTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStartTime < 10000) {
    Serial.write('.');
    //Serial.print(WiFi.status());
    // wait 500ms, flashing the blue LED to indicate WiFi connecting...
    digitalWrite(blueLedPin, LOW);
    delay(250);
    digitalWrite(blueLedPin, HIGH);
    delay(250);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Failed to connect to wifi"));
    return false;
  }
  Serial.println(F("Connected to "));
  Serial.println(ap_ssid);
  Serial.println(F("Ready"));
  Serial.print("IP address: ");
  unsigned long dhcpStartTime = millis();
  while ((WiFi.localIP().toString() == "0.0.0.0" || WiFi.localIP().toString() == "") && millis() - dhcpStartTime < 5000) {
    Serial.write('.');
    delay(500);
  }
  if (WiFi.localIP().toString() == "0.0.0.0" || WiFi.localIP().toString() == "") {
    Serial.println(F("Failed to get IP address"));
    return false;
  }
  Serial.println(WiFi.localIP());
  //ticker.detach(); // Stop blinking the LED because now we are connected:)
  //keep LED off (For Wemos D1-Mini)
  digitalWrite(blueLedPin, HIGH);
  return true;
}

// temperature helper functions
float toFahrenheit(float fromCelcius) {
  return round(1.8 * fromCelcius + 32.0);
}

float toCelsius(float fromFahrenheit) {
  return (fromFahrenheit - 32.0) / 1.8;
}

float getTemperature(float temperature, bool isFahrenheit) {
  if (isFahrenheit) {
    return toFahrenheit(temperature);
  } else {
    return temperature;
  }
}

float setTemperature(float temperature, bool isFahrenheit) {
  if (isFahrenheit) {
    return toCelsius(temperature);
  } else {
    return temperature;
  }
}

String getTemperatureScale() {
  if (useFahrenheit) {
    return "F";
  } else {
    return "C";
  }
}

//Check if header is present and correct
bool is_authenticated() {
  if (server.hasHeader("Cookie")) {
    //Found cookie;
    String cookie = server.header("Cookie");
    if (cookie.indexOf("M2MSESSIONID=1") != -1) {
      //Authentication Successful
      return true;
    }
  }
  //Authentication Failed
  return false;
}

void checkLogin() {
  if (!is_authenticated() and login_password.length() > 0) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    //use javascript in the case browser disable redirect
    String redirectPage = F("<html lang=\"en\" class=\"\"><head><meta charset='utf-8'>");
    redirectPage += F("<script>");
    redirectPage += F("setTimeout(function () {");
    redirectPage += F("window.location.href= '/login';");
    redirectPage += F("}, 1000);");
    redirectPage += F("</script>");
    redirectPage += F("</body></html>");
    server.send(301, F("text/html"), redirectPage);
    return;
  }
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  if (!captive and mqtt_config) {
    // Sync HVAC UNIT
    hp.sync();
    //MQTT failed retry to connect
    if (mqtt_client.state() < MQTT_CONNECTED)
    {
      if ((millis() > (lastMqttRetry + MQTT_RETRY_INTERVAL_MS)) or lastMqttRetry == 0) {
        mqttConnect();
      }
    }
    //MQTT config problem on MQTT do nothing
    else if (mqtt_client.state() > MQTT_CONNECTED ) return;
    //MQTT connected send status
    else {
      if (millis() > (lastTempSend + SEND_ROOM_TEMP_INTERVAL_MS)) { // only send the temperature every SEND_ROOM_TEMP_INTERVAL_MS
        hpStatusChanged(hp.getStatus());
        lastTempSend = millis();
      }
      mqtt_client.loop();
    }
  }
  else {
    dnsServer.processNextRequest();
  }
}
