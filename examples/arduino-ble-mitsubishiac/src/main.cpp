/* Ultra-low-power AHT30 + Mitsubishi AC control (ESP32-C3, Arduino)
 *
 * 功能：
 *  - 定时唤醒读取 AHT30 (I2C SDA=5, SCL=6)
 *  - 广播温湿度 (BLE Advertising + characteristic)
 *  - 若手机连接并写入指令 -> 发送 Mitsubishi IR 控制信号
 *  - 进入 deep sleep 前把 AHT30 置为 sleep (0xB0,0x98)
 *  - 睡眠时对 SDA/SCL 设下拉，关闭 WiFi/BT 以最低功耗
 *
 * 适配：ESP32-C3
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <NimBLEDevice.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Mitsubishi.h>
#include <esp_sleep.h>

#define LED_PIN         12      // 状态 LED（活动时点亮）
const uint16_t kIrLed = 7;      // IR LED
IRMitsubishiAC ac(kIrLed);

#define DEBUG 0
#if DEBUG
  #define LOG(x) Serial.println(x)
#else
  #define LOG(x)
#endif

// I2C pins (可按需更改)
const int I2C_SDA = 4;
const int I2C_SCL = 5;

// 省电参数
const uint32_t WAKE_INTERVAL_S = 300;   // 睡眠周期（秒），唤醒读取一次传感器（可调整）
const uint32_t ADVERTISE_WINDOW_MS = 3000; // 唤醒后广播/等待连接窗口（毫秒）
const int SLEEP_TIMEOUT_MS = 3000;      // 若已连接但3s无活动则进入睡眠（保持与你之前一致）

// BLE UUIDs (可自定义)
const char* BLE_NAME = "MitsuBLE_ULP";
const char* AC_SERVICE_UUID = "FFF1";
const char* AC_CHAR_UUID = "FFF2";
const char* ENV_SERVICE_UUID = "FFE1";
const char* ENV_CHAR_UUID = "FFE2";

// 全局变量
static NimBLEServer* pServer = nullptr;
static NimBLECharacteristic* pACChar = nullptr;
static NimBLECharacteristic* pEnvChar = nullptr;
static bool g_connected = false;
static unsigned long g_lastActionMs = 0;

Adafruit_AHTX0 aht;

// --------------- 辅助：把 AHT30 送入 Sleep ---------------
void aht30_sleep() {
  Wire.beginTransmission(0x38);  // AHT 系列默认 I2C 地址 0x38
  Wire.write(0xB0);
  Wire.write(0x98);
  Wire.endTransmission();
  LOG("AHT30 -> sleep command sent");
}

// --------------- 辅助：把 SDA/SCL 设为下拉输入（睡眠前） ---------------
void i2c_pins_to_pulldown() {
  gpio_num_t sda = (gpio_num_t)I2C_SDA;
  gpio_num_t scl = (gpio_num_t)I2C_SCL;

  gpio_set_direction(sda, GPIO_MODE_INPUT);
  gpio_set_pull_mode(sda, GPIO_PULLDOWN_ONLY);

  gpio_set_direction(scl, GPIO_MODE_INPUT);
  gpio_set_pull_mode(scl, GPIO_PULLDOWN_ONLY);

  LOG("I2C pins -> input pulldown");
}

// --------------- ESP32 进入深度睡眠 ---------------
void enter_deep_sleep() {
  LOG("Preparing to deep sleep...");

  // 1) 停止 BLE 广播/服务
  NimBLEDevice::stopAdvertising();
  // 2) 关闭 LED
  digitalWrite(LED_PIN, LOW);
  delay(50);

  // 3) 指令 AHT30 进入睡眠
  aht30_sleep();

  // 4) 将 I2C 引脚设为下拉输入以减少漏电
  i2c_pins_to_pulldown();

  // 5) 配置唤醒：定时唤醒
  esp_sleep_enable_timer_wakeup((uint64_t)WAKE_INTERVAL_S * 1000000ULL);
  LOG(String("Deep sleep for ") + String(WAKE_INTERVAL_S) + "s");

  // 6) 进入深度睡眠
  esp_deep_sleep_start();
}

// --------------- BLE 回调：AC 指令接收 ---------------
class ACCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string value = pCharacteristic->getValue();
    if (value.empty()) return;

    String cmd = String(value.c_str());
    LOG("[BLE] AC cmd received: " + cmd);

    // 解析简易命令：格式示例 "TEMP:24;MODE:COOL;PRESET:NORMAL;"
    int temp = 24;
    String mode = "COOL";
    String preset = "NORMAL";

    // 解析 TEMP
    size_t idx;
    idx = cmd.indexOf("TEMP:");
    if (idx >= 0) {
      int sem = cmd.indexOf(';', idx);
      String t = cmd.substring(idx+5, sem >= 0 ? sem : cmd.length());
      temp = t.toInt();
    }
    // MODE
    idx = cmd.indexOf("MODE:");
    if (idx >= 0) {
      int sem = cmd.indexOf(';', idx);
      mode = cmd.substring(idx+5, sem >= 0 ? sem : cmd.length());
    }
    // PRESET
    idx = cmd.indexOf("PRESET:");
    if (idx >= 0) {
      int sem = cmd.indexOf(';', idx);
      preset = cmd.substring(idx+7, sem >= 0 ? sem : cmd.length());
    }

    // 短亮 LED
    digitalWrite(LED_PIN, HIGH);

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
    delay(150);
    digitalWrite(LED_PIN, LOW);

    LOG("[IR] AC IR command sent");
    g_lastActionMs = millis();
  }
} acCharCbs;

// --------------- BLE Server 回调 ---------------
class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pSrv, NimBLEConnInfo& connInfo) override {
    g_connected = true;
    g_lastActionMs = millis();
    LOG("Client connected: " + String(connInfo.getAddress().toString().c_str()));
  }
  void onDisconnect(NimBLEServer* pSrv, NimBLEConnInfo& connInfo, int reason) override {
    g_connected = false;
    g_lastActionMs = millis();
    LOG("Client disconnected");
    // 重新广播，以便短时间内可被发现（若需要）
    NimBLEDevice::startAdvertising();
  }
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (!connInfo.isEncrypted()) {
      NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
      LOG("Encryption failed -> disconnected");
    } else {
      LOG("Auth & encryption OK");
    }
  }
} serverCbs;


// --------------- 初始化并广播温湿度（一次读取与广播） ---------------
void read_and_publish_env() {
  sensors_event_t humidity, temp;
  if (aht.getEvent(&humidity, &temp)) {
    String payload = "T=" + String(temp.temperature, 1) + ";H=" + String(humidity.relative_humidity, 1) + ";";
    if (pEnvChar) {
      pEnvChar->setValue(payload.c_str());
      // 仅在有连接时尝试通知
      if (g_connected) pEnvChar->notify();
    }
    LOG("[ENV] " + payload);
  } else {
    LOG("AHT read failed");
  }
}

// --------------- setup ---------------
void setup() {
#if DEBUG
  Serial.begin(115200);
  delay(100);
#endif

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // 关 WiFi/BT（降低功耗），再初始化必要外设
  WiFi.mode(WIFI_OFF);
  btStop();

  // 初始化 I2C（显式指定 SDA, SCL）
  Wire.begin(I2C_SDA, I2C_SCL);

  // AHT init (若处于 sleep，会在 begin() 时被唤醒)
  if (!aht.begin()) {
    LOG("AHT init failed");
    // 仍继续运行：可能在唤醒过程中 sensor 未响应
  } else {
    LOG("AHT ready");
  }

  // IR init
  ac.begin();
  // 将 IR LED 引脚设为低功耗初始状态
  digitalWrite(LED_PIN, LOW);

  // NimBLE init（低功耗配置）
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setPower(ESP_PWR_LVL_N12); // 最低发射功率

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCbs);

  // AC 控制服务（写入特征）
  NimBLEService* acService = pServer->createService(AC_SERVICE_UUID);
  pACChar = acService->createCharacteristic(AC_CHAR_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC
  );
  pACChar->setCallbacks(&acCharCbs);
  acService->start();

  // 环境数据服务（只读 + notify）
  NimBLEService* envService = pServer->createService(ENV_SERVICE_UUID);
  pEnvChar = envService->createCharacteristic(ENV_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  envService->start();

  // 广播设置
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(BLE_NAME);
  adv->addServiceUUID(AC_SERVICE_UUID);
  adv->addServiceUUID(ENV_SERVICE_UUID);
  adv->setMinInterval(160); // 100ms * 1.6 = 160ms? (NimBLE 的单位为 0.625ms — 这里只为示意)
  adv->setMaxInterval(320);
  adv->enableScanResponse(false);
  adv->start();

  LOG("BLE advertising started");

  // 读取并把温湿度放入特征值（便于连接后读取）
  read_and_publish_env();

  // 记录当前时间，用于判断是否进入深度睡眠
  g_lastActionMs = millis();
}

// --------------- 主循环（短时间窗口内等待连接与命令） ---------------
void loop() {
  unsigned long now = millis();

  // 每 1s 刷新一次环境数据（仅在唤醒窗口内）
  static unsigned long lastEnvMs = 0;
  if (now - lastEnvMs >= 1000) {
    lastEnvMs = now;
    read_and_publish_env();
  }

  // 若在唤醒后的广播窗口内且还未连接 -> 继续等待
  static unsigned long wakeStart = 0;
  if (wakeStart == 0) wakeStart = millis();

  // 若已连接，则维持直到断开并超时
  if (g_connected) {
    // 若连接后长时间无交互 (SLEEP_TIMEOUT_MS) -> 进入休眠
    if (now - g_lastActionMs > (unsigned long)SLEEP_TIMEOUT_MS) {
      LOG("Connected but idle timeout -> sleeping");
      delay(50);
      enter_deep_sleep();
    }
  } else {
    // 未连接：如果超过广播窗口 -> 进入深度睡眠
    if (now - wakeStart >= (unsigned long)ADVERTISE_WINDOW_MS) {
      LOG("No connection in advertise window -> sleeping");
      delay(50);
      enter_deep_sleep();
    }
  }

  delay(10); // 小延时，避免 busy-loop
}
