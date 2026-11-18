#pragma once
#include <Arduino.h>

// Estado compartido: lo llena main.cpp (LoRa) y lo leen web_server.cpp y Telegram.
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

  // El RSSI se considera vigente si llegó en los últimos 10 s
  bool rssiVigente() const { return rssiTs != 0 && (millis() - rssiTs) < 10000UL; }
};

extern Telemetria tel; // definida en main.cpp

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
