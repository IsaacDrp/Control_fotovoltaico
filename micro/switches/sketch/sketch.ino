/*
 * MONITOR DE LUCES - VERSIÓN SEGURA (WHITELIST)
 * Restricción de acceso solo para Proxy Inverso (.11)
 */

#include <WiFi.h>
#include <WebServer.h> 
#include <ArduinoJson.h>
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// --- CONFIGURACIÓN DE SEGURIDAD ---
// Solo el Proxy Inverso / DNS tiene permiso para controlar los focos
const IPAddress trustedProxy(192, 168, 1, 11); 

// --- DEFINICIÓN DE LÓGICA NEGATIVA ---
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// ===========================================
// --- CONFIGURACIÓN DE RED ---
// ===========================================
const char* ssid = "INFINITUM5F53";
const char* password = "aPduu6aAQq";

IPAddress staticIP(192, 168, 1, 59);
IPAddress gateway(192, 168, 1, 254);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 1, 11); // Apuntando a tu propio DNS

// ===========================================
// --- ESTRUCTURAS Y GLOBALES ---
// ===========================================
struct Light {
  const char* name;
  const uint8_t inputPin;
  const uint8_t outputPin;
  bool state;
  int lastPhysicalPos;
};

Light lights[] = {
  {"cuarto", 17, 14, false, HIGH},
  {"sala", 15, 27, false, HIGH},
  {"zotehuela", 16, 26, false, HIGH},
  {"comedor_2",     5, 33, false, HIGH},
  {"comedor_1",      18, 32, false, HIGH}  
};

const int NUM_LIGHTS = sizeof(lights) / sizeof(lights[0]);

WebServer server(80);
Preferences preferences;
const char* PREF_NAMESPACE = "lights_state";
SemaphoreHandle_t nvsMutex;

// ===========================================
// --- SISTEMA DE LOGGING ---
// ===========================================
void log(String msg) {
  Serial.printf("[%lu] [LOG] %s\n", millis() / 1000, msg.c_str());
}

// ===========================================
// --- FILTRO DE SEGURIDAD ---
// ===========================================
bool isAuthorized() {
    IPAddress clientIP = server.client().remoteIP();
    if (clientIP == trustedProxy) {
        return true;
    }
    // Loguear intento fallido para auditoría en el bunker
    log("¡BLOQUEADO! Intento de acceso desde IP no autorizada: " + clientIP.toString());
    server.send(403, "text/plain", "Forbidden: Acceso restringido al Proxy Inverso");
    return false;
}

// ===========================================
// --- LÓGICA DE CONTROL ---
// ===========================================

void saveLightState(int index) {
  xSemaphoreTake(nvsMutex, portMAX_DELAY);
  preferences.begin(PREF_NAMESPACE, false);
  preferences.putBool(lights[index].name, lights[index].state);
  preferences.end();
  xSemaphoreGive(nvsMutex);
}

void setLightState(int index, bool newState, String source) {
  if (index >= 0 && index < NUM_LIGHTS) {
    lights[index].state = newState;
    digitalWrite(lights[index].outputPin, newState ? RELAY_ON : RELAY_OFF);
    saveLightState(index);
    log("Foco '" + String(lights[index].name) + "' -> " + (newState ? "ON" : "OFF") + " (Origen: " + source + ")");
  }
}

int findLight(String name) {
  for (int i = 0; i < NUM_LIGHTS; i++) {
    if (name.equalsIgnoreCase(lights[i].name)) return i;
  }
  return -1;
}

// ===========================================
// --- HANDLERS DEL SERVIDOR ---
// ===========================================

void handleInfo() {
  if (!isAuthorized()) return; // Capa de seguridad

  StaticJsonDocument<1024> doc;
  doc["status"] = "ok";
  doc["uptime_sec"] = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();

  JsonArray array = doc.createNestedArray("focos");
  for (int i = 0; i < NUM_LIGHTS; i++) {
    JsonObject obj = array.createNestedObject();
    obj["nombre"] = lights[i].name;
    obj["estado"] = lights[i].state ? "on" : "off";
  }

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void handleOn() {
  if (!isAuthorized()) return; // Capa de seguridad

  if (server.hasArg("foco")) {
    String name = server.arg("foco");
    int idx = findLight(name);
    if (idx != -1) {
      setLightState(idx, true, "HTTP_API");
      server.send(200, "application/json", "{\"status\":\"on\",\"foco\":\"" + name + "\"}");
    } else {
      server.send(404, "text/plain", "Foco no encontrado");
    }
  }
}

void handleOff() {
  if (!isAuthorized()) return; // Capa de seguridad

  if (server.hasArg("foco")) {
    String name = server.arg("foco");
    int idx = findLight(name);
    if (idx != -1) {
      setLightState(idx, false, "HTTP_API");
      server.send(200, "application/json", "{\"status\":\"off\",\"foco\":\"" + name + "\"}");
    }
  }
}

// ===========================================
// --- TASK: SERVIDOR WEB (CORE 0) ---
// ===========================================
void taskWebServer(void *pvParameters) {
  log("Servidor Web seguro iniciado en Core 0.");
  server.on("/info", HTTP_GET, handleInfo);
  server.on("/on", HTTP_GET, handleOn);
  server.on("/off", HTTP_GET, handleOff);
  server.begin();

  while (true) {
    server.handleClient();
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

// ===========================================
// --- SETUP ---
// ===========================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
  Serial.begin(115200);
  delay(1000);
  log("SISTEMA INICIANDO...");

  nvsMutex = xSemaphoreCreateMutex();

  // 1. Configurar Pines y NVS
  xSemaphoreTake(nvsMutex, portMAX_DELAY);
  preferences.begin(PREF_NAMESPACE, true);
  for (int i = 0; i < NUM_LIGHTS; i++) {
    lights[i].state = preferences.getBool(lights[i].name, false);
    pinMode(lights[i].inputPin, INPUT_PULLUP);
    pinMode(lights[i].outputPin, OUTPUT);
    digitalWrite(lights[i].outputPin, lights[i].state ? RELAY_ON : RELAY_OFF);
    lights[i].lastPhysicalPos = digitalRead(lights[i].inputPin);
  }
  preferences.end();
  xSemaphoreGive(nvsMutex);

  // 2. WiFi
  WiFi.mode(WIFI_STA);
  WiFi.config(staticIP, gateway, subnet, primaryDNS);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  log("\nWiFi Conectado. IP: " + WiFi.localIP().toString());

  // 3. Lanzar Tarea
  xTaskCreatePinnedToCore(taskWebServer, "WebTask", 8192, NULL, 1, NULL, 0);
}

// ===========================================
// --- LOOP PRINCIPAL (CORE 1) ---
// ===========================================
void loop() {
  for (int i = 0; i < NUM_LIGHTS; i++) {
    int currentPos = digitalRead(lights[i].inputPin);
    if (currentPos != lights[i].lastPhysicalPos) {
      delay(70); 
      if (digitalRead(lights[i].inputPin) == currentPos) {
        setLightState(i, !lights[i].state, "SWITCH_FISICO");
        lights[i].lastPhysicalPos = currentPos;
      }
    }
  }
  vTaskDelay(20 / portTICK_PERIOD_MS); 
}