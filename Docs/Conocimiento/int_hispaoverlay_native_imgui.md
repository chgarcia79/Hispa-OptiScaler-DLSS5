---
tipo: integracion
resumen: "Arquitectura C++ de HispaOverlay_Native: Hooking VMT con MinHook en DirectX 11/12, ciclo ImGui y liberación de cursor Win32."
fecha_creacion: 2026-09-11
fecha_modificacion: 2026-09-11
related:
  - "[[000_Indice_Integraciones_Externas]]"
  - "[[arq_arquitectura_hispagamecontrol_wpf]]"
  - "[[arq_motor_hispaoptiscaler_dlss5]]"
estado: activo
---

# HispaOverlay Nativo: Hooking VMT, ImGui y Desbloqueo de Cursor

## 1. Visión General
`HispaOverlay_Native` es un subsistema C++ nativo de alto rendimiento que inyecta una interfaz visual de control (OSD) dentro de la cola de dibujo de juegos DirectX 11 y DirectX 12 sin generar cuelgues ni dependencias pesadas.

Ubicación del código fuente: [Src/DESKTOP/HispaGameControl/HispaOverlay_Native/](file:///T:/DESARROLLO/GCLOUD/PROYECTOS/HispaGameControl/Src/DESKTOP/HispaGameControl/HispaOverlay_Native)

---

## 2. Técnica de Hooking VMT con MinHook (`dllmain.cpp`)

Para evitar inyecciones invasivas de código ejecutable que alerten a sistemas anti-cheat o rompan la protección de páginas de memoria, HispaOverlay utiliza **hooking sobre la Tabla de Métodos Virtuales (VMT)**:

1. **Dispositivos temporales (*Dummy Devices*):**
   - En el hilo principal (`MainThread`), crea una ventana oculta Win32 y genera un `IDXGISwapChain` y `ID3D11Device` temporales.
   - Extrae la VMT de la interfaz DXGI: el **índice 8** corresponde a `IDXGISwapChain::Present`.
   - MinHook intercepta este puntero virtual y lo redirige a la función de renderizado `HookPresent`.
2. **Cola de comandos Direct3D 12:**
   - Inicializa un `ID3D12Device` y un `ID3D12CommandQueue` temporal.
   - Extrae la VMT de la cola: el **índice 10** corresponde a `ID3D12CommandQueue::ExecuteCommandLists`, redirigiéndolo a `HookExecuteCommandLists`.

---

## 3. Renderizado Dual (DirectX 11 vs DirectX 12)

En cada llamada a `HookPresent`:
- Comprueba si el dispositivo soporta `ID3D12Device`:
  - **En D3D12:** Obtiene el índice del búfer activo (`GetCurrentBackBufferIndex`), inserta una barrera de recurso para transicionar la textura a `D3D12_RESOURCE_STATE_RENDER_TARGET`, renderiza los comandos de ImGui directamente a la cola de dibujo y restaura la barrera a `D3D12_RESOURCE_STATE_PRESENT`.
  - **En D3D11:** Crea el `ID3D11RenderTargetView` correspondiente al backbuffer y procesa los vértices con `ImGui_ImplDX11_RenderDrawData`.
- **Captura de Entrada:** Subclasea el `WndProc` de la ventana (`SetWindowLongPtr`), interceptando la tecla de acceso rápido (por defecto `VK_END`), y desviando los eventos de ratón y teclado a `ImGui_ImplWin32_WndProcHandler` para congelar el control del juego mientras el menú esté visible.

---

## 4. Desbloqueo de Cursor Win32 (`cursor_unlocker.cpp` / `cursor-unlocker.addon64`)

Muchos títulos bloquean el cursor del ratón en el centro de la pantalla mediante llamadas periódicas del sistema operativo:
- **APIs Interceptadas:** MinHook intercepta `ClipCursor` y `ShowCursor` de `user32.dll`.
- **Activación:** Mediante teclas rápidas (`VK_HOME`, `VK_F10`, `VK_F6`), libera el ratón forzando `ClipCursor(NULL)`, `ReleaseCapture()`, `ShowCursor(TRUE)` y `SetCursor(LoadCursor(NULL, IDC_ARROW))`, permitiendo interactuar con monitores secundarios en setups multimonitor o con el menú OSD sin que el juego atrape el cursor.
