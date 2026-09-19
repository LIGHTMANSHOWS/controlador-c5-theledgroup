# Sender EXE C5 - estructura propuesta

Objetivo: crear un programa de PC que reciba Art-Net desde Resolume/xLights/otro software, procese brillo, límite de corriente y mapeo multi-controlador, y envíe UDP C5 a hasta 10 controladores ESP32-C5.

## 1. Flujo general

```text
Resolume / xLights
      |
      | Art-Net localhost o red, 510 canales utiles por universo
      v
Sender EXE C5
      |
      | 1) recibe universos Art-Net
      | 2) reconstruye pixeles RGB globales
      | 3) aplica master brightness
      | 4) aplica limitador de corriente
      | 5) convierte a RGB555 con mejor resolucion en brillo bajo
      | 6) reparte pixeles continuos por IP/controlador
      v
Controladores C5
      |
      v
Tiras WS2815
```

## 2. Entradas principales

- Art-Net bind:
  - `127.0.0.1` si Resolume manda a localhost.
  - `0.0.0.0` si se quiere recibir desde otra PC/red.
- Puerto Art-Net: `6454`.
- FPS de salida C5: 25/30/40 seleccionable.
- Orden color de entrada: RGB, GRB, RBG, GBR, BRG, BGR.
- Canales utiles por universo:
  - default `510`.
  - se ignoran canales 511 y 512 para no partir pixeles RGB.

## 3. Brillo con buena resolucion en niveles bajos

El C5 usa RGB555: cada pixel ocupa 2 bytes, con 5 bits por color.

Problema:

- Si se baja brillo de forma simple y luego se convierte a 5 bits, en brillos bajos se pierden muchos pasos.

Propuesta:

1. Mantener el color interno en alta precision, por ejemplo 16 bits por canal.
2. Aplicar brillo global en alta precision.
3. Aplicar limitador de corriente en alta precision.
4. Convertir al final a RGB555 con redondeo.
5. Opcional para despues: dithering temporal para mejorar aun mas los brillos bajos.

Formula base:

```text
rgb16 = rgb8 * 257
rgb16_brightness = rgb16 * master_brightness / 100
rgb16_limited = rgb16_brightness * current_scale
rgb555 = round(rgb16_limited * 31 / 65535)
```

Esto evita recortar demasiado pronto.

## 4. Limitador de corriente

El usuario define:

- Voltaje tira: 12 V.
- Tipo LED: WS2815/BWS2815.
- Corriente maxima permitida: ejemplo `2.5 A`.
- Corriente estimada por pixel en blanco completo: configurable, default sugerido `15 mA` por pixel.

Calculo por frame:

```text
pixel_current = pixel_full_white_mA * max(R, G, B) / 255
total_current = suma(pixel_current de todos los pixeles activos)
```

Si `total_current <= limite`, no se toca.

Si `total_current > limite`, se calcula:

```text
current_scale = limite / total_current
```

Ese `current_scale` se aplica a todo el frame antes de convertir a RGB555.

Resultado:

- Mantiene el color relativo.
- Evita exceder la corriente configurada.
- Reduce brillo automaticamente cuando hay demasiados pixeles encendidos.

Notas:

- La corriente real depende de la tira y la fuente, por eso `mA por pixel` debe ser editable.
- Para WS2815 12 V se deja como parametro, no fijo en codigo.

## 5. Multi-controlador continuo

Cada controlador C5 tiene:

- IP.
- 6 salidas.
- Pixeles logicos por salida.
- 1 pixel fisico reservado de estado/nivelador por salida, que no cuenta como pixel de contenido.
- Capacidad logica total = suma de pixeles logicos de sus 6 salidas.

El sender maneja un lienzo global continuo:

```text
Pixel global 1..N
```

Ejemplo:

```text
C5 #1 192.168.1.201: pixeles globales 1..1125
C5 #2 192.168.1.202: pixeles globales 1126..2250
C5 #3 192.168.1.203: pixeles globales 2251..3375
```

El usuario no tiene que calcular universo/canal por cada IP. El EXE lo calcula y muestra:

```text
C5 #1 termina en universo 6, canal 315
C5 #2 empieza en universo 6, canal 316
```

Importante:

- Art-Net sigue continuo.
- El pixel fisico de estado no consume canal Art-Net.
- El primer pixel Art-Net del controlador va al primer pixel real despues del pixel de estado.

## 6. Salida C5 por controlador

Cada IP recibe su propio frame C5:

- 1200 pixeles maximos por controlador en el protocolo actual.
- 2 paquetes UDP por frame.
- 600 pixeles por paquete.
- 1200 bytes de payload por paquete.

