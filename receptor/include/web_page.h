#pragma once
#include <Arduino.h>

// Página del panel web (se guarda en flash, no en RAM).
// Se edita aquí como HTML normal: el literal R"rawliteral(...)" evita tener que escapar comillas.
// La página pide /data cada segundo. Chart.js se carga de un CDN, así que el navegador que
// abra el panel necesita internet; el resto (actitud 3D, indicadores) funciona sin conexión.
static const char PAGINA_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>AeroSense · Estación Terrestre</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
/* ============================ Base ============================ */
:root {
  --fondo: #05080d;
  --panel: #0b1119;
  --borde: #1b2735;
  --texto: #dfe8f2;
  --tenue: #78899c;
  --acento: #00d4ff;
  --ok: #2ecc71;
  --aviso: #ffb020;
  --alerta: #ff5a5a;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  padding: 0 0 30px;
  background: radial-gradient(circle at 50% 0%, #0c1522 0%, var(--fondo) 60%);
  color: var(--texto);
  font-family: "Segoe UI", Roboto, Arial, sans-serif;
  min-height: 100vh;
}
.contenedor { max-width: 1100px; margin: 0 auto; padding: 0 14px; }

/* ============================ Encabezado ============================ */
header {
  display: flex; align-items: center; justify-content: space-between;
  flex-wrap: wrap; gap: 10px;
  padding: 14px 0; margin-bottom: 14px;
  border-bottom: 1px solid var(--borde);
}
.marca { font-size: 17px; letter-spacing: 3px; text-transform: uppercase; }
.marca span { color: var(--acento); }
.estado { display: flex; align-items: center; gap: 8px; font-size: 13px; color: var(--tenue); }
.led { width: 9px; height: 9px; border-radius: 50%; background: var(--tenue); }
.led.vivo { background: var(--ok); box-shadow: 0 0 10px var(--ok); }
.led.viejo { background: var(--aviso); box-shadow: 0 0 10px var(--aviso); }
.led.muerto { background: var(--alerta); box-shadow: 0 0 10px var(--alerta); }

/* ============================ Rejilla y tarjetas ============================ */
.rejilla { display: grid; gap: 14px; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); }
.rejilla.duo { grid-template-columns: 1.35fr 1fr; margin-bottom: 14px; }
@media (max-width: 760px) { .rejilla.duo { grid-template-columns: 1fr; } }
.tarjeta {
  background: var(--panel);
  border: 1px solid var(--borde);
  border-radius: 10px;
  padding: 14px;
}
.titulo {
  font-size: 11px; letter-spacing: 2px; text-transform: uppercase;
  color: var(--tenue); margin: 0 0 10px;
}

/* Cifras grandes del encabezado de telemetría */
.cifra { font-size: 34px; font-weight: 300; font-variant-numeric: tabular-nums; line-height: 1.1; }
.cifra small { font-size: 14px; color: var(--tenue); margin-left: 4px; }
.sub { font-size: 12px; color: var(--tenue); margin-top: 4px; }

/* Filas de dato suelto (presión, partículas...) */
.fila { display: flex; justify-content: space-between; padding: 5px 0; font-size: 14px; }
.fila + .fila { border-top: 1px solid rgba(255,255,255,0.05); }
.fila b { font-weight: 500; font-variant-numeric: tabular-nums; }

/* ============================ Actitud 3D ============================ */
/* El módulo es un prisma rectangular. Se dibuja con transformaciones CSS 3D y se inclina
   según el alabeo y el cabeceo calculados a partir del acelerómetro. */
