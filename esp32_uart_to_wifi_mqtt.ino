/*******************************************************************************
 * ESP32 USB 통신 진단 + WiFi + MQTT 전송 (v2)
 * 
 * ============================================================================
 * v4 주요 수정사항 (UART ACK FAIL 문제 해결):
 * ============================================================================
 * 1. FreeRTOS 태스크 분리 - UART를 별도 Core에서 처리
 * 2. ACK 우선 전송 - 패킷 처리 전에 먼저 ACK 응답
 * 3. MQTT 소켓 타임아웃 단축 (15초 → 2초)
 * 4. MQTT 연결을 Non-blocking으로 개선
 * 5. 뮤텍스로 공유 자원 보호
 * 
 * ============================================================================
 * 통신 구조:
 * ============================================================================
 * [OPC DA Server] → [BC++ Agent] → [UART] → [ESP32] → [MQTT] → [Broker]
 * 
 * ============================================================================
 * 패킷 구조:
 * ============================================================================
 * [STX(1)] [LEN_L(1)] [LEN_H(1)] [DATA(LEN)] [CHK(1)] [ETX(1)]
 * 
 * ============================================================================
 * 설정 방법:
 * ============================================================================
 * 1. 처음 부팅 또는 WiFi 연결 실패 시 → "ESP32-Wifi-Setup" AP 생성
 * 2. 스마트폰/PC로 해당 AP에 연결 (비밀번호: 12345678)
 * 3. 브라우저에서 192.168.4.1 접속
 * 4. WiFi 선택 및 MQTT 서버 설정 후 저장
 * 
 * ★ 설정 초기화: GPIO0 버튼을 5초간 누르면 WiFi 설정 삭제 후 재부팅
 * 
 * 작성일: 2026-02-05
 ******************************************************************************/

#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

//==============================================================================
// 디버그 설정
//==============================================================================

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

//==============================================================================
// 핀 및 상수 정의
//==============================================================================

#define PROTO_STX       0x02
#define PROTO_ETX       0x03
#define LED_PIN         2
#define RESET_PIN       0           // GPIO0 (BOOT 버튼) - WiFi 설정 초기화용
#define RX_BUF_SIZE     1024

// WiFiManager 설정
#define AP_NAME         "ESP32-Wifi-Setup"
#define AP_PASSWORD     "12345678"
#define PORTAL_TIMEOUT  180         // 설정 포털 타임아웃 (초)

// MQTT 큐 설정
#define MQTT_QUEUE_SIZE 100
#define MQTT_SOCKET_TIMEOUT 2       // MQTT 소켓 타임아웃 (초) - 기본 15초에서 단축

// MQTT Server 
#define SERVER_ADDRESS   "broker.hivemq.com"  //VWware: "192.168.0.190"
                                              // HK-IoT_Server: "112.218.90.189"

//==============================================================================
// MQTT 설정 변수
//==============================================================================

char mqtt_server[64] = SERVER_ADDRESS;  
char mqtt_port[6] = "1883";
char mqtt_client_id[32] = "ESP32_GA1_Agent";
char mqtt_user[32] = "";
char mqtt_pass[32] = "";

// MQTT 토픽
const char* MQTT_TOPIC_DATA   = "factory/scm_a3/data";
const char* MQTT_TOPIC_STATUS = "factory/scm_a3/status";
const char* MQTT_TOPIC_CMD    = "factory/scm_a3/command";

//==============================================================================
// WiFiManager 커스텀 파라미터
//==============================================================================

WiFiManager wifiManager;
Preferences preferences;

WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 64);
WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 6);
WiFiManagerParameter custom_mqtt_client("client", "Client ID", mqtt_client_id, 32);
WiFiManagerParameter custom_mqtt_user("user", "MQTT User (optional)", mqtt_user, 32);
WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Pass (optional)", mqtt_pass, 32);

//==============================================================================
// 전역 객체
//==============================================================================

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

//==============================================================================
// FreeRTOS 핸들 및 뮤텍스
//==============================================================================

TaskHandle_t uartTaskHandle = NULL;
TaskHandle_t mqttTaskHandle = NULL;
SemaphoreHandle_t queueMutex = NULL;

