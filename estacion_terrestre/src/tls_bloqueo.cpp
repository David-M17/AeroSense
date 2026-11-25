#include <Arduino.h>

#include "tls_bloqueo.h"

// Mutex único, creado en el primer uso (la inicialización de una estática local es segura
// entre tareas).
static SemaphoreHandle_t mutexTls()
{
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

BloqueoTls::BloqueoTls() { xSemaphoreTake(mutexTls(), portMAX_DELAY); }
BloqueoTls::~BloqueoTls() { xSemaphoreGive(mutexTls()); }