.escena {
  height: 260px;
  perspective: 760px;
  display: flex; align-items: center; justify-content: center;
  overflow: hidden;
}
/* .mundo mide lo mismo que el módulo: así el suelo se centra respecto a él */
.mundo {
  position: relative; width: 70px; height: 110px;
  transform-style: preserve-3d;
  transform: rotateX(-16deg) rotateY(-28deg); /* punto de vista fijo del observador */
}
.suelo {
  position: absolute; width: 260px; height: 260px;
  left: -95px; top: -75px; /* centrado sobre el módulo (70x110) */
  transform: rotateX(90deg) translateZ(-90px);
  background-image:
    linear-gradient(rgba(0,212,255,0.16) 1px, transparent 1px),
    linear-gradient(90deg, rgba(0,212,255,0.16) 1px, transparent 1px);
  background-size: 24px 24px;
  border: 1px solid rgba(0,212,255,0.25);
}
.modulo {
  position: relative; width: 70px; height: 110px;
  transform-style: preserve-3d;
  transition: transform 0.35s linear; /* suaviza el salto entre paquetes */
}
.cara {
  position: absolute;
  border: 1px solid var(--acento);
  background: rgba(0,212,255,0.10);
  display: flex; align-items: center; justify-content: center;
  font-size: 10px; letter-spacing: 1px; color: var(--acento);
}
.cara.frente, .cara.atras  { width: 70px; height: 110px; left: 0;  top: 0; }
.cara.der,    .cara.izq    { width: 50px; height: 110px; left: 10px; top: 0; }
.cara.arriba, .cara.abajo  { width: 70px; height: 50px;  left: 0;  top: 30px; }

.cara.frente { transform: translateZ(25px); }
.cara.atras  { transform: rotateY(180deg) translateZ(25px); }
.cara.der    { transform: rotateY(90deg)  translateZ(35px); }
.cara.izq    { transform: rotateY(-90deg) translateZ(35px); }
.cara.arriba { transform: rotateX(90deg)  translateZ(55px); background: rgba(0,212,255,0.26); }
.cara.abajo  { transform: rotateX(-90deg) translateZ(55px); background: rgba(0,212,255,0.04); }

.angulos { display: flex; justify-content: space-around; text-align: center; margin-top: 6px; }
.angulos div span { display: block; font-size: 10px; letter-spacing: 1px; color: var(--tenue); }
.angulos div b { font-size: 20px; font-weight: 400; font-variant-numeric: tabular-nums; }

.ejes { display: flex; justify-content: center; gap: 14px; flex-wrap: wrap; }

