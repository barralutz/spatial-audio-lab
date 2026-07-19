# QEMU/WHPX Atmos Endpoint: Brief de traspaso

**Fecha:** 2026-07-19

**Estado:** Brief para planificacion posterior; no es un plan de implementacion

## Proposito

Este documento conserva el alcance y las decisiones de la investigacion para que una sesion futura,
ejecutada desde WSL sobre el Windows real de desarrollo, inspeccione ese entorno y escriba el plan de
implementacion. No se deben inferir versiones, rutas, dependencias ni comandos definitivos antes de
esa inspeccion.

La hipotesis es que QEMU puede presentar a un Windows invitado un endpoint HDMI suficientemente fiel
para que Windows use un controlador inbox firmado, Dolby Access habilite Dolby Atmos for Home Theater
y el modelo de dispositivo de QEMU capture el carrier MAT sin instalar un controlador Windows propio.

## Contexto del proyecto

SpatialAudioLab ya demostro que un endpoint posterior al renderer Dolby puede recibir MAT 2.1 Profile
3. El prototipo actual obtiene esos bytes mediante un SysVAD modificado y test-signed que:

- anuncia formatos IEC 61937 MAT/DTS de 8 canales a 192 kHz;
- anuncia PCM 7.1.4 de 12 canales a 48 kHz;
- copia el stream de render a un ring kernel-to-user expuesto como `\\.\DolbyDecoderMat`.

La limitacion que motiva esta investigacion es el requisito de arrancar Windows con la comprobacion de
firmas deshabilitada. No se dispone de presupuesto para Microsoft attestation/production signing.

## Decision de producto de la investigacion

El primer hito sera un **PoC de captura a archivo**, no una ruta de reproduccion en vivo.

El experimento se ejecutara inicialmente con QEMU nativo para Windows y aceleracion WHPX, controlado
desde WSL. Codex, Git, edicion y orquestacion permanecen en WSL; desde ahi se pueden invocar
`powershell.exe`, herramientas Windows y `qemu-system-x86_64.exe`.

La estrategia de fuentes acordada es:

- mantener un fork `barralutz/qemu`;
- desarrollar en una rama `spatial-audio-lab`;
- fijar posteriormente un commit del fork desde SpatialAudioLab mediante un submodulo, siguiendo el
  patron ya usado para Cavern;
- seleccionar la version base de QEMU solamente despues de inspeccionar el entorno real y las
  versiones disponibles.

## Criterio de exito del PoC A

El PoC se considera exitoso solamente si se cumplen todos estos puntos:

1. Host e invitado mantienen habilitada la comprobacion normal de firmas.
2. El invitado no instala ningun INF, SYS, APO ni proveedor espacial propio de SpatialAudioLab.
3. Windows crea un endpoint de render para el dispositivo emulado y su cadena de controlador es
   inbox/Microsoft-signed.
4. El endpoint se clasifica como salida de audio de pantalla/HDMI, no solamente como altavoces PCM.
5. Dolby Access permite seleccionar Dolby Atmos for Home Theater para ese endpoint.
6. `ISpatialAudioClient` observa la firma esperada de Atmos Home Theater: cama estatica 7.1.4 y 20
   objetos dinamicos en la version de Windows validada.
7. Windows abre el carrier MAT efectivo de 8 canales, 192 kHz y 16 bits con el subtipo esperado.
8. QEMU guarda los bytes crudos antes de cualquier mezcla, remuestreo o conversion del backend de
   audio.
9. Las herramientas existentes de SpatialAudioLab reconocen en el archivo al menos una rafaga MAT
   valida y no detectan que QEMU haya alterado su estructura.

Que QEMU reproduzca PCM por DirectSound, que Windows enumere un dispositivo HDA generico o que el
panel muestre texto relacionado con Atmos sin abrir un stream espacial real no satisface el hito.

## Fuera de alcance del PoC A

