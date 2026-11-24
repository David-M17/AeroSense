"""Genera un modelo 3D limpio (GLB) del módulo AeroSense.

Uso (desde la raíz del repo):
    python herramientas/modelo_3d/generar_modelo.py herramientas/modelo_3d/modulo_aerosense.glb \
        --header receptor/include/modelo_glb.h

Requiere numpy y Pillow.

Ejes: X = ancho (11 cm), Y = alto (23 cm), Z = profundidad (5 cm).
La tapa (con el sensor de PM) mira hacia +Z; la base (batería, ESP32, LoRa) hacia -Z.
Unidades en metros, origen en el centro de la caja.
"""
import io
import json
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H, D = 0.110, 0.230, 0.050
TOP = H / 2


# ----------------------------------------------------------------- geometría
class Malla:
    """Triángulos con normales planas (vértices sin compartir)."""

    def __init__(self):
        self.pos, self.nor, self.uv = [], [], []

    def tri(self, a, b, c, uva=(0, 0), uvb=(0, 0), uvc=(0, 0)):
        a, b, c = map(np.asarray, (a, b, c))
        n = np.cross(b - a, c - a)
        n = n / (np.linalg.norm(n) or 1)
        for p, t in ((a, uva), (b, uvb), (c, uvc)):
            self.pos.append(p)
            self.nor.append(n)
            self.uv.append(t)

    def quad(self, a, b, c, d, uv=((0, 1), (1, 1), (1, 0), (0, 0))):
        self.tri(a, b, c, uv[0], uv[1], uv[2])
        self.tri(a, c, d, uv[0], uv[2], uv[3])

    def extender(self, otra):
        self.pos += otra.pos
        self.nor += otra.nor
        self.uv += otra.uv
        return self


def caja(centro, tam):
    cx, cy, cz = centro
    hx, hy, hz = np.asarray(tam) / 2
    v = lambda sx, sy, sz: (cx + sx * hx, cy + sy * hy, cz + sz * hz)
    m = Malla()
    m.quad(v(-1, -1, 1), v(1, -1, 1), v(1, 1, 1), v(-1, 1, 1))      # +Z
    m.quad(v(1, -1, -1), v(-1, -1, -1), v(-1, 1, -1), v(1, 1, -1))  # -Z
    m.quad(v(1, -1, 1), v(1, -1, -1), v(1, 1, -1), v(1, 1, 1))      # +X
    m.quad(v(-1, -1, -1), v(-1, -1, 1), v(-1, 1, 1), v(-1, 1, -1))  # -X
    m.quad(v(-1, 1, 1), v(1, 1, 1), v(1, 1, -1), v(-1, 1, -1))      # +Y
    m.quad(v(-1, -1, -1), v(1, -1, -1), v(1, -1, 1), v(-1, -1, 1))  # -Y
    return m


def perfil_redondeado(w, h, r, seg=8):
    """Contorno de un rectángulo con esquinas redondeadas (sentido antihorario)."""
    pts = []
    for cx, cy, a0 in ((w / 2 - r, -h / 2 + r, -90), (w / 2 - r, h / 2 - r, 0),
                       (-w / 2 + r, h / 2 - r, 90), (-w / 2 + r, -h / 2 + r, 180)):
        for i in range(seg + 1):
            a = np.radians(a0 + 90 * i / seg)
            pts.append((cx + r * np.cos(a), cy + r * np.sin(a)))
    return pts


def prisma_redondeado(centro, w, h, z0, z1, r, seg=8, tapas=True):
    """Rectángulo redondeado en XY, extruido de z0 a z1."""
    cx, cy, _ = centro
    pts = [(cx + x, cy + y) for x, y in perfil_redondeado(w, h, r, seg)]
    m = Malla()
    c0, c1 = (cx, cy, z0), (cx, cy, z1)
    for i in range(len(pts)):
        (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % len(pts)]
        m.quad((x0, y0, z0), (x1, y1, z0), (x1, y1, z1), (x0, y0, z1))
        if tapas:
            m.tri(c1, (x0, y0, z1), (x1, y1, z1))
            m.tri(c0, (x1, y1, z0), (x0, y0, z0))
    return m


