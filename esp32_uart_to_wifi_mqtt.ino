/*******************************************************************************
 * ESP32 USB 통신 진단 + WiFi + MQTT 전송 (v2)
 * 
 * 수정 사항 (v2):
 * - 길이 기반 패킷 수신으로 변경
 * - 데이터 중간에 0x03이 있어도 정상 수신
 * 
 * 사용법:
 * 1. 처음 부팅 또는 저장된 WiFi 연결 실패 시 → "ESP32-GA1-Setup" AP 생성
 * 2. 스마트폰/PC로 해당 AP에 연결
 * 3. 브라우저에서 192.168.4.1 접속
 * 4. WiFi 선택 및 비밀번호 입력 후 저장
 * 5. ESP32 자동 재부팅 후 설정한 WiFi에 연결
 * 
 * ★ 설정 초기화: GPIO0 버튼을 5초간 누르면 WiFi 설정 삭제 후 재부팅
 * 
 * 패킷 구조:
 * [STX(1)] [LEN_L(1)] [LEN_H(1)] [DATA(LEN)] [CHK(1)] [ETX(1)]
 * 
 * 작성일: 2026-01-21
 ******************************************************************************/

#include <WiFi.h>
#include <WiFiManager.h>  // ★ WiFiManager 라이브러리 추가
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>  // ★ NVS 저장용

#define USE_DEBUG_SERIAL 1

#if USE_DEBUG_SERIAL
  #define DBG_PRINT(x)    Serial2.print(x)
  #define DBG_PRINTLN(x)  Serial2.println(x)
  #define DBG_PRINTF(...) Serial2.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
  #define DBG_PRINTF(...)
#endif

#define PROTO_STX   0x02
#define PROTO_ETX   0x03
#define LED_PIN     2
#define RESET_PIN   0       // ★ GPIO0 (BOOT 버튼) - WiFi 설정 초기화용
#define RX_BUF_SIZE 1024

//==============================================================================
// WiFiManager 설정
//==============================================================================

#define AP_NAME         "ESP32-Wifi-Setup"   // 설정 모드 AP 이름
#define AP_PASSWORD     "12345678"          // 설정 모드 AP 비밀번호 (8자 이상)
#define PORTAL_TIMEOUT  180                 // 설정 포털 타임아웃 (초)

WiFiManager wifiManager;
Preferences preferences;

//==============================================================================
// MQTT 설정 (WiFiManager에서 설정 가능하도록 변수로 변경)
//==============================================================================

char mqtt_server[64] = "192.168.0.190";  //"broker.hivemq.com";
char mqtt_port[6] = "1883";
char mqtt_client_id[32] = "ESP32_GA1_Agent";
char mqtt_user[32] = "";
char mqtt_pass[32] = "";

// MQTT 토픽
const char* MQTT_TOPIC_DATA = "factory/scm_a3/data";
const char* MQTT_TOPIC_STATUS = "factory/scm_a3/status";
const char* MQTT_TOPIC_CMD = "factory/scm_a3/command";

// 커스텀 파라미터 (MQTT 설정)
WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 64);
WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 6);
WiFiManagerParameter custom_mqtt_client("client", "Client ID", mqtt_client_id, 32);
WiFiManagerParameter custom_mqtt_user("user", "MQTT User (optional)", mqtt_user, 32);
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Pass (optional)", mqtt_pass, 32);

//==============================================================================
// 전역 변수
//==============================================================================

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

bool wifiConnected = false;
bool mqttConnected = false;
unsigned long lastMqttAttempt = 0;
unsigned long packetCount = 0;
unsigned long mqttSentCount = 0;
unsigned long mqttFailCount = 0;
unsigned long lastStatusTime = 0;

// 리셋 버튼 관련
unsigned long resetButtonPressTime = 0;
bool resetButtonPressed = false;

//==============================================================================
// 패킷 수신 상태 머신
//==============================================================================

enum RxState {
    RX_WAIT_STX,
    RX_GET_LEN_L,
    RX_GET_LEN_H,
    RX_GET_DATA,
    RX_GET_CHK,
    RX_GET_ETX
};

RxState rxState = RX_WAIT_STX;
uint8_t rxBuffer[RX_BUF_SIZE];
int rxIndex = 0;
uint16_t rxDataLen = 0;
int rxDataCount = 0;
unsigned long rxStartTime = 0;

//==============================================================================
// MQTT 전송 큐
//==============================================================================

#define MQTT_QUEUE_SIZE 100

