#include "wled.h"
#include <Ticker.h>

/*
 * MQTT communication protocol for home automation
 */

#ifndef WLED_DISABLE_MQTT

#define MQTT_KEEP_ALIVE_TIME 15       // было 60, быстрее ловит "мертвяк"
#define MQTT_RECONNECT_DELAY_MS 5000  // попытка реконнекта раз в 5 сек

bool initMqtt(); // forward declaration

static Ticker mqttReconnectTimer;

// Последние опубликованные значения в шкале HomeKit — для защиты от петли
static uint8_t  lastPublishedBri = 101; // невозможное (>100)
static uint16_t lastPublishedHue = 361; // невозможное (>360)
static uint8_t  lastPublishedSat = 101; // невозможное (>100)
static bool     lastPublishedOn  = false;

static void mqttReconnectNow()
{
  initMqtt();
}

static void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  mqttReconnectTimer.detach();
  mqttReconnectTimer.once_ms(MQTT_RECONNECT_DELAY_MS, mqttReconnectNow);
}

// Вспомогательная функция: RGB → Hue (0-360) и Saturation (0-100)
static void rgbToHS(float &h, float &s)
{
  float r = colPri[0] / 255.0f;
  float g = colPri[1] / 255.0f;
  float b = colPri[2] / 255.0f;
  float mx = MAX(r, MAX(g, b));
  float mn = MIN(r, MIN(g, b));
  float delta = mx - mn;

  s = (mx > 0) ? (delta / mx * 100.0f) : 0.0f;
  h = 0.0f;
  if (delta > 0) {
    if      (mx == r) h = 60.0f * fmodf((g - b) / delta, 6.0f);
    else if (mx == g) h = 60.0f * ((b - r) / delta + 2.0f);
    else              h = 60.0f * ((r - g) / delta + 4.0f);
    if (h < 0) h += 360.0f;
  }
}

// Вспомогательная функция: Hue + Saturation → RGB с максимальной яркостью, W=0
static void hsToRGB(float h, float s)
{
  s /= 100.0f;
  float c = s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = 1.0f - c;

  float nr = 0, ng = 0, nb = 0;
  if      (h < 60)  { nr = c; ng = x; }
  else if (h < 120) { nr = x; ng = c; }
  else if (h < 180) { ng = c; nb = x; }
  else if (h < 240) { ng = x; nb = c; }
  else if (h < 300) { nr = x; nb = c; }
  else              { nr = c; nb = x; }

  colPri[0] = (uint8_t)((nr + m) * 255);
  colPri[1] = (uint8_t)((ng + m) * 255);
  colPri[2] = (uint8_t)((nb + m) * 255);
  colPri[3] = 0;
}

// Применить пресет 101 (белый) или fallback цвет
static void applyWhitePreset()
{
  String presetName;
  if (getPresetName(101, presetName)) {
    applyPreset(101, CALL_MODE_DIRECT_CHANGE);
  } else {
    colorFromDecOrHexString(colPri, (char*)"#CECECEFA");
    colorUpdated(CALL_MODE_DIRECT_CHANGE);
  }
}

static void parseMQTTBriPayload(char* payload)
{
  if (strstr(payload, "ON") || strstr(payload, "on") || strstr(payload, "true")) {
    if (lastPublishedOn) return;
    String presetName;
    if (getPresetName(101, presetName)) {
      applyPreset(101, CALL_MODE_DIRECT_CHANGE);
    } else {
      bri = briLast;
      stateUpdated(CALL_MODE_DIRECT_CHANGE);
    }
  }
  else if (strstr(payload, "OFF") || strstr(payload, "off") || strstr(payload, "false")) {
    if (!lastPublishedOn) return;
    briLast = bri;
    bri = 0;
    stateUpdated(CALL_MODE_DIRECT_CHANGE);
  }
  else if (strstr(payload, "T") || strstr(payload, "t")) {
    toggleOnOff();
    stateUpdated(CALL_MODE_DIRECT_CHANGE);
  }
  else {
    uint8_t in = strtoul(payload, NULL, 10);
    if (in == 0 && bri > 0) briLast = bri;
    bri = in;
    stateUpdated(CALL_MODE_DIRECT_CHANGE);
  }
}

