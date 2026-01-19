/*******************************************************************************
 * ESP32 USB 통신 진단 + WiFi + MQTT 전송 (v2)
 * 
 * 수정 사항 (v2):
 * - 길이 기반 패킷 수신으로 변경
 * - 데이터 중간에 0x03이 있어도 정상 수신
 * 
 * 패킷 구조:
 * [STX(1)] [LEN_L(1)] [LEN_H(1)] [DATA(LEN)] [CHK(1)] [ETX(1)]
 * 
 * 작성일: 2026-01-19
 ******************************************************************************/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

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

#define PROTO_STX 0x02
#define PROTO_ETX 0x03
#define LED_PIN   2

//==============================================================================
// WiFi 설정
//==============================================================================

//const char* WIFI_SSID = "형경산업";
//const char* WIFI_PASSWORD = "12341234";

const char* WIFI_SSID = "iptime";
const char* WIFI_PASSWORD = "";

//==============================================================================
// MQTT 설정
//==============================================================================

const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT = 1883;
const char* MQTT_CLIENT_ID = "ESP32_GA1_Agent";
const char* MQTT_TOPIC_DATA = "ga1agent/data";
const char* MQTT_TOPIC_STATUS = "ga1agent/status";
const char* MQTT_TOPIC_CMD = "ga1agent/command";

const char* MQTT_USER = "";
const char* MQTT_PASS = "";

//==============================================================================
// 전역 변수
//==============================================================================

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

bool wifiConnected = false;
bool mqttConnected = false;
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long packetCount = 0;
unsigned long mqttSentCount = 0;
unsigned long mqttFailCount = 0;
unsigned long lastStatusTime = 0;

//==============================================================================
// 패킷 수신 상태 머신
//==============================================================================

enum RxState {
    RX_WAIT_STX,      // STX 대기
    RX_GET_LEN_L,     // 길이 하위 바이트
    RX_GET_LEN_H,     // 길이 상위 바이트
    RX_GET_DATA,      // 데이터 수신
    RX_GET_CHK,       // 체크섬
    RX_GET_ETX        // ETX
};

RxState rxState = RX_WAIT_STX;
uint8_t rxBuffer[512];
int rxIndex = 0;
uint16_t rxDataLen = 0;
int rxDataCount = 0;
unsigned long rxStartTime = 0;

//==============================================================================
// MQTT 전송 큐
//==============================================================================

#define MQTT_QUEUE_SIZE 10

struct MqttQueueItem {
    bool valid;
    uint8_t itemCount;
    uint16_t itemIds[10];
    uint8_t itemQualities[10];
    int32_t itemValues[10];
    unsigned long timestamp;
};

MqttQueueItem mqttQueue[MQTT_QUEUE_SIZE];
int mqttQueueHead = 0;
int mqttQueueTail = 0;

//==============================================================================
// Setup
//==============================================================================

void setup() {
    Serial.begin(115200);
    delay(100);
    
    #if USE_DEBUG_SERIAL
    Serial2.begin(115200, SERIAL_8N1, 16, 17);
    delay(100);
    #endif
    
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    for (int i = 0; i < MQTT_QUEUE_SIZE; i++) {
        mqttQueue[i].valid = false;
    }
    
    delay(500);
    DBG_PRINTLN("\n========================================");
    DBG_PRINTLN("ESP32 UART + MQTT Gateway (v2)");
    DBG_PRINTLN("========================================");
    DBG_PRINTLN("Fixed: Length-based packet reception");
    DBG_PRINTLN("========================================\n");
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiAttempt = millis();
    
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    mqttClient.setBufferSize(512);
}

//==============================================================================
// Main Loop
//==============================================================================

void loop() 
{
    processUART();
    manageWiFi();
    manageMQTT();
    processMqttQueue();
    
    if (millis() - lastStatusTime > 30000) {
        lastStatusTime = millis();
        printStatus();
        publishStatus();
    }
}

//==============================================================================
// USB 데이터 수신 (상태 머신 기반)
// ★ 길이 필드를 읽고 해당 길이만큼 수신
//==============================================================================