struct MqttQueueItem {
    bool valid;
    uint8_t itemCount;
    uint16_t itemIds[MQTT_QUEUE_SIZE];
    uint8_t itemQualities[MQTT_QUEUE_SIZE];
    int32_t itemValues[MQTT_QUEUE_SIZE];
    unsigned long timestamp;
};

MqttQueueItem mqttQueue[MQTT_QUEUE_SIZE];

int mqttQueueHead = 0;
int mqttQueueTail = 0;

//==============================================================================
// NVS에 MQTT 설정 저장/로드
//==============================================================================

void saveMqttConfig() 
{
    preferences.begin("mqtt", false);
    preferences.putString("server", mqtt_server);
    preferences.putString("port", mqtt_port);
    preferences.putString("client", mqtt_client_id);
    preferences.putString("user", mqtt_user);
    preferences.putString("pass", mqtt_pass);
    preferences.end();
    
    DBG_PRINTLN("[NVS] MQTT config saved");
}

void loadMqttConfig() 
{
    preferences.begin("mqtt", true);
    
    String server = preferences.getString("server", "broker.hivemq.com");
    String port = preferences.getString("port", "1883");
    String client = preferences.getString("client", "ESP32_GA1_Agent");
    String user = preferences.getString("user", "");
    String pass = preferences.getString("pass", "");
    
    server.toCharArray(mqtt_server, 64);
    port.toCharArray(mqtt_port, 6);
    client.toCharArray(mqtt_client_id, 32);
    user.toCharArray(mqtt_user, 32);
    pass.toCharArray(mqtt_pass, 32);
    
    preferences.end();
    
    DBG_PRINTLN("[NVS] MQTT config loaded");
    DBG_PRINTF("  Server: %s:%s\n", mqtt_server, mqtt_port);
    DBG_PRINTF("  Client: %s\n", mqtt_client_id);
}

//==============================================================================
// WiFiManager 콜백 - 설정 저장 시 호출
//==============================================================================

void saveConfigCallback() 
{
    DBG_PRINTLN("[WiFiManager] Config saved callback");
    
    // 커스텀 파라미터 값 복사
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_client_id, custom_mqtt_client.getValue());
    strcpy(mqtt_user, custom_mqtt_user.getValue());
    strcpy(mqtt_pass, custom_mqtt_pass.getValue());
    
    // NVS에 저장
    saveMqttConfig();
}

//==============================================================================
// WiFi 설정 초기화
//==============================================================================

void resetWiFiSettings() 
{
    DBG_PRINTLN("\n[RESET] Clearing WiFi settings...");
    
    wifiManager.resetSettings();
    
    // MQTT 설정도 초기화
    preferences.begin("mqtt", false);
    preferences.clear();
    preferences.end();
    
    DBG_PRINTLN("[RESET] Done! Restarting...");
    delay(1000);
    ESP.restart();
}

//==============================================================================
// 리셋 버튼 체크 (GPIO0, 5초 누르면 초기화)
//==============================================================================

void checkResetButton() 
{
    if (digitalRead(RESET_PIN) == LOW) 
    {
        if (!resetButtonPressed) 
        {
            resetButtonPressed = true;
            resetButtonPressTime = millis();
            DBG_PRINTLN("[BUTTON] Pressed - hold 5s to reset WiFi");
        } 
        else if (millis() - resetButtonPressTime > 5000) 
        {
            // LED 빠르게 깜빡임
            for (int i = 0; i < 10; i++) 
            {
                digitalWrite(LED_PIN, !digitalRead(LED_PIN));
                delay(100);
            }
            resetWiFiSettings();
        }
    } 
    else 
    {
        if (resetButtonPressed) 
        {
            unsigned long pressDuration = millis() - resetButtonPressTime;
            DBG_PRINTF("[BUTTON] Released after %lu ms\n", pressDuration);
        }
        resetButtonPressed = false;
    }
}

//==============================================================================
// Setup
//==============================================================================