- Reproduccion 7.1.4 en vivo.
- PipeWire, ALSA o salidas fisicas Linux.
- Transporte guest-to-host de PCM decodificado.
- DTS:X, MAT 1.0 de Battlefield, E-AC-3 JOC o TrueHD.
- GPU passthrough, captura HDMI fisica o emulacion grafica completa.
- Rendimiento de juegos, latencia final y compatibilidad con anti-cheat.
- Instalador, interfaz grafica o distribucion de un binario QEMU.
- Solicitar firma, certificacion o publicacion de un controlador Windows.

## Enfoques que debe comparar el plan futuro

### 1. Extender el modelo de controlador HDA y agregar un codec/pin HDMI en QEMU

Es el enfoque inicial recomendado. Reutiliza el controlador ICH9 Intel HDA ya emulado, agrega la
topologia y capacidades de una salida HDMI y conduce el DMA de render a un sink MAT crudo. Tiene el
menor alcance, pero existe el riesgo de que el controlador inbox de Windows no exponga las
propiedades de display-audio o ELD necesarias para Dolby.

### 2. Emular una funcion PCI display-audio dedicada

Es el fallback si un codec HDA generico nunca puede convertirse en `DigitalAudioDisplayDevice` con
el controlador inbox. Podria modelar mejor la relacion GPU/HDMI/ELD, pero aumenta sustancialmente el
alcance y no debe iniciarse hasta documentar por que fallo el enfoque 1.

### 3. Usar el SysVAD actual unicamente como oraculo diagnostico

El endpoint test-signed existente puede proporcionar formatos, propiedades, capturas y resultados de
referencia en el Windows fisico. No puede formar parte de la solucion ni instalarse en el invitado que
valida el criterio de firmas.

No se consideran soluciones objetivo Hyper-V Enhanced Session, RDP audio, VMware, VirtualBox,
VirtIO Sound, USB Audio Class ni un dispositivo HDA PCM generico: pueden transportar PCM, pero no
demuestran el endpoint HDMI/MAT requerido.

## Puertas de investigacion

El plan real debe dividir el trabajo en puertas go/no-go y detener la expansion de alcance cuando una
puerta falle.

### Puerta 0: inventario reproducible del entorno

Registrar sin modificar el sistema:

- version y edicion del Windows host;
- version de WSL, distribucion y ubicacion real del checkout;
- estado de Windows Hypervisor Platform, Hyper-V/VBS y capacidad WHPX;
- CPU, RAM y espacio disponible;
- toolchains Windows, MSYS2 y MinGW ya instalados;
- version de Dolby Access y disponibilidad de la licencia/proveedor en una VM nueva;
- ISO/build de Windows invitado disponible;
- comandos actuales que permiten ejecutar probes Windows desde WSL.

Esta puerta decide si QEMU se compila con MinGW desde WSL, con MSYS2 nativo invocado desde WSL o con
otro flujo reproducible. No elegirlo anticipadamente.

### Puerta 1: baseline QEMU/WHPX sin modificaciones

Arrancar un Windows invitado limpio sobre `q35`/WHPX, conservar un snapshot y registrar el endpoint
HDA estandar con las herramientas de SpatialAudioLab. Debe demostrar que el ciclo
editar-compilar-arrancar-recolectar logs es repetible antes de cambiar el dispositivo.

### Puerta 2: identidad HDMI con controlador inbox

Modificar solamente la identidad/topologia necesaria para obtener un endpoint de audio de pantalla.
Verificar el controlador cargado, form factor, KS node type, propiedades MMDevice y comportamiento al
reiniciar. Si Windows exige un controlador de fabricante no disponible, documentar la evidencia antes
de evaluar el enfoque PCI dedicado.

### Puerta 3: negociacion exacta de formatos

Agregar capacidades incrementalmente: primero PCM conocido, luego 8 canales/192 kHz y finalmente los
subtipos IEC 61937/MAT. `IAudioClient::IsFormatSupported` y los eventos del dispositivo deben probar
el formato efectivo; no confiar solamente en el panel de configuracion.

### Puerta 4: activacion de Dolby Spatial Sound

Instalar Dolby Access en el invitado por el mecanismo normal del usuario, seleccionar Atmos Home
Theater y validar `ISpatialAudioClient`, objetos y formato abierto. No copiar paquetes, claves o estado
privado desde el host para forzar un resultado positivo.

