# Diana Game Streaming

Cliente Android y Android TV para transmitir juegos desde un PC con Vibepollo. Incluye soporte para la háptica y los gatillos adaptativos del mando DualSense por USB, y se actualiza con Obtainium.

> **Estado de las pruebas:** por ahora solo se ha probado en **Nvidia Shield TV**. Otros dispositivos Android pueden funcionar, pero no están verificados.

---

## Instalación con Obtainium

1. Abre Obtainium y toca **Añadir aplicación (+)**.
2. Pega esta URL: `https://github.com/marcko4r4/diana-game-streaming`
3. Toca **Añadir**. Obtainium te avisará de cada nueva versión estable.

Si quieres probar las betas, activa la opción de pre-releases en Obtainium.

**Paquete:** `com.limelight.diana`. Puede instalarse junto a la app Moonlight o Artemis sin conflictos.

---

## Requisitos

* **Android 5.0 o superior**, o Android TV.
* **Vibepollo** en el PC, que es el servidor de transmisión.
* Ambos equipos en la misma red, o con acceso entre ellos.

---

## Novedades de la versión 20.4.2

* **Traducción al español completa** en ajustes, menús, perfiles, depuración y portapapeles.
* **Vídeo estable a 4K60** en Nvidia Shield TV, sin pérdida de cuadros en las pruebas.
* **El ritmo de cuadros elegido se respeta.** Antes se forzaba el modo Balanceado al iniciar.
* **El cifrado del vídeo está desactivado.** Mejora la estabilidad en juegos 3D con alto bitrate. Los controles y la háptica siguen cifrados.

---

## Opciones que aún no funcionan

Estas opciones no tienen efecto en la versión actual:

* **LFR (experimental):** el modo de último cuadro no está activo.
* **Vsync ajustado (experimental):** sus umbrales no se aplican.

La latencia ultrabaja sí funciona.

---

## Mandos DualSense por USB (opcional)

Para usar la háptica y los gatillos adaptativos de un DualSense conectado por USB, el PC necesita el controlador de audio y háptica.

**Forma recomendada:**
1. Ejecuta el instalador de Vibepollo.
2. Marca el componente opcional **"DualSense USB audio and haptics"**. Viene desactivado por defecto.
3. Reinicia Windows si el instalador lo pide.
4. Inicia una nueva transmisión.

Si el controlador no está disponible, la opción de háptica muestra un error antes de iniciar el juego.

**Si instalaste Vibepollo sin ese componente:** puedes usar el script [actualizar_driver_dualsense.bat](actualizar_driver_dualsense.bat). Ejecútalo como administrador. Requiere Windows 10 o superior y Vibepollo instalado.

Si no usas un DualSense por USB, no necesitas nada de esto.

---

## Créditos

Diana se basa en Moonlight y Artemis. Vibepollo y su controlador de DualSense son de sus respectivos autores. Consulta los archivos de licencia del proyecto para más detalles.
