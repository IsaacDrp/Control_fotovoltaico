/*
 * MONITOR BMS DUAL - VERSIÓN "REENSAMBLAJE DE PAQUETES"
 * Soluciona: Lecturas parciales, SOC 0 y Corriente 0.
 */

#include <WiFi.h>
#include <WebServer.h> 
#include <ArduinoJson.h>
#include "BLEDevice.h"

// ===========================================
// --- CONFIGURACION ---
// ===========================================
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

static BLEUUID SERVICE_UUID("0000ff00-0000-1000-8000-00805f9b34fb");
static BLEUUID READ_UUID("0000ff01-0000-1000-8000-00805f9b34fb");
static BLEUUID WRITE_UUID("0000ff02-0000-1000-8000-00805f9b34fb");
static BLEUUID CCCD_UUID((uint16_t)0x2902); 

static uint8_t COMMAND_BASIC[] = {0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77};
static uint8_t COMMAND_CELLS[] = {0xDD, 0xA5, 0x04, 0x00, 0xFF, 0xFC, 0x77};

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
  bool valid_data = false; 
};

BMSData bmsData[2];
WebServer server(80);
BLEClient* pClient = nullptr; 

// Buffers Globales
static uint8_t g_packet_buffer[60]; // Buffer grande para unir los pedazos
static int g_buffer_index = 0;
static bool g_data_received = false;
static int g_current_bms_index = 0;
static uint8_t g_current_cmd = 0;

// ==========================================================
// --- PARSER ---
// ==========================================================
void parsePacket(int bmsIndex, uint8_t* data, size_t length) {
    BMSData &bms = bmsData[bmsIndex];
    
    // --- CELDAS (0x04) ---
    if (g_current_cmd == 0x04 && data[1] == 0x04) {
        int data_len = data[3];
        bms.cell_count = data_len / 2;
        float total = 0;
        for(int i = 0; i < bms.cell_count && i < 4; i++) {
            int offset = 4 + (i * 2);
            uint16_t val = (data[offset] << 8) | data[offset + 1];
            bms.cell_voltages[i] = (float)val / 1000.0;
            total += bms.cell_voltages[i];
        }
        bms.voltage = total; 
    } 
    // --- INFO BÁSICA (0x03) ---
    else if (g_current_cmd == 0x03 && data[1] == 0x03) {
        // Aunque el paquete llegue partido, aquí ya lo tenemos unido
        if (length < 24) return; 

        // Voltaje
        float v = (float)((uint16_t)((data[4] << 8) | data[5])) / 100.0;
        if (bms.voltage == 0) bms.voltage = v; // Usar este si no hay lectura de celdas

        // Corriente (Signed 16-bit)
        bms.current = (float)((int16_t)((data[6] << 8) | data[7])) / 100.0;
        
        // Capacidad
        float cap_rem = (float)((uint16_t)((data[8] << 8) | data[9])) / 100.0;
        float cap_nom = (float)((uint16_t)((data[10] << 8) | data[11])) / 100.0;
        
        // SOC: Intentamos leer byte 23
        int soc = 0;
        if (length > 23) soc = (int)data[23];
        
        // PLAN B: Si SOC es 0 o inválido, calcular por voltaje (LiFePO4 aprox)
        if (soc <= 0 || soc > 100) {
            if (cap_nom > 0) {
                soc = (int)((cap_rem / cap_nom) * 100.0);
            } else {
                // Estimación cruda por voltaje (4 celdas)
                if (bms.voltage > 13.6) soc = 100;
                else if (bms.voltage > 13.4) soc = 90;
                else if (bms.voltage > 13.3) soc = 70;
                else if (bms.voltage > 13.2) soc = 40;
                else soc = 20;
            }
        }
        bms.soc = soc;

        // Estado (Umbral bajado a 0.15A para detectar cargas pequeñas)
        if (bms.current < -0.15) bms.status = "Descargando";
        else if (bms.current > 0.15) bms.status = "Cargando";
        else bms.status = "Reposo";

        bms.valid_data = true;
    }
}

// CALLBACK INTELIGENTE: Pega los pedazos
static void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  for (int i = 0; i < length; i++) {
    uint8_t b = pData[i];
    
    // Si encontramos el inicio de trama, reseteamos el índice
    if (b == 0xDD) {
      g_buffer_index = 0; 
      g_packet_buffer[g_buffer_index++] = b;
    } 
    // Si no es inicio, seguimos llenando el buffer
    else if (g_buffer_index > 0 && g_buffer_index < 59) {
      g_packet_buffer[g_buffer_index++] = b;
    }

    // Si encontramos el final (0x77) Y tenemos una longitud decente, procesamos
    if (b == 0x77 && g_buffer_index > 3) {
      parsePacket(g_current_bms_index, g_packet_buffer, g_buffer_index);
      g_data_received = true; 
      // No reseteamos g_buffer_index aquí, esperamos al próximo DD
    }
  }
}

