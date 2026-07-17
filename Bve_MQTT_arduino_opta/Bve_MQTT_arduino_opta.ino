#include <Ethernet.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <OptaBlue.h>

// =====================================================================
// ネットワーク設定
// =====================================================================
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01};
IPAddress ip(192, 168, 11, 6);
IPAddress broker(192, 168, 11, 2);

EthernetClient ethClient;
PubSubClient client(ethClient);

unsigned long lastInputPublish = 0;
const unsigned long INPUT_PUBLISH_INTERVAL_MS = 1000;

// =====================================================================
// bve/panel(JSON配列)→ 拡張モジュール(D1608S)側リレー ch0-5
// =====================================================================
struct RelayMapping {
  const char* name;
  int panelIndex;
  int relayChannel;
};

RelayMapping panelRelayMap[] = {
  {"P電源",       2, 0},
  {"パターン接近", 3, 1},
  {"ブレーキ動作", 4, 2},
  {"ブレーキ開放", 5, 3},
  {"ATS-P",       6, 4},
  {"故障",        7, 5},
};
const int NUM_PANEL_RELAYS = sizeof(panelRelayMap) / sizeof(panelRelayMap[0]);
bool panelRelayState[NUM_PANEL_RELAYS] = {false};

const int PULSE_RELAY_CHANNEL = 6;   // panel変化時に1秒パルスさせる拡張ch
const unsigned long PULSE_DURATION_MS = 1000;
bool pulseActive = false;
unsigned long pulseStartTime = 0;
bool panelStateChanged = false;      // panel側のいずれかが変化したら true

bool expansionRelayNeedsUpdate = false;   // 拡張モジュールへのI2C反映が必要か

// =====================================================================
// bve/pilot(単一値)→ Opta本体リレー1(D0)
// =====================================================================
const int RELAY_D0_PIN = D0;
bool pilotState = false;

// =====================================================================
// bve/sound(JSON配列)→ Opta本体リレー2(D1)・リレー3(D2)
// 値が -1 のとき ON、それ以外(-10000など)は OFF
// =====================================================================
const int RELAY_D1_PIN = D1;         // 本体リレー2
const int RELAY_D2_PIN = D2;         // 本体リレー3
const int SOUND_INDEX_FOR_RELAY2 = 0;
const int SOUND_INDEX_FOR_RELAY3 = 1;
const int SOUND_VALUE_ON = -1;       // ONとみなす値
bool relay2SoundState = false;
bool relay3SoundState = false;

// =====================================================================
// 共通デバッグ関数:JSON配列の全要素をシリアル出力
// =====================================================================
void printJsonArray(const char* label, JsonArray arr)
{
  Serial.print(label);
  Serial.print(" [要素数=");
  Serial.print(arr.size());
  Serial.print("] : ");

  for (size_t i = 0; i < arr.size(); i++) {
    Serial.print("[");
    Serial.print(i);
    Serial.print("]=");
    Serial.print(arr[i].as<String>());
    if (i < arr.size() - 1) Serial.print(", ");
  }
  Serial.println();
}

// =====================================================================
// MQTTコールバック
// =====================================================================
void callback(char* topic, byte* payload, unsigned int length)
{
  String msg;
  for (unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];

  Serial.print("[受信] Topic: ");
  Serial.print(topic);
  Serial.print(" / Length: ");
  Serial.print(length);
  Serial.print(" / Raw: ");
  Serial.println(msg);

  // -------------------- bve/panel --------------------
  if (strcmp(topic, "bve/panel") == 0)
  {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, msg);

    if (err) {
      Serial.print("  JSON parse error (bve/panel): ");
      Serial.println(err.c_str());
      return;
    }

    printJsonArray("  bve/panel", doc.as<JsonArray>());

    for (int i = 0; i < NUM_PANEL_RELAYS; i++) {
      int value = doc[panelRelayMap[i].panelIndex].as<int>();
      bool newState = (value == 1);

      if (newState != panelRelayState[i]) {
        panelRelayState[i] = newState;
        expansionRelayNeedsUpdate = true;
        panelStateChanged = true;

        Serial.print("  ");
        Serial.print(panelRelayMap[i].name);
        Serial.print(" -> ");
        Serial.println(newState ? "ON" : "OFF");
      }
    }
  }

  // -------------------- bve/pilot --------------------
  else if (strcmp(topic, "bve/pilot") == 0)
  {
    int value = msg.toInt();
    bool newState = (value == 1);

    Serial.print("  bve/pilot 値=");
    Serial.println(value);

    if (newState != pilotState) {
      pilotState = newState;
      digitalWrite(RELAY_D0_PIN, pilotState ? HIGH : LOW);
      digitalWrite(LED_D0, pilotState ? HIGH : LOW);
      Serial.print("  知らせ灯(本体リレー1/D0) -> ");
      Serial.println(newState ? "ON" : "OFF");
    }
  }

  // -------------------- bve/sound --------------------
  else if (strcmp(topic, "bve/sound") == 0)
  {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, msg);

    if (err) {
      Serial.print("  JSON parse error (bve/sound): ");
      Serial.println(err.c_str());
      return;
    }

    printJsonArray("  bve/sound", doc.as<JsonArray>());

    // --- 本体リレー2(D1) ← sound[0] (-1のときON) ---
    bool newRelay2 = (doc[SOUND_INDEX_FOR_RELAY2].as<int>() == SOUND_VALUE_ON);
    if (newRelay2 != relay2SoundState) {
      relay2SoundState = newRelay2;
      digitalWrite(RELAY_D1_PIN, relay2SoundState ? HIGH : LOW);
      digitalWrite(LED_D1, relay2SoundState ? HIGH : LOW);
      Serial.print("  本体リレー2(D1) -> ");
      Serial.println(relay2SoundState ? "ON" : "OFF");
    }

    // --- 本体リレー3(D2) ← sound[1] (-1のときON) ---
    bool newRelay3 = (doc[SOUND_INDEX_FOR_RELAY3].as<int>() == SOUND_VALUE_ON);
    if (newRelay3 != relay3SoundState) {
      relay3SoundState = newRelay3;
      digitalWrite(RELAY_D2_PIN, relay3SoundState ? HIGH : LOW);
      digitalWrite(LED_D2, relay3SoundState ? HIGH : LOW);
      Serial.print("  本体リレー3(D2) -> ");
      Serial.println(relay3SoundState ? "ON" : "OFF");
    }
  }
}