//==============================================================================
// 상태 변수 (volatile - 멀티태스크 공유)
//==============================================================================

volatile bool wifiConnected = false;
volatile bool mqttConnected = false;
volatile unsigned long packetCount = 0;
volatile unsigned long mqttSentCount = 0;
volatile unsigned long mqttFailCount = 0;
volatile unsigned long ackSentCount = 0;

unsigned long lastMqttAttempt = 0;
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

volatile RxState rxState = RX_WAIT_STX;
uint8_t rxBuffer[RX_BUF_SIZE];
int rxIndex = 0;
uint16_t rxDataLen = 0;
int rxDataCount = 0;
unsigned long rxStartTime = 0;

//==============================================================================
// MQTT 전송 큐
//==============================================================================

struct MqttQueueItem {
    bool valid;
    uint8_t itemCount;
    uint16_t itemIds[20];
    uint8_t itemQualities[20];
    int32_t itemValues[20];
    unsigned long timestamp;
};

MqttQueueItem mqttQueue[MQTT_QUEUE_SIZE];
volatile int mqttQueueHead = 0;
volatile int mqttQueueTail = 0;

//==============================================================================
// 함수 프로토타입
//==============================================================================

void uartTask(void* param);
void mqttTask(void* param);
void processUART();
void processPacket(uint8_t* data, int len);
void sendAckResponse();
void addToMqttQueue(uint8_t* data, int len);
void processMqttQueue();
void manageWiFi();
void manageMQTT();
void connectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void saveMqttConfig();
void loadMqttConfig();
void saveConfigCallback();
void resetWiFiSettings();
void checkResetButton();
void printStatus();
void publishStatus();
int getMqttQueueSize();

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
    
    String server = preferences.getString("server", SERVER_ADDRESS);
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
// WiFiManager 콜백
//==============================================================================

void saveConfigCallback() 
{
    DBG_PRINTLN("[WiFiManager] Config saved callback");
    
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_client_id, custom_mqtt_client.getValue());
    strcpy(mqtt_user, custom_mqtt_user.getValue());
    strcpy(mqtt_pass, custom_mqtt_pass.getValue());
    
    saveMqttConfig();
}

//==============================================================================
// WiFi 설정 초기화
//==============================================================================

void resetWiFiSettings() 
{
    DBG_PRINTLN("\n[RESET] Clearing WiFi settings...");
    
    wifiManager.resetSettings();
    
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
        resetButtonPressed = false;
    }
}

//==============================================================================
// Setup
//==============================================================================

