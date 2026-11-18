#pragma once

// Requiere Wi-Fi ya conectado. Lanza una tarea por servicio en la nube (Blynk y ThingSpeak),
// cada una publica la telemetría más reciente cada 15 s. Un servicio sin credencial en
// secrets.h queda desactivado.
void nubeIniciar();