### Puerta 5: captura MAT cruda

Conectar el stream HDA/DMA a un backend especifico que escriba archivo o named pipe sin pasar por el
mixer de QEMU. Validar preambulos IEC 61937, secuencias, tamanos y rafagas MAT con los analizadores ya
existentes. Repetir desde un snapshot limpio para demostrar reproducibilidad.

## Evidencia que debe conservar cada puerta

- commit exacto de QEMU y diff local;
- linea de comandos completa y configuracion de la VM sin secretos;
- version/build del host y del invitado;
- IDs PnP, nombre y firma del controlador cargado;
- propiedades de endpoint relevantes, formatos exclusivos y salida del probe espacial;
- trazas QEMU acotadas a HDA/MMIO/DMA/formato;
- hashes de archivos capturados y salida de los analizadores;
- resultado `go`, `no-go` o `inconcluso`, con la observacion que lo justifica.

Las capturas grandes, imagenes de VM, medios con copyright, tokens y datos personales no se deben
subir al repositorio.

## Riesgos principales que el plan debe atacar primero

1. El controlador inbox HDA puede no crear una salida HDMI ni consumir ELD sin una funcion de audio
   integrada con un adaptador grafico.
2. Dolby Access puede depender de propiedades privadas del endpoint ademas de los formatos KS.
3. El modelo HDA actual de QEMU no implementa la ruta non-PCM/192 kHz requerida y su mixer normal no
   preserva un carrier IEC 61937.
4. Una licencia o restriccion de Microsoft Store dentro de la VM puede impedir distinguir un fallo de
   licencia de un fallo del dispositivo emulado.
5. Temporizacion WHPX/HDA puede producir una captura valida en estructura pero inestable en tiempo.
6. Emular IDs de un fabricante real puede crear dependencias, problemas legales o falsas expectativas;
   el diseño debe preferir IDs propios y controladores inbox genericos.

## Entregables del plan futuro

La sesion WSL posterior debe producir, despues de la inspeccion:

1. una decision documentada sobre toolchain y version base de QEMU;
2. una arquitectura de dispositivo y backend con limites claros;
3. tareas pequenas con pruebas y comandos de verificacion por puerta;
4. estrategia de fork, ramas, commits y actualizacion del submodulo;
5. procedimiento de snapshot/rollback de la VM;
6. criterios explicitos para pasar al fallback o cerrar la investigacion como no viable;
7. un plan de implementacion separado; este brief no debe convertirse mecanicamente en ese plan.

## Referencias de partida

- [`README.md`](../../README.md): arquitectura y limites actuales de SpatialAudioLab.
- [`driver/README.md`](../../driver/README.md): formatos y ring del SysVAD experimental.
- [`docs/TECHNICAL_NOTES.md`](../TECHNICAL_NOTES.md): evidencia MAT, probes y propiedades observadas.
- [`patches/windows-driver-samples/SpatialAudioLab.patch`](../../patches/windows-driver-samples/SpatialAudioLab.patch):
  formatos KS que sirven como oraculo, no como implementacion QEMU.
- [QEMU Windows Hypervisor Platform](https://www.qemu.org/docs/master/system/whpx.html): aceleracion
  WHPX en hosts Windows.
- [QEMU supported build platforms](https://www.qemu.org/docs/master/about/build-platforms.html):
  toolchains MinGW/MSYS2 soportados.
- [Microsoft Spatial Sound](https://learn.microsoft.com/en-us/windows/win32/coreaudio/spatial-sound):
  comportamiento esperado de `ISpatialAudioClient` y Atmos Home Theater.

## Instruccion para la siguiente sesion

Abrir este documento desde el checkout WSL que corre sobre el Windows real. Antes de proponer codigo o
instalar herramientas, inspeccionar la Puerta 0 y contrastar cada supuesto con el sistema. Luego crear
un plan de implementacion nuevo que cite los resultados observados y mantenga como unico objetivo el
PoC A.
