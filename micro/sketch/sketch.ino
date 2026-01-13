#include <WiFi.h>
#include <WebServer.h> 
#include <ArduinoJson.h>
#include "BLEDevice.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// --- LIBRERÍA PARA EVITAR REINICIOS POR VOLTAJE ---
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ... (El resto de tus configuraciones de IP/MAC siguen igual) ...
const char* ssid = "INFINITUM5F53";
const char* password = "aPduu6aAQq";

IPAddress staticIP(192, 168, 1, 98);   
IPAddress gateway(192, 168, 1, 254);  
IPAddress subnet(255, 255, 255, 0);   
IPAddress primaryDNS(192, 168, 1, 254);

#define BMS_MAC_1 "A5:C2:37:28:8C:99"
#define BMS_MAC_2 "A5:C2:37:28:81:78"

const int PIN_RELEVADOR = 33; 
const int INVERSOR_ON_STATE = LOW;
const int INVERSOR_OFF_STATE = HIGH;

// Estructuras y Globales (IGUAL QUE ANTES)
struct BMSData {
  String name;
  String mac;
  bool isConnected = false;
  String status = "Desconocido";
  float voltage = 0.0; 
  int cell_count = 0;
  float cell_voltages[4] = {0.0, 0.0, 0.0, 0.0}; 
  float current = 0.0;
  int soc = 0;
  unsigned long last_update_ms = 0; 
};

BMSData bmsDataShared[2]; 
SemaphoreHandle_t dataMutex; 
WebServer server(80);

// UUIDs (IGUAL QUE ANTES)
static BLEUUID SERVICE_UUID("0000ff00-0000-1000-8000-00805f9b34fb");
static BLEUUID READ_UUID("0000ff01-0000-1000-8000-00805f9b34fb");
static BLEUUID WRITE_UUID("0000ff02-0000-1000-8000-00805f9b34fb");
static uint8_t COMMAND_BASIC[] = {0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77};
static uint8_t COMMAND_CELLS[] = {0xDD, 0xA5, 0x04, 0x00, 0xFF, 0xFC, 0x77};

// --- LOGICA BLE (Copia exacta de la versión anterior "ANTI-CRASH") ---
BLEClient* pClient = nullptr;
BMSData tempBmsRead; 
volatile bool g_data_received = false;
uint8_t g_rx_buffer[256];
int g_rx_len = 0;
uint8_t g_current_cmd = 0;

static void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (g_rx_len + length > 255) g_rx_len = 0;
    memcpy(&g_rx_buffer[g_rx_len], pData, length);
    g_rx_len += length;
    if (g_rx_len > 5 && g_rx_buffer[g_rx_len-1] == 0x77) g_data_received = true;
}

void processBuffer() {
    if (g_rx_buffer[0] != 0xDD) {
        for(int i=0; i<g_rx_len; i++){
             if(g_rx_buffer[i] == 0xDD) {
                 int newLen = g_rx_len - i;
                 memmove(g_rx_buffer, &g_rx_buffer[i], newLen);
                 g_rx_len = newLen;
                 break;
             }
        }
    }
    if (g_rx_buffer[0] != 0xDD) return;
    uint8_t dataLen = g_rx_buffer[3];
    int expectedLen = 4 + dataLen + 3;
    if (g_rx_len < expectedLen) return;

    if (g_current_cmd == 0x03) {
        uint16_t v = (g_rx_buffer[4] << 8) | g_rx_buffer[5];
        if (tempBmsRead.voltage == 0) tempBmsRead.voltage = v / 100.0;
        int16_t c = (int16_t)((g_rx_buffer[6] << 8) | g_rx_buffer[7]);
        tempBmsRead.current = c / 100.0;
        uint16_t cap_full = (g_rx_buffer[10] << 8) | g_rx_buffer[11];
        uint16_t cap_now = (g_rx_buffer[8] << 8) | g_rx_buffer[9];
        int soc = 0;
        if (dataLen >= 19) soc = g_rx_buffer[23];
        if (soc <= 0 || soc > 100) {
            if (cap_full > 0) soc = (int)(((float)cap_now / (float)cap_full) * 100.0);
            else { 
                if (tempBmsRead.voltage > 13.4) soc = 100;
                else if (tempBmsRead.voltage > 12.0) soc = 50;
                else soc = 0;
            }
        }
        tempBmsRead.soc = soc;
        if (tempBmsRead.current < -0.2) tempBmsRead.status = "Descargando";
        else if (tempBmsRead.current > 0.2) tempBmsRead.status = "Cargando";
        else tempBmsRead.status = "Standby";

    } else if (g_current_cmd == 0x04) {
        tempBmsRead.cell_count = dataLen / 2;
        float total = 0;
        for(int k=0; k < tempBmsRead.cell_count && k < 4; k++) {
            int offset = 4 + (k*2);
            uint16_t val = (g_rx_buffer[offset] << 8) | g_rx_buffer[offset+1];
            tempBmsRead.cell_voltages[k] = (float)val / 1000.0;
            total += tempBmsRead.cell_voltages[k];
        }
        if (total > 0) tempBmsRead.voltage = total;
    }
}