// ==========================================================
// --- LECTURA ---
// ==========================================================

bool readBMS(int index) {
    g_current_bms_index = index;
    BMSData &bms = bmsData[index];
    bms.valid_data = false;
    
    // Creamos cliente nuevo
    if (pClient == nullptr) pClient = BLEDevice::createClient();

    if (!pClient->connect(BLEAddress(bms.mac.c_str()))) {
        bms.isConnected = false;
        bms.status = "Error Conexión";
        return false;
    }

    // Solicitar MTU mayor (Aunque el BMS lo ignore, ayuda al stack del ESP32)
    pClient->setMTU(256);

    BLERemoteService* pService = pClient->getService(SERVICE_UUID);
    if (pService == nullptr) { pClient->disconnect(); return false; }
    
    BLERemoteCharacteristic* pWrite = pService->getCharacteristic(WRITE_UUID);
    BLERemoteCharacteristic* pRead = pService->getCharacteristic(READ_UUID);
    if (!pWrite || !pRead) { pClient->disconnect(); return false; }

    if (pRead->canNotify()) {
        pRead->registerForNotify(notifyCallback);
        delay(50);
        BLERemoteDescriptor* pCCCD = pRead->getDescriptor(CCCD_UUID);
        if(pCCCD) {
             uint8_t val[] = {0x01, 0x00};
             pCCCD->writeValue(val, 2, true);
        }
    }
    delay(100); 

    // --- LECTURA ROBUSTA (Timeout largo) ---
    // Intentamos 2 veces leer Info Básica
    for(int k=0; k<2; k++) {
        g_current_cmd = 0x03;
        g_data_received = false;
        pWrite->writeValue(COMMAND_BASIC, sizeof(COMMAND_BASIC), false);
        
        // Esperamos HASTA 2.5 SEGUNDOS para que lleguen todos los pedazos
        unsigned long start = millis();
        while (!g_data_received && millis() - start < 2500) { delay(10); }
        
        if (g_data_received) break; // Ya tenemos datos, salir del loop
    }

    // Celdas (Es rápido, 1 seg basta)
    g_current_cmd = 0x04;
    g_data_received = false;
    pWrite->writeValue(COMMAND_CELLS, sizeof(COMMAND_CELLS), false);
    unsigned long start = millis();
    while (!g_data_received && millis() - start < 1500) { delay(10); }

    pClient->disconnect();
    bms.isConnected = true;
    return true;
}

void performScan() {
    readBMS(0); 
    delay(200); 
    readBMS(1); 
    // Limpieza de memoria agresiva
    if (pClient != nullptr) {
        delete pClient;
        pClient = nullptr;
    }
}

// ==========================================================
// --- ENDPOINTS ---
// ==========================================================

void handleStatus() {
    performScan(); 

    StaticJsonDocument<2048> doc;
    float total_power = 0;
    JsonArray batteries = doc.createNestedArray("batteries");

    for(int i=0; i<2; i++) {
        JsonObject bms = batteries.createNestedObject();
        bms["name"] = bmsData[i].name;
        bms["connected"] = bmsData[i].isConnected;
        
        if (bmsData[i].isConnected) {
            bms["status"] = bmsData[i].status;
            bms["voltage"] = bmsData[i].voltage;
            bms["current"] = bmsData[i].current;
            bms["soc"] = bmsData[i].soc;
            float p = bmsData[i].voltage * bmsData[i].current;
            bms["power"] = p;
            total_power += p;
            
            JsonArray cells = bms.createNestedArray("cells");
            for(int j=0; j<4; j++) cells.add(bmsData[i].cell_voltages[j]);
        }
    }

    doc["summary"]["total_power"] = total_power;
    bool is_on = (digitalRead(PIN_RELEVADOR) == INVERSOR_ON_STATE);
    doc["inverter"]["on"] = is_on;
    
    String output;
    serializeJson(doc, output);
    server.send(200, "application/json", output);
}

void handleOn() {
    digitalWrite(PIN_RELEVADOR, INVERSOR_ON_STATE);
    server.send(200, "text/plain", "OK: ON");
}

void handleOff() {
    digitalWrite(PIN_RELEVADOR, INVERSOR_OFF_STATE);
    server.send(200, "text/plain", "OK: OFF");
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_RELEVADOR, OUTPUT);
    digitalWrite(PIN_RELEVADOR, INVERSOR_ON_STATE);

    bmsData[0].name = "Bat 1"; bmsData[0].mac = BMS_MAC_1;
    bmsData[1].name = "Bat 2"; bmsData[1].mac = BMS_MAC_2;

    WiFi.mode(WIFI_STA);
    WiFi.config(staticIP, gateway, subnet, primaryDNS);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) delay(500);
    
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/on", HTTP_GET, handleOn);
    server.on("/off", HTTP_GET, handleOff);
    server.begin();

    BLEDevice::init("ESP32_Gateway");
}

void loop() {
    server.handleClient();
}