# AC Event Timer

Plugin para OBS Studio con cuentas regresivas animadas para **eventos y lanzamientos**. A diferencia de los timers que escriben sobre una fuente de texto, este trae su propia fuente con diseño y un dock para controlarlo todo en vivo.

*OBS Studio plugin with animated countdowns for events and launches: its own designed source plus a control dock.*

## Qué hace

- **Tres tipos de timer:** hasta una fecha y hora (lanzamientos), por duración ("empezamos en 5 min") y cronómetro.
- **Cinco estilos:** Bloques, Flip (reloj de aeropuerto), Anillos, Minimal y Misión (T− con barra de progreso).
- **Últimos segundos:** los números cambian de color y laten al ritmo de cada segundo.
- **Al llegar a cero:** mensaje final con confeti, ceros parpadeando o tiempo extra; puede ocultarse solo y **cambiar de escena** automáticamente.
- **Dock de control:** todos los timers en una lista con su tiempo en vivo, Iniciar / Pausar, Reiniciar, −1 min / +1 min y un editor rápido con calendario y atajos ("En 15 min", "Hoy 8 PM"…).
- **Plantillas:** Empezamos pronto, Lanzamiento, Ya volvemos y Cronómetro en vivo. Se añaden a la escena actual, centradas.
- **Atajos de teclado** por timer (Ajustes → Atajos): iniciar/pausar, reiniciar, +1 y −1 minuto.
- Título, subtítulo, fuente, colores, panel de fondo, etiquetas y días automáticos.
- El timer se dibuja en un hilo propio: no se congela aunque OBS tenga un menú o diálogo abierto.

## Uso

1. Abre el dock en **Docks → AC Event Timer**.
2. Pulsa **Nuevo timer** y elige una plantilla.
3. Pulsa **Editar** en la tarjeta para cambiar la fecha o duración, el título, el estilo y qué pasa al terminar. **Todas las opciones…** abre las propiedades completas.

También puedes añadirlo como cualquier fuente: **Fuentes → + → Timer de evento (AC)**.

La opción *Reiniciar y arrancar cuando la fuente salga al aire* hace que un timer de duración empiece solo al entrar a su escena.

## Notas

- La fecha usa la hora del computador.
- El cambio de escena al terminar solo se dispara si el timer llegó a cero con OBS abierto (no al abrir OBS con una fecha ya pasada).
- El estado (en marcha, pausado, tiempo restante) se guarda con la colección de escenas.

## Instalación

Descarga el archivo de tu sistema desde [Releases](../../releases).

Para publicar una versión nueva, sube una rama `release/<versión>` (por ejemplo `release/1.0.0`) o una etiqueta con la versión.

- **macOS:** ejecuta el `.pkg`, o descomprime el `.tar.xz` y copia `ac-event-timer.plugin` en `~/Library/Application Support/obs-studio/plugins/`.
- **Windows:** ejecuta el instalador o descomprime el `.zip` en la carpeta de OBS.
- **Ubuntu:** instala el `.deb`.

Requiere OBS Studio 31.1 o superior.

## Licencia

GPL-2.0-or-later. Basado en [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
