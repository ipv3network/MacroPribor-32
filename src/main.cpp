#include <WebServer.h>
#include <FastAccelStepper.h>
#include <BLEDevice.h>
#include <WiFiManager.h>
#include <LittleFS.h>

// ================= НАСТРОЙКИ СЕТИ =================
// Если не удается подключиться к ранее сохраненной точке доступа — поднимаем точку доступа "MacroPribor-32" с паролем "MacroPribor-32"
const char* APssid     = "MacroPribor-32";
const char* APpassword = "MacroPribor-32";
const char* Hostname   = "macropribor-32";

WiFiManager wifiManager;

// ================= НАСТРОЙКИ BLE (SONY) =================
#define CAMERA_NAME "ILCE-7C"  // Имя камеры в Bluetooth меню

// UUID сервисов Sony
static BLEUUID serviceUUID("8000FF00-FF00-FFFF-FFFF-FFFFFFFFFFFF");
static BLEUUID charCommandUUID((uint16_t)0xFF01);
static BLEUUID charNotifyUUID((uint16_t)0xFF02);

// Команды управления камерой
uint8_t HALF_DOWN[] = {0x01, 0x07};  // Половинное нажатие
uint8_t FULL_DOWN[] = {0x01, 0x09};  // Полное нажатие
uint8_t FULL_UP[]   = {0x01, 0x08};  // Отпустить
uint8_t HALF_UP[]   = {0x01, 0x06};  // Отпустить на половину

// ================= ПИНЫ МОТОРА =================
#define PIN_STEP 0
#define PIN_DIR 1

// ================= НАСТРОЙКИ МЕХАНИКИ =================
#define FIXED_ACCELERATION 1400
#define SPEED 5000
#define SHUTTER_PULSE_MS 200

FastAccelStepperEngine engine  = FastAccelStepperEngine();
FastAccelStepper*      stepper = NULL;

WebServer server(80);

// ================= ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ =================
long          pointA     = 0;
long          pointB     = 0;
bool          isStacking = false;
int           stackState = 0;
unsigned long stateTimer = 0;
long          nextPos    = 0;
int           direction  = 1;

// Параметры с веба
long web_speed          = 1000;
long web_stack_step     = 100;
long web_settle_delay   = 1000;
long web_exposure_delay = 1000;

// BLE Переменные
boolean                  bleConnected = false;
boolean                  doConnect    = false;
boolean                  doScan       = false;
BLEAddress*              pServerAddress;
BLERemoteCharacteristic* remoteCommand;
BLERemoteCharacteristic* remoteNotify;
BLEClient*               pClient;

// ================= BLE КЛАССЫ ОБРАТНОЙ СВЯЗИ =================

// Клиент подключился/отключился
class MyClientCallback : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) { Serial.println("BLE Connected!"); }
    void onDisconnect(BLEClient* pclient) {
        bleConnected = false;
        Serial.println("BLE Disconnected!");
    }
};

// Поиск устройств
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        Serial.print("BLE Found: ");
        Serial.println(advertisedDevice.getName().c_str());

        // Ищем камеру по имени
        if (advertisedDevice.getName() == CAMERA_NAME) {
            advertisedDevice.getScan()->stop();
            pServerAddress = new BLEAddress(advertisedDevice.getAddress());
            doConnect      = true;
            doScan         = false;
            Serial.println("Camera Found! Ready to connect.");
        }
    }
};

// Спаривание
class MySecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() { return 123456; }
    void     onPassKeyNotify(uint32_t pass_key) {}
    bool     onSecurityRequest() { return true; }
    void     onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
        if (cmpl.success)
            Serial.println("Pairing Success");
        else
            Serial.println("Pairing Failed");
    }
    bool onConfirmPIN(uint32_t pin) { return true; }
};

// ================= ФУНКЦИИ BLE =================

