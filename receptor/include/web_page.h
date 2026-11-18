#pragma once
#include <Arduino.h>

// Página del panel web. Se edita aquí como HTML normal (no hay que escapar comillas).
// Pide /data cada 2 s y dibuja las gráficas con Chart.js (el navegador necesita internet para el CDN).
static const char PAGINA_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>AeroSense - Estación Terrestre</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
  body { font-family: Arial, sans-serif; background: #121212; color: #e0e0e0; text-align: center; margin: 0; padding: 15px; }
  .card { background: #1e1e1e; max-width: 600px; margin: 15px auto; padding: 15px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); }
  h1 { color: #4CAF50; font-size: 22px; }
  h3 { color: #e0e0e0; font-size: 16px; margin-bottom: 5px; }
  .data { font-size: 15px; margin: 6px 0; }
  .highlight { color: #2196F3; font-weight: bold; }
  canvas { background: #121212; border-radius: 8px; margin-top: 10px; max-height: 160px; }
</style>
</head>
<body>
<h1>🛰️ AeroSense · Estación Terrestre (Globo amarrado 100 m)</h1>

<div class="card">
  <p class="data">Paquete ID: <span id="pid" class="highlight">--</span> | Último hace <span id="page" class="highlight">--</span> s</p>
  <p class="data">🏔️ Altitud: <span id="palt" class="highlight">--</span> m</p>
  <p class="data">🌡️ Temperatura: <span id="ptemp" class="highlight">--</span> °C | 💧 Humedad: <span id="phum" class="highlight">--</span> %</p>
  <p class="data">🌫️ PM1.0: <span id="ppm1" class="highlight">--</span> | PM2.5: <span id="ppm25" class="highlight">--</span> | PM10: <span id="ppm10" class="highlight">--</span> µg/m³</p>
  <p class="data">🔵 Presión: <span id="ppres" class="highlight">--</span> hPa</p>
  <p class="data">📶 RSSI: <span id="prssi" class="highlight">--</span> dBm (<span id="pcal">--</span>)</p>
</div>

<div class="card"><h3>📈 Altitud (m)</h3><canvas id="altChart"></canvas></div>
<div class="card"><h3>📈 Temperatura (°C) y Humedad (%)</h3><canvas id="envChart"></canvas></div>
<div class="card"><h3>📈 Calidad del aire - Material particulado (µg/m³)</h3><canvas id="pmChart"></canvas></div>
<div class="card"><h3>📈 Señal LoRa - RSSI (dBm)</h3><canvas id="rssiChart"></canvas></div>

<script>
const MAX_PUNTOS = 20;
const opciones = { responsive: true, animation: false,
  scales: { x: { ticks: { color: '#888' } }, y: { ticks: { color: '#e0e0e0' } } } };

function nuevaGrafica(id, datasets) {
  return new Chart(document.getElementById(id).getContext('2d'), {
    type: 'line', data: { labels: [], datasets: datasets }, options: opciones
  });
}

const altChart = nuevaGrafica('altChart', [
  { label: 'Altitud (m)', data: [], borderColor: '#2196F3', backgroundColor: 'rgba(33,150,243,0.1)', fill: true, tension: 0.3 }
]);
const envChart = nuevaGrafica('envChart', [
  { label: 'Temp (°C)', data: [], borderColor: '#FF9800', tension: 0.3 },
  { label: 'Humedad (%)', data: [], borderColor: '#00BCD4', tension: 0.3 }
]);
const pmChart = nuevaGrafica('pmChart', [
  { label: 'PM2.5', data: [], borderColor: '#E91E63', tension: 0.3 },
  { label: 'PM10', data: [], borderColor: '#9C27B0', tension: 0.3 }
]);
const rssiChart = nuevaGrafica('rssiChart', [
  { label: 'RSSI (dBm)', data: [], borderColor: '#4CAF50', backgroundColor: 'rgba(76,175,80,0.1)', fill: true, tension: 0.3 }
]);

function agregar(grafica, etiqueta, valores) {
  if (grafica.data.labels.length >= MAX_PUNTOS) {
    grafica.data.labels.shift();
    grafica.data.datasets.forEach(function (ds) { ds.data.shift(); });
  }
  grafica.data.labels.push(etiqueta);
  valores.forEach(function (v, i) { grafica.data.datasets[i].data.push(v); });
  grafica.update();
}

function calidadRssi(dbm) {
  if (dbm === null || dbm === undefined) return '--';
  if (dbm >= -80) return 'Excelente';
  if (dbm >= -100) return 'Buena';
  if (dbm >= -110) return 'Regular';
  return 'Débil';
}

function texto(id, valor, decimales) {
  var t = '--';
  if (valor !== null && valor !== undefined) {
    t = (decimales === undefined) ? valor : Number(valor).toFixed(decimales);
  }
  document.getElementById(id).innerText = t;
}

function pedirDatos() {
  fetch('/data').then(function (res) { return res.json(); }).then(function (d) {
    var pm = d.pm || {};
    texto('pid', d.id);
    texto('page', d.edad);
    texto('palt', d.alt, 1);
    texto('ptemp', d.temp, 1);
    texto('phum', d.hum);
    texto('ppm1', pm.pm1);
    texto('ppm25', pm.pm25);
    texto('ppm10', pm.pm10);
    texto('ppres', d.pres, 1);
    texto('prssi', d.rssi);
    document.getElementById('pcal').innerText = calidadRssi(d.rssi);

    if (d.edad === null || d.edad === undefined) return; // aún no llega ningún paquete

    var hora = new Date().toLocaleTimeString();
    agregar(altChart, hora, [d.alt]);
    agregar(envChart, hora, [d.temp, d.hum]);
    agregar(pmChart, hora, [pm.pm25, pm.pm10]);
    agregar(rssiChart, hora, [d.rssi]);
  }).catch(function () {
    document.getElementById('page').innerText = 'sin conexión';
  });
}

setInterval(pedirDatos, 2000);
pedirDatos();
</script>
</body>
</html>
)rawliteral";