def cilindro(centro, eje, radio, largo, seg=20):
    eje = np.asarray(eje, float) / np.linalg.norm(eje)
    u = np.cross(eje, (0, 1, 0) if abs(eje[1]) < 0.9 else (1, 0, 0))
    u /= np.linalg.norm(u)
    v = np.cross(eje, u)
    c = np.asarray(centro, float)
    a, b = c - eje * largo / 2, c + eje * largo / 2
    m = Malla()
    for i in range(seg):
        t0, t1 = 2 * np.pi * i / seg, 2 * np.pi * (i + 1) / seg
        d0 = radio * (np.cos(t0) * u + np.sin(t0) * v)
        d1 = radio * (np.cos(t1) * u + np.sin(t1) * v)
        m.quad(a + d0, a + d1, b + d1, b + d0)
        m.tri(b, b + d0, b + d1)
        m.tri(a, a + d1, a + d0)
    return m


def tubo(puntos, radio, seg=6, pasos=24):
    """Tubo a lo largo de una curva Bézier (para los cables)."""
    P = np.asarray(puntos, float)
    n = len(P) - 1
    from math import comb
    t = np.linspace(0, 1, pasos)[:, None]
    curva = sum(comb(n, k) * (1 - t) ** (n - k) * t ** k * P[k] for k in range(n + 1))
    m = Malla()
    anillos = []
    for i, p in enumerate(curva):
        tan = curva[min(i + 1, pasos - 1)] - curva[max(i - 1, 0)]
        tan /= np.linalg.norm(tan)
        u = np.cross(tan, (0, 0, 1) if abs(tan[2]) < 0.9 else (1, 0, 0))
        u /= np.linalg.norm(u)
        v = np.cross(tan, u)
        anillos.append([(p, np.cos(a) * u + np.sin(a) * v)
                        for a in np.linspace(0, 2 * np.pi, seg, endpoint=False)])
    for r0, r1 in zip(anillos, anillos[1:]):
        for j in range(seg):
            k = (j + 1) % seg
            for (c, n) in (r0[j], r0[k], r1[k], r0[j], r1[k], r1[j]):
                m.pos.append(c + radio * n)
                m.nor.append(n)
                m.uv.append((0, 0))
    return m


def placa_xz(centro, tx, tz, grosor=0.0016):
    """Placa acostada sobre la cara superior (plano XZ)."""
    return caja(centro, (tx, grosor, tz))


# ----------------------------------------------------------------- materiales
def mat(nombre, rgb, rough=0.6, metal=0.0, alpha=1.0, emisivo=None, textura=None):
    m = {"name": nombre, "doubleSided": alpha < 1,
         "pbrMetallicRoughness": {"baseColorFactor": [*rgb, alpha],
                                  "metallicFactor": metal, "roughnessFactor": rough}}
    if alpha < 1:
        m["alphaMode"] = "BLEND"
    if emisivo:
        m["emissiveFactor"] = emisivo
    if textura is not None:
        m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": textura}
    return m


