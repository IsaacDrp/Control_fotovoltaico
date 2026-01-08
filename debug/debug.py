import asyncio
from bleak import BleakScanner, BleakClient

# --- CONFIGURACIÓN ---
# Pon aquí la MAC de una de tus baterías (la que quieras analizar)
# Si estás en macOS, bleak usa UUIDs en lugar de MACs, avísame si es el caso.
BMS_MAC = "A5:C2:37:28:8C:99" 

# UUIDs Estándar JBD / Xiaoxiang
UUID_WRITE = "0000ff02-0000-1000-8000-00805f9b34fb"
UUID_READ  = "0000ff01-0000-1000-8000-00805f9b34fb"

# Comando 0x03 (Info Básica: Voltaje, Amperaje, SOC, etc.)
CMD_BASIC = bytearray([0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77])

async def run():
    print(f"Buscando BMS {BMS_MAC}...")
    device = await BleakScanner.find_device_by_address(BMS_MAC, timeout=10.0)

    if not device:
        print("❌ No se encontró el BMS. Asegúrate de estar cerca y que el ESP32 no lo esté usando.")
        return

    print(f"✅ Conectando a {device.name}...")

    async with BleakClient(device) as client:
        print("🔗 Conectado. Suscribiendo a notificaciones...")

        # Callback que se ejecuta cuando llega respuesta del BMS
        def notification_handler(sender, data):
            print("\n" + "="*40)
            print(f"📩 RESPUESTA RECIBIDA ({len(data)} bytes):")
            print("="*40)
            
            # Imprimimos en HEX bonito y numerado para mapear
            hex_str = " ".join([f"{b:02X}" for b in data])
            print(f"RAW HEX: {hex_str}\n")
            
            print("--- ANÁLISIS BYTE A BYTE ---")
            for i, b in enumerate(data):
                print(f"Byte {i:02d}: 0x{b:02X}  (Dec: {b})")
            print("="*40 + "\n")
            
            # Evento para terminar el script
            event.set()

        event = asyncio.Event()
        
        await client.start_notify(UUID_READ, notification_handler)
        await asyncio.sleep(1.0) # Estabilizar

        print(f"📤 Enviando comando 0x03...")
        await client.write_gatt_char(UUID_WRITE, CMD_BASIC, response=False)

        # Esperar respuesta (máximo 5 segundos)
        try:
            await asyncio.wait_for(event.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            print("⚠️ Timeout: El BMS no respondió.")

        await client.stop_notify(UUID_READ)
        print("🔌 Desconectando...")

if __name__ == "__main__":
    asyncio.run(run())