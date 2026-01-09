import os
from sqlalchemy import create_engine
from sqlalchemy.ext.declarative import declarative_base
from sqlalchemy.orm import sessionmaker

# Leemos las variables del .env (Docker las inyectará)
POSTGRES_USER = os.getenv("POSTGRES_USER", "postgres")
POSTGRES_PASSWORD = os.getenv("POSTGRES_PASSWORD", "password")
POSTGRES_DB = os.getenv("POSTGRES_DB", "solar_db")
POSTGRES_HOST = os.getenv("POSTGRES_HOST", "db") # "db" es el nombre del servicio en docker-compose

SQLALCHEMY_DATABASE_URL = f"postgresql://{POSTGRES_USER}:{POSTGRES_PASSWORD}@{POSTGRES_HOST}/{POSTGRES_DB}"

# Creamos el motor (Engine)
engine = create_engine(SQLALCHEMY_DATABASE_URL)

# Fábrica de sesiones
SessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=engine)

# Clase base para los modelos ORM
Base = declarative_base()

# Función de utilidad para obtener DB session
def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()