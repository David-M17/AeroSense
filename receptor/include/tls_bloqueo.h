#pragma once

// Las conexiones TLS (Telegram, Blynk, ThingSpeak) piden decenas de KB de memoria contigua cada
// una. Con varias a la vez el heap se agota o se fragmenta ("SSL - Memory allocation failed"),
// así que solo se permite una a la vez. Se usa creando un objeto en el bloque que hace la
// conexión; al salir del bloque se libera solo:
//
//   { BloqueoTls bloqueo; ...consultas HTTPS... }
class BloqueoTls
{
public:
  BloqueoTls();
  ~BloqueoTls();
  BloqueoTls(const BloqueoTls &) = delete;
  BloqueoTls &operator=(const BloqueoTls &) = delete;
};
