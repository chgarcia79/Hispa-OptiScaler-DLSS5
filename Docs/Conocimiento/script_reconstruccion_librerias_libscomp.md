---
tipo: script
resumen: "Script de reconstrucción automatizada de librerías comprimidas en Libscomp (multi-volúmenes zip de DLSS-NR y Streamline) para evasión de límites Git."
fecha_creacion: "11/09/2026 03:40:00"
fecha_modificacion: "11/09/2026 03:40:00"
related: ["[[000_Indice_Operaciones_y_Scripts]]", "[[int_gestor_librerias_graficas_dlss_fsr_xess]]"]
tecnologias: [PowerShell, Zip Multi-part, FileStream, Git Large Files Mitigation]
archivos_claves: ["/Src/DESKTOP/HispaGameControl/Repository/ExtraerLibrerias.ps1", "/Src/DESKTOP/HispaGameControl/Repository/ExtraerLibrerias.bat"]
rutas_claves: ["/Src/DESKTOP/HispaGameControl/Repository"]
prioridad_busqueda: 4
tags: [powershell, extraction, zip_split, dlss_nr, streamline, repository]
criticidad: "Media"
resolucion: "Resuelto"
---

# Reconstrucción de Librerías Gráficas Comprimidas en Libscomp (Anti-Límite Git)

## 1. Problema de Límites de Tamaño en Git y GitHub

Los modelos de escalado neural de última generación y los SDKs de renderizado presentan tamaños binarios significativos:
- **DLSS 5 Neural Rendering (`nvngx_dlssnr.dll`)**: 165 MB por versión.
- **NVIDIA Streamline SDK 2.14**: Archivo comprimido monolítico superior a 263 MB.

GitHub impone restricciones estrictas:
1. Rechazo bloqueante de cualquier commit que contenga archivos individuales mayores de 100 MB.
2. Alerta preventiva para archivos mayores de 50 MB.
3. Límite de tamaño de paquete (*packfile*) en operaciones de push (`remote rejected (exceeds 2 GB)`).

Para preservar el repositorio liviano y evitar cuotas de pago o dependencias de Git LFS, HispaGameControl implementa la arquitectura **Libscomp**.

---

## 2. Estrategia Libscomp: Fragmentación y Streaming de Volúmenes

En lugar de subir los binarios descomprimidos o archivos ZIP gigantes:
1. Los archivos superiores a 50 MB se comprimen y fragmentan en volúmenes multipartes de máximo 40 MB cada uno:
   - `nvngx_dlssnr.zip.001`
   - `nvngx_dlssnr.zip.002`
2. Dichos volúmenes residen en subcarpetas denominadas `Libscomp/` y son versionados limpiamente por Git sin superar los límites de GitHub.
3. El archivo `.gitignore` excluye las carpetas y binarios resultantes de la extracción (`nvngx_dlssnr.dll`, DLLs descomprimidas de Streamline, etc.).

---

## 3. Lógica de Reensamblado en `ExtraerLibrerias.ps1`

El script [`ExtraerLibrerias.ps1`](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/Repository/ExtraerLibrerias.ps1) ejecuta la reconstrucción de bajo nivel:

```powershell
# 1. Localización ordenada de los chunks multipartes
$chunks = Get-ChildItem $libscomp -Filter "nvngx_dlssnr.zip.*" | Sort-Object Name

# 2. Ensamblado directo por FileStream en memoria/temporal
$tempZip = Join-Path $env:TEMP "nvngx_dlssnr_${vName}_assembled.zip"
$outStream = [System.IO.File]::Create($tempZip)
foreach ($c in $chunks) {
    $inStream = [System.IO.File]::OpenRead($c.FullName)
    $inStream.CopyTo($outStream)
    $inStream.Dispose()
}
$outStream.Dispose()

# 3. Descompresión y eliminación del ZIP ensamblado transitorio
Expand-Archive -Path $tempZip -DestinationPath $dest -Force
Remove-Item $tempZip -Force
```

### Propagación a Versiones Derivadas
Para no almacenar múltiples copias de DLLs idénticas entre versiones menores (como `v310.8.2` y `v310.9.0`), el script toma como base la DLL de la versión oficial base (`v310.8.0`) y realiza una copia rápida local hacia las carpetas destino si no existen, reduciendo a cero la redundancia en el repositorio.

---

## 4. Wrapper de Ejecución Rápida (`ExtraerLibrerias.bat`)

Para permitir la reconstrucción con un doble clic o durante scripts de setup de primer arranque:
```bat
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0ExtraerLibrerias.ps1"
pause
```

---

## 5. Diccionario de Base de Datos y APIs

- [[ExtraerLibrerias_ps1]] [explicacion:: Script PowerShell para concatenar volúmenes binarios y descomprimir librerías pesadas]
- [[ExtraerLibrerias_bat]] [explicacion:: Wrapper por lotes Win32 para ejecución desatendida con ExecutionPolicy Bypass]
- [[Libscomp]] [explicacion:: Patrón de directorio para almacenamiento fragmentado de assets binarios mayores a 50 MB]