/* ============================ Barra de señal ============================ */
.barra { height: 6px; background: #16202d; border-radius: 3px; overflow: hidden; margin-top: 8px; }
.barra i { display: block; height: 100%; width: 0; background: var(--ok); transition: width 0.4s; }

/* ============================ Gráficas ============================ */
canvas { max-height: 170px; }
</style>
</head>

<body>
<div class="contenedor">

  <header>
    <div class="marca">Aero<span>Sense</span> · Estación Terrestre</div>
    <div class="estado">
      <span id="led" class="led"></span>
      <span id="txtEstado">Esperando al globo…</span>
      <span>·</span>
      <span>Paquete <b id="pid">--</b></span>
    </div>
  </header>

  <!-- Cifras principales -->
  <div class="rejilla" style="margin-bottom:14px">
    <div class="tarjeta">
      <p class="titulo">Altitud</p>
      <div class="cifra"><span id="palt">--</span><small>m</small></div>
      <div class="sub">Presión <b id="ppres">--</b> hPa</div>
    </div>
    <div class="tarjeta">
      <p class="titulo">Temperatura</p>
      <div class="cifra"><span id="ptemp">--</span><small>°C</small></div>
      <div class="sub">Humedad <b id="phum">--</b> %</div>
    </div>
    <div class="tarjeta">
      <p class="titulo">Enlace LoRa</p>
      <div class="cifra"><span id="prssi">--</span><small>dBm</small></div>
      <div class="sub" id="pcal">--</div>
      <div class="barra"><i id="barraRssi"></i></div>
    </div>
  </div>

  <div class="rejilla duo">

    <!-- Actitud del módulo -->
    <div class="tarjeta">
      <p class="titulo">Actitud del módulo</p>
      <div class="escena">
        <div class="mundo">
          <div class="suelo"></div>
          <div class="modulo" id="modulo">
            <div class="cara frente"></div>
            <div class="cara atras"></div>
            <div class="cara der"></div>
            <div class="cara izq"></div>
            <div class="cara arriba">ARRIBA</div>
            <div class="cara abajo"></div>
          </div>
        </div>
      </div>
      <div class="angulos">
        <div><span>ALABEO</span><b id="proll">--</b>°</div>
        <div><span>CABECEO</span><b id="ppitch">--</b>°</div>
        <div><span>INCLINACIÓN</span><b id="pincl">--</b>°</div>
      </div>
      <div class="sub ejes">
        <span><b id="pg">--</b> g</span>
        <span>X <b id="pax">--</b></span>
        <span>Y <b id="pay">--</b></span>
        <span>Z <b id="paz">--</b> m/s²</span>
      </div>
    </div>

    <!-- Calidad del aire -->
    <div class="tarjeta">
      <p class="titulo">Material particulado</p>
      <div class="fila"><span>PM1.0</span><b><span id="ppm1">--</span> µg/m³</b></div>
      <div class="fila"><span>PM2.5</span><b><span id="ppm25">--</span> µg/m³</b></div>
      <div class="fila"><span>PM10</span><b><span id="ppm10">--</span> µg/m³</b></div>
      <canvas id="pmChart"></canvas>
    </div>
  </div>

  <div class="rejilla">
    <div class="tarjeta"><p class="titulo">Altitud (m)</p><canvas id="altChart"></canvas></div>
    <div class="tarjeta"><p class="titulo">Temperatura (°C) y humedad (%)</p><canvas id="envChart"></canvas></div>
    <div class="tarjeta"><p class="titulo">RSSI (dBm)</p><canvas id="rssiChart"></canvas></div>

  </div>
</div>

<script>
"use strict";

var INTERVALO_MS = 1000;   // cada cuánto se pide /data
var MAX_PUNTOS = 30;       // puntos visibles en cada gráfica
var RSSI_MIN = -120;       // extremos para la barra de señal
var RSSI_MAX = -10;

/* ============================ Utilidades ============================ */

function $(id) { return document.getElementById(id); }

function hayValor(v) { return v !== null && v !== undefined; }

// Escribe un número en pantalla, o "--" si el dato no llegó.
function texto(id, valor, decimales) {
  var destino = $(id);
  if (!hayValor(valor)) { destino.innerText = "--"; return; }
  destino.innerText = (decimales === undefined) ? valor : Number(valor).toFixed(decimales);
}

function grados(radianes) { return radianes * 180 / Math.PI; }

function calidadRssi(dbm) {
  if (!hayValor(dbm)) return "sin datos";
  if (dbm >= -80) return "Excelente";
  if (dbm >= -100) return "Buena";
  if (dbm >= -110) return "Regular";
  return "Débil";
}

/* ============================ Estado del enlace ============================ */

function pintarEstado(edad) {
  var led = $("led");
  var txt = $("txtEstado");
  led.className = "led";

  if (!hayValor(edad)) { txt.innerText = "Esperando al globo…"; return; }
  if (edad <= 5)       { led.classList.add("vivo");   txt.innerText = "Recibiendo · hace " + edad + " s"; }
  else if (edad <= 30) { led.classList.add("viejo");  txt.innerText = "Señal intermitente · hace " + edad + " s"; }
  else                 { led.classList.add("muerto"); txt.innerText = "Sin señal · hace " + edad + " s"; }
}

function pintarRssi(dbm) {
  texto("prssi", dbm);
  $("pcal").innerText = calidadRssi(dbm);

  var porcentaje = 0;
  if (hayValor(dbm)) {
    porcentaje = (dbm - RSSI_MIN) * 100 / (RSSI_MAX - RSSI_MIN);
    porcentaje = Math.max(0, Math.min(100, porcentaje));
  }
  var barra = $("barraRssi");
  barra.style.width = porcentaje + "%";
  barra.style.background = porcentaje > 60 ? "var(--ok)" : (porcentaje > 30 ? "var(--aviso)" : "var(--alerta)");
}

/* ============================ Actitud ============================ */
// El acelerómetro mide la gravedad, así que con el módulo en reposo su vector apunta "abajo"
// y de ahí salen el alabeo y el cabeceo. En caída o sacudida la lectura deja de ser fiable.
// Si el prisma se inclina al revés que el módulo real, cambia el signo aquí.

function pintarActitud(acc) {
  if (!acc || !hayValor(acc.x)) return;

  var ax = acc.x, ay = acc.y, az = acc.z;
  var magnitud = Math.sqrt(ax * ax + ay * ay + az * az);

  var alabeo = grados(Math.atan2(ay, az));
  var cabeceo = grados(Math.atan2(-ax, Math.sqrt(ay * ay + az * az)));
  var inclinacion = magnitud > 0 ? grados(Math.acos(Math.min(1, Math.abs(az) / magnitud))) : 0;

  $("modulo").style.transform = "rotateZ(" + alabeo.toFixed(1) + "deg) rotateX(" + cabeceo.toFixed(1) + "deg)";

  texto("proll", alabeo, 0);
  texto("ppitch", cabeceo, 0);
  texto("pincl", inclinacion, 0);
  texto("pg", magnitud / 9.81, 2);
  texto("pax", ax, 2);
  texto("pay", ay, 2);
  texto("paz", az, 2);
}

/* ============================ Gráficas ============================ */

var opciones = {
  responsive: true,
  animation: false,
  plugins: { legend: { labels: { color: "#78899c", boxWidth: 10, font: { size: 10 } } } },
  scales: {
    x: { ticks: { color: "#4a5a6b", font: { size: 9 }, maxTicksLimit: 6 }, grid: { color: "#121b26" } },
    y: { ticks: { color: "#78899c", font: { size: 10 } }, grid: { color: "#121b26" } }
  }
};

function nuevaGrafica(id, conjuntos) {
  return new Chart($(id).getContext("2d"), {
    type: "line",
    data: { labels: [], datasets: conjuntos },
    options: opciones
  });
}

function linea(etiqueta, color, relleno) {
  return {
    label: etiqueta, data: [], borderColor: color, borderWidth: 2,
    pointRadius: 0, tension: 0.3,
    fill: relleno === true, backgroundColor: color + "22"
  };
}

var altChart = nuevaGrafica("altChart", [linea("Altitud (m)", "#00d4ff", true)]);
var envChart = nuevaGrafica("envChart", [linea("Temp (°C)", "#ffb020"), linea("Humedad (%)", "#00bcd4")]);
var pmChart = nuevaGrafica("pmChart", [linea("PM2.5", "#e91e63"), linea("PM10", "#9c27b0")]);
var rssiChart = nuevaGrafica("rssiChart", [linea("RSSI (dBm)", "#2ecc71", true)]);

function agregar(grafica, etiqueta, valores) {
  if (grafica.data.labels.length >= MAX_PUNTOS) {
    grafica.data.labels.shift();
    grafica.data.datasets.forEach(function (ds) { ds.data.shift(); });
  }
  grafica.data.labels.push(etiqueta);
  valores.forEach(function (v, i) { grafica.data.datasets[i].data.push(v); });
  grafica.update();
}

/* ============================ Ciclo principal ============================ */

var ultimoIdGraficado = null;

function pintarDatos(d) {
  var pm = d.pm || {};

  pintarEstado(d.edad);
  pintarRssi(d.rssi);
  pintarActitud(d.acc);

  texto("pid", d.id);
  texto("palt", d.alt, 1);
  texto("ppres", d.pres, 1);
  texto("ptemp", d.temp, 1);
  texto("phum", d.hum);
  texto("ppm1", pm.pm1);
  texto("ppm25", pm.pm25);
  texto("ppm10", pm.pm10);

  // Las gráficas solo avanzan con un paquete nuevo: así no se dibujan puntos repetidos
  // mientras no llega nada del globo.
  if (!hayValor(d.edad) || d.id === ultimoIdGraficado) return;
  ultimoIdGraficado = d.id;

  var hora = new Date().toLocaleTimeString();
  agregar(altChart, hora, [d.alt]);
  agregar(envChart, hora, [d.temp, d.hum]);
  agregar(pmChart, hora, [pm.pm25, pm.pm10]);
  agregar(rssiChart, hora, [d.rssi]);
}

function pedirDatos() {
  fetch("/data")
    .then(function (res) { return res.json(); })
    .then(pintarDatos)
    .catch(function () {
      $("led").className = "led muerto";
      $("txtEstado").innerText = "Sin conexión con la estación";
    });
}

setInterval(pedirDatos, INTERVALO_MS);
pedirDatos();
</script>
</body>
</html>
)rawliteral";