void processBMS(int index, String macAddress) {
    tempBmsRead.name = (index == 0) ? "Bat 1" : "Bat 2";
    tempBmsRead.mac = macAddress;
    tempBmsRead.voltage = 0; 
    
    if (pClient->connect(BLEAddress(macAddress.c_str()))) {
        tempBmsRead.isConnected = true;
        pClient->setMTU(517); 
        vTaskDelay(100 / portTICK_PERIOD_MS);

        BLERemoteService* pService = pClient->getService(SERVICE_UUID);
        if (pService) {
            BLERemoteCharacteristic* pWrite = pService->getCharacteristic(WRITE_UUID);
            BLERemoteCharacteristic* pRead = pService->getCharacteristic(READ_UUID);
            if (pWrite && pRead && pRead->canNotify()) {
                pRead->registerForNotify(notifyCallback);
                vTaskDelay(100 / portTICK_PERIOD_MS);
                
                // CMD 0x03
                g_rx_len = 0; g_current_cmd = 0x03; g_data_received = false;
                pWrite->writeValue(COMMAND_BASIC, sizeof(COMMAND_BASIC), false);
                unsigned long t = millis();
                while(!g_data_received && millis() - t < 1000) { vTaskDelay(10 / portTICK_PERIOD_MS); }
                if (!g_data_received) {
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                    pWrite->writeValue(COMMAND_BASIC, sizeof(COMMAND_BASIC), false);
                    t = millis();
                    while(!g_data_received && millis() - t < 1000) { vTaskDelay(10 / portTICK_PERIOD_MS); }
                }
                if (g_data_received) processBuffer();

                // CMD 0x04
                g_rx_len = 0; g_current_cmd = 0x04; g_data_received = false;
                pWrite->writeValue(COMMAND_CELLS, sizeof(COMMAND_CELLS), false);
                t = millis();
                while(!g_data_received && millis() - t < 1000) { vTaskDelay(10 / portTICK_PERIOD_MS); }
                if (g_data_received) processBuffer();
            }
        }
        pClient->disconnect();
    } else {
        tempBmsRead.isConnected = false;
        tempBmsRead.status = "Desconectado";
    }

    tempBmsRead.last_update_ms = millis();
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    bmsDataShared[index] = tempBmsRead;
    xSemaphoreGive(dataMutex);
    vTaskDelay(250 / portTICK_PERIOD_MS);
}

void taskBMSReader(void *pvParameters) {
    pClient = BLEDevice::createClient();
    while(1) {
        processBMS(0, BMS_MAC_1);
        processBMS(1, BMS_MAC_2);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

// --- LOGICA WEB (IGUAL QUE ANTES) ---
void handleStatus() {
    StaticJsonDocument<2500> doc;
    float total_power = 0;
    JsonArray batteries = doc.createNestedArray("batteries");

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    for(int i=0; i<2; i++) {
        JsonObject bms = batteries.createNestedObject();
        bms["name"] = bmsDataShared[i].name;
        bms["mac"] = bmsDataShared[i].mac;
        bms["connected"] = bmsDataShared[i].isConnected;
        bms["status"] = bmsDataShared[i].status;
        bms["voltage"] = bmsDataShared[i].voltage;
        bms["current"] = bmsDataShared[i].current;
        bms["soc"] = bmsDataShared[i].soc;
        bms["age_ms"] = millis() - bmsDataShared[i].last_update_ms;
        float p = bmsDataShared[i].voltage * bmsDataShared[i].current;
        bms["power"] = p;
        total_power += p;
        JsonArray cells = bms.createNestedArray("cells");
        for(int j=0; j<4; j++) cells.add(bmsDataShared[i].cell_voltages[j]);
    }
    xSemaphoreGive(dataMutex);

    doc["summary"]["total_power"] = total_power;
    doc["summary"]["uptime"] = millis();
    doc["inverter"]["on"] = (digitalRead(PIN_RELEVADOR) == INVERSOR_ON_STATE);
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
}
void handleOn() { digitalWrite(PIN_RELEVADOR, INVERSOR_ON_STATE); server.send(200, "application/json", "{\"inverter\": true}"); }
void handleOff() { digitalWrite(PIN_RELEVADOR, INVERSOR_OFF_STATE); server.send(200, "application/json", "{\"inverter\": false}"); }

// --- SETUP MODIFICADO (SOLUCIÓN POWERON_RESET) ---
void setup() {
    // 1. DESACTIVAR BROWNOUT DETECTOR (Truco de Software)
    // Esto evita que el ESP32 se reinicie si el voltaje baja momentáneamente
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
    
    Serial.begin(115200);
    
    // 2. RETRASAR ENCENDIDO DEL RELÉ
    // Inicializamos APAGADO para no consumir corriente en el arranque WiFi
    pinMode(PIN_RELEVADOR, OUTPUT);
    digitalWrite(PIN_RELEVADOR, INVERSOR_OFF_STATE); 

    dataMutex = xSemaphoreCreateMutex();
    bmsDataShared[0].name = "Bat 1"; bmsDataShared[1].name = "Bat 2";

    WiFi.mode(WIFI_STA);
    WiFi.config(staticIP, gateway, subnet, primaryDNS);
    WiFi.begin(ssid, password);
    
    // Esperamos WiFi SIN encender el relé aún
    Serial.print("Conectando WiFi");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nWiFi OK.");

    // 3. AHORA QUE EL WIFI ESTÁ ESTABLE, ENCENDEMOS EL RELÉ (Si es necesario)
    // Pequeña pausa para asegurar que el pico del WiFi pasó
    delay(500); 
    digitalWrite(PIN_RELEVADOR, INVERSOR_ON_STATE); 
    Serial.println("Relé activado.");

    BLEDevice::init("ESP32_Slave");
    xTaskCreatePinnedToCore(taskBMSReader, "BMS_Task", 20000, NULL, 1, NULL, 1);

    server.on("/status", HTTP_GET, handleStatus);
    server.on("/on", HTTP_GET, handleOn);   
    server.on("/on", HTTP_POST, handleOn);
    server.on("/off", HTTP_GET, handleOff);
    server.on("/off", HTTP_POST, handleOff);
    server.begin();
}

void loop() {
    server.handleClient();
}