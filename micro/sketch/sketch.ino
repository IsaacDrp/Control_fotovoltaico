/*
 * MONITOR BMS DUAL + CONTROL INVERSOR
 * MODELO: ON-DEMAND ESTABLE (Sin reinicio de stack BLE)
 */

#include <WiFi.h>
#include <WebServer.h> 
#include <ArduinoJson.h>
#include "BLEDevice.h"

// ===========================================
// --- CONFIGURACION DE USUARIO ---
// ===========================================
const char* ssid = "INFINITUM5F53";
const char* password = "aPduu6aAQq";
IPAddress staticIP(192, 168, 1, 98);   
IPAddress gateway(192, 168, 1, 254);  
IPAddress subnet(255, 255, 255, 0);   
IPAddress primaryDNS(192, 168, 1, 254);

#define BMS_MAC_1 "A5:C2:37:28:8C:99" // Bateria 01
#define BMS_MAC_2 "A5:C2:37:28:81:78" // Bateria 02

const int PIN_RELEVADOR = 33; 
const int INVERSOR_ON_STATE = LOW;
const int INVERSOR_OFF_STATE = HIGH;

// --- UUIDs BMS ---
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
  String status = "Desconocido"; // Estado inicial neutro
  float voltage = 0.0; 
  int cell_count = 0;
  float cell_voltages[4] = {0.0, 0.0, 0.0, 0.0}; 
  float current = 0.0;
  int soc = 0;
  float capacity_remaining = 0.0;
  float capacity_nominal = 0.0;
  int cycles = 0;
};

BMSData bmsData[2];
WebServer server(80);
BLEClient* pClient = nullptr; 

// Flags de control
bool isScanning = false; 
static uint8_t g_packet_buffer[50];
static int g_buffer_index = 0;
static bool g_data_received = false;
static int g_current_bms_index = 0;
static uint8_t g_current_cmd = 0;

// ==========================================================
// --- PARSER ---
// ==========================================================
void parsePacket(int bmsIndex, uint8_t* data, size_t length) {
    BMSData &bms = bmsData[bmsIndex];
    
    // --- CASO 1: CELDAS (0x04) ---
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
        bms.voltage = total; // Sumatoria precisa de celdas
    } 
    // --- CASO 2: INFO BÁSICA (0x03) - CORREGIDO CON TU HEX ---
    else if (g_current_cmd == 0x03 && data[1] == 0x03 && length >= 24) {
        
        // 1. Voltaje Total (Bytes 4 y 5)
        // Hex: 05 2E -> 13.26V
        bms.voltage = (float)((uint16_t)((data[4] << 8) | data[5])) / 100.0;
        
        // 2. Corriente (Bytes 6 y 7) - IMPORTANTE: int16_t para negativos
        // Hex: FD 92 -> -6.22A
        bms.current = (float)((int16_t)((data[6] << 8) | data[7])) / 100.0;
        
        // 3. Capacidad Restante (Bytes 8 y 9)
        // Hex: 21 40 -> 85.12Ah
        bms.capacity_remaining = (float)((uint16_t)((data[8] << 8) | data[9])) / 100.0;
        
        // 4. Capacidad Nominal (Bytes 10 y 11)
        // Hex: 27 10 -> 100.00Ah
        bms.capacity_nominal = (float)((uint16_t)((data[10] << 8) | data[11])) / 100.0;
        
        // 5. Ciclos (Bytes 12 y 13)
        // Hex: 00 DB -> 219 ciclos
        bms.cycles = (int)((uint16_t)((data[12] << 8) | data[13]));

        // 6. SOC (Porcentaje)
        // Normalmente Byte 23 (índice 23).
        // Calculamos un fallback por si el byte viene en 0
        int calculated_soc = 0;
        if (bms.capacity_nominal > 0) {
            calculated_soc = (int)((bms.capacity_remaining / bms.capacity_nominal) * 100.0);
        }

        // Si el byte 23 parece válido, úsalo. Si no, usa el calculado.
        if (data[23] > 0 && data[23] <= 100) {
            bms.soc = (int)data[23];
        } else {
            bms.soc = calculated_soc;
        }
        
        // 7. Estado Lógico
        // -0.2A a 0.2A lo consideramos "Reposo" para evitar ruido
        if (bms.current < -0.2) bms.status = "Descargando";
        else if (bms.current > 0.2) bms.status = "Cargando";
        else bms.status = "Reposo";
        
        Serial.printf("[DEBUG] %s -> V: %.2f, A: %.2f, SOC: %d%%\n", 
                      bms.name.c_str(), bms.voltage, bms.current, bms.soc);
    }
}

