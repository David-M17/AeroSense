#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "secrets.h" // BLYNK_AUTH_TOKEN, THINGSPEAK_API_KEY (ignorado por Git)
#include "nube.h"
#include "registro_pc.h"
#include "telemetria.h"
#include "tls_bloqueo.h"

// ThingSpeak (plan gratuito) acepta como máximo 1 actualización cada 15 s; se usa el mismo
// periodo para Blynk.
static constexpr unsigned long PERIODO_ENVIO_MS = 15000;
static constexpr unsigned long DATOS_MAX_EDAD_MS = 20000; // no se suben datos de un paquete viejo
static constexpr int TIMEOUT_HTTP_MS = 5000;
static constexpr uint32_t TAREA_PILA = 10240; // TLS necesita mucha pila
static constexpr UBaseType_t TAREA_PRIORIDAD = 1;
static constexpr BaseType_t TAREA_NUCLEO = 1;

static const char *BLYNK_SERVIDOR = "blynk.cloud";
static const char *THINGSPEAK_SERVIDOR = "api.thingspeak.com";

// ---------------------------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------------------------

// Arma la parte "clave=valor&clave=valor" de una URL.
class Consulta
{
public:
  void agregar(const char *clave, float valor, uint8_t decimales)
  {
    agregarClave(clave);
    _texto += String(valor, static_cast<unsigned int>(decimales));
  }
  void agregar(const char *clave, int valor)
  {
    agregarClave(clave);
    _texto += String(valor);
  }
  const String &texto() const { return _texto; }

private:
  void agregarClave(const char *clave)
  {
    if (_texto.length() > 0)
      _texto += '&';
    _texto += clave;
    _texto += '=';
  }
  String _texto;
};

// GET por HTTPS. Devuelve el código HTTP (<= 0 si falló la conexión). Si se pasa `cuerpo`,
// ahí queda la respuesta del servidor.
//
// setInsecure(): no valida el certificado del servidor. Es aceptable para un proyecto escolar,
// pero el token viaja cifrado sin comprobar con quién se habla; para producción conviene fijar
// la CA raíz de cada servicio (como se hace con Telegram).
static int httpsGet(const String &url, String *cuerpo = nullptr)
{
  WiFiClientSecure cliente;
  cliente.setInsecure();

  HTTPClient http;
  http.setTimeout(TIMEOUT_HTTP_MS);
  if (!http.begin(cliente, url))
    return -1;

  const int codigo = http.GET();
  if (codigo > 0 && cuerpo != nullptr)
    *cuerpo = http.getString();
  http.end();
  return codigo;
}

static bool datosFrescos(const Telemetria &t)
{
  return t.hayPaquete() && t.edadMs() < DATOS_MAX_EDAD_MS;
}

// ---------------------------------------------------------------------------------------------
// Blynk (API HTTP). Datastreams virtuales de la plantilla "AeroSense Globo":
//   V0 ID paquete   V1 altitud (m)   V2 temperatura (°C)   V3 humedad (%)   V4 presión (hPa)
//   V5 PM1.0        V6 PM2.5         V7 PM10               V8 RSSI (dBm)
// ---------------------------------------------------------------------------------------------

static bool enviarBlynk(const Telemetria &t)
{
  Consulta datos;
  datos.agregar("v0", (int)t.id);
  datos.agregar("v1", t.alt, 1);
  datos.agregar("v2", t.temp, 1);
  datos.agregar("v3", t.hum);
  datos.agregar("v4", t.pres, 1);
  if (t.pmValido)
  {
    datos.agregar("v5", t.pm1);
    datos.agregar("v6", t.pm25);
    datos.agregar("v7", t.pm10);
  }
  if (t.rssiVigente())
    datos.agregar("v8", t.rssiDbm);

  const String url = String("https://") + BLYNK_SERVIDOR + "/external/api/batch/update?token=" +
                     BLYNK_AUTH_TOKEN + "&" + datos.texto();
  return httpsGet(url) == 200;
}

// ---------------------------------------------------------------------------------------------
// ThingSpeak. Campos que debes crear en el canal:
//   1 altitud  2 temperatura  3 humedad  4 presión  5 PM1.0  6 PM2.5  7 PM10  8 RSSI
// ---------------------------------------------------------------------------------------------

static bool enviarThingSpeak(const Telemetria &t)
{
  Consulta datos;
  datos.agregar("field1", t.alt, 1);
  datos.agregar("field2", t.temp, 1);
  datos.agregar("field3", t.hum);
  datos.agregar("field4", t.pres, 1);
  if (t.pmValido)
  {
    datos.agregar("field5", t.pm1);
    datos.agregar("field6", t.pm25);
    datos.agregar("field7", t.pm10);
  }
  if (t.rssiVigente())
    datos.agregar("field8", t.rssiDbm);

  const String url = String("https://") + THINGSPEAK_SERVIDOR + "/update?api_key=" +
                     THINGSPEAK_API_KEY + "&" + datos.texto();

  // ThingSpeak responde 200 con el cuerpo "0" cuando rechaza la actualización (p. ej. por ir
  // más rápido de 15 s); si la acepta, el cuerpo es el número de entrada.
  String cuerpo;
  return httpsGet(url, &cuerpo) == 200 && cuerpo != "0";
}

// ---------------------------------------------------------------------------------------------
// Tareas: una por servicio, con la misma estructura
// ---------------------------------------------------------------------------------------------

struct Servicio
{
  const char *nombre;
  const char *credencial;             // vacía = servicio desactivado
  bool (*enviar)(const Telemetria &); // publica una lectura; true si el servidor la aceptó
  unsigned long desfaseInicialMs;     // evita que dos conexiones TLS coincidan en el tiempo
};

static const Servicio SERVICIOS[] = {
    {"Blynk", BLYNK_AUTH_TOKEN, enviarBlynk, 0},
    {"ThingSpeak", THINGSPEAK_API_KEY, enviarThingSpeak, PERIODO_ENVIO_MS / 2},
};

static void tareaServicio(void *parametro)
{
  const Servicio &servicio = *static_cast<const Servicio *>(parametro);
  vTaskDelay(pdMS_TO_TICKS(servicio.desfaseInicialMs));

  for (;;)
  {
    const Telemetria t = telSnapshot();
    if (WiFi.status() == WL_CONNECTED && datosFrescos(t))
    {
      BloqueoTls bloqueo; // una sola conexión TLS a la vez en todo el firmware
      const unsigned long inicio = millis();
      const bool ok = servicio.enviar(t);
      Serial.printf("☁️ %s: %s (%lu ms)\n", servicio.nombre, ok ? "enviado" : "FALLÓ", millis() - inicio);
      if (!ok)
      {
        Serial.printf("   Memoria libre: %u bytes (bloque mayor: %u)\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        registroPcError("nube", (String(servicio.nombre) + "_fallo").c_str(),
                        "memoria libre " + String(ESP.getFreeHeap()) + ", bloque mayor " + String(ESP.getMaxAllocHeap()));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(PERIODO_ENVIO_MS));
  }
}

void nubeIniciar()
{
  for (const Servicio &servicio : SERVICIOS)
  {
    if (strlen(servicio.credencial) == 0)
    {
      Serial.printf("☁️ %s desactivado (falta la credencial en secrets.h)\n", servicio.nombre);
      continue;
    }
    xTaskCreatePinnedToCore(tareaServicio, servicio.nombre, TAREA_PILA,
                            const_cast<Servicio *>(&servicio), TAREA_PRIORIDAD, nullptr, TAREA_NUCLEO);
  }
}
