#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_system.h>

#include "registro_pc.h"
#include "secrets.h" // opcional: REGISTRO_PC_HOST / REGISTRO_PC_PUERTO (ignorado por Git)

// Con el hotspot de Windows la PC siempre es 192.168.137.1. Para otra red, define
// REGISTRO_PC_HOST en secrets.h. Dejarlo "" desactiva el registro.
#ifndef REGISTRO_PC_HOST
#define REGISTRO_PC_HOST "192.168.137.1"
#endif
#ifndef REGISTRO_PC_PUERTO
#define REGISTRO_PC_PUERTO 8000
#endif

static constexpr size_t COLA_PAQUETES = 100;
static constexpr size_t COLA_ERRORES = 20;
static constexpr int TIMEOUT_CONEXION_MS = 1000;
static constexpr int TIMEOUT_RESPUESTA_MS = 2000;
static constexpr unsigned long ESPERA_TRAS_FALLO_MS = 5000; // la PC no responde: no insistir cada 50 ms
static constexpr unsigned long ESPERA_VACIA_MS = 200;
static constexpr unsigned long AVISO_COLA_LLENA_MS = 60000; // máximo un aviso por minuto
static constexpr uint32_t TAREA_PILA = 6144;
static constexpr UBaseType_t TAREA_PRIORIDAD = 1;
static constexpr BaseType_t TAREA_NUCLEO = 1;

struct PaqueteCola
{
  unsigned long ts; // millis() en que llegó
  unsigned int id;
  float alt, pres, temp;
  int hum;
  bool pmValido;
  int pm1, pm25, pm10;
  float accX, accY, accZ;
  bool tieneRssi;
  int rssiDbm;
};

struct ErrorCola
{
  unsigned long ts;
  char origen[16];
  char tipo[28];
  char detalle[100];
};

static QueueHandle_t colaPaquetes = nullptr;
static QueueHandle_t colaErrores = nullptr;
static unsigned long ultimoAvisoLlena = 0;

static void tareaRegistro(void *parametro);
static bool enviar(const char *ruta, const String &json);
static bool enviarPaquete(const PaqueteCola &p);
static bool enviarError(const ErrorCola &e);
static const char *motivoReinicio();
static void copiarTexto(char *destino, size_t tam, const char *origen);

void registroPcIniciar()
{
  if (strlen(REGISTRO_PC_HOST) == 0)
  {
    Serial.println("🗄️ Registro en PC desactivado (REGISTRO_PC_HOST vacío)");
    return;
  }

  colaPaquetes = xQueueCreate(COLA_PAQUETES, sizeof(PaqueteCola));
  colaErrores = xQueueCreate(COLA_ERRORES, sizeof(ErrorCola));
  registroPcError("receptor", "reinicio", motivoReinicio());

  xTaskCreatePinnedToCore(tareaRegistro, "registro_pc", TAREA_PILA, nullptr,
                          TAREA_PRIORIDAD, nullptr, TAREA_NUCLEO);
  Serial.printf("🗄️ Registro en PC: http://%s:%d\n", REGISTRO_PC_HOST, REGISTRO_PC_PUERTO);
}

// Si la cola está llena se descarta el elemento más viejo: el dato reciente vale más.
// Devuelve true si cupo sin descartar nada.
static bool encolar(QueueHandle_t cola, const void *elemento)
{
  if (xQueueSend(cola, elemento, 0) == pdTRUE)
    return true;

  alignas(8) uint8_t descartado[sizeof(PaqueteCola) > sizeof(ErrorCola) ? sizeof(PaqueteCola) : sizeof(ErrorCola)];
  xQueueReceive(cola, descartado, 0);
  xQueueSend(cola, elemento, 0);
  return false;
}

void registroPcPaquete(const Telemetria &t, bool tieneRssi, int rssiDbm)
{
  if (colaPaquetes == nullptr)
    return;

  PaqueteCola p = {millis(), t.id, t.alt, t.pres, t.temp, t.hum, t.pmValido, t.pm1, t.pm25, t.pm10,
                   t.accX, t.accY, t.accZ, tieneRssi, rssiDbm};
  if (!encolar(colaPaquetes, &p) && millis() - ultimoAvisoLlena > AVISO_COLA_LLENA_MS)
  {
    ultimoAvisoLlena = millis();
    registroPcError("receptor", "cola_llena", "la PC no responde: se descartan los paquetes más viejos");
  }
}

void registroPcError(const char *origen, const char *tipo, const String &detalle)
{
  if (colaErrores == nullptr)
    return;

  ErrorCola e = {millis(), {0}, {0}, {0}};
  copiarTexto(e.origen, sizeof(e.origen), origen);
  copiarTexto(e.tipo, sizeof(e.tipo), tipo);
  copiarTexto(e.detalle, sizeof(e.detalle), detalle.c_str());
  encolar(colaErrores, &e);
}

