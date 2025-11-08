#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <NimBLECharacteristic.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Mitsubishi.h>

#define LED_PIN 12
const uint16_t kIrLed = 7;  // ESP8266 GPIO pin to use. Recommended: 4 (D2).
IRMitsubishiAC ac(kIrLed);  // Set the GPIO used for sending messages.

static NimBLEServer* pServer;

void parseCommand(const std::string &cmd, int &temp, String &mode, String &preset) {
  temp = 24;   // 默认温度
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

class ACCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string value = pCharacteristic->getValue();
        if (value.empty()) return;
        Serial.printf("[BLE] 收到命令: %s\n", value.c_str());
        int temp;
        String mode;
        String preset;
        parseCommand(value, temp, mode, preset);
        Serial.printf("[解析结果] 温度=%d, 模式=%s, 预设=%s\n", temp, mode.c_str(), preset.c_str());
        // ------------------------------
        // 配置并发送红外命令
        // ------------------------------
	digitalWrite(LED_PIN, HIGH);
        ac.on();
        ac.setTemp(temp);
        if (mode.equalsIgnoreCase("COOL")) {
          ac.setMode(kMitsubishiAcCool);
        } else if (mode.equalsIgnoreCase("HEAT")) {
          ac.setMode(kMitsubishiAcHeat);
        } else if (mode.equalsIgnoreCase("HEAT_COOL")) {
          ac.setMode(kMitsubishiAcAuto);
        } else if (mode.equalsIgnoreCase("FAN_ONLY")) {
          ac.setMode(kMitsubishiAcFan);
	} else if (mode.equalsIgnoreCase("OFF")) {
	  digitalWrite(LED_PIN, LOW);
          ac.off();
        } else {
          ac.setMode(kMitsubishiAcAuto);
        }
        // 可根据 PRESET 添加风速、节能等设置
        ac.send();
        Serial.println("[IR] 红外指令已发送。");
    }
} acCallbacks;

/**  None of these are required as they will be handled by the library with defaults. **
 **                       Remove as you see fit for your needs                        */
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        Serial.printf("Client address: %s\n", connInfo.getAddress().toString().c_str());

        /**
         *  We can use the connection handle here to ask for different connection parameters.
         *  Args: connection handle, min connection interval, max connection interval
         *  latency, supervision timeout.
         *  Units; Min/Max Intervals: 1.25 millisecond increments.
         *  Latency: number of intervals allowed to skip.
         *  Timeout: 10 millisecond increments.
         */
        pServer->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 180);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        Serial.printf("Client disconnected - start advertising\n");
        NimBLEDevice::startAdvertising();
    }

    void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
        Serial.printf("MTU updated: %u for connection ID: %u\n", MTU, connInfo.getConnHandle());
    }

    /********************* Security handled here *********************/
    uint32_t onPassKeyDisplay() override {
        Serial.printf("Server Passkey Display\n");
        /**
         * This should return a random 6 digit number for security
         *  or make your own static passkey as done here.
         */
        return 123456;
    }

    void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pass_key) override {
        Serial.printf("The passkey YES/NO number: %" PRIu32 "\n", pass_key);
        /** Inject false if passkeys don't match. */
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        /** Check that encryption was successful, if not we disconnect the client */
        if (!connInfo.isEncrypted()) {
            NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
            Serial.printf("Encrypt connection failed - disconnecting client\n");
            return;
        }

        Serial.printf("Secured connection to: %s\n", connInfo.getAddress().toString().c_str());
    }
} serverCallbacks;

void setup(void) {
    ac.begin();
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    Serial.printf("Starting NimBLE Server\n");

    /** Initialize NimBLE and set the device name */
    NimBLEDevice::init("NimBLE");

    /**
     * Set the IO capabilities of the device, each option will trigger a different pairing method.
     *  BLE_HS_IO_DISPLAY_ONLY    - Passkey pairing
     *  BLE_HS_IO_DISPLAY_YESNO   - Numeric comparison pairing
     *  BLE_HS_IO_NO_INPUT_OUTPUT - DEFAULT setting - just works pairing
     */
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    // NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY); // use passkey
    // NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO); //use numeric comparison

    /**
     *  2 different ways to set security - both calls achieve the same result.
     *  no bonding, no man in the middle protection, BLE secure connections.
     *
     *  These are the default values, only shown here for demonstration.
     */
    NimBLEDevice::setSecurityAuth(true, true, true);
    // NimBLEDevice::setSecurityAuth(false, false, true);
    // NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM | BLE_SM_PAIR_AUTHREQ_SC);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(&serverCallbacks);

    NimBLEService*        pACService = pServer->createService("FFF1");
    NimBLECharacteristic* pACCharacteristic =
        pACService->createCharacteristic("FFF2",
                                           NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
                                               /** Require a secure connection for read and write access */
                                               NIMBLE_PROPERTY::READ_ENC | // only allow reading if paired / encrypted
                                               NIMBLE_PROPERTY::WRITE_ENC  // only allow writing if paired / encrypted
        );

    pACCharacteristic->setValue("0");
    pACCharacteristic->setCallbacks(&acCallbacks); 
    
    /** Start the services when finished creating all Characteristics and Descriptors */
    pACService->start();

    /** Create an advertising instance and add the services to the advertised data */
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setName("NimBLE-Server");
    pAdvertising->addServiceUUID(pACService->getUUID());
    /**
     *  If your device is battery powered you may consider setting scan response
     *  to false as it will extend battery life at the expense of less data sent.
     */
    pAdvertising->enableScanResponse(false);
    pAdvertising->start();

    Serial.printf("Advertising Started\n");
}

void loop() {
    /** Loop here and send notifications to connected peers */
    delay(2000);
}
