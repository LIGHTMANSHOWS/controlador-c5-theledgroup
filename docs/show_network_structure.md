# Show Network

El `Show Network` junta varios trajes en un show. xLights no envía a cada controlador; xLights envía todo a localhost.

```text
xLights -> 127.0.0.1:6454 -> Bridge -> C5 201..220
```

## Responsabilidades

### xLights

- Emite Art-Net a `127.0.0.1`.
- Usa universos consecutivos.
- Usa 510 canales útiles por universo.
- No sabe las IP reales de los C5.

### Bridge

- Recibe todo el stream Art-Net local.
- Lee `show_network.json`.
- Sabe qué rango de pixeles pertenece a cada traje.
- Sabe qué C5/IP/salida recibe cada parte.
- Envía protocolo C5 UDP a cada controlador.

## Archivo

```json
{
  "schema": "lightman.show_network.v1",
  "input": {
    "protocol": "artnet",
    "bind": "127.0.0.1",
    "port": 6454,
    "fps": 30,
    "dmx_data_channels": 510
  },
  "suits": []
}
```

## Suit dentro del show

```json
{
  "id": "alas_v1",
  "name": "Traje Alas V1",
  "path": "Suit_Alas_V1/suit.json",
  "start_pixel": 1,
  "pixel_count": 1840,
  "start_universe": 0,
  "start_channel": 1
}
```

## Regla de universos

Con 510 canales útiles:

```text
1 pixel = 3 canales
170 pixeles exactos por universo
canales 511/512 se ignoran
```

## Generación automática

El Show Builder debe calcular automáticamente:

- `start_pixel`;
- `pixel_count`;
- `start_universe`;
- `start_channel`;
- universos totales para xLights;
- patch global para el bridge.

