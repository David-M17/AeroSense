#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
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
static constexpr size_t LOTE_PAQUETES = 10; // se mandan juntos: una conexión por lote, no por paquete
static constexpr size_t LOTE_ERRORES = 5;
static constexpr int TIMEOUT_CONEXION_MS = 1500;
static constexpr unsigned long TIMEOUT_RESPUESTA_MS = 5000; // la PC puede tardar en guardar; mejor esperar que reenviar
static constexpr unsigned long ESPERA_VACIA_MS = 1000;
static constexpr unsigned long ESPERA_CON_PENDIENTES_MS = 20;
static constexpr unsigned long ESPERA_FALLO_MIN_MS = 2000;
static constexpr unsigned long ESPERA_FALLO_MAX_MS = 15000;
static constexpr unsigned long AVISO_DESCARTES_MS = 60000; // máximo un aviso por minuto
static constexpr uint32_t TAREA_PILA = 8192;
static constexpr UBaseType_t TAREA_PRIORIDAD = 2; // mayor que Telegram/nube (1): casi siempre espera la red
static constexpr BaseType_t TAREA_NUCLEO = 1;

struct PaqueteCola
{
  unsigned long ts; // millis() en que llegó
  unsigned int id;
  float alt, pres, temp;
  bool alturaValida;
  float altura;
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
  int cantidad;
  char origen[16];
  char tipo[28];
  char detalle[100];
};

struct Lote
{
  PaqueteCola paquetes[LOTE_PAQUETES];
  ErrorCola errores[LOTE_ERRORES];
  size_t nPaquetes = 0;
  size_t nErrores = 0;
  bool vacio() const { return nPaquetes == 0 && nErrores == 0; }
};

static QueueHandle_t colaPaquetes = nullptr;
static QueueHandle_t colaErrores = nullptr;
static uint32_t paquetesDescartados = 0; // por cola llena, desde el último aviso; solo la tarea LoRa lo toca
static unsigned long ultimoAvisoDescartes = 0;

static void tareaRegistro(void *parametro);
static void sacarLote(Lote &lote);
static void devolverLote(const Lote &lote);
static int enviarLote(const Lote &lote);
static int postearJson(const char *ruta, const String &cuerpo);
static const char *motivoReinicio();
static void copiarAscii(char *destino, size_t tam, const String &origen);

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

  PaqueteCola p = {millis(), t.id, t.alt, t.pres, t.temp, t.alturaValida, t.altura, t.hum, t.pmValido, t.pm1, t.pm25, t.pm10,
                   t.accX, t.accY, t.accZ, tieneRssi, rssiDbm};
  if (!encolar(colaPaquetes, &p))
    paquetesDescartados++;

  if (paquetesDescartados > 0 && millis() - ultimoAvisoDescartes > AVISO_DESCARTES_MS)
  {
    ultimoAvisoDescartes = millis();
    registroPcError("receptor", "registros_descartados", "la PC no recibe a tiempo; se descartaron los paquetes mas viejos",
                    (int)paquetesDescartados);
    paquetesDescartados = 0;
  }
}

void registroPcError(const char *origen, const char *tipo, const String &detalle, int cantidad)
{
  if (colaErrores == nullptr)
    return;

  ErrorCola e = {millis(), cantidad, {0}, {0}, {0}};
  copiarAscii(e.origen, sizeof(e.origen), origen);
  copiarAscii(e.tipo, sizeof(e.tipo), tipo);
  copiarAscii(e.detalle, sizeof(e.detalle), detalle);
  encolar(colaErrores, &e);
}

// --- Tarea ---