bool connectToCamera() {
    Serial.print("Connecting to ");
    Serial.println(pServerAddress->toString().c_str());

    if (pClient->connect(*pServerAddress)) {
        Serial.println("Connected to server");

        BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
        if (pRemoteService == nullptr) {
            Serial.println("Failed to find service UUID");
            return false;
        }

        remoteCommand = pRemoteService->getCharacteristic(charCommandUUID);
        remoteNotify  = pRemoteService->getCharacteristic(charNotifyUUID);

        if (remoteCommand == nullptr || remoteNotify == nullptr) {
            Serial.println("Failed to find characteristics");
            return false;
        }

        bleConnected = true;
        return true;
    }
    return false;
}

void triggerShutterBLE() {
    if (!bleConnected || remoteCommand == nullptr) return;

    remoteCommand->writeValue(HALF_DOWN, 2, true);
    delay(50);
    remoteCommand->writeValue(FULL_DOWN, 2, true);
}

void releaseShutterBLE() {
    if (!bleConnected || remoteCommand == nullptr) return;

    remoteCommand->writeValue(FULL_UP, 2, true);
    delay(50);
    remoteCommand->writeValue(HALF_UP, 2, true);
}

// ================= ЛОГИКА СЕРВЕРА =================

void handleRoot() {
    File indexHTML = LittleFS.open("/index.html", "r");
    if (!indexHTML) {
        server.send(500, "text/plain", "HTML file not found. Did you run pio run --target uploadfs ?");
        return;
    }

    server.streamFile(indexHTML, "text/html");
    indexHTML.close();
}

void handleState() {
    long framesLeft = 0;
    if (isStacking && web_stack_step > 0) {
        long dist = 0;
        if (direction == 1)
            dist = pointB - stepper->getCurrentPosition();
        else
            dist = stepper->getCurrentPosition() - pointB;
        if (dist < 0) dist = 0;
        framesLeft = dist / web_stack_step;
    }

    // BLE Статус: 0 - откл, 1 - подкл, 2 - поиск
    int bleStatus = 0;
    if (bleConnected)
        bleStatus = 1;
    else if (doScan)
        bleStatus = 2;

    String isRunning = "0";
    String isMovingToA = "0";
    if ( isStacking && stackState != 0 ) { isRunning = "1"; };
    if ( isStacking && stackState == 0 ) { isMovingToA = "1"; };
    String json = "{";
    json += "\"running\": " + isRunning + ",";
    json += "\"movingA\": " + isMovingToA + ",";
    json += "\"pos\": " + String(stepper->getCurrentPosition()) + ",";
    json += "\"rem\": " + String(framesLeft) + ",";
    json += "\"ble\": " + String(bleStatus) + ",";
    json += "\"pointA\": " + String(pointA) + ",";
    json += "\"pointB\": " + String(pointB);
    json += "}";
    server.send(200, "application/json", json);
}

void handleBLEConnect() {
    if (!bleConnected && !doScan) {
        doScan = true;
        server.send(200, "text/plain", "SCANNING");
    } else {
        server.send(200, "text/plain", "ALREADY ACTIVE");
    }
}

void handleJog() {
    if (isStacking) return;
    web_speed = server.arg("spd").toInt();
    long dist = server.arg("dist").toInt() * server.arg("dir").toInt();
    stepper->setSpeedInHz(web_speed);
    stepper->move(dist);  // относительное перемещение (добавляет к последней целевой позиции)
    server.send(200, "text/plain", "OK");
}

void handleSetA() {
    pointA = stepper->getCurrentPosition();
    server.send(200, "text/plain", "A");
}

void handleSetB() {
    pointB = stepper->getCurrentPosition();
    server.send(200, "text/plain", "B");
}

void handleStartStack() {
    if (isStacking) return;
    web_speed          = server.arg("spd").toInt();
    web_stack_step     = server.arg("step").toInt();
    web_settle_delay   = server.arg("set").toInt();
    web_exposure_delay = server.arg("exp").toInt();

    if (pointB >= pointA)
        direction = 1;
    else
        direction = -1;
    web_stack_step = abs(web_stack_step);

    stepper->setSpeedInHz(web_speed);
    stepper->moveTo(pointA);
    isStacking = true;
    stackState = 0;
    server.send(200, "text/plain", "OK");
}

