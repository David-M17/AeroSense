# AeroSense

Telemetría para globo meteorológico amarrado (~100 m) con módulos LoRa E220-900T22D.

## Estructura

| Carpeta | Descripción |
|---|---|
| `transmisor/` | Payload del globo (ESP32 + DHT22, BMP180, ADXL345, PMS7003M + LoRa E220). Envía paquetes JSON cada 2 s. |
| `receptor/` | Estación terrestre (ESP32 + LoRa E220). Recibe los paquetes, sirve un panel web con gráficas y manda alertas por Telegram. |

Cada carpeta es un proyecto independiente de PlatformIO (`platformio.ini` propio).

## Configurar credenciales (receptor)

1. En `receptor/include/` copia `secrets.h.example` como `secrets.h`.
2. Llena tu WiFi, `BOT_TOKEN` y `CHAT_ID` de Telegram.
3. Compila y sube con PlatformIO.

`secrets.h` está ignorado por Git y no se sube al repositorio.

## Comandos de Telegram

`/start`, `/datos`, `/estado`
