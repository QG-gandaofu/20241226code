#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <CircularBuffer.h>
#include <math.h>

// WiFi设置
const char* ssid = "HIKVISION_A22128";
const char* password = "00000000";

// 创建异步WebServer对象
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// 波形生成设置
const int PWM_PIN = 2;        // 输出引脚
const int PWM_FREQ = 5000;    // PWM频率
const int PWM_RESOLUTION = 8; // PWM分辨率
const int PWM_CHANNEL = 0;    // PWM通道

// 波形参数
enum WaveType {NONE, SINE, TRIANGLE, SQUARE};
volatile WaveType currentWave = NONE;
volatile float frequency = 1.0;  // 默认1Hz
volatile bool outputEnabled = false;

// 波形生成任务句柄
TaskHandle_t waveTaskHandle = NULL;

// 添加串口示波器相关变量
const unsigned long SERIAL_SEND_INTERVAL = 200;  // 串口数据发送间隔
unsigned long lastSerialSendTime = 0;

void generateWave(void * parameter) {
  const int SAMPLES = 256;  // 采样点数
  float phase = 0;
  float step;
  
  while(true) {
    if(outputEnabled) {
      step = 2 * PI * frequency / SAMPLES;
      
      switch(currentWave) {
        case SINE:
          for(int i = 0; i < SAMPLES && outputEnabled; i++) {
            int value = (sin(phase) + 1) * 127.5;
            ledcWrite(PWM_CHANNEL, value);
            phase += step;
            if(phase >= 2 * PI) phase -= 2 * PI;
            delayMicroseconds(1000000 / (frequency * SAMPLES));
          }
          break;
          
        case TRIANGLE:
          for(int i = 0; i < SAMPLES && outputEnabled; i++) {
            float normalized = phase / (2 * PI);
            int value;
            if(normalized < 0.5) {
              value = normalized * 2 * 255;
            } else {
              value = (2 - normalized * 2) * 255;
            }
            ledcWrite(PWM_CHANNEL, value);
            phase += step;
            if(phase >= 2 * PI) phase -= 2 * PI;
            delayMicroseconds(1000000 / (frequency * SAMPLES));
          }
          break;
          
        case SQUARE:
          for(int i = 0; i < SAMPLES && outputEnabled; i++) {
            int value = (phase < PI) ? 255 : 0;
            ledcWrite(PWM_CHANNEL, value);
            phase += step;
            if(phase >= 2 * PI) phase -= 2 * PI;
            delayMicroseconds(1000000 / (frequency * SAMPLES));
          }
          break;
          
        default:
          ledcWrite(PWM_CHANNEL, 0);
          delay(100);
      }
    } else {
      ledcWrite(PWM_CHANNEL, 0);
      delay(100);
    }
  }
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;
    String message = (char*)data;
    
    if(message.startsWith("wave=")) {
      String type = message.substring(5);
      outputEnabled = true;
      if(type == "sine") currentWave = SINE;
      else if(type == "triangle") currentWave = TRIANGLE;
      else if(type == "square") currentWave = SQUARE;
      else if(type == "stop") {
        outputEnabled = false;
        currentWave = NONE;
      }
    }
    else if(message.startsWith("freq=")) {
      frequency = message.substring(5).toFloat();
      if(frequency < 0.1) frequency = 0.1;
      if(frequency > 100) frequency = 100;
    }
  }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.println("WebSocket客户端连接");
      break;
    case WS_EVT_DISCONNECT:
      Serial.println("WebSocket客户端断开");
      break;
    case WS_EVT_DATA:
      handleWebSocketMessage(arg, data, len);
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32-C3 波形发生器启动 ===");

  // 设置PWM
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);

  if(!SPIFFS.begin(true)) {
    Serial.println("SPIFFS挂载失败");
    return;
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi已连接");
  Serial.print("IP地址: ");
  Serial.println(WiFi.localIP());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html");
  });

  server.begin();
  Serial.println("Web服务器已启动");

  // 创建波形生成任务
  xTaskCreatePinnedToCore(
    generateWave,    // 任务函数
    "WaveTask",      // 任务名称
    4096,           // 堆栈大小
    NULL,           // 任务参数
    1,              // 任务优先级
    &waveTaskHandle,// 任务句柄
    0              // 在核心0上运行
  );
}

void loop() {
  // 处理串口数据
  if (Serial.available()) {
    String data = Serial.readStringUntil('\n');
    data.trim();
    float value = data.toFloat();
    
    // 按照固定间隔发送数据到网页
    if (millis() - lastSerialSendTime >= SERIAL_SEND_INTERVAL) {
      lastSerialSendTime = millis();
      String message = "data=" + String(value, 2);  // 添加"data="前缀以区分波形控制命令
      ws.textAll(message);
    }
  }
  
  delay(10);
}