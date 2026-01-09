from fastapi import FastAPI, HTTPException, Header
from fastapi.middleware.cors import CORSMiddleware

import redis
import json
import os
import requests
from typing import Optional

app = FastAPI(title="Solar BMS API")

# Configurar CORS (Para que tu frontend Angular pueda llamar a esta API)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"], # En prod, pon aquí tu dominio de Synectura
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Configuración
REDIS_HOST = os.getenv("REDIS_HOST", "redis")
ESP32_BASE_URL = os.getenv("ESP32_IP", "http://192.168.1.98")
SECRET_TOKEN = os.getenv("API_SECRET_TOKEN", "123456")

# Conexión Redis (Lectura)
r = redis.Redis(host=REDIS_HOST, port=6379, decode_responses=True)

@app.get("/")
def read_root():
    return {"status": "Solar BMS Backend Running"}

@app.get("/api/dashboard")
def get_dashboard_data():
    """Devuelve el último estado conocido desde la RAM (Redis)"""
    raw_data = r.get("solar:live")
    last_update = r.get("solar:last_update")
    
    if not raw_data:
        raise HTTPException(status_code=503, detail="No hay datos del sistema solar aún")
    
    return {
        "data": json.loads(raw_data),
        "last_update": last_update,
        "source": "cache_hot"
    }

@app.post("/api/control/{action}")
def control_inverter(action: str, x_secret_token: Optional[str] = Header(None)):
    """
    Controla el inversor (Proxy hacia ESP32)
    Action: 'on' o 'off'
    """
    # 1. Seguridad
    if x_secret_token != SECRET_TOKEN:
        raise HTTPException(status_code=401, detail="Token de seguridad inválido")
    
    if action not in ["on", "off"]:
        raise HTTPException(status_code=400, detail="Acción inválida. Use 'on' o 'off'")

    # 2. Reintento simple (Por si el ESP32 está ocupado leyendo baterías)
    max_retries = 3
    for attempt in range(max_retries):
        try:
            # Llamamos al ESP32
            url = f"{ESP32_BASE_URL}/{action}"
            res = requests.get(url, timeout=3)
            
            if res.status_code == 200:
                # Forzamos una actualización inmediata en Redis del estado del inversor
                # Para que la UI reaccione rápido
                return {"status": "success", "message": f"Inversor {action.upper()}"}
            
        except requests.exceptions.RequestException:
            pass # Falla de red, reintentar
            
        import time
        time.sleep(1) # Esperar 1s antes de reintentar

    raise HTTPException(status_code=502, detail="El ESP32 no responde o está ocupado")