void setup() 
{
    // 시리얼 초기화
    Serial.begin(115200);       // UART0 - Agent 통신용
    delay(100);
    
    #if USE_DEBUG_SERIAL
    Serial2.begin(115200, SERIAL_8N1, 16, 17);  // UART2 - 디버그용
    delay(100);
    #endif
    
    // GPIO 초기화
    pinMode(LED_PIN, OUTPUT);
    pinMode(RESET_PIN, INPUT_PULLUP);
    digitalWrite(LED_PIN, LOW);
    
    // 뮤텍스 생성
    queueMutex = xSemaphoreCreateMutex();
    
    // MQTT 큐 초기화
    for (int i = 0; i < MQTT_QUEUE_SIZE; i++) 
    {
        mqttQueue[i].valid = false;
    }
    
    delay(500);
    
    DBG_PRINTLN("\n================================================");
    DBG_PRINTLN("  ESP32 UART + MQTT Gateway (v4)");
    DBG_PRINTLN("  - FreeRTOS Task Separation");
    DBG_PRINTLN("  - ACK Priority Response");
    DBG_PRINTLN("================================================");
    DBG_PRINTLN("Hold GPIO0 for 5s to reset WiFi");
    DBG_PRINTLN("================================================\n");
    
    // NVS에서 MQTT 설정 로드
    loadMqttConfig();
    
    //==========================================================================
    // WiFiManager 설정
    //==========================================================================
    
    wifiManager.setDebugOutput(true);
    wifiManager.setConfigPortalTimeout(PORTAL_TIMEOUT);
    wifiManager.setConnectTimeout(20);
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    
    wifiManager.addParameter(&custom_mqtt_server);
    wifiManager.addParameter(&custom_mqtt_port);
    wifiManager.addParameter(&custom_mqtt_client);
    wifiManager.addParameter(&custom_mqtt_user);
    wifiManager.addParameter(&custom_mqtt_pass);
    
    wifiManager.setAPCallback([](WiFiManager* wm) {
        DBG_PRINTLN("\n[WiFiManager] Entered config portal");
        DBG_PRINTF("  AP: %s / %s\n", AP_NAME, AP_PASSWORD);
        DBG_PRINTLN("  Connect and go to 192.168.4.1");
        digitalWrite(LED_PIN, HIGH);
    });
    
    //==========================================================================
    // WiFi 연결
    //==========================================================================
    
    DBG_PRINTLN("[WiFiManager] Attempting auto-connect...");
    
    if (!wifiManager.autoConnect(AP_NAME, AP_PASSWORD)) 
    {
        DBG_PRINTLN("[WiFiManager] Failed! Restarting...");
        delay(3000);
        ESP.restart();
    }
    
    wifiConnected = true;
    digitalWrite(LED_PIN, LOW);
    
    DBG_PRINTLN("\n[WiFi] Connected!");
    DBG_PRINTF("  IP: %s\n", WiFi.localIP().toString().c_str());
    DBG_PRINTF("  SSID: %s\n", WiFi.SSID().c_str());
    DBG_PRINTF("  RSSI: %d dBm\n", WiFi.RSSI());
    
    //==========================================================================
    // MQTT 설정
    //==========================================================================
    
    mqttClient.setServer(mqtt_server, atoi(mqtt_port));
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    mqttClient.setBufferSize(RX_BUF_SIZE);
    mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT);  // ★ 타임아웃 단축!
    
    DBG_PRINTF("\n[MQTT] Server: %s:%s\n", mqtt_server, mqtt_port);
    DBG_PRINTF("[MQTT] Socket Timeout: %d sec\n", MQTT_SOCKET_TIMEOUT);
    
    //==========================================================================
    // FreeRTOS 태스크 생성
    //==========================================================================
    
    DBG_PRINTLN("\n[RTOS] Creating tasks...");
    
    // UART 태스크 - Core 1에서 실행 (높은 우선순위)
    xTaskCreatePinnedToCore(
        uartTask,           // 태스크 함수
        "UART_Task",        // 태스크 이름
        4096,               // 스택 크기
        NULL,               // 파라미터
        3,                  // 우선순위 (높음)
        &uartTaskHandle,    // 핸들
        1                   // Core 1 (WiFi는 Core 0 사용)
    );
    
    // MQTT 태스크 - Core 0에서 실행
    xTaskCreatePinnedToCore(
        mqttTask,
        "MQTT_Task",
        8192,
        NULL,
        1,                  // 우선순위 (낮음)
        &mqttTaskHandle,
        0                   // Core 0
    );
    
    DBG_PRINTLN("[RTOS] Tasks created!");
    DBG_PRINTLN("  - UART_Task on Core 1 (Priority 3)");
    DBG_PRINTLN("  - MQTT_Task on Core 0 (Priority 1)");
    DBG_PRINTLN();
}

//==============================================================================
// Main Loop (최소한의 작업만)
//==============================================================================

void loop() 
{
    checkResetButton();
    
    // 상태 출력 (30초마다)
    if (millis() - lastStatusTime > 30000) 
    {
        lastStatusTime = millis();
        printStatus();
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS);  // 100ms 대기
}

//==============================================================================
// UART 태스크 (Core 1에서 실행) - 최우선 처리
//==============================================================================

void uartTask(void* param) 
{
    DBG_PRINTLN("[UART Task] Started on Core 1");
    
    while (true) 
    {
        processUART();
        vTaskDelay(1);  // 1ms 양보 (watchdog 방지)
    }
}

//==============================================================================
// MQTT 태스크 (Core 0에서 실행)
//==============================================================================

void mqttTask(void* param) 
{
    DBG_PRINTLN("[MQTT Task] Started on Core 0");
    
    while (true) 
    {
        manageWiFi();
        manageMQTT();
        processMqttQueue();
        
        // 주기적 상태 발행
        static unsigned long lastPublish = 0;
        if (millis() - lastPublish > 30000) 
        {
            lastPublish = millis();
            publishStatus();
        }
        
        vTaskDelay(10 / portTICK_PERIOD_MS);  // 10ms 대기
    }
}