// --- Tarea ---

// Primero los errores (son pocos), luego los paquetes en orden de llegada. Un elemento sale de la
// cola solo cuando la PC confirmó haberlo guardado.
static void tareaRegistro(void *parametro)
{
  bool pcCaida = false;

  for (;;)
  {
    bool huboTrabajo = false;
    bool fallo = false;

    if (WiFi.status() == WL_CONNECTED)
    {
      ErrorCola e;
      PaqueteCola p;
      if (xQueuePeek(colaErrores, &e, 0) == pdTRUE)
      {
        huboTrabajo = true;
        if (enviarError(e))
          xQueueReceive(colaErrores, &e, 0);
        else
          fallo = true;
      }
      else if (xQueuePeek(colaPaquetes, &p, 0) == pdTRUE)
      {
        huboTrabajo = true;
        if (enviarPaquete(p))
          xQueueReceive(colaPaquetes, &p, 0);
        else
          fallo = true;
      }
    }

    if (fallo && !pcCaida)
      Serial.printf("🗄️ La PC no responde; %u paquete(s) esperando\n", (unsigned)uxQueueMessagesWaiting(colaPaquetes));
    if (!fallo && huboTrabajo && pcCaida)
      Serial.println("🗄️ PC de nuevo en línea, vaciando la cola");
    if (huboTrabajo)
      pcCaida = fallo;

    vTaskDelay(pdMS_TO_TICKS(fallo ? ESPERA_TRAS_FALLO_MS : (huboTrabajo ? 20 : ESPERA_VACIA_MS)));
  }
}

static bool enviarPaquete(const PaqueteCola &p)
{
  JsonDocument doc;
  doc["id"] = p.id;
  doc["alt"] = p.alt;
  doc["pres"] = p.pres;
  doc["temp"] = p.temp;
  doc["hum"] = p.hum;
  if (p.pmValido)
  {
    JsonObject pm = doc["pm"].to<JsonObject>();
    pm["pm1"] = p.pm1;
    pm["pm25"] = p.pm25;
    pm["pm10"] = p.pm10;
  }
  else
  {
    doc["pm"] = nullptr;
  }
  JsonObject acc = doc["acc"].to<JsonObject>();
  acc["x"] = p.accX;
  acc["y"] = p.accY;
  acc["z"] = p.accZ;
  if (p.tieneRssi)
    doc["rssi"] = p.rssiDbm;
  doc["hace_ms"] = millis() - p.ts; // la PC calcula la hora real; así un reenvío tardío no miente

  String cuerpo;
  serializeJson(doc, cuerpo);
  return enviar("/api/registro", cuerpo);
}

static bool enviarError(const ErrorCola &e)
{
  JsonDocument doc;
  doc["origen"] = e.origen;
  doc["tipo"] = e.tipo;
  doc["detalle"] = e.detalle;
  doc["hace_ms"] = millis() - e.ts;

  String cuerpo;
  serializeJson(doc, cuerpo);
  return enviar("/api/error", cuerpo);
}

// HTTP plano (red local): no gasta memoria en TLS. Devuelve true solo con respuesta 200.
static bool enviar(const char *ruta, const String &json)
{
  WiFiClient cliente;
  HTTPClient http;
  http.setConnectTimeout(TIMEOUT_CONEXION_MS);
  http.setTimeout(TIMEOUT_RESPUESTA_MS);
  if (!http.begin(cliente, REGISTRO_PC_HOST, REGISTRO_PC_PUERTO, ruta))
    return false;

  http.addHeader("Content-Type", "application/json");
  const int codigo = http.POST(json);
  http.end();
  return codigo == 200;
}

static const char *motivoReinicio()
{
  switch (esp_reset_reason())
  {
  case ESP_RST_POWERON: return "encendido";
  case ESP_RST_SW: return "reinicio por software";
  case ESP_RST_PANIC: return "fallo del programa (panic)";
  case ESP_RST_INT_WDT:
  case ESP_RST_TASK_WDT:
  case ESP_RST_WDT: return "watchdog (programa colgado)";
  case ESP_RST_BROWNOUT: return "caída de voltaje (brownout)";
  case ESP_RST_DEEPSLEEP: return "salida de sueño profundo";
  default: return "motivo desconocido";
  }
}

static void copiarTexto(char *destino, size_t tam, const char *origen)
{
  size_t n = 0;
  for (; origen[n] != '\0' && n < tam - 1; n++)
    destino[n] = (origen[n] >= 32 && origen[n] < 127) ? origen[n] : '?'; // solo ASCII: el JSON queda válido
  destino[n] = '\0';
}
