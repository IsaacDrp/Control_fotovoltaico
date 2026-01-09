# Usamos Python 3.11 ligero
FROM python:3.11-slim

# Evita que Python genere archivos .pyc y burocraxia de logs
ENV PYTHONDONTWRITEBYTECODE 1
ENV PYTHONUNBUFFERED 1

# Directorio de trabajo dentro del contenedor
WORKDIR /code

# Instalamos dependencias del sistema necesarias para compilar drivers de Postgres
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libpq-dev \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Copiamos los requirements e instalamos (Cache layer)
COPY requirements.txt /code/
RUN pip install --no-cache-dir --upgrade -r requirements.txt

# Copiamos el resto del código (carpeta app)
COPY ./app /code/app

# El comando por defecto se define en el docker-compose