//==============================================================================
// UART 데이터 수신 (상태 머신 기반)
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
                
                if (rxDataLen > RX_BUF_SIZE - 12) 
                {
                    DBG_PRINTLN("[RX] ERROR: Length too large!");
                    rxState = RX_WAIT_STX;
                } 
                else if (rxDataLen == 0) 
                {
                    rxState = RX_GET_CHK;
                }
                else 
                {
                    rxState = RX_GET_DATA;
                }
                break;
            
            case RX_GET_DATA:
                rxBuffer[rxIndex++] = b;
                rxDataCount++;
                if (rxDataCount >= rxDataLen) 
                {
                    rxState = RX_GET_CHK;
                }
                break;
            
            case RX_GET_CHK:
                rxBuffer[rxIndex++] = b;
                rxState = RX_GET_ETX;
                break;
            
            case RX_GET_ETX:
                rxBuffer[rxIndex++] = b;
                
                if (b == PROTO_ETX) 
                {
                    //==========================================================
                    // ★★★ 핵심 수정: ACK를 먼저 보낸다! ★★★
                    //==========================================================
                    sendAckResponse();  // 1. ACK 즉시 전송
                    
                    // 2. 그 다음 패킷 처리 (큐에 추가)
                    processPacket(rxBuffer, rxIndex);
                    
                    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
                    packetCount++;
                } 
                else 
                {
                    DBG_PRINTF("[RX] ERROR: Expected ETX, got 0x%02X\n", b);
                }
                
                rxState = RX_WAIT_STX;
                break;
        }
        
        // 타임아웃 체크 (1초)
        if (rxState != RX_WAIT_STX && millis() - rxStartTime > 1000) 
        {
            DBG_PRINTLN("[RX] TIMEOUT! Resetting.");
            rxState = RX_WAIT_STX;
        }
        
        // 버퍼 오버플로우 체크
        if (rxIndex >= RX_BUF_SIZE - 2) 
        {
            DBG_PRINTLN("[RX] BUFFER OVERFLOW!");
            rxState = RX_WAIT_STX;
        }
    }
}

//==============================================================================
// ACK 응답 (최우선 - 즉시 전송)
//==============================================================================

void sendAckResponse()
{
    uint8_t response[5] = {
        PROTO_STX,
        0x01,       // Length Low
        0x00,       // Length High
        0x01,       // Data (ACK)
        PROTO_ETX
    };
    
    Serial.write(response, 5);
    Serial.flush();  // 즉시 전송 보장
    
    ackSentCount++;
}

//==============================================================================
// 패킷 처리 및 MQTT 큐에 추가
//==============================================================================

void processPacket(uint8_t* data, int len) 
{
    if (len < 5) return;
    
    // 체크섬 검증
    int chkPos = len - 2;
    uint8_t calcChk = 0;
    for (int i = 1; i < chkPos; i++) 
    {
        calcChk ^= data[i];
    }
    
    if (data[chkPos] != calcChk) 
    {
        DBG_PRINTF("[RX] Checksum FAIL! recv=0x%02X calc=0x%02X\n", 
                   data[chkPos], calcChk);
        return;
    }
    
    if (rxDataLen < 1) return;
    
    // MQTT 큐에 추가 (뮤텍스 보호)
    if (xSemaphoreTake(queueMutex, portMAX_DELAY) == pdTRUE) 
    {
        int nextHead = (mqttQueueHead + 1) % MQTT_QUEUE_SIZE;
        
        // 큐가 가득 찬 경우 가장 오래된 항목 삭제
        if (nextHead == mqttQueueTail && mqttQueue[mqttQueueTail].valid) 
        {
            DBG_PRINTLN("[QUEUE] Full! Dropping oldest.");
            mqttQueueTail = (mqttQueueTail + 1) % MQTT_QUEUE_SIZE;
        }
        
        MqttQueueItem* item = &mqttQueue[mqttQueueHead];
        
        int itemCount = data[3];
        item->itemCount = min(itemCount, 20);
        item->timestamp = millis();
        
        if (item->itemCount > 0) 
        {
            int pos = 4;
            for (int i = 0; i < item->itemCount && pos + 6 < chkPos; i++) 
            {
                item->itemIds[i] = data[pos] | (data[pos+1] << 8);
                item->itemQualities[i] = data[pos+2];
                item->itemValues[i] = (int32_t)(
                    data[pos+3] | 
                    (data[pos+4] << 8) | 
                    (data[pos+5] << 16) | 
                    (data[pos+6] << 24)
                );
                pos += 7;
            }
        }
        
        item->valid = true;
        mqttQueueHead = nextHead;
        
        xSemaphoreGive(queueMutex);
    }
}

