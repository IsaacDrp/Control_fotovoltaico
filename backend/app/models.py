from sqlalchemy import Column, Integer, Float, Boolean, DateTime
from sqlalchemy.sql import func
from .database import Base

class SolarMetric(Base):
    __tablename__ = "metrics"

    id = Column(Integer, primary_key=True, index=True)
    timestamp = Column(DateTime(timezone=True), server_default=func.now(), index=True)
    
    # Batería 1
    bat1_voltage = Column(Float)
    bat1_current = Column(Float)
    bat1_soc = Column(Integer)
    
    # Batería 2
    bat2_voltage = Column(Float)
    bat2_current = Column(Float)
    bat2_soc = Column(Integer)
    
    # Sistema
    total_power = Column(Float)
    inverter_status = Column(Boolean) # True=ON, False=OFF