static void onMqttConnect(bool sessionPresent)
{
  mqttReconnectTimer.detach();

  char subuf[38];

  if (mqttDeviceTopic[0] != 0) {
    strlcpy(subuf, mqttDeviceTopic, 33);
    mqtt->subscribe(subuf, 0);

    strlcpy(subuf, mqttDeviceTopic, 33);
    strcat_P(subuf, PSTR("/col"));
    mqtt->subscribe(subuf, 0);

    strlcpy(subuf, mqttDeviceTopic, 33);
    strcat_P(subuf, PSTR("/api"));
    mqtt->subscribe(subuf, 0);

    strlcpy(subuf, mqttDeviceTopic, 33);
    strcat_P(subuf, PSTR("/g"));
    mqtt->subscribe(subuf, 0);

    strlcpy(subuf, mqttDeviceTopic, 33);
    strcat_P(subuf, PSTR("/h"));
    mqtt->subscribe(subuf, 0);

    strlcpy(subuf, mqttDeviceTopic, 33);
    strcat_P(subuf, PSTR("/s"));
    mqtt->subscribe(subuf, 0);
  }

  if (mqttGroupTopic[0] != 0) {
    strlcpy(subuf, mqttGroupTopic, 33);
    mqtt->subscribe(subuf, 0);

    strlcpy(subuf, mqttGroupTopic, 33);
    strcat_P(subuf, PSTR("/col"));
    mqtt->subscribe(subuf, 0);

    strlcpy(subuf, mqttGroupTopic, 33);
    strcat_P(subuf, PSTR("/api"));
    mqtt->subscribe(subuf, 0);

    strlcpy(subuf, mqttGroupTopic, 33);
    strcat_P(subuf, PSTR("/g"));
    mqtt->subscribe(subuf, 0);

    strlcpy(subuf, mqttGroupTopic, 33);
    strcat_P(subuf, PSTR("/h"));
    mqtt->subscribe(subuf, 0);

    strlcpy(subuf, mqttGroupTopic, 33);
    strcat_P(subuf, PSTR("/s"));
    mqtt->subscribe(subuf, 0);
  }

  UsermodManager::onMqttConnect(sessionPresent);
  DEBUG_PRINTLN(F("MQTT ready"));
  publishMqtt();
}

static void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  static char *payloadStr;

  DEBUG_PRINTF_P(PSTR("MQTT msg: %s\n"), topic);

  if (payload == nullptr) {
    DEBUG_PRINTLN(F("no payload -> leave"));
    return;
  }

  if (index == 0) {
    if (payloadStr) delete[] payloadStr;
    payloadStr = new char[total+1];
  }
  if (payloadStr == nullptr) return;

  char* buff = payloadStr + index;
  memcpy(buff, payload, len);
  if (index + len >= total) {
    payloadStr[total] = '\0';
  } else {
    DEBUG_PRINTLN(F("MQTT partial packet received."));
    return;
  }

  DEBUG_PRINTLN(payloadStr);

  size_t topicPrefixLen = strlen(mqttDeviceTopic);
  if (strncmp(topic, mqttDeviceTopic, topicPrefixLen) == 0) {
    topic += topicPrefixLen;
  } else {
    topicPrefixLen = strlen(mqttGroupTopic);
    if (strncmp(topic, mqttGroupTopic, topicPrefixLen) == 0) {
      topic += topicPrefixLen;
    } else {
      UsermodManager::onMqttMessage(topic, payloadStr);
      delete[] payloadStr;
      payloadStr = nullptr;
      return;
    }
  }

  if (strcmp_P(topic, PSTR("/col")) == 0) {
    colorFromDecOrHexString(colPri, payloadStr);
    colorUpdated(CALL_MODE_DIRECT_CHANGE);

  } else if (strcmp_P(topic, PSTR("/g")) == 0) {
    uint8_t inPct = (uint8_t)strtoul(payloadStr, NULL, 10);
    if (inPct == lastPublishedBri) {
      delete[] payloadStr;
      payloadStr = nullptr;
      return;
    }
    uint8_t in = (uint8_t)roundf(inPct * 2.55f);
    if (in == 0 && bri > 0) briLast = bri;
    bri = in;
    if (bri == 0) mqtt->publish(mqttDeviceTopic, 0, retainMqttMsg, "OFF");
    stateUpdated(CALL_MODE_DIRECT_CHANGE);

  } else if (strcmp_P(topic, PSTR("/h")) == 0) {
    uint16_t h = (uint16_t)roundf(atof(payloadStr));
    if (h == lastPublishedHue) {
      delete[] payloadStr;
      payloadStr = nullptr;
      return;
    }
    float dummy, s;
    rgbToHS(dummy, s);
    if (s > 5.0f) {
      hsToRGB((float)h, s);
      colorUpdated(CALL_MODE_DIRECT_CHANGE);
    }
    // s <= 5 — белый, ждём /s чтобы подтвердить

  } else if (strcmp_P(topic, PSTR("/s")) == 0) {
    uint8_t s = (uint8_t)roundf(atof(payloadStr));
    if (s == lastPublishedSat) {
      delete[] payloadStr;
      payloadStr = nullptr;
      return;
    }
    if (s <= 5) {
      // Белый — применяем пресет 101
      applyWhitePreset();
    } else {
      float h, dummy;
      rgbToHS(h, dummy);
      hsToRGB(h, (float)s);
      colorUpdated(CALL_MODE_DIRECT_CHANGE);
    }

  } else if (strcmp_P(topic, PSTR("/api")) == 0) {
    if (requestJSONBufferLock(15)) {
      if (payloadStr[0] == '{') {
        deserializeJson(*pDoc, payloadStr);
        deserializeState(pDoc->as<JsonObject>());
      } else {
        String apireq = "win"; apireq += '&';
        apireq += payloadStr;
        handleSet(nullptr, apireq);
      }
      releaseJSONBufferLock();
    }

  } else if (strlen(topic) != 0) {
    UsermodManager::onMqttMessage(topic, payloadStr);

  } else {
    parseMQTTBriPayload(payloadStr);
  }

  delete[] payloadStr;
  payloadStr = nullptr;
}

