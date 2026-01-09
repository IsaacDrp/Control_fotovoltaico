from pydantic import BaseModel
from typing import List, Optional
from datetime import datetime

# Modelo para una celda individual (si quisieras guardarlo, por ahora simplificado)
class BatteryData(BaseModel):
    name: str
    connected: bool
    status: str
    voltage: float
    current: float
    soc: int
    power: Optional[float] = 0.0

class Summary(BaseModel):
    total_power: float

class InverterStatus(BaseModel):
    on: bool

# El JSON completo que manda el ESP32
class SolarSystemData(BaseModel):
    batteries: List[BatteryData]
    summary: Summary
    inverter: InverterStatus

# Modelo para guardar en Base de Datos (Flat structure)
class MetricDB(BaseModel):
    timestamp: datetime
    bat1_voltage: float
    bat1_current: float
    bat1_soc: int
    bat2_voltage: float
    bat2_current: float
    bat2_soc: int
    total_power: float
    inverter_on: bool
    