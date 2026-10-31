---
description: Reglas estrictas de versionado, nombres de release y control de fases Beta vs Estable
globs: ["**/*"]
---

# Política de Versionado y Releases (Diana Game Streaming)

## 1. Regla Fundamental de Fases
* **Fase Beta / Pruebas (Por Defecto):**
  - Todo cambio, corrección de bugs, optimización o refactorización durante el desarrollo debe publicarse **exclusivamente como pre-release / beta** (ejemplo: `v20.4.0-beta3`, `v20.4.0-beta4`, etc.).
  - Nunca se debe avanzar ni retroceder la rama mayor/menor a versiones anteriores (ej. no volver a `20.3.x` si ya se abrió la línea `20.4.x`).
  - Al publicar en GitHub Releases, debe marcarse siempre el flag `--prerelease` (`isPrerelease: true`).

* **Paso a Versión Estable (Solo bajo instrucción explícita del usuario):**
  - **NUNCA** publicar una versión estable de forma autónoma.
  - El salto a versión estable (ejemplo: `v20.4.0`) solo se realizará cuando el usuario lo solicite expresamente con frases como: *"Libera la versión como estable"*, *"Crea release estable"* o *"Cierra la beta"*.

## 2. Consistencia en `app/build.gradle`
En cada cambio que requiera nuevo APK o release:
1. **`versionCode`:** Debe ser un entero estrictamente incremental y correlativo (`prev_versionCode + 1`).
2. **`versionName`:** Debe ser exactamente igual al nombre del tag de Git (ejemplo: si el tag es `v20.4.0-beta3`, `versionName` debe ser `"20.4.0-beta3"`).

## 3. Identidad de Paquete (Application ID) Única e Inmutable
Para garantizar la coexistencia simultánea de **Diana** y **Artemis original** sin colisiones en Android ni en Obtainium:
* **Application ID Oficial de Diana:**
  - Release: `com.limelight.diana` (definido por base `com.limelight` + `applicationIdSuffix ".diana"`).
  - Debug: `com.limelight.dianadebug`.
* **Prohibición Estricta:**
  - **NUNCA** usar el sufijo `.noir` ni el paquete `com.limelight.noir` en este proyecto, ya que pertenece en exclusiva al fork original Artemis de ClassicOldSong.
  - Diana debe mantener su propio ID independiente para no sobreescribir ni interferir con Artemis en Obtainium o en el sistema operativo.

## 4. Nomenclatura de Tags y Commits
* Formato de tag: `v<MAJOR>.<MINOR>.<PATCH>[-beta<N>|-rc<N>]`
* Mensaje de commit de release: `chore: bump version to v<VERSION>` o `fix(...): ... (v<VERSION>)`