// =====================================================================
// MQTT再接続
// =====================================================================
void reconnect()
{
  while (!client.connected())
  {
    Serial.print("MQTT接続試行中...");
    if (client.connect("Opta001")) {
      Serial.println("接続成功");
      client.subscribe("bve/panel");
      client.subscribe("bve/pilot");
      client.subscribe("bve/speed");
      client.subscribe("bve/sound");
    } else {
      Serial.print("失敗 rc=");
      Serial.println(client.state());
      delay(3000);
    }
  }
}

// =====================================================================
// セットアップ
// =====================================================================
void setup()
{
  Serial.begin(115200);
  delay(2000);

  // --- 本体リレー用ピン設定 ---
  pinMode(RELAY_D0_PIN, OUTPUT);
  pinMode(RELAY_D1_PIN, OUTPUT);
  pinMode(RELAY_D2_PIN, OUTPUT);
  pinMode(LED_D0, OUTPUT);
  pinMode(LED_D1, OUTPUT);
  pinMode(LED_D2, OUTPUT);

  // --- Ethernet / MQTT ---
  Ethernet.begin(mac, ip);
  Serial.print("IP: ");
  Serial.println(Ethernet.localIP());

  client.setServer(broker, 1883);
  client.setBufferSize(512);
  client.setCallback(callback);

  // --- Opta拡張モジュール ---
  OptaController.begin();
  delay(2000);
  OptaController.update();

  Serial.print("検出された拡張モジュール数: ");
  Serial.println(OptaController.getExpansionNum());

  if (OptaController.getExpansionNum() > 0) {
    ExpansionType_t type = OptaController.getExpansionType(0);
    if (type == EXPANSION_OPTA_DIGITAL_STS) {
      Serial.println("D1608S(Solid State)を認識");
    } else {
      Serial.println("警告: 想定と異なる拡張モジュールが接続されています");
    }
  } else {
    Serial.println("警告: 拡張モジュールが検出されていません");
  }
}

// =====================================================================
// メインループ
// =====================================================================
void loop()
{
  // --- MQTT ---
  if (!client.connected())
    reconnect();
  client.loop();

  // --- Opta拡張モジュールとの通信を維持 ---
  OptaController.update();

  // --- 拡張モジュール側リレー(bve/panelの6ch + パルスリレー)をまとめて反映 ---
  if (expansionRelayNeedsUpdate && OptaController.getExpansionNum() > 0) {
    DigitalStSolidExpansion digitalExp = OptaController.getExpansion(0);

    for (int i = 0; i < NUM_PANEL_RELAYS; i++) {
      digitalExp.digitalWrite(panelRelayMap[i].relayChannel, panelRelayState[i] ? HIGH : LOW);
    }

    if (panelStateChanged) {
      digitalExp.digitalWrite(PULSE_RELAY_CHANNEL, HIGH);
      pulseActive = true;
      pulseStartTime = millis();
      panelStateChanged = false;
      Serial.println("パルスリレー(拡張ch6) ON (1秒後に自動OFF)");
    }

    digitalExp.updateDigitalOutputs();
    expansionRelayNeedsUpdate = false;
  }

  // --- パルスリレーの1秒経過チェック(自動OFF) ---
  if (pulseActive && (millis() - pulseStartTime >= PULSE_DURATION_MS)) {
    if (OptaController.getExpansionNum() > 0) {
      DigitalStSolidExpansion digitalExp = OptaController.getExpansion(0);
      digitalExp.digitalWrite(PULSE_RELAY_CHANNEL, LOW);
      digitalExp.updateDigitalOutputs();
    }
    pulseActive = false;
    Serial.println("パルスリレー(拡張ch6) OFF");
  }

  // --- (任意)入力値の定期publish ---
  if (millis() - lastInputPublish > INPUT_PUBLISH_INTERVAL_MS) {
    lastInputPublish = millis();
    int value = digitalRead(A0);
    char msg[10];
    sprintf(msg, "%d", value);
    client.publish("opta/input", msg);
  }
}
