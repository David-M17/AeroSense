#pragma once
#include <Arduino.h>

#include "telemetria.h"

// Envía a la PC (servidor_registro) cada paquete recibido y los errores del sistema. Todo pasa por
// colas en RAM: si la PC está apagada o el Wi-Fi cae, los datos esperan y se mandan al volver
// (hasta ~100 paquetes; si se llena, se descarta el más viejo y se anota cuántos).
//
// Las funciones de encolado no bloquean ni usan la red, así que se pueden llamar desde cualquier
// tarea, incluida la de LoRa. Los envíos van por lotes en una tarea propia.

void registroPcIniciar(); // después de telIniciar(); registra el motivo del reinicio

// `rssiDbm` solo cuenta si `tieneRssi` es true.
void registroPcPaquete(const Telemetria &t, bool tieneRssi, int rssiDbm);

// origen: "lora", "nube", "telegram", "receptor"...   tipo: código corto sin espacios
// cantidad: cuántos eventos representa (p. ej. paquetes perdidos de golpe)
void registroPcError(const char *origen, const char *tipo, const String &detalle = "", int cantidad = 1);