MATERIALES = {
    "carcasa":  mat("Carcasa humo", (0.70, 0.74, 0.79), rough=0.55, alpha=0.16),
    "junta":    mat("Junta tapa", (0.70, 0.74, 0.78), rough=0.4, alpha=0.55),
    "pcb_neg":  mat("PCB negro", (0.04, 0.04, 0.05), rough=0.5),
    "pcb_azul": mat("PCB azul", (0.06, 0.20, 0.62), rough=0.45),
    "pcb_verde": mat("PCB verde", (0.05, 0.42, 0.22), rough=0.45),
    "chip":     mat("Chip", (0.02, 0.02, 0.02), rough=0.4),
    "metal":    mat("Blindaje", (0.75, 0.77, 0.80), rough=0.3, metal=0.9),
    "oro":      mat("Oro SMA", (0.95, 0.72, 0.25), rough=0.25, metal=1.0),
    "bateria":  mat("Batería", (0.06, 0.06, 0.07), rough=0.55),
    "dht":      mat("DHT22", (0.93, 0.93, 0.92), rough=0.7),
    "led_rojo": mat("LED rojo", (1, 0.1, 0.05), emisivo=[1, 0.1, 0.05]),
    "led_azul": mat("LED azul", (0.3, 0.5, 1), emisivo=[0.3, 0.5, 1]),
    "etiqueta": mat("Etiqueta AeroSense", (1, 1, 1), rough=0.6, textura=0),
    "cable_rojo": mat("Cable rojo", (0.85, 0.12, 0.10)),
    "cable_azul": mat("Cable azul", (0.15, 0.30, 0.85)),
    "cable_verde": mat("Cable verde", (0.05, 0.50, 0.40)),
    "cable_naranja": mat("Cable naranja", (0.95, 0.55, 0.10)),
    "cable_amarillo": mat("Cable amarillo", (0.95, 0.85, 0.15)),
    "cable_blanco": mat("Cable blanco", (0.90, 0.90, 0.90)),
    "cable_morado": mat("Cable morado", (0.50, 0.25, 0.60)),
    "cable_negro": mat("Cable negro", (0.08, 0.08, 0.08)),
}


def textura_etiqueta():
    """Cinta negra con 'AeroSense' a mano, como la del módulo real."""
    img = Image.new("RGB", (512, 96), (12, 12, 14))
    d = ImageDraw.Draw(img)
    fuente = None
    for f in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"):
        try:
            fuente = ImageFont.truetype(f, 62)
            break
        except OSError:
            pass
    d.text((256, 50), "AeroSense", fill=(235, 235, 235), anchor="mm",
           font=fuente or ImageFont.load_default())
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


