#include "telemetria.h"

static Telemetria telActual;
static SemaphoreHandle_t telMutex = nullptr;

// Candado RAII: toma el mutex al crearse y lo suelta al salir del bloque, pase lo que pase.
class Candado
{
public:
  Candado() { xSemaphoreTake(telMutex, portMAX_DELAY); }
  ~Candado() { xSemaphoreGive(telMutex); }
  Candado(const Candado &) = delete;
  Candado &operator=(const Candado &) = delete;
};

void telIniciar()
{
  telMutex = xSemaphoreCreateMutex();
}

Telemetria telSnapshot()
{
  Candado candado;
  return telActual;
}

void telGuardarPaquete(const Telemetria &paquete)
{
  Candado candado;
  const int rssiDbm = telActual.rssiDbm;
  const unsigned long rssiTs = telActual.rssiTs;

  telActual = paquete;
  telActual.rssiDbm = rssiDbm;
  telActual.rssiTs = rssiTs;
  telActual.ultimaRecepcion = millis();
}

bool telGuardarRssi(int dbm)
{
  Candado candado;
  if (!telActual.hayPaquete())
    return false;

  telActual.rssiDbm = dbm;
  telActual.rssiTs = millis();
  return true;
}
