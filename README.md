# AeroSense

Telemetría en tiempo real para un globo cautivo con helio: mide variables atmosféricas y partículas suspendidas, las transmite por radio LoRa hasta una estación en tierra y las publica en la nube.

Proyecto para Arquitectura de Computadoras — Instituto Tecnológico de Zacatepec (TecNM). Equipo: Mastache De la Paz David · Ramírez Díaz Salvador · Sánchez Julián Isaac.

## Arquitectura

```
┌──────────────────┐   LoRa 915 MHz   ┌────────────────────┐
│   Globo (ESP32)  │ ──────────────▶ │ Tierra (ESP32)     │
│ DHT22, BMP180,   │   E220-900T22D   │  E220-900T22D      │
│ ADXL345, PMS7003M│                  │                    │
└──────────────────┘                  └─────────┬──────────┘
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
| `herramientas/servidor_registro/` | Servidor en Python (sin dependencias) que guarda en SQLite lo que manda el receptor: tabla `registros` y tabla `errores`, con visor web y exportación a CSV. |
| `herramientas/modelo_3d/` | Script en Python que genera el modelo 3D del módulo (`.glb`) y `receptor/include/modelo_glb.h`. |

Cada carpeta es un proyecto independiente de PlatformIO.

## Receptor: módulos

| Archivo | Responsabilidad |
|---|---|
| `lora_rx` | Recibe y decodifica los paquetes del E220 en una tarea de FreeRTOS propia, para no perder paquetes si otra tarea se bloquea. |
| `telemetria` | Estado compartido entre tareas, protegido con mutex (`telSnapshot()` da una copia consistente). |
| `web_server` + `web_page` | Panel web con gráficas (Chart.js) y el modelo 3D del módulo (Three.js) que se inclina según el acelerómetro. El modelo lo sirve el ESP32 en `/modelo.glb` desde `modelo_glb.h`. |
| `telegram_bot` | Comandos (`/start`, `/datos`, `/estado`) y alertas (sin señal, PM2.5 alto, RSSI bajo), en su propia tarea. |
| `nube` | Publica en Blynk y ThingSpeak cada 15 s, una tarea por servicio. |
| `tls_bloqueo` | Permite una sola conexión TLS a la vez (Telegram, Blynk, ThingSpeak) para no agotar la memoria. |
| `registro_pc` | Manda cada paquete y cada error a la PC por HTTP. Usa colas en RAM: si la PC no responde, espera y reenvía. |

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

## Registro en la PC

La estación de tierra puede guardar todo el historial en tu computadora (SQLite, dos tablas: `registros` y `errores`).

1. En la PC: `python herramientas/servidor_registro/servidor.py` (o doble clic en `iniciar_servidor.bat`). Solo requiere Python 3.8+.
2. La ESP32 receptora debe estar en la misma red que la PC. Con el hotspot de Windows la PC es `192.168.137.1` (valor por defecto); para otra red, define `REGISTRO_PC_HOST` en `secrets.h`.
3. Abre `http://localhost:8000` para ver el enlace por sesión, los errores y los últimos registros. `/exportar/registros.csv` y `/exportar/errores.csv` descargan todo.

La columna `sesion` sube cada vez que el transmisor reinicia (su id vuelve a 0). Hay tres tipos de falta que no hay que mezclar: `paquetes_perdidos` (los cuenta el receptor: nunca llegaron por LoRa), `registros_descartados` (el receptor sí los recibió, pero la PC no alcanzó a recibirlos y se saturó su cola) y `huecos_en_registros` (los ve el servidor por los saltos de id: suma de las dos anteriores). Los envíos van por lotes y un reintento nunca duplica registros. Si el firewall de Windows pregunta, permite Python en redes privadas.

El monitor serie del receptor guarda una copia con hora de cada línea en `receptor/platformio-device-monitor-*.log` (filtro `log2file`), porque la terminal de VS Code solo conserva las últimas líneas.

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