void setup() 
{
    Serial.begin(115200);
    delay(100);
    
    #if USE_DEBUG_SERIAL
    Serial2.begin(115200, SERIAL_8N1, 16, 17);
    delay(100);
    #endif
    
    pinMode(LED_PIN, OUTPUT);
    pinMode(RESET_PIN, INPUT_PULLUP);
    digitalWrite(LED_PIN, LOW);
    
    for (int i = 0; i < MQTT_QUEUE_SIZE; i++) {
        mqttQueue[i].valid = false;
    }
    
    delay(500);
    DBG_PRINTLN("\n========================================");
    DBG_PRINTLN("ESP32 UART + MQTT Gateway (v3)");
    DBG_PRINTLN("========================================");
    DBG_PRINTLN("WiFiManager enabled!");
    DBG_PRINTLN("Hold GPIO0 for 5s to reset WiFi");
    DBG_PRINTLN("========================================\n");
    
    // NVS에서 MQTT 설정 로드
    loadMqttConfig();
    
    //==========================================================================
    // WiFiManager 설정
    //==========================================================================
    
    // 디버그 출력
    wifiManager.setDebugOutput(true);
    
    // 설정 포털 타임아웃 (초)
    wifiManager.setConfigPortalTimeout(PORTAL_TIMEOUT);
    
    // 연결 타임아웃
    wifiManager.setConnectTimeout(20);
    
    // 저장 콜백 등록
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    
    // 커스텀 파라미터 추가 (MQTT 설정)
    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_port);
    wifiManager.addParameter(&custom_mqtt_client);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_pass);
    
    // AP 모드일 때 표시할 정보
    wifiManager.setAPCallback([](WiFiManager* wm) {
        DBG_PRINTLN("\n[WiFiManager] Entered config portal");
        DBG_PRINTF("  AP Name: %s\n", AP_NAME);
        DBG_PRINTF("  AP Pass: %s\n", AP_PASSWORD);
        DBG_PRINTLN("  Connect and go to 192.168.4.1");
        
        // LED 깜빡임으로 설정 모드 표시
        digitalWrite(LED_PIN, HIGH);
    });
    
    //==========================================================================
    // WiFi 자동 연결 시도
    //==========================================================================
    
    DBG_PRINTLN("[WiFiManager] Attempting auto-connect...");
    
    // autoConnect: 저장된 WiFi에 연결 시도, 실패하면 AP 모드로 전환
    if (!wifiManager.autoConnect(AP_NAME, AP_PASSWORD)) 
    {
        DBG_PRINTLN("[WiFiManager] Failed to connect and portal timeout");
        DBG_PRINTLN("Restarting...");
        delay(3000);
        ESP.restart();
    }
    
    // 연결 성공!
    wifiConnected = true;
    digitalWrite(LED_PIN, LOW);
    
    DBG_PRINTLN("\n[WiFi] Connected!");
    DBG_PRINT("  IP: ");
    DBG_PRINTLN(WiFi.localIP());
    DBG_PRINT("  SSID: ");
    DBG_PRINTLN(WiFi.SSID());
    DBG_PRINTF("  RSSI: %d dBm\n", WiFi.RSSI());
    
    // MQTT 설정
    mqttClient.setServer(mqtt_server, atoi(mqtt_port));
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    mqttClient.setBufferSize(RX_BUF_SIZE);
    
    DBG_PRINTF("\n[MQTT] Server: %s:%s\n", mqtt_server, mqtt_port);
}

//==============================================================================
// Main Loop
//==============================================================================

void loop() 
{
    checkResetButton();     // ★ 리셋 버튼 체크
    processUART();
    manageWiFi();
    manageMQTT();
    processMqttQueue();
    
    if (millis() - lastStatusTime > 30000) 
    {
        lastStatusTime = millis();
        printStatus();
        publishStatus();
    }
}

//==============================================================================
// USB 데이터 수신 (상태 머신 기반)
//==============================================================================