// Saca un lote de las colas, lo manda y decide qué hacer según la respuesta:
//   200       guardado: el lote se olvida.
//   4xx       la PC lo rechazó: reintentar no serviría, se descarta (si no, un dato defectuoso
//             bloquearía la cola para siempre).
//   otro      sin conexión o fallo del servidor: el lote vuelve al frente de la cola y se reintenta
//             con una espera creciente.
static void tareaRegistro(void *parametro)
{
  unsigned long esperaFallo = ESPERA_FALLO_MIN_MS;
  unsigned long fallosSeguidos = 0;
  static Lote lote; // grande para la pila; solo esta tarea lo usa

  for (;;)
  {
    unsigned long espera = ESPERA_VACIA_MS;

    if (WiFi.status() == WL_CONNECTED)
    {
      sacarLote(lote);
      if (!lote.vacio())
      {
        const unsigned long inicio = millis();
        const int codigo = enviarLote(lote);
        const unsigned long duracion = millis() - inicio;

        if (codigo == 200)
        {
          if (fallosSeguidos > 0)
            Serial.printf("🗄️ PC de nuevo en línea tras %lu fallo(s)\n", fallosSeguidos);
          fallosSeguidos = 0;
          esperaFallo = ESPERA_FALLO_MIN_MS;
          espera = (uxQueueMessagesWaiting(colaPaquetes) > 0) ? ESPERA_CON_PENDIENTES_MS : ESPERA_VACIA_MS;
          if (duracion > 1500)
            Serial.printf("🗄️ La PC tardó %lu ms en confirmar un lote de %u paquete(s)\n", duracion, (unsigned)lote.nPaquetes);
        }
        else if (codigo >= 400 && codigo < 500)
        {
          Serial.printf("🗄️ La PC rechazó un lote (HTTP %d); se descartan %u paquete(s) y %u error(es)\n",
                        codigo, (unsigned)lote.nPaquetes, (unsigned)lote.nErrores);
          espera = ESPERA_CON_PENDIENTES_MS;
        }
        else
        {
          devolverLote(lote);
          fallosSeguidos++;
          if (fallosSeguidos == 1 || fallosSeguidos % 10 == 0) // no inundar el monitor serie
            Serial.printf("🗄️ Envío a la PC falló (código %d, %lu ms); %u paquete(s) esperando, reintento en %lu s\n",
                          codigo, duracion, (unsigned)uxQueueMessagesWaiting(colaPaquetes), esperaFallo / 1000);
          espera = esperaFallo;
          esperaFallo = min(esperaFallo * 2, ESPERA_FALLO_MAX_MS);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(espera));
  }
}

static void sacarLote(Lote &lote)
{
  lote.nPaquetes = 0;
  lote.nErrores = 0;
  while (lote.nErrores < LOTE_ERRORES && xQueueReceive(colaErrores, &lote.errores[lote.nErrores], 0) == pdTRUE)
    lote.nErrores++;
  while (lote.nPaquetes < LOTE_PAQUETES && xQueueReceive(colaPaquetes, &lote.paquetes[lote.nPaquetes], 0) == pdTRUE)
    lote.nPaquetes++;
}

// Devuelve el lote al frente de las colas en su orden original. Si mientras tanto se llenaron
// con datos nuevos, lo más viejo se pierde (misma regla que al encolar).
static void devolverLote(const Lote &lote)
{
  for (size_t i = lote.nErrores; i > 0; i--)
    xQueueSendToFront(colaErrores, &lote.errores[i - 1], 0);
  for (size_t i = lote.nPaquetes; i > 0; i--)
    if (xQueueSendToFront(colaPaquetes, &lote.paquetes[i - 1], 0) != pdTRUE)
      paquetesDescartados++;
}

// HTTP plano (red local): no gasta memoria en TLS. Devuelve el código HTTP (<= 0: sin conexión).
static int enviarLote(const Lote &lote)
{
  JsonDocument doc;
  const unsigned long ahora = millis();

  JsonArray paquetes = doc["paquetes"].to<JsonArray>();
  for (size_t i = 0; i < lote.nPaquetes; i++)
  {
    const PaqueteCola &p = lote.paquetes[i];
    JsonObject o = paquetes.add<JsonObject>();
    o["id"] = p.id;
    o["alt"] = p.alt;
    o["pres"] = p.pres;
    if (p.alturaValida)
      o["h"] = p.altura;
    o["temp"] = p.temp;
    o["hum"] = p.hum;
    if (p.pmValido)
    {
      JsonObject pm = o["pm"].to<JsonObject>();
      pm["pm1"] = p.pm1;
      pm["pm25"] = p.pm25;
      pm["pm10"] = p.pm10;
    }
    else
    {
      o["pm"] = nullptr;
    }
    JsonObject acc = o["acc"].to<JsonObject>();
    acc["x"] = p.accX;
    acc["y"] = p.accY;
    acc["z"] = p.accZ;
    if (p.tieneRssi)
      o["rssi"] = p.rssiDbm;
    o["hace_ms"] = ahora - p.ts; // la PC calcula la hora real; así un reenvío tardío no miente
  }

  JsonArray errores = doc["errores"].to<JsonArray>();
  for (size_t i = 0; i < lote.nErrores; i++)
  {
    const ErrorCola &e = lote.errores[i];
    JsonObject o = errores.add<JsonObject>();
    o["origen"] = e.origen;
    o["tipo"] = e.tipo;
    o["detalle"] = e.detalle;
    o["cantidad"] = e.cantidad;
    o["hace_ms"] = ahora - e.ts;
  }

  String cuerpo;
  serializeJson(doc, cuerpo);
  return postearJson("/api/lote", cuerpo);
}

// Petición HTTP mínima. Se arma completa (encabezados + cuerpo) y sale en una sola escritura con
// Nagle apagado: así nada se queda esperando la confirmación TCP de un segmento anterior. Con
// HTTPClient los encabezados y el cuerpo salían por separado y, si esa confirmación se perdía,
// el cuerpo llegaba a la PC 3, 6 o 9 s después (retransmisiones de TCP).
static int postearJson(const char *ruta, const String &cuerpo)
{
  WiFiClient cliente;
  if (!cliente.connect(REGISTRO_PC_HOST, REGISTRO_PC_PUERTO, TIMEOUT_CONEXION_MS))
    return -1;
  cliente.setNoDelay(true);

  String peticion;
  peticion.reserve(cuerpo.length() + 160);
  peticion += "POST ";
  peticion += ruta;
  peticion += " HTTP/1.1\r\nHost: " REGISTRO_PC_HOST "\r\n"
              "Content-Type: application/json\r\nConnection: close\r\nContent-Length: ";
  peticion += cuerpo.length();
  peticion += "\r\n\r\n";
  peticion += cuerpo;

  if (cliente.write(reinterpret_cast<const uint8_t *>(peticion.c_str()), peticion.length()) != peticion.length())
  {
    cliente.stop();
    return -2;
  }

  // Solo interesa la primera línea: "HTTP/1.0 200 OK"
  String linea;
  const unsigned long inicio = millis();
  while (millis() - inicio < TIMEOUT_RESPUESTA_MS)
  {
    while (cliente.available())
    {
      const char c = (char)cliente.read();
      if (c == '\n')
      {
        int codigo = 0;
        sscanf(linea.c_str(), "HTTP/%*s %d", &codigo);
        cliente.stop();
        return codigo > 0 ? codigo : -3; // respondió algo que no es HTTP
      }
      if (linea.length() < 40)
        linea += c;
    }
    if (!cliente.connected())
      break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  cliente.stop();
  return -11; // sin respuesta a tiempo
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
  case ESP_RST_BROWNOUT: return "caida de voltaje (brownout)";
  case ESP_RST_DEEPSLEEP: return "salida de sueno profundo";
  default: return "motivo desconocido";
  }
}

// Pasa el texto a ASCII para que el JSON siempre sea válido, venga de donde venga (hasta ruido de
// radio): quita los acentos del español (á -> a, ñ -> n...) y cambia lo demás por '.'.
// Recorre por longitud, no por '\0', porque la basura de radio puede traer bytes nulos.
static char sinAcento(uint8_t segundoByte)
{
  switch (segundoByte)
  {
  case 0xA1: case 0xA0: case 0xA2: case 0xA4: return 'a';
  case 0xA9: case 0xA8: case 0xAA: case 0xAB: return 'e';
  case 0xAD: case 0xAC: case 0xAE: case 0xAF: return 'i';
  case 0xB3: case 0xB2: case 0xB4: case 0xB6: return 'o';
  case 0xBA: case 0xB9: case 0xBB: case 0xBC: return 'u';
  case 0xB1: return 'n';
  case 0x81: return 'A';
  case 0x89: return 'E';
  case 0x8D: return 'I';
  case 0x93: return 'O';
  case 0x9A: return 'U';
  case 0x91: return 'N';
  default: return '.';
  }
}

static void copiarAscii(char *destino, size_t tam, const String &origen)
{
  size_t n = 0;
  const size_t largo = origen.length();
  for (size_t i = 0; i < largo && n < tam - 1; i++)
  {
    const uint8_t c = (uint8_t)origen[i];
    if (c >= 32 && c < 127)
      destino[n++] = (char)c;
    else if (c == 0xC3 && i + 1 < largo)
      destino[n++] = sinAcento((uint8_t)origen[++i]);
    else if (c == 0xC2 && i + 1 < largo)
      i++; // ¡ ¿ µ ° y similares: se omiten
    else if (c < 0x80 || c >= 0xC0)
      destino[n++] = '.'; // control o inicio de otro carácter multibyte
    // los bytes de continuación (0x80-0xBF) se saltan
  }
  destino[n] = '\0';
}