void handleStop() {
    isStacking = false;
    stepper->stopMove();
    server.send(200, "text/plain", "STOPPED");
}

void handleTestPhoto() {
    triggerShutterBLE();
    delay(200);
    releaseShutterBLE();
    server.send(200, "text/plain", "OK");
}

void handleWifiReset() {
    wifiManager.resetSettings();
    ESP.restart();
}

void handleUpload() {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        String filename = "/" + upload.filename;
        Serial.print("Upload Start: ");
        Serial.println(filename);
        LittleFS.remove(filename);
        File file = LittleFS.open(filename, "w");
        file.close();
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        File file = LittleFS.open("/" + upload.filename, "a");
        file.write(upload.buf, upload.currentSize);
        file.close();
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.print("Upload End: ");
        Serial.println(upload.totalSize);
        server.send(200, "text/html", "File uploaded<br><a href=/>go back</a>");
    }
}

// ================= SETUP =================
void setup() {
    Serial.begin(115200);

    // Инициализация FastAccelStepper
    engine.init();
    stepper = engine.stepperConnectToPin(PIN_STEP);
    if (stepper) {
        stepper->setDirectionPin(PIN_DIR);
        stepper->setSpeedInHz(SPEED);
        stepper->setAcceleration(FIXED_ACCELERATION);
        stepper->setCurrentPosition(0);
    }

    // Init BLE Client
    BLEDevice::init("MacroPribor-32");
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
    BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());

    // WIFI
    wifiManager.setHostname(Hostname);
    wifiManager.autoConnect(APssid, APpassword);

    // Инициализируем ФС
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed!");
        return;
    }

    // Веб сервер
    server.on("/", handleRoot);
    server.on("/state", handleState);
    server.on("/ble_connect", handleBLEConnect);
    server.on("/jog", handleJog);
    server.on("/setA", handleSetA);
    server.on("/setB", handleSetB);
    server.on("/start_stack", handleStartStack);
    server.on("/stop", handleStop);
    server.on("/wifi", handleWifiReset);
    server.on("/test_photo", handleTestPhoto);
    server.on("/upload", HTTP_POST, []() { server.send(200); }, handleUpload);
    server.begin();
}

// ================= LOOP =================
void loop() {
    server.handleClient();

    // --- ОБРАБОТКА BLE ---
    if (doScan) {
        BLEScan* pBLEScan = BLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
        pBLEScan->setActiveScan(true);
        pBLEScan->start(5, false);
        if (!doConnect) {
            doScan = false;
            Serial.println("Scan finished, camera not found.");
        }
    }

    if (doConnect) {
        if (connectToCamera()) {
            Serial.println("Success connection!");
        } else {
            Serial.println("Failed connection.");
        }
        doConnect = false;
    }

    // Стекинг
    if (isStacking) {
        switch (stackState) {
            // 0: Возврат к старту
            case 0:
                if (!stepper->isRunning()) {
                    delay(1000);
                    nextPos    = pointA;
                    stackState = 1;
                }
                break;

            // 1: Движение к позиции кадра
            case 1:
                if ((direction == 1 && stepper->getCurrentPosition() >= pointB) || (direction == -1 && stepper->getCurrentPosition() <= pointB)) {
                    isStacking = false;
                } else {
                    if (!stepper->isRunning()) {
                        stateTimer = millis();
                        stackState = 2;
                    }
                }
                break;

            // 2: Стабилизация (Settle)
            case 2:
                if (millis() - stateTimer > web_settle_delay) {
                    triggerShutterBLE();
                    stateTimer = millis();
                    stackState = 3;
                }
                break;

            // 3: Импульс спуска
            case 3:
                if (millis() - stateTimer > SHUTTER_PULSE_MS) {
                    releaseShutterBLE();
                    stateTimer = millis();
                    stackState = 4;
                }
                break;

            // 4: Ожидание выдержки
            case 4:
                if (millis() - stateTimer > web_exposure_delay) {
                    nextPos = stepper->getCurrentPosition() + (web_stack_step * direction);
                    stepper->moveTo(nextPos);
                    stackState = 1;
                }
                break;
        }
    }
}