void processUART() 
{
    while (Serial.available()) 
    {
        uint8_t b = Serial.read();
        
        switch (rxState) 
        {
            case RX_WAIT_STX:
                if (b == PROTO_STX) 
                {
                    rxIndex = 0;
                    rxBuffer[rxIndex++] = b;
                    rxStartTime = millis();
                    rxState = RX_GET_LEN_L;
                    
                    DBG_PRINTLN("[RX] ===== PACKET START =====");
                    DBG_PRINTF("[RX] STX at %lu ms\n", rxStartTime);
                }
                break;
            
            case RX_GET_LEN_L:
                rxBuffer[rxIndex++] = b;
                rxDataLen = b;
                rxState = RX_GET_LEN_H;
                break;
            
            case RX_GET_LEN_H:
                rxBuffer[rxIndex++] = b;
                rxDataLen |= (b << 8);
                rxDataCount = 0;
                
                DBG_PRINTF("[RX] Data length: %d bytes\n", rxDataLen);
                
                if (rxDataLen > RX_BUF_SIZE - 12) 
                {
                    DBG_PRINTLN("[RX] ERROR: Length too large! Resetting.");
                    rxState = RX_WAIT_STX;
                } 
                else if (rxDataLen == 0) rxState = RX_GET_CHK;
                else rxState = RX_GET_DATA;
                break;
            
            case RX_GET_DATA:
                rxBuffer[rxIndex++] = b;
                rxDataCount++;
                if (rxDataCount >= rxDataLen) rxState = RX_GET_CHK;
                break;
            
            case RX_GET_CHK:
                rxBuffer[rxIndex++] = b;
                rxState = RX_GET_ETX;
                break;
            
            case RX_GET_ETX:
                rxBuffer[rxIndex++] = b;
                
                if (b == PROTO_ETX) 
                {
                    unsigned long endTime = millis();
                    
                    DBG_PRINT("[RX] HEX: ");
                    for (int i = 0; i < rxIndex; i++) DBG_PRINTF("%02X ", rxBuffer[i]);
                    DBG_PRINTLN();
                    DBG_PRINTF("[RX] Total: %d bytes, Duration: %lu ms\n", rxIndex, endTime - rxStartTime);
                    
                    processPacket(rxBuffer, rxIndex);
                    sendAckResponse();
                    
                    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
                    packetCount++;
                    
                    DBG_PRINTLN("[RX] ===== PACKET END =====\n");
                } 
                else DBG_PRINTF("[RX] ERROR: Expected ETX(0x03), got 0x%02X\n", b);
                
                rxState = RX_WAIT_STX;
                break;
        }
        
        if (rxState != RX_WAIT_STX && millis() - rxStartTime > 1000) 
        {
            DBG_PRINTLN("[RX] TIMEOUT! Resetting state machine.");
            rxState = RX_WAIT_STX;
        }
        
        if (rxIndex >= RX_BUF_SIZE - 2) 
        {
            DBG_PRINTLN("[RX] BUFFER OVERFLOW! Resetting.");
            rxState = RX_WAIT_STX;
        }
    }
}

//==============================================================================
// 패킷 처리 및 MQTT 큐에 추가
//==============================================================================

void processPacket(uint8_t* data, int len) 
{
    DBG_PRINTLN("[ANALYZE] Packet structure:");
    
    if (len < 5) 
    {
        DBG_PRINTLN("[ANALYZE] Too short!");
        return;
    }
    
    int chkPos = len - 2;
    uint8_t calcChk = 0;
    
    for (int i = 1; i < chkPos; i++) calcChk ^= data[i];
    uint8_t recvChk = data[chkPos];
    
    bool checksumOk = (recvChk == calcChk);
    
    DBG_PRINTF("  STX: 0x%02X ✓\n", data[0]);
    DBG_PRINTF("  Length: %d bytes\n", rxDataLen);
    DBG_PRINTF("  Checksum: recv=0x%02X, calc=0x%02X %s\n", 
              recvChk, calcChk, checksumOk ? "✓" : "✗");
    DBG_PRINTF("  ETX: 0x%02X %s\n", data[len-1],
              (data[len-1] == PROTO_ETX) ? "✓" : "✗");
    
    if (!checksumOk) 
    {
        DBG_PRINTLN("[ANALYZE] Checksum FAILED! Skipping MQTT queue.");
        return;
    }
    
    if (rxDataLen < 1) 
    {
        DBG_PRINTLN("[ANALYZE] No data!");
        return;
    }
    
    int itemCount = data[3];
    DBG_PRINTF("  Item Count: %d\n", itemCount);
    
    MqttQueueItem* item = &mqttQueue[mqttQueueHead];
    
    int nextHead = (mqttQueueHead + 1) % MQTT_QUEUE_SIZE;
    if (nextHead == mqttQueueTail && mqttQueue[mqttQueueTail].valid) 
    {
        DBG_PRINTLN("[MQTT] Queue full! Dropping oldest.");
        mqttQueueTail = (mqttQueueTail + 1) % MQTT_QUEUE_SIZE;
    }
    
    item->itemCount = min(itemCount, 10);
    item->timestamp = millis();
    
    if (item->itemCount > 0) 
    {
        DBG_PRINTLN("[ANALYZE] Items:");
        int pos = 4;
        for (int i = 0; i < item->itemCount && pos + 6 < chkPos; i++) 
        {
            item->itemIds[i] = data[pos] | (data[pos+1] << 8);
            item->itemQualities[i] = data[pos+2];
            item->itemValues[i] = (int32_t)(data[pos+3] | 
                                           (data[pos+4]<<8) | 
                                           (data[pos+5]<<16) | 
                                           (data[pos+6]<<24));
            
            DBG_PRINTF("    [%d] ID=%d, Q=%d, V=%ld\n", 
                      i, item->itemIds[i], item->itemQualities[i], item->itemValues[i]);
            
            pos += 7;
        }
    }
    
    item->valid = true;
    mqttQueueHead = nextHead;
    
    DBG_PRINTF("[MQTT] Queued! (size: %d)\n", getMqttQueueSize());
}

