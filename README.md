# Diana Game Streaming (Cliente Android / Android TV)

Cliente de streaming de videojuegos para Android TV y dispositivos móviles, basado en Moonlight / Artemis. Incluye soporte nativo para mandos **Sony DualSense** por USB (audio háptico PCM estéreo y gatillos adaptativos), interfaz Material 3 y actualizaciones automáticas con Obtainium.

---

## 1. Instalación con Obtainium (recomendado)

1. Abre Obtainium y toca **Añadir aplicación (+)**.
2. Pega la URL del repositorio: `https://github.com/marcko4r4/diana-game-streaming`
3. Toca **Añadir**. Obtainium descargará la última versión estable y te avisará de las nuevas.

Para probar betas antes que nadie, activa la opción de pre-releases en los ajustes de la app en Obtainium.

**Versión estable actual:** `v20.4.2` (versionCode `68`). Consulta las [Releases](https://github.com/marcko4r4/diana-game-streaming/releases) para ver el historial.

**Identificador de paquete:** `com.limelight.diana`. Puede instalarse junto a Artemis original (`com.limelight.noir`) sin conflictos.

---

## 2. Novedades de la versión 20.4.2

* **Traducción al español completa:** más de 400 textos que antes aparecían en inglés en ajustes, menús, perfiles, depuración y portapapeles.
* **Renderizado de vídeo corregido:** se eliminó la pérdida de cuadros en 4K60 en Nvidia Shield TV. Se restauró el bucle de renderizado sin descarte de cuadros, se respeta el ritmo de cuadros elegido en ajustes y se desactivaron banderas del decodificador Nvidia que causaban pérdidas.
* **Cifrado de vídeo desactivado:** el cifrado del flujo de vídeo provocaba pérdida de paquetes en juegos 3D a alto bitrate. Los controles y la háptica siguen protegidos por su canal independiente.

---

## 3. Opciones pendientes de reparar

Estas opciones no tienen efecto en la versión actual:

* **LFR (Experimental):** el bucle de renderizado restaurado no usa el modo de último cuadro.
* **Vsync ajustado (Experimental):** sus umbrales no se aplican en el bucle de renderizado.

La latencia ultrabaja sigue funcionando.

---

## 4. Arquitectura

### A. Pipeline de DualSense por USB
1. **Háptica estéreo PCM:** los paquetes de audio llegan por un canal ENet dedicado y se envían a los endpoints isócronos USB, con latencia menor a 2 ms. Las transferencias se suspenden cuando no hay háptica activa.
2. **Gatillos adaptativos:** los reportes de Sunshine se traducen a reportes de salida HID para resistencia, vibración y disparo continuo.
3. **Sensores y sticks:** acelerómetro y giroscopio se activan bajo demanda, con límite de 125 Hz. Los sticks se limitan a `[-32767, 32767]` para evitar desbordamientos.

### B. Vídeo
* El cifrado de vídeo está desactivado a propósito (`streamConfig.encryptionFlags = ENCFLG_AUDIO`).

---

## 5. Compilación

* **Módulo:** `moonlight-android/`
* **SDK:** build-tools `34.0.0`, platform `android-34`
* **NDK:** `27.0.12077973`
* **JDK:** 17

---

## 6. Requisitos del lado del PC (servidor)

Diana es el cliente. Necesitas un servidor compatible en el PC:
* **Apollo** (recomendado): es el servidor que usa todas las funciones avanzadas, como la pantalla virtual, la sincronización del portapapeles y los comandos del servidor.
* **Sunshine**: funciona para streaming básico. Algunas opciones no están disponibles.

---

## 7. Mandos DualSense (Windows, opcional)

Para usar la háptica por USB y los gatillos adaptativos de un DualSense con Apollo, el PC necesita el controlador usbip. El archivo [actualizar_driver_dualsense.bat](actualizar_driver_dualsense.bat) lo instala.

**Cómo usarlo:**
1. Instala y actualiza Apollo en el PC.
2. Haz doble clic en el archivo. Si no tienes permisos de administrador, Windows te los pedirá.
3. Espera al mensaje "Proceso completado". Si Windows pide reiniciar, reinicia el equipo.

**Qué hace:**
* Busca Apollo en las carpetas habituales de Windows. Si lo instalaste en otra ruta, escribe la carpeta cuando te la pida o pásala al ejecutarlo: `actualizar_driver_dualsense.bat "D:\Apollo"`.
* Detiene el servicio de Apollo solo si estaba activo y lo vuelve a iniciar al terminar.
* Instala los dos paquetes del controlador que trae Apollo y elimina las versiones anteriores de esos mismos paquetes. No toca otros controladores.
* Requiere Windows 10 o superior.

Si no usas un DualSense por USB, no necesitas este script.

---

## 8. Privacidad y seguridad

* El repositorio no contiene contraseñas, claves ni tokens. Los datos de conexión de los dispositivos de prueba se mantienen fuera del repositorio.
* Las pruebas de red usaban direcciones locales de la red del autor. Esas direcciones solo aparecen en versiones antiguas del historial de git.
* Para reportar problemas de seguridad, abre un issue en el repositorio sin incluir claves ni datos de tu red.

---

## 9. Créditos

Diana se basa en el trabajo de Moonlight y Artemis. Los cambios del upstream se integran cuando es posible, y el fork mantiene su propio identificador de paquete.