static void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  for (int i = 0; i < length; i++) {
    uint8_t b = pData[i];
    if (b == 0xDD) {
      g_buffer_index = 0;
      g_packet_buffer[g_buffer_index++] = b;
    } 
    else if (g_buffer_index > 0 && g_buffer_index < 50) {
      g_packet_buffer[g_buffer_index++] = b;
    }
    if (b == 0x77 && g_buffer_index > 0) {
      parsePacket(g_current_bms_index, g_packet_buffer, g_buffer_index);
      g_data_received = true; 
      g_buffer_index = 0; 
    }
  }
}

// ==========================================================
// --- LECTURA ON-DEMAND ROBUSTA ---
// ==========================================================

bool readBMS(int index) {
    g_current_bms_index = index;
    BMSData &bms = bmsData[index];
    BLEAddress bmsAddr(bms.mac.c_str());
    
    Serial.printf(">> Conectando a %s...\n", bms.name.c_str());

    // Reusamos el cliente si existe, si no, creamos uno
    if (pClient == nullptr) {
        pClient = BLEDevice::createClient();
    }

    // Intento de conexión con timeout implícito del stack
    if (!pClient->connect(bmsAddr)) {
        Serial.println("   Fallo conexión BLE (Busy/Timeout)");
        bms.isConnected = false;
        bms.status = "Error Conexión";
        return false;
    }

    // Obtener servicio
    BLERemoteService* pService = pClient->getService(SERVICE_UUID);
    if (pService == nullptr) { pClient->disconnect(); return false; }
    
    BLERemoteCharacteristic* pWrite = pService->getCharacteristic(WRITE_UUID);
    BLERemoteCharacteristic* pRead = pService->getCharacteristic(READ_UUID);
    if (!pWrite || !pRead) { pClient->disconnect(); return false; }

    // Suscribirse
    if (pRead->canNotify()) {
        pRead->registerForNotify(notifyCallback);
        BLERemoteDescriptor* pCCCD = pRead->getDescriptor(CCCD_UUID);
        if(pCCCD) {
             uint8_t val[] = {0x01, 0x00};
             pCCCD->writeValue(val, 2, true);
        }
    }
    delay(50); 

    // --- LEER DATOS BASICOS ---
    g_current_cmd = 0x03;
    g_data_received = false;
    pWrite->writeValue(COMMAND_BASIC, sizeof(COMMAND_BASIC), false);
    
    // Espera activa con yield() para no colgar el Watchdog
    unsigned long start = millis();
    while (!g_data_received && millis() - start < 1000) { 
        delay(5); // Importante para estabilidad
    }

    // --- LEER CELDAS ---
    g_current_cmd = 0x04;
    g_data_received = false;
    pWrite->writeValue(COMMAND_CELLS, sizeof(COMMAND_CELLS), false);
    
    start = millis();
    while (!g_data_received && millis() - start < 1000) { 
        delay(5); 
    }

    // Desconexión limpia
    pClient->disconnect();
    bms.isConnected = true;
    return true;
}

void performScan() {
    if (isScanning) return; // Evitar colisión si dos usuarios entran a la vez
    isScanning = true;
    
    // Leemos secuencialmente
    readBMS(0); 
    delay(100); // Respiro para el stack BLE
    readBMS(1); 
    
    isScanning = false;
}

// ==========================================================
// --- ENDPOINTS ---
// ==========================================================

void handleStatus() {
    // Si ya está escaneando, rechazamos para no colgar
    if (isScanning) {
        server.send(503, "text/plain", "Sistema ocupado leyendo baterias... reintente en 3s");
        return;
    }

    // Ejecutamos lectura (Esto tardará unos 3-4 segundos)
    performScan();

    StaticJsonDocument<2048> doc;
    
    // Parche corriente sistema
    if (bmsData[1].isConnected) {
        bmsData[0].current = bmsData[1].current;
    }

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
            bms["power"] = bmsData[i].voltage * bmsData[i].current;
            total_power += (bmsData[i].voltage * bmsData[i].current);
            
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
    digitalWrite(PIN_RELEVADOR, INVERSOR_ON_STATE); // Inicializar ENCENDIDO

    bmsData[0].name = "Bat 1"; bmsData[0].mac = BMS_MAC_1;
    bmsData[1].name = "Bat 2"; bmsData[1].mac = BMS_MAC_2;

    WiFi.mode(WIFI_STA);
    WiFi.config(staticIP, gateway, subnet, primaryDNS);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) delay(500);
    
    Serial.println("WiFi OK.");
    
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/on", HTTP_GET, handleOn);
    server.on("/off", HTTP_GET, handleOff);
    server.begin();

    // INICIALIZACION UNICA DEL BLE (Vital para que no se cuelgue)
    BLEDevice::init("ESP32_BMS_Gateway");
    Serial.println("BLE Iniciado (Modo Espera)");
}

void loop() {
    server.handleClient();
    delay(2); // Yield para Watchdog
}