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
        # 1. Pull del ESP32
        response = requests.get(ESP32_URL, timeout=30)
        
        if response.status_code == 200:
            data = response.json()
            
            # 2. Guardar en REDIS (Sobrescribe 'solar:live')
            r.set("solar:live", json.dumps(data))
            # Guardamos timestamp para saber si el dato es viejo en el frontend
            r.set("solar:last_update", str(datetime.now()))
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Dato actualizado en Redis")
            
            return data
        else:
            print(f"Error HTTP ESP32: {response.status_code}")
            return None
            
    except Exception as e:
        print(f"Error conectando al ESP32: {e}")
        return None

def save_history():
    """Toma lo último de Redis y lo guarda en Postgres"""
    raw_data = r.get("solar:live")
    if not raw_data:
        return

    data = json.loads(raw_data)
    
    try:
        db = SessionLocal()
        
        # Mapeo del JSON a la Tabla SQL
        # Asumimos que data['batteries'][0] siempre es Bat 1
        metric = SolarMetric(
            bat1_voltage = data['batteries'][0]['voltage'],
            bat1_current = data['batteries'][0]['current'],
            bat1_soc     = data['batteries'][0]['soc'],
            
            bat2_voltage = data['batteries'][1]['voltage'],
            bat2_current = data['batteries'][1]['current'],
            bat2_soc     = data['batteries'][1]['soc'],
            
            total_power  = data['summary']['total_power'],
            inverter_status = data['inverter']['on']
        )
        
        db.add(metric)
        db.commit()
        db.close()
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Histórico guardado en Postgres")
        
    except Exception as e:
        print(f"Error guardando en DB: {e}")

# --- PLANIFICACIÓN ---
# Ejecutar polling cada 5 segundos
schedule.every(120).seconds.do(fetch_and_store)

# Ejecutar guardado histórico cada 1 minuto
schedule.every(4).minutes.do(save_history)

# Bucle principal
if __name__ == "__main__":
    while True:
        schedule.run_pending()
        time.sleep(1)