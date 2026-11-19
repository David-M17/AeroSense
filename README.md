# AeroSense

Telemetría en tiempo real para un globo cautivo con helio: mide variables atmosféricas y partículas suspendidas, las transmite por radio LoRa hasta una estación en tierra y las publica en la nube.

Proyecto para Arquitectura de Computadoras — Instituto Tecnológico de Zacatepec (TecNM). Equipo: Mastache De la Paz David · Ramírez Díaz Salvador · Sánchez Julián Isaac.

## Arquitectura

```
┌─────────────────┐   LoRa 915 MHz   ┌──────────────────┐
│   Globo (ESP32)  │ ───────────────▶ │ Tierra (ESP32)    │
│ DHT22, BMP180,   │   E220-900T22D   │  E220-900T22D     │
│ ADXL345, PMS7003M│                  │                    │
└─────────────────┘                  └─────────┬──────────┘
                                                │ Wi-Fi
                       ┌────────────┬───────────┼───────────┬────────────┐
                       ▼            ▼           ▼           ▼            
                  Panel web     Telegram      Blynk     ThingSpeak
                 (gráficas +    (comandos y
                  actitud 3D)    alertas)
```

El módulo del globo mide temperatura, humedad, presión, altitud, inclinación, vibración y PM2.5/PM10, y envía un paquete JSON cada 2 s por LoRa. La estación de tierra lo recibe, lo valida y lo reparte a un panel web propio, un bot de Telegram y dos plataformas IoT (Blynk y ThingSpeak).

## Estructura del repositorio

| Carpeta | Descripción |
|---|---|
| `transmisor/` | Firmware del globo: lee los 4 sensores y transmite por LoRa. |
| `receptor/` | Estación terrestre: recibe por LoRa, sirve el panel web, atiende Telegram y publica en la nube. Modularizado (ver abajo). |
| `herramientas/configurar_e220_rssi/` | Sketch de un solo uso para activar el byte de RSSI en el módulo E220. |

Cada carpeta es un proyecto independiente de PlatformIO.

## Receptor: módulos

| Archivo | Responsabilidad |
|---|---|
| `lora_rx` | Recibe y decodifica los paquetes del E220 en una tarea de FreeRTOS propia, para no perder paquetes si otra tarea se bloquea. |
| `telemetria` | Estado compartido entre tareas, protegido con mutex (`telSnapshot()` da una copia consistente). |
| `web_server` + `web_page` | Panel web con gráficas (Chart.js) y una vista 3D del módulo que se inclina según el acelerómetro. |
| `telegram_bot` | Comandos (`/start`, `/datos`, `/estado`) y alertas (sin señal, PM2.5 alto, RSSI bajo), en su propia tarea. |
| `nube` | Publica en Blynk y ThingSpeak cada 15 s, una tarea por servicio. |

## Enlace LoRa

Los dos módulos E220-900T22D usan el mismo canal y dirección. El RSSI del enlace se activa por configuración (ver `herramientas/configurar_e220_rssi/`) y viaja como un byte extra al final de cada paquete recibido.

## Puesta en marcha

### 1. Credenciales (receptor)

```
cd receptor/include
cp secrets.h.example secrets.h
```

Completa en `secrets.h`:

| Variable | Para qué |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | Red del receptor |
| `BOT_TOKEN`, `CHAT_ID` | Bot de Telegram |
| `BLYNK_AUTH_TOKEN` | Auth Token de la plantilla en Blynk (vacío = desactivado) |
| `THINGSPEAK_API_KEY` | Write API Key del canal en ThingSpeak (vacío = desactivado) |

`secrets.h` está en `.gitignore` y nunca se sube al repositorio.

### 2. Compilar y subir

Con PlatformIO, compila y sube `transmisor/` al ESP32 del globo y `receptor/` al ESP32 de tierra.

### 3. Usar

- **Panel web:** entra a la IP que imprime el receptor por Serial al conectarse al Wi-Fi.
- **Telegram:** `/start`, `/datos`, `/estado`.
- **Blynk / ThingSpeak:** configura la plantilla/canal con los campos descritos en `nube.cpp` antes de la primera ejecución.

## Datastreams / campos en la nube

| # | Blynk (V) | ThingSpeak (Field) |
|---|---|---|
| Altitud | V1 | 1 |
| Temperatura | V2 | 2 |
| Humedad | V3 | 3 |
| Presión | V4 | 4 |
| PM1.0 | V5 | 5 |
| PM2.5 | V6 | 6 |
| PM10 | V7 | 7 |
| RSSI | V8 | 8 |
| ID de paquete | V0 | — |

## Créditos y licencia

Proyecto académico de código abierto, reutilizable para fines educativos. Ver el reporte del proyecto para la justificación técnica, el cálculo de sustentación y las pruebas completas.