Si un controlador usa menos pixeles, los pixeles sobrantes se mandan en negro.

## 7. Tester desde el EXE

Primera version:

- Off / Live Art-Net.
- Blanco.
- Rojo.
- Verde.
- Azul.
- Salidas: cada salida un color.
- Scan por salida.
- Arcoiris.
- Brillo bajo 5%, 10%, 25% para validar resolucion.

Despues:

- Tester por zonas usando layout de xLights.
- Cargar layout/modelos.
- Seleccionar zona/modelo y encender solo esa zona.

## 8. Pantallas del EXE

### Dashboard

- Estado Art-Net: paquetes/s, universos activos.
- Estado C5 por IP: online/offline, fps, paquetes, drops.
- Brillo master.
- Corriente estimada.
- Corriente limite.
- Escala aplicada por limitador.

### Controladores

Tabla editable:

- Habilitado.
- Nombre.
- IP.
- Pixeles salida 1..6.
- Total logico.
- Rango global calculado.
- Universo/canal inicio.
- Universo/canal fin.

### Entrada Art-Net

- Bind IP.
- Puerto.
- Universo inicial.
- 510/512 canales.
- Orden color.
- FPS salida.

### Brillo y corriente

- Master brightness.
- Corriente maxima A.
- Voltaje.
- mA por pixel blanco.
- Mostrar estimacion en vivo.

### Tester

- Modo tester.
- IP objetivo: todos o uno.
- Salida objetivo: todas o una.
- Color.
- Brillo de tester.
- Duracion.

### Logs

- Frames enviados.
- Paquetes enviados.
- Universos recibidos.
- IPs sin respuesta.
- Drops reportados por C5.

## 9. Servidor web para tablet/celular

El EXE puede levantar un servidor web local para control remoto desde la misma red.

Ejemplo:

```text
PC Sender: 192.168.1.50
Web control: http://192.168.1.50:8080
```

Funciones desde tablet/celular:

- Ver estado live:
  - Art-Net activo.
  - FPS.
  - corriente estimada.
  - limitador activo.
  - C5 conectados/offline.
- Controles seguros de show:
  - START LIVE.
  - PAUSE.
  - BLACKOUT.
  - STOP.
- Tester rápido:
  - todos los controladores o uno específico.
  - todas las salidas o una salida.
  - blanco/rojo/verde/azul/arcoiris/scan.
- Setup básico protegido:
  - brillo master.
  - límite de corriente.
  - modo de entrada.

Recomendación:

- Puerto configurable, default `8080`.
- Opción activar/desactivar servidor web.
- PIN o password simple para evitar cambios accidentales durante show.
- Modo solo lectura opcional para técnicos/cliente.
- Mostrar QR en el EXE para abrir rápido desde el celular.

Arquitectura:

```text
EXE Sender
  ├─ motor Art-Net/C5
  ├─ UI local del EXE
  └─ servidor web interno
        ├─ GET /              pagina tablet
        ├─ GET /api/status    estado JSON
        ├─ POST /api/live     start/pause/stop/blackout
        ├─ POST /api/tester   tester rapido
        └─ POST /api/setup    cambios permitidos
```

La UI web debe ser responsive, con botones grandes para celular.

## 10. Formato de configuracion

Guardar en JSON junto al EXE:

```json
{
  "artnet": {
    "bind": "127.0.0.1",
    "port": 6454,
    "start_universe": 0,
    "dmx_data_channels": 510,
    "fps": 25,
    "order": "RGB"
  },
  "power": {
    "master_brightness_percent": 100,
    "current_limit_a": 2.5,
    "voltage_v": 12,
    "pixel_full_white_ma": 15
  },
  "web_server": {
    "enabled": true,
    "bind": "0.0.0.0",
    "port": 8080,
    "pin": "1234",
    "read_only": false
  },
  "controllers": [
    {
      "enabled": true,
      "name": "C5-201",
      "ip": "192.168.1.201",
      "pixels_per_output": [188, 188, 188, 187, 187, 187]
    }
  ]
}
```

## 11. Etapas de desarrollo

1. Mockup HTML para validar interfaz.
2. Convertir el sender Python actual a arquitectura modular:
   - ArtNetReceiver
   - PixelMapper
   - BrightnessProcessor
   - CurrentLimiter
   - C5Sender
   - Tester
   - ConfigStore
   - WebServer
3. Crear app local con UI.
4. Agregar servidor web responsive para tablet/celular.
5. Empaquetar como EXE.
6. Agregar layout xLights para tester por zonas.
