# C5 ESP32-C5 RGB555 6x200

Firmware integrado para usar solo el C5: recibe video UDP, maneja 6 salidas WS281x de 200 pixeles cada una, graba/reproduce desde microSD y conserva la interfaz HTTP de control.

## Versión definitiva: 11 trajes Ninja

El puente definitivo es `dist\C5SenderManagerApp.exe`. Recibe Art-Net desde xLights y asigna siempre un bloque fijo de 1.200 píxeles (3.600 canales RGB) a cada C5. Por ello, aunque cada traje use 936 píxeles, el siguiente controlador conserva su inicio previsto:

| C5 | Píxel inicial | Canal RGB inicial |
| --- | ---: | ---: |
| 1 | 1 | 1 |
| 2 | 1201 | 3601 |
| 3 | 2401 | 7201 |
| 11 | 12001 | 36001 |

El show definitivo de xLights está en `TLG\`: incluye el layout de los 11 trajes, las 78 redes Art-Net, la secuencia y los exports `.xmodel`. Usa el puente en modo automático, sin `patch_map`, para conservar este direccionamiento fijo.

Para crear shows nuevos desde una canción sin alterar esta infraestructura, usa el [contrato de creación para Traffic LED](docs/traffic-led-show-authoring.md).

## Decision de salida LED

En ESP32-C5 no conviene usar I2S para las WS281x. El C5 tiene I2S, pero para seis lineas sincronizadas el periferico correcto es PARLIO TX con GDMA:

- RMT no alcanza para seis salidas porque el ESP32-C5 expone solo dos canales TX.
- PARLIO TX entrega un bus paralelo de hasta 8 bits con DMA.
- Cada byte DMA representa el estado simultaneo de las seis tiras en bits 0..5.
- La codificacion WS usa 3 muestras por bit a 2.4 MHz: alto, dato, bajo.

## Protocolo UDP

- Puerto: `7777`
- Formato pixel: `RGB555_BE`
- Frame logico: `1200` pixeles, `2400` bytes
- Paquetes por frame: `2`
- Pixeles por paquete: `600`
- Payload por paquete: `1200` bytes
- Magic: `C5P6`
- Version: `2`

RGB555 usa palabra big-endian `0RRRRRGGGGGBBBBB`. La expansion interna a 8 bits es simetrica:

```c
out8 = (value5 << 3) | (value5 >> 2);
```

## Pines actuales

Estos valores estan en `main/protocol.h` y se pueden mover si el PCB final ya tiene otro pinout.

| Funcion | GPIO |
| --- | --- |
| LED salida 1 | 0 |
| LED salida 2 | 1 |
| LED salida 3 | 8 |
| LED salida 4 | 9 |
| LED salida 5 | 11 |
| LED salida 6 | 12 |
| microSD CS | 10 |
| microSD SCLK | 4 |
| microSD MISO | 5 |
| microSD MOSI | 6 |
| Reservado | 7 |

## Build

Probado con ESP-IDF `5.5.4`:

```powershell
C:\Espressif\frameworks\esp-idf-v5.5.4-2\export.ps1
idf.py set-target esp32c5
idf.py build
```

Binario generado:

```text
build\c5_esp32c5_rgb555_6x200.bin
```

## Flash

```powershell
idf.py -p COMx flash monitor
```

O usando los argumentos generados por IDF desde `build`:

```powershell
python -m esptool --chip esp32c5 -b 460800 --before default_reset --after hard_reset --no-stub write_flash "@flash_args"
```

## Prueba UDP

Autotest de protocolo en PC:

```powershell
python tools\send_udp_test_frame.py --self-test
```

Enviar un patron:

```powershell
python tools\send_udp_test_frame.py --host 192.168.1.201 --pattern gray-ramp
```

## Configuracion web

Abre:

```text
http://192.168.1.201/config
```

Desde esa pagina se puede editar:

- 6 pines de salida LED
- pines SPI de microSD
- pixeles activos por salida, de 1 a 200
- orden fisico RGB/GRB/BRG/BGR
- grabacion temporal en memoria si no se detecta SD

Los cambios se guardan en NVS. Reinicia desde `/reboot` para aplicar cambios de pines.
`/status` y `/config.json` devuelven la configuracion actual en JSON.

Si no hay microSD y esta activo el fallback de memoria, `/record/start` graba en RAM como
`mem://record`. Es temporal y se pierde al reiniciar.

## Art-Net a LMP3

La herramienta de PC `tools/artnet_to_lmp3.py` captura Art-Net ArtDmx y genera un archivo `.lmp3`
compatible con el reproductor del C5. Usa 6 salidas, 200 pixeles por salida y universos
consecutivos.

Ejemplo:

```powershell
python tools\artnet_to_lmp3.py shows\demo.lmp3 --start-universe 0 --fps 30 --seconds 60 --order RGB
```

Mapa por defecto:

- salida 1: universos 0 y 1
- salida 2: universos 2 y 3
- salida 3: universos 4 y 5
- salida 4: universos 6 y 7
- salida 5: universos 8 y 9
- salida 6: universos 10 y 11

Cada archivo `.lmp3` usa el contenedor `LFS2` interno: cabecera + frames RGB555.

## Notas de hardware

- ESP32-C5 soporta Wi-Fi 6 de 2.4 GHz y 5 GHz; el firmware queda configurado para `REDPIXEL_5G`.
- Las tiras WS281x deben alimentarse con fuente externa adecuada; no desde el modulo C5.
- Comparte GND entre C5, fuente LED y microSD.
- Para instalaciones reales, usa buffer/level shifter 3.3 V a 5 V en las seis lineas de datos.
- Valida los GPIO finales contra el modulo/PCB exacto antes de fabricar o cablear definitivo.