# ----------------------------------------------------------------- ensamblado
def construir():
    partes = []  # (nombre, malla, material)
    agregar = lambda n, m, k: partes.append((n, m, k))

    # Carcasa tipo almeja: base (-Z) y tapa (+Z) con esquinas redondeadas
    agregar("Carcasa", prisma_redondeado((0, 0, 0), W, H, -D / 2, D / 2, 0.012, seg=10), "carcasa")
    agregar("Junta", prisma_redondeado((0, 0, 0), W + 0.0014, H + 0.0014, -0.0008, 0.0008, 0.0127,
                                       seg=10, tapas=False), "junta")

    # Asa colgante en el tope (marco con ranura)
    asa_z, asa_y = -0.006, TOP + 0.011
    asa = Malla()
    for c, t in (((0, asa_y + 0.0085, asa_z), (0.060, 0.005, 0.002)),
                 ((0, asa_y - 0.0085, asa_z), (0.060, 0.005, 0.002)),
                 ((-0.0275, asa_y, asa_z), (0.005, 0.022, 0.002)),
                 ((0.0275, asa_y, asa_z), (0.005, 0.022, 0.002))):
        asa.extender(caja(c, t))
    agregar("Asa", asa, "junta")

    # Broche lateral
    agregar("Broche", caja((-W / 2 - 0.002, 0.0, 0.004), (0.004, 0.026, 0.020)), "junta")

    # Batería (abajo, en la base)
    agregar("Bateria", prisma_redondeado((0, -0.088, 0), 0.096, 0.044, -0.024, -0.002, 0.010), "bateria")
    agregar("Bateria USB-C", caja((-0.030, -0.074, -0.0018), (0.009, 0.003, 0.0006)), "metal")
    leds = Malla()
    for i in range(4):
        leds.extender(caja((-0.006 + i * 0.004, -0.106, -0.0016), (0.002, 0.002, 0.0006)))
    agregar("Bateria LEDs", leds, "led_azul")

    # Sensor PM (pegado a la tapa, frente a la batería)
    pm_c = (-0.020, -0.082, 0.0165)
    agregar("PMS7003M", caja(pm_c, (0.048, 0.037, 0.012)), "metal")
    agregar("PMS ventilador", cilindro((pm_c[0] - 0.008, pm_c[1] + 0.003, pm_c[2] + 0.0062),
                                        (0, 0, 1), 0.0085, 0.0008), "chip")
    agregar("PMS rejilla", cilindro((pm_c[0] - 0.008, pm_c[1] + 0.003, pm_c[2] + 0.0068),
                                    (0, 0, 1), 0.0045, 0.0006), "metal")
    rej = Malla()
    for i in range(3):
        rej.extender(caja((pm_c[0] + 0.012 + i * 0.0035, pm_c[1] + 0.003, pm_c[2] + 0.0063),
                          (0.0015, 0.010, 0.0006)))
    agregar("PMS ventila", rej, "chip")
    agregar("PMS conector", caja((pm_c[0] - 0.012, pm_c[1] + 0.024, pm_c[2] - 0.002),
                                 (0.017, 0.013, 0.0016)), "pcb_verde")

    # ESP32 sobre su placa de expansión (centro, en la base)
    esp_c = (0.010, -0.018, -0.0215)
    agregar("Shield ESP32", caja(esp_c, (0.062, 0.058, 0.0016)), "pcb_neg")
    agregar("ESP32 DevKit", caja((esp_c[0], esp_c[1], esp_c[2] + 0.004), (0.028, 0.052, 0.0016)), "pcb_neg")
    agregar("ESP32 WROOM", caja((esp_c[0], esp_c[1] + 0.012, esp_c[2] + 0.0055), (0.018, 0.025, 0.003)), "metal")
    agregar("ESP32 LED", caja((esp_c[0] + 0.004, esp_c[1] - 0.008, esp_c[2] + 0.0052),
                              (0.0016, 0.0016, 0.001)), "led_rojo")
    pines = Malla()
    for dx in (-0.026, -0.0125, 0.0125, 0.026):
        pines.extender(caja((esp_c[0] + dx, esp_c[1], esp_c[2] + 0.0028), (0.0024, 0.048, 0.0040)))
    agregar("Headers", pines, "chip")

    # DHT22 en la pared izquierda: la rejilla atraviesa un recorte rectangular de la caja
    dht_c = (-W / 2 + 0.0055, 0.030, -0.006)
    sx = -W / 2 - 0.0033  # centro del sensor: sobresale ~3 mm por fuera de la pared
    agregar("DHT22 PCB", caja((-W / 2 + 0.0055, dht_c[1] - 0.006, dht_c[2]), (0.0016, 0.034, 0.016)), "pcb_neg")
    agregar("DHT22", caja((sx + 0.0015, dht_c[1], dht_c[2]), (0.0078, 0.025, 0.0151)), "dht")
    rejilla = Malla()
    for fy in range(5):
        for fz in range(3):
            rejilla.extender(caja((sx - 0.0026, dht_c[1] - 0.008 + fy * 0.004,
                                   dht_c[2] - 0.004 + fz * 0.004), (0.0004, 0.0022, 0.0022)))
    agregar("DHT22 rejilla", rejilla, "chip")
    recorte = Malla()
    for c, t in (((-W / 2 - 0.0002, dht_c[1] + 0.0135, dht_c[2]), (0.0006, 0.0012, 0.0175)),
                 ((-W / 2 - 0.0002, dht_c[1] - 0.0135, dht_c[2]), (0.0006, 0.0012, 0.0175)),
                 ((-W / 2 - 0.0002, dht_c[1], dht_c[2] + 0.0082), (0.0006, 0.026, 0.0012)),
                 ((-W / 2 - 0.0002, dht_c[1], dht_c[2] - 0.0082), (0.0006, 0.026, 0.0012))):
        recorte.extender(caja(c, t))
    agregar("Recorte DHT22", recorte, "junta")

    # Módulo LoRa E220 (arriba a la derecha) + conector SMA hacia afuera
    lora_c = (0.027, 0.090, -0.020)
    agregar("E220 PCB", caja(lora_c, (0.036, 0.021, 0.0016)), "pcb_verde")
    agregar("E220 blindaje", caja((lora_c[0] - 0.003, lora_c[1], lora_c[2] + 0.0018),
                                  (0.024, 0.016, 0.002)), "metal")
    agregar("SMA cuerpo", cilindro((W / 2 + 0.004, lora_c[1] + 0.002, lora_c[2]), (1, 0, 0), 0.0032, 0.016), "oro")
    agregar("SMA tuerca", cilindro((W / 2 + 0.0015, lora_c[1] + 0.002, lora_c[2]), (1, 0, 0), 0.0045, 0.003), "oro")
    agregar("E220 header", caja((lora_c[0] - 0.0185, lora_c[1], lora_c[2] + 0.003),
                                (0.0025, 0.018, 0.005)), "chip")

    # ADXL345 y BMP180 atravesando el tope de la caja.
    # Ejes del ADXL345 montado: X del sensor -> -X del modelo (hacia el DHT22),
    # Y del sensor -> +Z del modelo (hacia la etiqueta), Z del sensor -> +Y (hacia arriba).
    adxl_c = (0.008, TOP + 0.0009, -0.004)
    agregar("ADXL345", placa_xz(adxl_c, 0.020, 0.015), "pcb_azul")
    agregar("ADXL345 chip", caja((adxl_c[0], adxl_c[1] + 0.0012, adxl_c[2]), (0.005, 0.0012, 0.005)), "chip")
    bmp_c = (-0.022, TOP + 0.0009, -0.004)
    agregar("BMP180", placa_xz(bmp_c, 0.013, 0.010), "pcb_azul")
    agregar("BMP180 sensor", caja((bmp_c[0] + 0.002, bmp_c[1] + 0.0012, bmp_c[2]), (0.0035, 0.0012, 0.0035)), "metal")

    # Etiqueta AeroSense en la tapa (con textura)
    ez, ey = D / 2 + 0.0003, 0.085
    etq = Malla()
    etq.quad((-0.045, ey - 0.009, ez), (0.045, ey - 0.009, ez), (0.045, ey + 0.009, ez), (-0.045, ey + 0.009, ez),
             uv=((0, 1), (1, 1), (1, 0), (0, 0)))
    agregar("Etiqueta", etq, "etiqueta")

    # Cableado (curvas Bézier desde el ESP32 hacia cada módulo)
    e = np.array(esp_c) + (0, 0, 0.006)
    cables = [
        ("cable_amarillo", [e + (0.004, 0.020, 0), (0.030, 0.045, -0.005), (0.020, 0.070, -0.010), lora_c + np.array((-0.018, -0.006, 0.004))]),
        ("cable_naranja", [e + (0.006, 0.020, 0), (0.040, 0.050, -0.004), (0.015, 0.075, -0.008), lora_c + np.array((-0.018, -0.003, 0.004))]),
        ("cable_rojo", [e + (-0.010, 0.022, 0), (-0.020, 0.060, 0.004), (-0.010, 0.100, 0.002), adxl_c + np.array((-0.006, -0.002, 0))]),
        ("cable_azul", [e + (-0.008, 0.022, 0), (-0.012, 0.065, 0.006), (0.000, 0.100, 0.004), bmp_c + np.array((-0.004, -0.002, 0))]),
        ("cable_verde", [e + (-0.006, 0.022, 0), (-0.005, 0.070, 0.008), (0.006, 0.100, 0.006), adxl_c + np.array((0.006, -0.002, 0))]),
        ("cable_morado", [e + (-0.014, 0.010, 0), (-0.035, 0.020, 0.000), (-0.040, 0.025, -0.006), dht_c + np.array((0.0012, -0.020, 0))]),
        ("cable_blanco", [e + (-0.014, -0.010, 0), (-0.030, -0.030, 0.010), (-0.025, -0.050, 0.012), np.array(pm_c) + (-0.012, 0.030, -0.002)]),
        ("cable_negro", [e + (0.020, -0.020, 0), (0.045, -0.040, -0.010), (0.040, -0.060, -0.012), (0.030, -0.070, -0.010)]),
    ]
    for color, pts in cables:
        agregar(f"Cable {color.split('_')[1]}", tubo(pts, 0.0007), color)

    return partes