//==============================================================================
// WiFi 관리
//==============================================================================

void manageWiFi()
{
    static unsigned long disconnectTime = 0;
    
    if (WiFi.status() == WL_CONNECTED) 
    {
        if (!wifiConnected) 
        {
            wifiConnected = true;
            disconnectTime = 0;
            DBG_PRINTF("[WiFi] Reconnected! IP: %s\n", 
                       WiFi.localIP().toString().c_str());
        }
    } 
    else 
    {
        if (wifiConnected) 
        {
            wifiConnected = false;
            mqttConnected = false;
            disconnectTime = millis();
            DBG_PRINTLN("[WiFi] Disconnected!");
        }
        
        // 2분 동안 연결 안되면 재부팅
        if (disconnectTime > 0 && millis() - disconnectTime > 120000) 
        {
            DBG_PRINTLN("[WiFi] Lost for 2min, restarting...");
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
        
        // 5초마다 재연결 시도
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
    DBG_PRINTF("[MQTT] Connecting to %s:%s... ", mqtt_server, mqtt_port);
    
    bool connected;
    if (strlen(mqtt_user) > 0) 
    {
        connected = mqttClient.connect(mqtt_client_id, mqtt_user, mqtt_pass);
    }
    else 
    {
        connected = mqttClient.connect(mqtt_client_id);
    }
    
    if (connected) 
    {
        DBG_PRINTLN("OK!");
        mqttConnected = true;
        mqttClient.subscribe(MQTT_TOPIC_CMD);
        publishStatus();
    } 
    else 
    {
        DBG_PRINTF("FAILED (rc=%d)\n", mqttClient.state());
    }
}

//==============================================================================
// MQTT 콜백
//==============================================================================

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
    DBG_PRINTF("[MQTT] Msg on %s: ", topic);
    
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
            
            if (strcmp(cmd, "status") == 0) 
            {
                publishStatus();
            }
            else if (strcmp(cmd, "reset_count") == 0) 
            {
                packetCount = mqttSentCount = mqttFailCount = ackSentCount = 0;
                DBG_PRINTLN("[CMD] Counters reset");
            }
            else if (strcmp(cmd, "led_on") == 0) 
            {
                digitalWrite(LED_PIN, HIGH);
            }
            else if (strcmp(cmd, "led_off") == 0) 
            {
                digitalWrite(LED_PIN, LOW);
            }
            else if (strcmp(cmd, "wifi_reset") == 0) 
            {
                resetWiFiSettings();
            }
            else if (strcmp(cmd, "reboot") == 0) 
            {
                DBG_PRINTLN("[CMD] Reboot requested");
                delay(500);
                ESP.restart();
            }
        }
    }
}

//==============================================================================
// MQTT 큐 크기 조회
//==============================================================================

int getMqttQueueSize()
{
    int size = 0;
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) 
    {
        for (int i = 0; i < MQTT_QUEUE_SIZE; i++) 
        {
            if (mqttQueue[i].valid) size++;
        }
        xSemaphoreGive(queueMutex);
    }
    return size;
}

//==============================================================================
// MQTT 큐 처리
//==============================================================================

