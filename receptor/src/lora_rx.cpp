#include <Arduino.h>
#include <ArduinoJson.h>

#include "lora_rx.h"
#include "telemetria.h"

// Pines del E220 en la estación terrestre
static constexpr int LORA_RX = 16;
static constexpr int LORA_TX = 17;
static constexpr int LORA_AUX = 15;

static constexpr uint32_t LORA_BAUDIOS = 9600;
static constexpr size_t LORA_BUFER_UART = 512;     // búfer del driver (seguro extra)
static constexpr size_t LORA_LINEA_MAX = 512;      // más que esto en una línea es ruido
static constexpr uint8_t RSSI_BYTE_MIN = 0x80;     // todo RSSI válido (<= -1 dBm) es >= 0x80
static constexpr uint32_t LORA_PERIODO_MS = 5;     // cada cuánto se vacía el UART
static constexpr uint32_t LORA_TAREA_PILA = 8192;
static constexpr UBaseType_t LORA_TAREA_PRIORIDAD = 3;
static constexpr BaseType_t LORA_TAREA_NUCLEO = 0;

static HardwareSerial loraSerial(2);
static String lineaLora; // línea en construcción; solo la usa la tarea LoRa

static void procesarRssi(uint8_t byteRssi);
static void procesarPaquete(const String &linea);
static void leerLora();
static void tareaLora(void *parametro);

void loraIniciar()
{
  loraSerial.setRxBufferSize(LORA_BUFER_UART);
  loraSerial.begin(LORA_BAUDIOS, SERIAL_8N1, LORA_RX, LORA_TX);
  pinMode(LORA_AUX, INPUT);

  // Tarea propia: así las esperas de Telegram, web o nube no hacen perder paquetes.
  xTaskCreatePinnedToCore(tareaLora, "lora", LORA_TAREA_PILA, nullptr,
                          LORA_TAREA_PRIORIDAD, nullptr, LORA_TAREA_NUCLEO);
}

static void tareaLora(void *parametro)
{
  for (;;)
  {
    leerLora();
    vTaskDelay(pdMS_TO_TICKS(LORA_PERIODO_MS));
  }
}

// Con la opción "RSSI byte" del E220 activa, cada paquete recibido trae 1 byte extra al final:
// dBm = -(256 - byte). El JSON es ASCII puro (< 0x80), así que ese byte nunca se confunde con texto.
static void leerLora()
{
  while (loraSerial.available() > 0)
  {
    const uint8_t b = loraSerial.read();

    if (b >= RSSI_BYTE_MIN)
    {
      procesarRssi(b);
    }
    else if (b == '\n')
    {
      lineaLora.trim();
      procesarPaquete(lineaLora);
      lineaLora = "";
    }
    else if (b != '\r')
    {
      if (lineaLora.length() < LORA_LINEA_MAX)
        lineaLora += (char)b;
      else
        lineaLora = ""; // línea demasiado larga: ruido, se descarta
    }
  }
}

static void procesarRssi(uint8_t byteRssi)
{
  const int dbm = -(256 - (int)byteRssi);
  if (telGuardarRssi(dbm))
    Serial.printf("   RSSI: %d dBm (%s)\n", dbm, calidadRssi(dbm));
}

static void procesarPaquete(const String &linea)
{
  if (!linea.startsWith("{") || !linea.endsWith("}"))
    return;

  JsonDocument doc;
  if (deserializeJson(doc, linea))
    return;

  // El transmisor manda null en pm cuando el PMS7003M no tiene lectura reciente
  JsonObject pm = doc["pm"];
  JsonObject acc = doc["acc"];

  Telemetria paquete;
  paquete.id = doc["id"];
  paquete.alt = doc["alt"];
  paquete.pres = doc["pres"];
  paquete.temp = doc["temp"];
  paquete.hum = doc["hum"];
  paquete.pmValido = !pm["pm1"].isNull() && !pm["pm25"].isNull() && !pm["pm10"].isNull();
  if (paquete.pmValido)
  {
    paquete.pm1 = pm["pm1"];
    paquete.pm25 = pm["pm25"];
    paquete.pm10 = pm["pm10"];
  }
  paquete.accX = acc["x"];
  paquete.accY = acc["y"];
  paquete.accZ = acc["z"];

  telGuardarPaquete(paquete);

  Serial.println("-> Paquete LoRa procesado correctamente:");
  Serial.println(linea);
}