# ----------------------------------------------------------------- exportar GLB
def exportar(partes, ruta):
    nombres_mat = list(MATERIALES)
    blob = bytearray()
    vistas, accesores, mallas, nodos = [], [], [], []

    def vista(datos, objetivo=None):
        while len(blob) % 4:
            blob.append(0)
        v = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(datos)}
        if objetivo:
            v["target"] = objetivo
        blob.extend(datos)
        vistas.append(v)
        return len(vistas) - 1

    def accesor(arr, tipo, minmax=False):
        a = {"bufferView": vista(arr.astype(np.float32).tobytes(), 34962),
             "componentType": 5126, "count": len(arr), "type": tipo}
        if minmax:
            a["min"], a["max"] = arr.min(0).tolist(), arr.max(0).tolist()
        accesores.append(a)
        return len(accesores) - 1

    for nombre, m, k in partes:
        filas = np.hstack([np.array(m.pos), np.array(m.nor), np.array(m.uv)]).astype(np.float32)
        unicas, indices = np.unique(np.round(filas, 6), axis=0, return_inverse=True)
        attrs = {"POSITION": accesor(unicas[:, 0:3], "VEC3", True),
                 "NORMAL": accesor(unicas[:, 3:6], "VEC3")}
        if k == "etiqueta":
            attrs["TEXCOORD_0"] = accesor(unicas[:, 6:8], "VEC2")
        idx = indices.ravel().astype(np.uint16)
        accesores.append({"bufferView": vista(idx.tobytes(), 34963), "componentType": 5123,
                          "count": len(idx), "type": "SCALAR"})
        prim = {"attributes": attrs, "indices": len(accesores) - 1, "material": nombres_mat.index(k)}
        mallas.append({"name": nombre, "primitives": [prim]})
        nodos.append({"name": nombre, "mesh": len(mallas) - 1})

    img_vista = vista(textura_etiqueta())
    gltf = {
        "asset": {"version": "2.0", "generator": "AeroSense generar_modelo.py"},
        "scene": 0,
        "scenes": [{"name": "AeroSense", "nodes": [len(nodos)]}],
        "nodes": nodos + [{"name": "ModuloAeroSense", "children": list(range(len(nodos)))}],
        "meshes": mallas,
        "materials": [MATERIALES[k] for k in nombres_mat],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9729}],
        "images": [{"bufferView": img_vista, "mimeType": "image/png"}],
        "accessors": accesores,
        "bufferViews": vistas,
        "buffers": [{"byteLength": len(blob)}],
    }
    while len(blob) % 4:
        blob.append(0)
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * ((-len(js)) % 4)
    total = 12 + 8 + len(js) + 8 + len(blob)
    with open(ruta, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<I4s", len(js), b"JSON") + js)
        f.write(struct.pack("<I4s", len(blob), b"BIN\x00") + bytes(blob))
    return total


def exportar_header(ruta_glb, ruta_h):
    """Guarda el GLB comprimido con gzip como arreglo PROGMEM para el ESP32."""
    import gzip
    datos = gzip.compress(open(ruta_glb, "rb").read(), compresslevel=9, mtime=0)
    filas = [", ".join(f"0x{b:02x}" for b in datos[i:i + 20]) for i in range(0, len(datos), 20)]
    with open(ruta_h, "w") as f:
        f.write("#pragma once\n#include <Arduino.h>\n\n")
        f.write("// Modelo 3D del módulo (GLB comprimido con gzip). Archivo GENERADO:\n")
        f.write("// no editar a mano, se regenera con herramientas/modelo_3d/generar_modelo.py\n")
        f.write(f"static const size_t MODELO_GLB_GZ_LEN = {len(datos)};\n")
        f.write("static const uint8_t MODELO_GLB_GZ[] PROGMEM = {\n  " + ",\n  ".join(filas) + "\n};\n")
    return len(datos)


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description="Genera el modelo 3D del módulo AeroSense")
    p.add_argument("salida", nargs="?", default="modulo_aerosense.glb", help="ruta del .glb")
    p.add_argument("--header", help="también escribe el .h para el receptor (gzip + PROGMEM)")
    args = p.parse_args()
    n = exportar(construir(), args.salida)
    print(f"{args.salida}: {n / 1024:.1f} KB")
    if args.header:
        print(f"{args.header}: {exportar_header(args.salida, args.header) / 1024:.1f} KB comprimido")