//==============================================================================
// ACK 응답
//==============================================================================

void sendAckResponse()
{
    uint8_t response[5] = 
    {
        PROTO_STX,
        0x01,
        0x00,
        0x01,
        PROTO_ETX
    };
    
    DBG_PRINT("[TX] ACK: ");
    for (int i = 0; i < 5; i++) DBG_PRINTF("%02X ", response[i]);
    DBG_PRINTLN();
    
    Serial.write(response, 5);
    Serial.flush();
    
    DBG_PRINTLN("[TX] Sent!");
}

//==============================================================================
// WiFi 관리 (WiFiManager 사용으로 간소화)
//==============================================================================

void manageWiFi()
{
    if (WiFi.status() == WL_CONNECTED) 
    {
        if (!wifiConnected) 
        {
            wifiConnected = true;
            DBG_PRINT("[WiFi] Reconnected! IP: ");
            DBG_PRINTLN(WiFi.localIP());
        }
    } 
    else 
    {
        if (wifiConnected) 
        {
            wifiConnected = false;
            mqttConnected = false;
            DBG_PRINTLN("[WiFi] Disconnected!");
        }
        
        // WiFiManager가 자동으로 재연결 시도
        // 일정 시간 후에도 연결 안되면 재부팅 고려
        static unsigned long disconnectTime = 0;
        if (disconnectTime == 0) disconnectTime = millis();
        
        if (millis() - disconnectTime > 60000)  // 1분 동안 연결 안되면
        {
            DBG_PRINTLN("[WiFi] Connection lost for 60s, restarting...");
            ESP.restart();
        }
    }
}

//==============================================================================
// MQTT 관리
//==============================================================================

void manageMQTT()
{
    if (!wifiConnected) return;
    
    if (mqttClient.connected()) 
    {
        if (!mqttConnected) 
        {
            mqttConnected = true;
            DBG_PRINTLN("[MQTT] Connected!");
        }
        mqttClient.loop();
    } 
    else 
    {
        if (mqttConnected) 
        {
            mqttConnected = false;
            DBG_PRINTLN("[MQTT] Disconnected!");
        }
        
        if (millis() - lastMqttAttempt > 5000) 
        {
            lastMqttAttempt = millis();
            connectMQTT();
        }
    }
}

//==============================================================================
// MQTT 연결
//==============================================================================

void connectMQTT()
{
    DBG_PRINTF("[MQTT] Connecting to %s:%s...\n", mqtt_server, mqtt_port);
    
    bool connected;
    if (strlen(mqtt_user) > 0) 
        connected = mqttClient.connect(mqtt_client_id, mqtt_user, mqtt_pass);
    else 
        connected = mqttClient.connect(mqtt_client_id);
    
    if (connected) 
    {
        DBG_PRINTLN("[MQTT] Connected!");
        mqttConnected = true;
        mqttClient.subscribe(MQTT_TOPIC_CMD);
        publishStatus();
    } 
    else 
        DBG_PRINTF("[MQTT] Failed! rc=%d\n", mqttClient.state());
}

//==============================================================================
// MQTT 콜백
//==============================================================================

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    DBG_PRINTF("[MQTT] Received on %s\n", topic);
    
    char message[256];
    int copyLen = min((int)length, 255);
    memcpy(message, payload, copyLen);
    message[copyLen] = '\0';
    
    DBG_PRINTLN(message);
    
    if (strcmp(topic, MQTT_TOPIC_CMD) == 0) 
    {
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, message) == DeserializationError::Ok) 
        {
            const char* cmd = doc["cmd"];
            if (strcmp(cmd, "status") == 0) publishStatus();
            else if (strcmp(cmd, "reset") == 0) packetCount = mqttSentCount = mqttFailCount = 0;
            else if (strcmp(cmd, "led_on") == 0) digitalWrite(LED_PIN, HIGH);
            else if (strcmp(cmd, "led_off") == 0) digitalWrite(LED_PIN, LOW);
            else if (strcmp(cmd, "wifi_reset") == 0) resetWiFiSettings();  // ★ 원격 WiFi 초기화
            else if (strcmp(cmd, "reboot") == 0) ESP.restart();            // ★ 원격 재부팅
        }
    }
}

