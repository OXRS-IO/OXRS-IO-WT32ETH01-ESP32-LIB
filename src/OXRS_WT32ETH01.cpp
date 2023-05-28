/*
 * OXRS_WT32ETH01.cpp
 */

#include "Arduino.h"
#include "OXRS_WT32ETH01.h"

#include <WebServer_WT32_ETH01.h>     // For WT32-ETH01 networking from https://github.com/khoih-prog/WebServer_WT32_ETH01
#include <LittleFS.h>                 // For file system access
#include <MqttLogger.h>               // For logging

// Macro for converting env vars to strings
#define STRINGIFY(s) STRINGIFY1(s)
#define STRINGIFY1(s) #s

// Network client (for MQTT)/server (for REST API)
WiFiClient _client;
WiFiServer _server(REST_API_PORT);

// MQTT client
PubSubClient _mqttClient(_client);
OXRS_MQTT _mqtt(_mqttClient);

// REST API
OXRS_API _api(_mqtt);

// Logging (topic updated once MQTT connects successfully)
MqttLogger _logger(_mqttClient, "log", MQTT_LOGGER_MODE);

// Supported firmware config and command schemas
DynamicJsonDocument _fwConfigSchema(JSON_CONFIG_MAX_SIZE);
DynamicJsonDocument _fwCommandSchema(JSON_COMMAND_MAX_SIZE);

// MQTT callbacks wrapped by _mqttConfig/_mqttCommand
jsonCallback _onConfig;
jsonCallback _onCommand;

/* JSON helpers */
void _mergeJson(JsonVariant dst, JsonVariantConst src)
{
  if (src.is<JsonObjectConst>())
  {
    for (JsonPairConst kvp : src.as<JsonObjectConst>())
    {
      if (dst[kvp.key()])
      {
        _mergeJson(dst[kvp.key()], kvp.value());
      }
      else
      {
        dst[kvp.key()] = kvp.value();
      }
    }
  }
  else
  {
    dst.set(src);
  }
}

/* Adoption info builders */
void _getFirmwareJson(JsonVariant json)
{
  JsonObject firmware = json.createNestedObject("firmware");

  firmware["name"] = FW_NAME;
  firmware["shortName"] = FW_SHORT_NAME;
  firmware["maker"] = FW_MAKER;
  firmware["version"] = STRINGIFY(FW_VERSION);
  
#if defined(FW_GITHUB_URL)
  firmware["githubUrl"] = FW_GITHUB_URL;
#endif
}

void _getSystemJson(JsonVariant json)
{
  JsonObject system = json.createNestedObject("system");

  system["heapUsedBytes"] = ESP.getHeapSize();
  system["heapFreeBytes"] = ESP.getFreeHeap();
  system["heapMaxAllocBytes"] = ESP.getMaxAllocHeap();
  system["flashChipSizeBytes"] = ESP.getFlashChipSize();

  system["sketchSpaceUsedBytes"] = ESP.getSketchSize();
  system["sketchSpaceTotalBytes"] = ESP.getFreeSketchSpace();

  system["fileSystemUsedBytes"] = LittleFS.usedBytes();
  system["fileSystemTotalBytes"] = LittleFS.totalBytes();
}

void _getNetworkJson(JsonVariant json)
{
  JsonObject network = json.createNestedObject("network");

  network["mode"] = "ethernet";
  network["ip"] = ETH.localIP();
  network["mac"] = ETH.macAddress();
}

void _getConfigSchemaJson(JsonVariant json)
{
  JsonObject configSchema = json.createNestedObject("configSchema");
  
  // Config schema metadata
  configSchema["$schema"] = JSON_SCHEMA_VERSION;
  configSchema["title"] = FW_SHORT_NAME;
  configSchema["type"] = "object";

  JsonObject properties = configSchema.createNestedObject("properties");

  // Firmware config schema (if any)
  if (!_fwConfigSchema.isNull())
  {
    _mergeJson(properties, _fwConfigSchema.as<JsonVariant>());
  }
}

void _getCommandSchemaJson(JsonVariant json)
{
  JsonObject commandSchema = json.createNestedObject("commandSchema");
  
  // Command schema metadata
  commandSchema["$schema"] = JSON_SCHEMA_VERSION;
  commandSchema["title"] = FW_SHORT_NAME;
  commandSchema["type"] = "object";

  JsonObject properties = commandSchema.createNestedObject("properties");

  // Firmware command schema (if any)
  if (!_fwCommandSchema.isNull())
  {
    _mergeJson(properties, _fwCommandSchema.as<JsonVariant>());
  }

  // Generic commands
  JsonObject restart = properties.createNestedObject("restart");
  restart["title"] = "Restart";
  restart["type"] = "boolean";
}

