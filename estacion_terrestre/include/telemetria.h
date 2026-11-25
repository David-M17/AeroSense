#pragma once
#include <Arduino.h>

// Estado compartido entre la recepción LoRa, el panel web, Telegram y los servicios en la nube.
struct Telemetria
{
  unsigned int id = 0;
  float alt = 0.0, pres = 0.0, temp = 0.0;
  int hum = 0;

  bool pmValido = false; // false = el transmisor no tiene lectura reciente del PMS7003M
  int pm1 = 0, pm25 = 0, pm10 = 0;

  float accX = 0.0, accY = 0.0, accZ = 0.0;

  unsigned long ultimaRecepcion = 0; // millis() del último paquete LoRa (0 = ninguno aún)

  // RSSI (dBm) que reporta el módulo E220 del receptor
  int rssiDbm = 0;
  unsigned long rssiTs = 0; // millis() de la última vez que llegó un byte RSSI (0 = nunca)

  bool hayPaquete() const { return ultimaRecepcion != 0; }
  unsigned long edadMs() const { return millis() - ultimaRecepcion; }

  // El RSSI se considera vigente si llegó en los últimos 10 s
  bool rssiVigente() const { return rssiTs != 0 && (millis() - rssiTs) < 10000UL; }
};

// Debe llamarse una vez, antes de crear cualquier tarea que use la telemetría.
void telIniciar();

// La telemetría la escribe la tarea LoRa y la leen otras tareas, por eso nadie accede al dato
// original: se pide una copia consistente con telSnapshot().
Telemetria telSnapshot();

// Guarda un paquete recién decodificado (conserva el RSSI y fija ultimaRecepcion = ahora).
void telGuardarPaquete(const Telemetria &paquete);

// Guarda el RSSI de un paquete. Devuelve false (y lo descarta) si aún no ha llegado ningún
// paquete válido: antes del primero solo hay ruido de arranque.
bool telGuardarRssi(int dbm);

// Descripción cualitativa del enlace (umbrales aproximados, ajústalos con tus pruebas)
inline const char *calidadRssi(int dbm)
{
  if (dbm >= -80)
    return "Excelente";
  if (dbm >= -100)
    return "Buena";
  if (dbm >= -110)
    return "Regular";
  return "Débil";
}
