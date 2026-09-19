# Suit Package

El `Suit Package` describe un traje completo. Es la fuente única para saber:

- cuántos pixeles tiene el traje;
- qué controladores C5 usa;
- cuántos pixeles hay en cada salida;
- qué zonas existen para tester;
- qué orden de color necesita.

El firmware C5 no necesita saber nada del traje. El bridge usa este archivo para cortar el stream global que llega desde xLights por localhost.

## Archivo principal

```text
Suit_Nombre/
  suit.json
  models/
    Nombre.xmodel
  zones.json
```

## Reglas

- xLights siempre emite Art-Net a `127.0.0.1`.
- El traje no define IP de xLights.
- El traje sí define qué C5/IP controla cada salida.
- El primer LED físico de status/nivelador no consume pixel de xLights.
- Cada salida puede tener distinta cantidad de pixeles.
- El bridge es quien deriva pixeles a los C5.

## Campos mínimos de `suit.json`

```json
{
  "schema": "lightman.suit.v1",
  "name": "Traje Alas V1",
  "id": "alas_v1",
  "color_order": "GRB",
  "controllers": []
}
```

## Controller

```json
{
  "number": 1,
  "ip": "192.168.1.201",
  "enabled": true,
  "outputs": [
    { "output": 1, "pixels": 180, "zone": "ala_izquierda" }
  ]
}
```

## Outputs

Cada salida describe pixeles lógicos de contenido. No se cuenta el pixel físico de status.

```json
{ "output": 1, "pixels": 180, "zone": "ala_izquierda" }
```

## Zonas

Las zonas permiten tester por partes:

```json
{
  "zones": [
    { "name": "ala izquierda", "outputs": ["201:1", "201:2"] }
  ]
}
```

