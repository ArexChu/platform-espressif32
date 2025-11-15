#include <Arduino.h>
#include <WiFi.h>               // ✅ 修复 WiFi 未定义
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <NimBLECharacteristic.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Mitsubishi.h>
#include <esp_sleep.h>

#define LED_PIN 12
const uint16_t kIrLed = 7;
IRMitsubishiAC ac(kIrLed);

#define DEBUG 0
#define SLEEP_TIMEOUT 3000  // 30秒无连接自动睡眠

#if DEBUG
  #define LOG(x) Serial.println(x)
#else
  #define LOG(x)
#endif

static NimBLEServer* pServer;
bool connected = false;
unsigned long lastActionTime = 0;

// ------------------ 命令解析 ------------------
void parseCommand(const std::string &cmd, int &temp, String &mode, String &preset) {
  temp = 24;
  mode = "COOL";
  preset = "NORMAL";

  if (cmd.find("TEMP:") != std::string::npos) {
    size_t start = cmd.find("TEMP:") + 5;
    size_t end = cmd.find(';', start);
    temp = atoi(cmd.substr(start, end - start).c_str());
  }
  if (cmd.find("MODE:") != std::string::npos) {
    size_t start = cmd.find("MODE:") + 5;
    size_t end = cmd.find(';', start);
    mode = cmd.substr(start, end - start).c_str();
  }
  if (cmd.find("PRESET:") != std::string::npos) {
    size_t start = cmd.find("PRESET:") + 7;
    size_t end = cmd.find(';', start);
    preset = cmd.substr(start, end - start).c_str();
  }
}

// ------------------ BLE 回调 ------------------
class ACCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string value = pCharacteristic->getValue();
    if (value.empty()) return;

    LOG("[BLE] 收到命令: " + String(value.c_str()));

    int temp;
    String mode, preset;
    parseCommand(value, temp, mode, preset);

    LOG("[解析结果] 温度=" + String(temp) + ", 模式=" + mode + ", 预设=" + preset);

    // 红外发送
    digitalWrite(LED_PIN, HIGH);
    delay(100);

    if (mode.equalsIgnoreCase("OFF")) {
      ac.off();
    } else {
      ac.on();
      ac.setTemp(temp);
      if (mode.equalsIgnoreCase("COOL")) ac.setMode(kMitsubishiAcCool);
      else if (mode.equalsIgnoreCase("HEAT")) ac.setMode(kMitsubishiAcHeat);
      else if (mode.equalsIgnoreCase("HEAT_COOL")) ac.setMode(kMitsubishiAcAuto);
      else if (mode.equalsIgnoreCase("FAN_ONLY")) ac.setMode(kMitsubishiAcFan);
      else ac.setMode(kMitsubishiAcAuto);
    }

    ac.send();
    delay(200);
    digitalWrite(LED_PIN, LOW);
    LOG("[IR] 红外指令已发送。");
  }
} acCallbacks;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    connected = true;
    LOG("客户端已连接: " + String(connInfo.getAddress().toString().c_str()));
    pServer->updateConnParams(connInfo.getConnHandle(), 80, 160, 0, 400);
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    connected = false;
    lastActionTime = millis();
    LOG("客户端断开连接，重新广播...");
    NimBLEDevice::startAdvertising();
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (!connInfo.isEncrypted()) {
      NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
      LOG("加密失败，断开连接");
    } else {
      LOG("安全连接成功");
    }
  }
} serverCallbacks;

// ------------------ 深度睡眠 ------------------
void goToSleep() {
  LOG("🔋 无连接超时，进入深度睡眠...");
  NimBLEDevice::stopAdvertising();
  digitalWrite(LED_PIN, LOW);
  delay(100);

  esp_sleep_enable_timer_wakeup(180 * 1000000ULL); // 60秒唤醒
  // esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);  // 可选按键唤醒

  esp_deep_sleep_start();
}

// ------------------ 初始化 ------------------
void setup() {
#if DEBUG
  Serial.begin(115200);
#endif

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  ac.begin();

  // ⚙️ 节能设置
  WiFi.mode(WIFI_OFF);
  btStop();
  // adc_power_off(); // ❌ ESP32C3 无此函数

  LOG("🚀 启动低功耗 NimBLE 服务器...");

  NimBLEDevice::init("MitsuBLE");
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setPower(ESP_PWR_LVL_N12);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);

  NimBLEService* pACService = pServer->createService("FFF1");
  NimBLECharacteristic* pACCharacteristic = pACService->createCharacteristic(
      "FFF2",
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
      NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE_ENC
  );

  pACCharacteristic->setCallbacks(&acCallbacks);
  pACService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName("MitsuBLE");
  pAdvertising->addServiceUUID(pACService->getUUID());

  // ✅ 修复：ESP32C3 NimBLE 没有 setScanResponse()
  pAdvertising->enableScanResponse(false);

  // 延长广播间隔（1秒）
  pAdvertising->setMinInterval(800);
  pAdvertising->setMaxInterval(800);
  pAdvertising->start();

  LOG("✅ BLE 广播已启动 (低功耗模式)");
  lastActionTime = millis();
}

// ------------------ 主循环 ------------------
void loop() {
  if (!connected && millis() - lastActionTime > SLEEP_TIMEOUT) {
    goToSleep();
  }
  delay(200);
}