/* API callbacks */
void _apiAdopt(JsonVariant json)
{
  // Build device adoption info
  _getFirmwareJson(json);
  _getSystemJson(json);
  _getNetworkJson(json);
  _getConfigSchemaJson(json);
  _getCommandSchemaJson(json);
}

/* MQTT callbacks */
void _mqttConnected() 
{
  // MqttLogger doesn't copy the logging topic to an internal
  // buffer so we have to use a static array here
  static char logTopic[64];
  _logger.setTopic(_mqtt.getLogTopic(logTopic));

  // Publish device adoption info
  DynamicJsonDocument json(JSON_ADOPT_MAX_SIZE);
  _mqtt.publishAdopt(_api.getAdopt(json.as<JsonVariant>()));

  // Log the fact we are now connected
  _logger.println("[wt32] mqtt connected");
}

void _mqttDisconnected(int state) 
{
  // Log the disconnect reason
  // See https://github.com/knolleary/pubsubclient/blob/2d228f2f862a95846c65a8518c79f48dfc8f188c/src/PubSubClient.h#L44
  switch (state)
  {
    case MQTT_CONNECTION_TIMEOUT:
      _logger.println(F("[wt32] mqtt connection timeout"));
      break;
    case MQTT_CONNECTION_LOST:
      _logger.println(F("[wt32] mqtt connection lost"));
      break;
    case MQTT_CONNECT_FAILED:
      _logger.println(F("[wt32] mqtt connect failed"));
      break;
    case MQTT_DISCONNECTED:
      _logger.println(F("[wt32] mqtt disconnected"));
      break;
    case MQTT_CONNECT_BAD_PROTOCOL:
      _logger.println(F("[wt32] mqtt bad protocol"));
      break;
    case MQTT_CONNECT_BAD_CLIENT_ID:
      _logger.println(F("[wt32] mqtt bad client id"));
      break;
    case MQTT_CONNECT_UNAVAILABLE:
      _logger.println(F("[wt32] mqtt unavailable"));
      break;
    case MQTT_CONNECT_BAD_CREDENTIALS:
      _logger.println(F("[wt32] mqtt bad credentials"));
      break;      
    case MQTT_CONNECT_UNAUTHORIZED:
      _logger.println(F("[wt32] mqtt unauthorised"));
      break;      
  }
}

void _mqttConfig(JsonVariant json)
{
  // Pass on to the firmware callback
  if (_onConfig) { _onConfig(json); }
}

void _mqttCommand(JsonVariant json)
{
  // Check for GPIO32 commands
  if (json.containsKey("restart") && json["restart"].as<bool>())
  {
    ESP.restart();
  }

  // Pass on to the firmware callback
  if (_onCommand) { _onCommand(json); }
}

void _mqttCallback(char * topic, byte * payload, int length) 
{
  // Pass down to our MQTT handler and check it was processed ok
  int state = _mqtt.receive(topic, payload, length);
  switch (state)
  {
    case MQTT_RECEIVE_ZERO_LENGTH:
      _logger.println(F("[wt32] empty mqtt payload received"));
      break;
    case MQTT_RECEIVE_JSON_ERROR:
      _logger.println(F("[wt32] failed to deserialise mqtt json payload"));
      break;
    case MQTT_RECEIVE_NO_CONFIG_HANDLER:
      _logger.println(F("[wt32] no mqtt config handler"));
      break;
    case MQTT_RECEIVE_NO_COMMAND_HANDLER:
      _logger.println(F("[wt32] no mqtt command handler"));
      break;
  }
}

/* Main program */
void OXRS_WT32ETH01::setMqttBroker(const char * broker, uint16_t port)
{
  _mqtt.setBroker(broker, port);
}

void OXRS_WT32ETH01::setMqttClientId(const char * clientId)
{
  _mqtt.setClientId(clientId);
}

void OXRS_WT32ETH01::setMqttAuth(const char * username, const char * password)
{
  _mqtt.setAuth(username, password);
}

void OXRS_WT32ETH01::setMqttTopicPrefix(const char * prefix)
{
  _mqtt.setTopicPrefix(prefix);
}

