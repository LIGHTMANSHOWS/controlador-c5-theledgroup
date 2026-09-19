# Bridge Architecture

El bridge es el centro del sistema.

```text
xLights localhost
    ↓ Art-Net 510ch
Bridge
    ↓ patch dinámico
C5 por IP
```

## Módulos

```text
ArtNetReceiver
ShowNetworkLoader
SuitLoader
PatchBuilder
FrameRouter
BrightnessProcessor
CurrentLimiter
C5Sender
ControllerMonitor
ZoneTester
WebTabletServer
GuiApp
```

## Flujo de datos

1. `ArtNetReceiver` recibe universos desde `127.0.0.1:6454`.
2. Reconstruye un buffer global de pixeles RGB.
3. `PatchBuilder` sabe dónde va cada pixel global.
4. `BrightnessProcessor` aplica brillo master.
5. `CurrentLimiter` aplica límite de corriente si hace falta.
6. `FrameRouter` arma frames locales para cada C5.
7. `C5Sender` envía 2 paquetes por controlador.
8. `ControllerMonitor` consulta `/status`.
9. `ZoneTester` puede reemplazar temporalmente el stream live.

## Patch global

Formato interno recomendado:

```json
{
  "global_pixel": 1,
  "suit_id": "alas_v1",
  "zone": "ala_izquierda",
  "controller_ip": "192.168.1.201",
  "controller_number": 1,
  "output": 1,
  "output_pixel": 1,
  "universe": 0,
  "channel": 1
}
```

## Importante

El firmware C5 no se modifica para esto. El firmware solo:

- recibe frame local;
- muestra sus salidas;
- reporta `/status`;
- permite OTA/config/tester local.

Toda la inteligencia de traje/show vive en el bridge.

