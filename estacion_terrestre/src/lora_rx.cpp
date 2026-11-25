#include <Arduino.h>
#include <ArduinoJson.h>

#include "lora_rx.h"
#include "registro_pc.h"
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
static constexpr uint32_t RSSI_ESPERA_MS = 300;    // el byte RSSI llega pegado al paquete; si no, se manda sin él
static constexpr uint32_t INVALIDO_VENTANA_MS = 500; // RSSI justo tras una línea inválida: ya se reportó
static constexpr UBaseType_t LORA_TAREA_PRIORIDAD = 3;
static constexpr BaseType_t LORA_TAREA_NUCLEO = 0;

static HardwareSerial loraSerial(2);
static String lineaLora; // línea en construcción; solo la usa la tarea LoRa

// Diagnóstico del enlace: el transmisor numera cada paquete, así que los saltos de id son
// paquetes que nunca llegaron. Solo los usa la tarea LoRa.
static int32_t ultimoId = -1;
static uint32_t totalRecibidos = 0;
static uint32_t totalPerdidos = 0;

static void registrarEnlace(int32_t id);

// El RSSI llega justo después del paquete, así que el paquete espera un instante para ir a la PC
// con su RSSI. Solo lo usa la tarea LoRa.
static Telemetria paqueteEnEspera;
static bool hayPaqueteEnEspera = false;
static unsigned long paqueteEnEsperaTs = 0;
static unsigned long ultimoInvalidoTs = 0;

static void enviarPaqueteEnEspera(bool tieneRssi, int dbm);
static void lineaInvalida(const String &linea);

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
    if (hayPaqueteEnEspera && millis() - paqueteEnEsperaTs > RSSI_ESPERA_MS)
      enviarPaqueteEnEspera(false, 0);
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

  if (hayPaqueteEnEspera)
    enviarPaqueteEnEspera(true, dbm);
  else if (millis() - ultimoInvalidoTs > INVALIDO_VENTANA_MS)
    registroPcError("lora", "rssi_sin_paquete", "llegó un RSSI de " + String(dbm) + " dBm sin paquete: se perdió o llegó dañado");
}

static void procesarPaquete(const String &linea)
{
  if (!linea.startsWith("{") || !linea.endsWith("}"))
  {
    lineaInvalida(linea);
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, linea))
  {
    lineaInvalida(linea);
    return;
  }

  // El transmisor manda null en pm cuando el PMS7003M no tiene lectura reciente
  JsonObject pm = doc["pm"];
  JsonObject acc = doc["acc"];

  Telemetria paquete;
  paquete.id = doc["id"];
  paquete.alt = doc["alt"];
  paquete.pres = doc["pres"];
  paquete.alturaValida = !doc["h"].isNull();
  paquete.altura = doc["h"] | 0.0f;
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
  registrarEnlace(paquete.id);

  if (hayPaqueteEnEspera)
    enviarPaqueteEnEspera(false, 0); // llegó otro paquete sin que llegara el RSSI del anterior
  paqueteEnEspera = paquete;
  hayPaqueteEnEspera = true;
  paqueteEnEsperaTs = millis();

  Serial.println("-> Paquete LoRa procesado correctamente:");
  Serial.println(linea);
}

// Un id menor que el anterior significa que el transmisor se reinició: se empieza de cero.
static void registrarEnlace(int32_t id)
{
  if (id < ultimoId)
  {
    ultimoId = -1;
    totalRecibidos = 0;
    totalPerdidos = 0;
  }

  const int32_t saltados = (ultimoId >= 0) ? id - ultimoId - 1 : 0;
  if (saltados > 0)
  {
    totalPerdidos += saltados;
    Serial.printf("   ⚠️ Se perdieron %ld paquete(s) antes del id %ld\n", (long)saltados, (long)id);
    registroPcError("lora", "paquetes_perdidos", "faltaron " + String(saltados) + " paquete(s) antes del id " + String(id),
                    (int)saltados);
  }
  ultimoId = id;
  totalRecibidos++;

  const uint32_t esperados = totalRecibidos + totalPerdidos;
  Serial.printf("   Enlace: %lu recibidos, %lu perdidos (%lu%% de pérdida)\n",
                (unsigned long)totalRecibidos, (unsigned long)totalPerdidos,
                (unsigned long)(100UL * totalPerdidos / esperados));
}

static void enviarPaqueteEnEspera(bool tieneRssi, int dbm)
{
  registroPcPaquete(paqueteEnEspera, tieneRssi, dbm);
  hayPaqueteEnEspera = false;
}

// Una línea vacía no es error (es solo el '\n' suelto); cualquier otra que no sea JSON válido sí.
static void lineaInvalida(const String &linea)
{
  if (linea.length() == 0)
    return;
  ultimoInvalidoTs = millis();
  registroPcError("lora", "json_invalido", String(linea.length()) + " bytes: " + linea.substring(0, 60));
}
