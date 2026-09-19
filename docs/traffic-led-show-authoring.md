# Traffic LED · contrato para crear shows xLights desde una canción

Este documento define las reglas para crear shows de xLights para los trajes Ninja controlados por el puente C5. Convierte una canción en una propuesta de cues editable y ensayable, sin modificar el sistema físico.

## Alcance

Aplica únicamente a Traffic LED: los once trajes Ninja del layout `TLG`. No aplica a banderas, árboles, escenario, Resolume, V20 ni motion tracking.

## Fuente física inmutable

El layout `TLG` es la fuente de verdad: once trajes, `T01` a `T11`. Cada traje conserva un bloque reservado de 1.200 píxeles (3.600 canales RGB), aunque el uso físico actual sea de 936 píxeles. Los bloques comienzan en los canales 1, 3601, 7201 y continúan hasta 36001.

Un show o generador nunca puede cambiar modelos, nombres, orden de píxeles, `StartChannel`, bloques reservados, universos, IPs de controladores, firmware C5 ni cableado. Tampoco puede enviar Art-Net directamente a un C5.

xLights envía el stream completo a `127.0.0.1:6454`, con universos consecutivos y 510 canales útiles por universo (170 píxeles RGB). El bridge C5 es el único componente que conoce las IP reales y entrega cada bloque al traje correspondiente.

## Grupos utilizables

Cada traje tiene grupos ya creados para cabeza, pecho derecho e izquierdo, brazos, antebrazos, manos, espalda, piernas, pies y colecciones globales. Los efectos se construyen sobre esos grupos, nunca sobre líneas individuales como `Single Line` o `Poly Line`.

Un cue puede dirigirse a todos los trajes, a uno protagonista, a una selección explícita, a una parte corporal global o a un lado derecho/izquierdo. Los movimientos en pareja pueden ser iguales o espejo. El preview y el ensayo validan el resultado físico; no se presupone simetría solo por el nombre.

## Entrada requerida por canción

La canción aporta tempo, compases, secciones, golpes, pausas y energía. El brief debe añadir título, duración, BPM confirmado, trajes T01–T11 participantes, estilo visual, paleta, energía, solos/dúos/conjunto, restricciones de brillo y estrobo, e inicio y final coreográfico.

La canción no define por sí sola la coreografía, el traje protagonista ni la seguridad física. Si falta una decisión, el resultado debe marcarla como pendiente de dirección artística.

## Formato de cue

Cada cue editable debe declarar inicio y fin exactos; sección y función musical (golpe, subida, pausa, transición, solo o cierre); destino corporal existente; efecto; paleta; brillo; dirección; relación izquierda/derecha; y estado `propuesto`, `aprobado en preview` o `aprobado en ensayo`.

Los silencios visuales forman parte del diseño. No se rellenan automáticamente los grupos ni se mantienen todos los trajes encendidos.

## Límites de salida y seguridad

El C5 recibe frames RGB555: los colores demasiado sutiles pueden perderse a brillo bajo. El bridge aplica brillo, gamma, dithering temporal y límite de corriente. Los cues no deben depender de blanco continuo ni de todos los píxeles a máxima intensidad como base.

El operador debe disponer siempre de `PAUSE`, `BLACKOUT` y `STOP`. Cada show debe tener inicio controlado, blackout seguro y salida sin estados visuales imprevistos.

La referencia es 30 FPS, salvo que el operador configure otra tasa y la valide. La aprobación exige comprobar audio, xLights, bridge y trajes; el editor no sustituye el ensayo físico.

## Prompt operativo

> Analiza esta canción para el layout Traffic LED `TLG` de once trajes Ninja. Devuelve una propuesta de cues editable basada en BPM, compases, secciones, golpes, pausas y energía. Usa únicamente grupos existentes de T01–T11 y sus colecciones corporales. Para cada cue indica tiempo, destino, función musical, efecto, paleta, intensidad, dirección, regla izquierda/derecha y estado de aprobación. Conserva silencios visuales cuando correspondan. No modifiques modelos, StartChannel, universos, red, IPs, firmware, cableado ni la configuración del bridge. Marca como pendiente toda decisión coreográfica que la canción no pueda determinar.

## Validación antes de entregar

1. Abrir el show sobre `TLG` sin cambiar red ni modelos.
2. Verificar que cada cue apunta a un grupo existente.
3. Revisar en preview los movimientos derecha/izquierda y protagonismos.
4. Renderizar sin activar salida a luces.
5. Probar con brillo limitado y comprobar FPS, pérdida de paquetes y respuesta de los trajes presentes.
6. Ensayar con audio antes de declarar el show aprobado.