//==============================================================================
// MQTT 큐 처리
//==============================================================================

int getMqttQueueSize()
{
    int size = 0;
    for (int i = 0; i < MQTT_QUEUE_SIZE; i++)   
    {
      if (mqttQueue[i].valid) size++;
    }
    return size;
}

void processMqttQueue()
{
    if (!mqttConnected) return;
    if (!mqttQueue[mqttQueueTail].valid) return;
    
    MqttQueueItem* item = &mqttQueue[mqttQueueTail];
    
    StaticJsonDocument<512> doc;
    doc["device"] = mqtt_client_id;
    doc["timestamp"] = item->timestamp;
    doc["packet_id"] = packetCount;
    
    for (int i = 0; i < item->itemCount; i++) 
    {
        String valueKey = "value_" + String(item->itemIds[i]);
        String qualityKey = "quality_" + String(item->itemIds[i]);
        
        doc[valueKey] = item->itemValues[i];
        doc[qualityKey] = item->itemQualities[i];
    }
    
    char jsonBuffer[512];
    serializeJson(doc, jsonBuffer);
    
    DBG_PRINTF("[MQTT] Publish: %s\n", jsonBuffer);
    
    if (mqttClient.publish(MQTT_TOPIC_DATA, jsonBuffer)) 
    {
        DBG_PRINTLN("[MQTT] OK!");
        mqttSentCount++;
    } 
    else 
    {
        DBG_PRINTLN("[MQTT] FAILED!");
        mqttFailCount++;
    }
    
    item->valid = false;
    mqttQueueTail = (mqttQueueTail + 1) % MQTT_QUEUE_SIZE;
}

//==============================================================================
// 상태 출력
//==============================================================================
void printStatus()
{
    DBG_PRINTLN("\n======== STATUS ========");
    DBG_PRINTF("Uptime: %lu sec\n", millis() / 1000);
    DBG_PRINTF("Packets: %lu\n", packetCount);
    DBG_PRINTF("WiFi: %s", wifiConnected ? "OK" : "NO");
    if (wifiConnected) 
    {
        DBG_PRINTF(" (SSID: %s, IP: %s, %ddBm)", 
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(), 
                  WiFi.RSSI());
    }
    DBG_PRINTLN();
    DBG_PRINTF("MQTT: %s (%s:%s)\n", mqttConnected ? "OK" : "NO", mqtt_server, mqtt_port);
    DBG_PRINTF("  sent:%lu, fail:%lu, queue:%d\n", mqttSentCount, mqttFailCount, getMqttQueueSize());
    DBG_PRINTLN("========================\n");
}

void publishStatus()
{
    if (!mqttConnected) return;
    
    StaticJsonDocument<256> doc;
    doc["device"] = mqtt_client_id;
    doc["uptime"] = millis() / 1000;
    doc["packets"] = packetCount;
    doc["mqtt_sent"] = mqttSentCount;
    doc["mqtt_fail"] = mqttFailCount;
    doc["rssi"] = WiFi.RSSI();
    doc["heap"] = ESP.getFreeHeap();
    doc["ip"] = WiFi.localIP().toString();
    doc["ssid"] = WiFi.SSID();
    
    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC_STATUS, buf);
}

/*
================================================================================
v3 변경사항 (WiFiManager 적용):
================================================================================

1. WiFiManager 라이브러리 추가
   - 처음 부팅 시 저장된 WiFi에 자동 연결 시도
   - 연결 실패 시 "ESP32-GA1-Setup" AP 모드로 전환
   - 브라우저에서 192.168.4.1 접속하여 WiFi 설정

2. MQTT 설정도 웹에서 설정 가능
   - Server, Port, Client ID, User, Password

3. 설정 초기화 기능
   - GPIO0 (BOOT) 버튼 5초 누르면 WiFi 설정 삭제 후 재부팅
   - MQTT 명령으로도 가능: {"cmd": "wifi_reset"}

4. NVS 저장
   - WiFi 및 MQTT 설정이 비휘발성 메모리에 저장됨

================================================================================
라이브러리 설치 (Arduino IDE):
================================================================================

1. 스케치 → 라이브러리 포함 → 라이브러리 관리
2. "WiFiManager" 검색 → "WiFiManager by tzapu" 설치
3. "PubSubClient" 검색 → 설치
4. "ArduinoJson" 검색 → 설치

================================================================================
*/