void processMqttQueue()
{
    if (!mqttConnected) return;
    
    if (xSemaphoreTake(queueMutex, pdMS_TO_TICKS(100)) == pdTRUE) 
    {
        if (!mqttQueue[mqttQueueTail].valid) 
        {
            xSemaphoreGive(queueMutex);
            return;
        }
        
        MqttQueueItem item = mqttQueue[mqttQueueTail];  // 복사
        mqttQueue[mqttQueueTail].valid = false;
        mqttQueueTail = (mqttQueueTail + 1) % MQTT_QUEUE_SIZE;
        
        xSemaphoreGive(queueMutex);
        
        // 뮤텍스 해제 후 MQTT 전송 (blocking 가능)
        StaticJsonDocument<512> doc;
        doc["device"] = mqtt_client_id;
        doc["timestamp"] = item.timestamp;
        doc["packet_id"] = packetCount;
        
        for (int i = 0; i < item.itemCount; i++) 
        {
            String valueKey = "v_" + String(item.itemIds[i]);
            String qualityKey = "q_" + String(item.itemIds[i]);
            
            doc[valueKey] = item.itemValues[i];
            doc[qualityKey] = item.itemQualities[i];
        }
        
        char jsonBuffer[512];
        serializeJson(doc, jsonBuffer);
        
        if (mqttClient.publish(MQTT_TOPIC_DATA, jsonBuffer)) 
        {
            mqttSentCount++;
        } 
        else 
        {
            mqttFailCount++;
            DBG_PRINTLN("[MQTT] Publish FAILED!");
        }
    }
}

//==============================================================================
// 상태 출력
//==============================================================================
void printStatus()
{
    DBG_PRINTLN("\n============ STATUS ============");
    DBG_PRINTF("Uptime: %lu sec\n", millis() / 1000);
    DBG_PRINTF("Free Heap: %d bytes\n", ESP.getFreeHeap());
    DBG_PRINTLN("--------------------------------");
    DBG_PRINTF("Packets RX: %lu\n", packetCount);
    DBG_PRINTF("ACK Sent:   %lu\n", ackSentCount);
    DBG_PRINTLN("--------------------------------");
    DBG_PRINTF("WiFi: %s", wifiConnected ? "OK" : "NO");
    if (wifiConnected) 
    {
        DBG_PRINTF(" (%s, %ddBm)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } 
    else 
    {
        DBG_PRINTLN();
    }
    DBG_PRINTF("MQTT: %s (%s:%s)\n", 
               mqttConnected ? "OK" : "NO", mqtt_server, mqtt_port);
    DBG_PRINTF("  Sent: %lu, Fail: %lu, Queue: %d\n", 
               mqttSentCount, mqttFailCount, getMqttQueueSize());
    DBG_PRINTLN("================================\n");
}

//==============================================================================
// 상태 발행
//==============================================================================

void publishStatus()
{
    if (!mqttConnected) return;
    
    StaticJsonDocument<384> doc;
    doc["device"] = mqtt_client_id;
    doc["uptime"] = millis() / 1000;
    doc["packets"] = packetCount;
    doc["ack_sent"] = ackSentCount;
    doc["mqtt_sent"] = mqttSentCount;
    doc["mqtt_fail"] = mqttFailCount;
    doc["queue_size"] = getMqttQueueSize();
    doc["rssi"] = WiFi.RSSI();
    doc["heap"] = ESP.getFreeHeap();
    doc["ip"] = WiFi.localIP().toString();
    doc["ssid"] = WiFi.SSID();
    doc["version"] = "v4";
    
    char buf[384];
    serializeJson(doc, buf);
    mqttClient.publish(MQTT_TOPIC_STATUS, buf);
}

/*
================================================================================
v4 변경 요약:
================================================================================

1. FreeRTOS 태스크 분리
   - UART 태스크: Core 1, 우선순위 3 (높음)
   - MQTT 태스크: Core 0, 우선순위 1 (낮음)
   - UART 처리가 MQTT에 의해 blocking되지 않음

2. ACK 우선 전송
   - 패킷 수신 완료 시 즉시 ACK 전송
   - 그 후에 패킷 처리 및 큐 추가

3. MQTT 소켓 타임아웃 단축
   - 기본 15초 → 2초
   - 연결 실패 시 빠르게 복구

4. 뮤텍스로 큐 보호
   - 멀티태스크 환경에서 안전한 큐 접근

5. 통계 추가
   - ACK 전송 횟수 카운트

================================================================================
*/