void processUART() 
{
    while (Serial.available()) 
    {
        uint8_t b = Serial.read();
        
        switch (rxState) 
        {
            //------------------------------------------------------------------
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
            
            //------------------------------------------------------------------
            case RX_GET_LEN_L:
                rxBuffer[rxIndex++] = b;
                rxDataLen = b;  // 하위 바이트
                rxState = RX_GET_LEN_H;
                break;
            
            //------------------------------------------------------------------
            case RX_GET_LEN_H:
                rxBuffer[rxIndex++] = b;
                rxDataLen |= (b << 8);  // 상위 바이트
                rxDataCount = 0;
                
                DBG_PRINTF("[RX] Data length: %d bytes\n", rxDataLen);
                
                // 길이 유효성 검사
                if (rxDataLen > 500) 
                {
                    DBG_PRINTLN("[RX] ERROR: Length too large! Resetting.");
                    rxState = RX_WAIT_STX;
                } 
                else if (rxDataLen == 0) rxState = RX_GET_CHK;
                else rxState = RX_GET_DATA;

                break;
            
            //------------------------------------------------------------------
            case RX_GET_DATA:
                rxBuffer[rxIndex++] = b;
                rxDataCount++;
                
                if (rxDataCount >= rxDataLen) rxState = RX_GET_CHK;
                break;
            
            //------------------------------------------------------------------
            case RX_GET_CHK:
                rxBuffer[rxIndex++] = b;
                rxState = RX_GET_ETX;
                break;
            
            //------------------------------------------------------------------
            case RX_GET_ETX:
                rxBuffer[rxIndex++] = b;
                
                if (b == PROTO_ETX) 
                {
                    unsigned long endTime = millis();
                    
                    DBG_PRINT("[RX] HEX: ");
                    for (int i = 0; i < rxIndex; i++) DBG_PRINTF("%02X ", rxBuffer[i]);
                    DBG_PRINTLN();
                    DBG_PRINTF("[RX] Total: %d bytes, Duration: %lu ms\n", rxIndex, endTime - rxStartTime);
                    
                    // 패킷 처리
                    processPacket(rxBuffer, rxIndex);
                    
                    // ACK 응답
                    sendAckResponse();
                    
                    // LED 토글
                    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
                    packetCount++;
                    
                    DBG_PRINTLN("[RX] ===== PACKET END =====\n");
                } 
                else DBG_PRINTF("[RX] ERROR: Expected ETX(0x03), got 0x%02X\n", b);
                
                rxState = RX_WAIT_STX;
                break;
        }
        
        // 타임아웃 체크 (수신 중 1초 초과시 리셋)
        if (rxState != RX_WAIT_STX && millis() - rxStartTime > 1000) 
        {
            DBG_PRINTLN("[RX] TIMEOUT! Resetting state machine.");
            rxState = RX_WAIT_STX;
        }
        
        // 버퍼 오버플로우 방지
        if (rxIndex >= 510) 
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
    
    // 최소 길이 확인
    if (len < 5) 
    {
        DBG_PRINTLN("[ANALYZE] Too short!");
        return;
    }
    
    // 체크섬 계산 (LEN_L ~ DATA 끝까지 XOR)
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
    
    // 아이템 파싱
    if (rxDataLen < 1) 
    {
        DBG_PRINTLN("[ANALYZE] No data!");
        return;
    }
    
    int itemCount = data[3];  // 아이템 개수
    DBG_PRINTF("  Item Count: %d\n", itemCount);
    
    // MQTT 큐에 추가
    MqttQueueItem* item = &mqttQueue[mqttQueueHead];
    
    // 큐 가득 찬 경우
    int nextHead = (mqttQueueHead + 1) % MQTT_QUEUE_SIZE;
    if (nextHead == mqttQueueTail && mqttQueue[mqttQueueTail].valid) 
    {
        DBG_PRINTLN("[MQTT] Queue full! Dropping oldest.");
        mqttQueueTail = (mqttQueueTail + 1) % MQTT_QUEUE_SIZE;
    }
    
    item->itemCount = min(itemCount, 10);
    item->timestamp = millis();
    
    // 아이템 데이터 추출
    if (item->itemCount > 0) 
    {
        DBG_PRINTLN("[ANALYZE] Items:");
        int pos = 4;  // 아이템 시작 위치
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
// ACK 응답 (즉시!)
//==============================================================================

void sendAckResponse()
{
    uint8_t response[5] = 
    {
        PROTO_STX,
        0x01,  // CMD_ACK
        0x00,  // STATUS_OK
        0x01,  // Checksum
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
// WiFi 관리
//==============================================================================

void manageWiFi()
{
    if (WiFi.status() == WL_CONNECTED) 
    {
        if (!wifiConnected) 
        {
            wifiConnected = true;
            DBG_PRINT("[WiFi] Connected! IP: ");
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
        
        if (millis() - lastWifiAttempt > 10000) 
        {
            lastWifiAttempt = millis();
            DBG_PRINTLN("[WiFi] Reconnecting...");
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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
    DBG_PRINTF("[MQTT] Connecting to %s...\n", MQTT_SERVER);
    
    bool connected;
    if (strlen(MQTT_USER) > 0) connected = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
    else connected = mqttClient.connect(MQTT_CLIENT_ID);
    
    if (connected) 
    {
        DBG_PRINTLN("[MQTT] Connected!");
        mqttConnected = true;
        mqttClient.subscribe(MQTT_TOPIC_CMD);
        publishStatus();
    } 
    else DBG_PRINTF("[MQTT] Failed! rc=%d\n", mqttClient.state());
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

//
void processMqttQueue()
{
    if (!mqttConnected) return;
    if (!mqttQueue[mqttQueueTail].valid) return;
    
    MqttQueueItem* item = &mqttQueue[mqttQueueTail];
    
    StaticJsonDocument<512> doc;
    doc["device"] = MQTT_CLIENT_ID;
    doc["timestamp"] = item->timestamp;
    doc["packet_id"] = packetCount;
    
    JsonArray items = doc.createNestedArray("items");
    for (int i = 0; i < item->itemCount; i++) 
    {
        JsonObject obj = items.createNestedObject();
        obj["id"] = item->itemIds[i];
        obj["quality"] = item->itemQualities[i];
        obj["value"] = item->itemValues[i];
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
    if (wifiConnected) DBG_PRINTF(" (%s, %ddBm)", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    DBG_PRINTLN();
    DBG_PRINTF("MQTT: %s (sent:%lu, fail:%lu, queue:%d)\n", 
              mqttConnected ? "OK" : "NO", mqttSentCount, mqttFailCount, getMqttQueueSize());
    DBG_PRINTLN("========================\n");
}

void publishStatus()
{
    if (!mqttConnected) return;
    
    StaticJsonDocument<256> doc;
    doc["device"] = MQTT_CLIENT_ID;
    doc["uptime"] = millis() / 1000;
    doc["packets"] = packetCount;
    doc["mqtt_sent"] = mqttSentCount;
    doc["mqtt_fail"] = mqttFailCount;
    doc["rssi"] = WiFi.RSSI();
    doc["heap"] = ESP.getFreeHeap();
    doc["ip"] = WiFi.localIP().toString();
    
    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC_STATUS, buf);
}

/*
================================================================================
v2 변경사항:
================================================================================

1. 상태 머신 기반 패킷 수신:
   RX_WAIT_STX → RX_GET_LEN_L → RX_GET_LEN_H → RX_GET_DATA → RX_GET_CHK → RX_GET_ETX
   
2. 길이 필드를 먼저 읽고, 해당 길이만큼 수신:
   - 데이터 중간에 0x03이 있어도 ETX로 오인하지 않음 ✓
   
3. 타임아웃 처리:
   - 수신 중 1초 초과시 상태 리셋
   
4. 버퍼 오버플로우 방지:
   - 510바이트 초과시 리셋

================================================================================
*/
