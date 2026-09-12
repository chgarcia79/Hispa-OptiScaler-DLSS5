---
tipo: despliegue
resumen: "Protocolo y directiva de publicación automática de HispaGameControl en el entorno operativo local C:/Soft/Juegos/HispaGameControl/."
fecha_creacion: "11/09/2026 05:30:00"
fecha_modificacion: "11/09/2026 05:30:00"
related: ["[[000_Indice_Publicacion_y_Despliegues]]", "[[000_Indice_Estandares_y_Guias_Codigo]]", "[[arq_arquitectura_hispagamecontrol_wpf]]"]
tecnologias: [.NET 9, WPF, C#, dotnet publish]
archivos_claves:
  - .agent/rules.md
  - Src/DESKTOP/HispaGameControl/HispaGameControl.csproj
rutas_claves:
  - C:/Soft/Juegos/HispaGameControl
  - Src/DESKTOP/HispaGameControl
prioridad_busqueda: 4
tags: [despliegue, publish, local, release, desktop, wpf]
criticidad: "Media"
resolucion: "Resuelto"
---

# Publicación Automática de HispaGameControl en Entorno Operativo Local

## 1. Propósito y Contexto
Para garantizar que cualquier modificación de código probada y estable esté inmediatamente disponible para su uso real en el equipo de desarrollo, se define la directiva local de publicación continua en `C:\Soft\Juegos\HispaGameControl\`.

Esta regla está formalizada en el archivo de gobernanza local [`rules.md`](file:///t:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/.agent/rules.md) bajo el estándar **RFC 2119**.

---

## 2. Protocolo Operativo (3 Fases)

### Fase 1: Certificación de Compilación y Estabilidad
Antes de cualquier despliegue, es obligatorio verificar que la solución compile con 0 errores:
```powershell
dotnet build "Src/DESKTOP/HispaGameControl/HispaGameControl.csproj"
```

### Fase 2: Prevención de Bloqueos de Procesos (Process Check)
Si `HispaGameControl.exe` se encuentra en ejecución, Windows bloqueará la sobrescritura de archivos binarios (.dll/.exe):
```powershell
Get-Process -Name 'HispaGameControl' -ErrorAction SilentlyContinue | Stop-Process -Force
```

### Fase 3: Despliegue en Release
Ejecución del comando de publicación optimizado hacia el directorio operativo:
```powershell
dotnet publish "Src/DESKTOP/HispaGameControl/HispaGameControl.csproj" -c Release -o "C:\Soft\Juegos\HispaGameControl\"
```

---

## 3. Verificación del Despliegue
El directorio destino contiene la estructura completa necesaria para la ejecución independiente:
* `HispaGameControl.exe` y `HispaGameControl.dll`
* Recursos de interfaz (`Assets/`, iconos)
* Módulos de soporte (`Modules/`, perfiles DLSS5)
* Runtimes y dependencias nativas de renderizado