void OXRS_WT32ETH01::setMqttTopicSuffix(const char * suffix)
{
  _mqtt.setTopicSuffix(suffix);
}

void OXRS_WT32ETH01::begin(jsonCallback config, jsonCallback command)
{
  // Get our firmware details
  DynamicJsonDocument json(512);
  _getFirmwareJson(json.as<JsonVariant>());

  // Log firmware details
  _logger.print(F("[wt32] "));
  serializeJson(json, _logger);
  _logger.println();

  // We wrap the callbacks so we can intercept messages intended for the GPIO32
  _onConfig = config;
  _onCommand = command;
  
  // Set up network and obtain an IP address
  byte mac[6];
  _initialiseNetwork(mac);

  // Set up MQTT (don't attempt to connect yet)
  _initialiseMqtt(mac);

  // Set up the REST API
  _initialiseRestApi();
}

void OXRS_WT32ETH01::loop(void)
{
  // Check our network connection
  if (_isNetworkConnected())
  {
    // Handle any MQTT messages
    _mqtt.loop();
    
    // Handle any REST API requests
    WiFiClient client = _server.available();
    _api.loop(&client);
  }
}

void OXRS_WT32ETH01::setConfigSchema(JsonVariant json)
{
  _fwConfigSchema.clear();
  _mergeJson(_fwConfigSchema.as<JsonVariant>(), json);
}

void OXRS_WT32ETH01::setCommandSchema(JsonVariant json)
{
  _fwCommandSchema.clear();
  _mergeJson(_fwCommandSchema.as<JsonVariant>(), json);
}

void OXRS_WT32ETH01::apiGet(const char * path, Router::Middleware * middleware)
{
  _api.get(path, middleware);
}

void OXRS_WT32ETH01::apiPost(const char * path, Router::Middleware * middleware)
{
  _api.post(path, middleware);
}

boolean OXRS_WT32ETH01::publishStatus(JsonVariant json)
{
  // Exit early if no network connection
  if (!_isNetworkConnected()) { return false; }
  return _mqtt.publishStatus(json);
}

boolean OXRS_WT32ETH01::publishTelemetry(JsonVariant json)
{
  // Exit early if no network connection
  if (!_isNetworkConnected()) { return false; }
  return _mqtt.publishTelemetry(json);
}

size_t OXRS_WT32ETH01::write(uint8_t character)
{
  // Pass to logger - allows firmware to use `GPIO32.println("Log this!")`
  return _logger.write(character);
}

void OXRS_WT32ETH01::_initialiseNetwork(byte * mac)
{
  // To be called before ETH.begin()
  WT32_ETH01_onEvent();

  // Start ethernet
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER);

  // Wait for ethernet to connect
  WT32_ETH01_waitForConnect();

  // Get the ethernet MAC address
  ETH.macAddress(mac);

  // Display the MAC address on serial
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  _logger.print(F("[wt32] mac address: "));
  _logger.println(mac_display);

  // Display IP address on serial
  _logger.print(F("[wt32] ip address: "));
  _logger.println(ETH.localIP());
}

void OXRS_WT32ETH01::_initialiseMqtt(byte * mac)
{
  // NOTE: this must be called *before* initialising the REST API since
  //       that will load MQTT config from file, which has precendence

  // Set the default client ID to last 3 bytes of the MAC address
  char clientId[32];
  sprintf_P(clientId, PSTR("%02x%02x%02x"), mac[3], mac[4], mac[5]);  
  _mqtt.setClientId(clientId);
  
  // Register our callbacks
  _mqtt.onConnected(_mqttConnected);
  _mqtt.onDisconnected(_mqttDisconnected);
  _mqtt.onConfig(_mqttConfig);
  _mqtt.onCommand(_mqttCommand);
  
  // Start listening for MQTT messages
  _mqttClient.setCallback(_mqttCallback);
}

void OXRS_WT32ETH01::_initialiseRestApi(void)
{
  // NOTE: this must be called *after* initialising MQTT since that sets
  //       the default client id, which has lower precendence than MQTT
  //       settings stored in file and loaded by the API

  // Set up the REST API
  _api.begin();
  
  // Register our callbacks
  _api.onAdopt(_apiAdopt);

  // Start listening
  _server.begin();
}

boolean OXRS_WT32ETH01::_isNetworkConnected(void)
{
  return WT32_ETH01_isConnected();
}