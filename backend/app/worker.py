import time
import requests
import json
import redis
import os
import schedule
from .database import SessionLocal, engine, Base
from .models import SolarMetric
from datetime import datetime

# Configuración
ESP32_URL = os.getenv("ESP32_IP", "http://192.168.1.98") + "/status"
REDIS_HOST = os.getenv("REDIS_HOST", "redis")

# Conexión Redis (Cache Caliente)
r = redis.Redis(host=REDIS_HOST, port=6379, decode_responses=True)

# Crear tablas en DB si no existen (Migración automática al inicio)
Base.metadata.create_all(bind=engine)

print("Worker Iniciado. Conectado a Redis y Postgres.")

def fetch_and_store():
    """Consulta al ESP32 y actualiza Redis"""
    try:
        # CAMBIO 1: Timeout de 3 segundos (Si tarda más, algo anda mal)
        response = requests.get(ESP32_URL, timeout=3.0)
        
        if response.status_code == 200:
            data = response.json()
            
            # 2. Guardar en REDIS
            r.set("solar:live", json.dumps(data))
            r.set("solar:last_update", str(datetime.now()))
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Redis Actualizado.")
            return data
        else:
            print(f"Advertencia: ESP32 respondió {response.status_code}")
            return None
            
    except requests.exceptions.Timeout:
        print("Timeout: El ESP32 tardó demasiado en responder.")
    except Exception as e:
        print(f"Error conectando al ESP32: {e}")
        return None

def save_history():
    """Toma lo último de Redis y lo guarda en Postgres"""
    raw_data = r.get("solar:live")
    if not raw_data:
        return

    data = json.loads(raw_data)
    
    # CAMBIO 2: Protección contra arrays vacíos
    batteries = data.get('batteries', [])
    if len(batteries) < 2:
        print(f"Error: Datos incompletos. Se esperaban 2 baterías, se encontraron {len(batteries)}")
        return

    try:
        db = SessionLocal()
        
        metric = SolarMetric(
            # Acceso seguro ahora que validamos el length
            bat1_voltage = batteries[0]['voltage'],
            bat1_current = batteries[0]['current'],
            bat1_soc     = batteries[0]['soc'],
            
            bat2_voltage = batteries[1]['voltage'],
            bat2_current = batteries[1]['current'],
            bat2_soc     = batteries[1]['soc'],
            
            total_power  = data['summary']['total_power'],
            inverter_status = data['inverter']['on']
        )
        
        db.add(metric)
        db.commit()
        db.close()
        print(f"[{datetime.now().strftime('%H:%M:%S')}] --> Postgres Guardado.")
        
    except Exception as e:
        print(f"Error guardando en DB: {e}")
# --- PLANIFICACIÓN ---
# Ejecutar polling cada 5 segundos
schedule.every(10).seconds.do(fetch_and_store)

# Ejecutar guardado histórico cada 1 minuto
schedule.every(4).minutes.do(save_history)

# Bucle principal
if __name__ == "__main__":
    while True:
        schedule.run_pending()
        time.sleep(1)