// Print adapter for flat buffers
namespace {
  class bufferPrint : public Print {
    char* _buf;
    size_t _size, _offset;
  public:
    bufferPrint(char* buf, size_t size) : _buf(buf), _size(size), _offset(0) {};
    size_t write(const uint8_t *buffer, size_t size) {
      size = std::min(size, _size - _offset);
      memcpy(_buf + _offset, buffer, size);
      _offset += size;
      return size;
    }
    size_t write(uint8_t c) {
      return this->write(&c, 1);
    }
    char* data() const { return _buf; }
    size_t size() const { return _offset; }
    size_t capacity() const { return _size; }
  };
}; // anonymous namespace

void publishMqtt()
{
  if (!WLED_MQTT_CONNECTED) return;
  DEBUG_PRINTLN(F("Publish MQTT"));

#ifndef USERMOD_SMARTNEST
  char s[10];
  char subuf[48];

  // Головной топик ON/OFF
  lastPublishedOn = (bri > 0);
  strlcpy(subuf, mqttDeviceTopic, 33);
  mqtt->publish(subuf, 0, retainMqttMsg, lastPublishedOn ? "ON" : "OFF");

  // Яркость /g (0-100) — без retain
  lastPublishedBri = (uint8_t)roundf((bri / 255.0f) * 100.0f);
  sprintf_P(s, PSTR("%u"), lastPublishedBri);
  strlcpy(subuf, mqttDeviceTopic, 33);
  strcat_P(subuf, PSTR("/g"));
  mqtt->publish(subuf, 0, false, s);

  // Цвет /c (#RRGGBBWW) — с retain
  sprintf_P(s, PSTR("#%08X"), (colPri[0] << 24) | (colPri[1] << 16) | (colPri[2] << 8) | colPri[3]);
  strlcpy(subuf, mqttDeviceTopic, 33);
  strcat_P(subuf, PSTR("/c"));
  mqtt->publish(subuf, 0, retainMqttMsg, s);

  // Hue /h (0-360) — без retain
  float h, sv;
  rgbToHS(h, sv);
  lastPublishedHue = (uint16_t)roundf(h);
  lastPublishedSat = (uint8_t)roundf(sv);
  sprintf_P(s, PSTR("%u"), lastPublishedHue);
  strlcpy(subuf, mqttDeviceTopic, 33);
  strcat_P(subuf, PSTR("/h"));
  mqtt->publish(subuf, 0, false, s);

  // Saturation /s (0-100) — без retain
  sprintf_P(s, PSTR("%u"), lastPublishedSat);
  strlcpy(subuf, mqttDeviceTopic, 33);
  strcat_P(subuf, PSTR("/s"));
  mqtt->publish(subuf, 0, false, s);

  // Status /status — с retain
  strlcpy(subuf, mqttDeviceTopic, 33);
  strcat_P(subuf, PSTR("/status"));
  mqtt->publish(subuf, 0, true, "online");

  //// /v закомментирован намеренно
  //DynamicBuffer buf(1024);
  //bufferPrint pbuf(buf.data(), buf.size());
  //XML_response(pbuf);
  //strlcpy(subuf, mqttDeviceTopic, 33);
  //strcat_P(subuf, PSTR("/v"));
  //mqtt->publish(subuf, 0, retainMqttMsg, buf.data(), pbuf.size());

#endif
}

bool initMqtt()
{
  if (!mqttEnabled || mqttServer[0] == 0 || !WLED_CONNECTED) return false;

  if (mqtt == nullptr) {
    mqtt = new AsyncMqttClient();
    if (!mqtt) return false;
    mqtt->onMessage(onMqttMessage);
    mqtt->onConnect(onMqttConnect);
    mqtt->onDisconnect(onMqttDisconnect);
  }

  if (mqtt->connected()) return true;

  DEBUG_PRINTLN(F("Reconnecting MQTT"));

  IPAddress mqttIP;
  if (mqttIP.fromString(mqttServer)) {
    mqtt->setServer(mqttIP, mqttPort);
  } else {
    mqtt->setServer(mqttServer, mqttPort);
  }

  mqtt->setClientId(mqttClientID);
  if (mqttUser[0] && mqttPass[0]) mqtt->setCredentials(mqttUser, mqttPass);

#ifndef USERMOD_SMARTNEST
  strlcpy(mqttStatusTopic, mqttDeviceTopic, 33);
  strcat_P(mqttStatusTopic, PSTR("/status"));
  mqtt->setWill(mqttStatusTopic, 0, true, "offline");
#endif

  mqtt->setKeepAlive(MQTT_KEEP_ALIVE_TIME);
  mqtt->connect();
  return true;
}

#endif
