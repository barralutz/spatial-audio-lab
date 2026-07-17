# dolbyDecoder: experimento EDID/MAT

Este repositorio investiga la captura y el renderizado de Dolby Atmos para obtener una salida
analogica configurable. La ruta de juegos ya reproduce Dolby MAT como 7.1.4 mediante tres
dispositivos WASAPI; la ruta de peliculas E-AC-3 JOC y TrueHD Atmos conserva por ahora la salida
7.1.2. El endpoint virtual SysVAD recibe Dolby MAT 2.1 antes de HDMI.

## Conclusion tecnica

Un override EDID puede hacer que el driver AMD anuncie LPCM 7.1, E-AC-3 JOC y la familia
MAT/MLP. No decodifica MAT ni redirige ese audio a Realtek. Dolby MAT usa un carrier IEC 61937 de
8 canales a 192 kHz; esos ocho canales son transporte, no ocho feeds de altavoces.

La secuencia experimental completada fue:

1. Aplicar el EDID de prueba al S34J55x.
2. Confirmar que el endpoint AMD acepta MAT 2.0 y que Dolby Access permite Atmos Home Theater.
3. Capturar el endpoint AMD con Atmos activo como diagnostico complementario.
4. Confirmar que WASAPI compartido solo expone PCM y que un endpoint SysVAD posterior al renderer
   Dolby recibe el carrier MAT 2.1 real.

WASAPI loopback solo captura el mezclador compartido y no observa streams exclusivos. Encontrar MAT
seria evidencia positiva; encontrar PCM o cero preambulos no demuestra que el driver no este
emitiendo MAT aguas abajo. La extraccion fiable requerira un miniport virtual basado en SysVAD o
hardware de captura HDMI HBR. SysVAD ya valido la primera ruta: Windows entrega MAT 2.1 Profile 3,
con PCM de cama y objetos sin compresion dentro del transporte.

## Estado del equipo

Diagnostico realizado entre el 15 y el 16 de julio de 2026:

- Placa: Gigabyte X570 AORUS ELITE WIFI, codec Realtek `DEV_0B00`.
- GPU: AMD Radeon RX 6700 XT.
- HDMI/DP: `5 - S34J55x`, endpoint `DigitalAudioDisplayDevice`.
- Realtek trasero: PCM 7.1 exacto, 8 canales/48 kHz, mascara `0x63f`.
- Realtek frontal: PCM estereo exacto en modo exclusivo.
- EDID original S34J55x: solo LPCM 2.0, 32/44.1/48 kHz.
- Dolby Access esta instalado y Atmos Home Theater esta activo en el endpoint HDMI Hisense.
- Antes del override, el endpoint AMD rechaza PCM 7.1 y MAT 1.0/2.0/2.1 en modo exclusivo.

Esto confirma que Realtek ofrece diez salidas analogicas potenciales, 8 + 2. El DAC USB C-1U agrega
otras dos. La configuracion actual usa Realtek trasero para la cama 7.1, C-1U para `TFL/TFR` y el
panel frontal para `TBL/TBR`.

## Salida configurable 7.1.4

`live-layout` reemplaza las rutas de salida fijas por el perfil
`configs/realtek-c1u-714.ini`. El perfil define azimut, elevacion y nivel de cada parlante, los
parlantes asignados a cada endpoint, retardo inicial y cual dispositivo actua como maestro. El
renderer abre todos los endpoints como PCM float a 48 kHz y mantiene una unica cola de programa.

Cada salida secundaria tiene su propio resampler fraccional. `IAudioClock` estima tanto la
frecuencia acumulada del DAC como su fase respecto del maestro; un servo lento corrige deriva sin
saltos y limita la variacion a 2000 ppm. Esta arquitectura no depende de C-1U: un DAC USB futuro o
una tarjeta PCIe adicional solo requiere otra seccion `[output.*]` en el perfil.

La prueba silenciosa de 120 segundos abrio los tres dispositivos sin starvation. El panel frontal
midio una diferencia fija maxima de 10 ms y el C-1U termino alineado con el maestro; ambos
convergieron mediante resampling adaptativo. Una captura MAT estatica 7.1.4 recupero los doce
canales, incluidos `TBL/TBR` a aproximadamente -25.05 dBFS RMS, con cero rafagas malformadas,
cero gaps, cero bytes descartados y cero clipping. Otra prueba con un objeto dinamico arriba y
detras tambien produjo senal en ambos canales superiores traseros. La salida 7.1.2 historica
conserva exactamente el SHA-256 de referencia.

El editor WPF permite arrastrar cada parlante en vistas de planta y elevacion, modificar azimut,
elevacion, nivel y destino fisico, editar endpoints/retardos y probar un parlante aislado. El selector
de destino identifica cada slot PCM como `ruta / canal` y cambia las asignaciones mediante un
intercambio: por ejemplo, `TFL -> rear / BL` envia el objeto superior frontal izquierdo al quinto
canal del endpoint Realtek y mueve el `BL` que ocupaba ese slot al destino anterior de `TFL`. No
requiere reasignar conectores en el controlador Realtek. El orden resultante se guarda en
`speakers=` y es el orden de canales que consume el renderer.

Guarda directamente el mismo INI consumido por el motor. La pestaña `Salidas` enumera los endpoints
de reproduccion activos en un selector, permite actualizar la lista despues de conectar hardware y
conserva los filtros de dispositivos desconectados marcandolos como no disponibles. `Restablecer
posiciones` devuelve azimut y elevacion al esquema 7.1.4 estandar sin cambiar niveles, endpoints ni
retardos.
La pestaña `Puentes` selecciona `Dolby MAT` o `DTS:X`, inicia y detiene el script correspondiente,
controla ganancia, prebuffer y duracion, muestra el PID activo y abre su registro. Si un puente ya
esta ejecutandose, el editor selecciona su modo, bloquea el arranque del otro y dirige `Detener` al
proceso activo. El puente se ejecuta como proceso independiente y continua activo al cerrar el
editor; si hace falta elevacion, Windows solicita UAC al ejecutar el script correspondiente.

```powershell
.\tools\Build-DolbyProbe.ps1
.\tools\Build-SpeakerLayoutEditor.ps1
.\tools\Start-SpeakerLayoutEditor.ps1

.\build\dolby-probe.exe test-layout 120 `
  .\configs\realtek-c1u-714.ini 0
.\build\dolby-probe.exe test-speaker 3 `
  .\configs\realtek-c1u-714.ini TBR 0.08
.\build\dolby-probe.exe spatial-test 3 `
  'SinkDescription Sample' 714

# Requiere el arranque F7 y el driver SysVAD de prueba.
.\tools\Start-Live714.ps1 -DurationSeconds 3600 -Gain 0.25
.\tools\Stop-Live714.ps1
```

`live-layout` es actualmente la ruta configurable para MAT de juegos. `DolbyPlayer` todavia
renderiza peliculas a la pareja Realtek 8+2; compartir el mismo renderer 7.1.4 es la siguiente
integracion, no una capacidad que deba asumirse ya terminada.

## Salida GPU dedicada

La tarjeta ASRock RX 6700 XT tiene tres DisplayPort y un HDMI. Windows muestra actualmente dos
rutas fisicas: S34J55x por DisplayPort y S27F350 mediante DVI/adaptador, asi que quedan dos salidas
libres. Esta es la ruta preferida para la prueba porque no altera el EDID del monitor principal.

Una salida desconectada no basta: AMD necesita hot-plug detect y un EDID valido antes de crear el
display y su endpoint HDMI Audio. Las opciones son:

1. HDMI libre -> emulador EDID programable que mantenga HPD aun sin pantalla.
2. DisplayPort libre -> adaptador DP-HDMI con audio -> emulador EDID programable.
3. Reubicar el S27F350 en otro DisplayPort y liberar el HDMI fisico, si ahora usa HDMI-DVI.

No sirve un dummy plug de EDID fijo si solo anuncia estereo. El dispositivo debe aceptar un `.bin`
personalizado por USB o software. Windows conserva un EDID real de un televisor Hisense que anuncia
E-AC-3 JOC, MAT object/channel PCM, DTS-HD y los bloques HDMI correspondientes; fue exportado como
`generated/reference-edid/HISENSE-Atmos-MAT-reference.bin`. Es la primera imagen que conviene cargar
en el emulador. Si AMD no acepta sus modos de video, se usara el EDID generado para el S34J55x.

El dummy debe permanecer como pantalla extendida para conservar el endpoint. El audio se selecciona
en `5/6/... - HISENSE (AMD High Definition Audio Device)` y se activa Atmos Home Theater ahi. Esto
aisla el experimento, pero el flujo sigue saliendo fisicamente por HDMI: el emulador EDID no lo
devuelve al programa. Para extraerlo todavia hace falta miniport virtual o hardware HDMI HBR.

El dummy conectado el 15 de julio de 2026 aparece como
`DISPLAY\DEFAULT_MONITOR\7&1892BE41&0&UID524`: hot-plug activo, sin EDID y sin endpoint de audio.
Para instalar o retirar un override solo en esa instancia, desde PowerShell como administrador:

```powershell
.\tools\Set-EdidOverride.ps1 -Action Apply `
  -InstanceId 'DISPLAY\DEFAULT_MONITOR\7&1892BE41&0&UID524' `
  -EdidFile '.\generated\reference-edid\HISENSE-Atmos-MAT-reference.bin'

.\tools\Set-EdidOverride.ps1 -Action Remove `
  -InstanceId 'DISPLAY\DEFAULT_MONITOR\7&1892BE41&0&UID524'
```

El script exporta primero `Device Parameters` a `generated/registry-backup`, valida checksums y
reinicia unicamente el monitor con `pnputil`. Si el adaptador sigue declarado como DVI o AMD no crea
un endpoint de audio, el limite estara en el adaptador DP-HDMI/dummy y habra que usar hardware que
exponga HDMI con audio.

### Resultado con la Hisense real

La TV conectada directamente por HDMI creo el endpoint
`{695db34d-c46c-4940-a903-6d6bbec976a8}`. Antes de activar Atmos tenia mix estereo y cero objetos.
Despues de configurar Dolby Atmos Home Theater:

- `ISpatialAudioClient`: 20 objetos dinamicos y mascara estatica `0xC1FFE`.
- Mix compartido: PCM float 7.1, 48 kHz, mascara `0x63F`.
- Formatos exclusivos nativos: Dolby Digital Plus y MAT/MLP 1.0.
- La consulta directa MAT 2.0/2.1 sigue devolviendo formato no soportado; el encoder Dolby queda
  abstraido detras del endpoint espacial.
- Loopback compartido capturo 101760 frames, 83040 no silenciosos, como PCM 7.1 y sin preambulos IEC
  61937. Esto confirma que ese tap esta antes del carrier HDMI, no que el enlace fisico carezca de MAT.

El endpoint activo registra `Microsoft Audio Home Theater Effects`; su snapshot completo esta en
`generated/hisense-atmos-endpoint.reg`. La fase EDID/activacion queda validada. La extraccion requiere
un punto posterior al renderer Dolby: miniport virtual que reciba el formato KS, o captura HDMI HBR.

### Captura de Battlefield 2042

Con `BF2042.exe` ejecutandose y Atmos Home Theater activo se capturaron 20 segundos del loopback
compartido en `captures/battlefield-2042-atmos-shared.wav`. El contenedor es PCM float 7.1/48 kHz,
pero el contenido medido fue:

```text
Channel  RMS dBFS  Peak dBFS  Nonzero
    FL    -22.55      -2.76   100.00%
    FR    -21.93      -2.14   100.00%
    FC    -26.31      -0.92    18.13%
   LFE      -inf       -inf     0.00%
    BL      -inf       -inf     0.00%
    BR      -inf       -inf     0.00%
    SL    -40.64     -16.44   100.00%
    SR    -37.23     -12.83   100.00%
```

`BL/BR` estuvieron digitalmente en cero durante los 960480 frames; `SL/SR` contienen senal distinta,
no copias. Por tanto, en esta ejecucion 2042 escribio una cama 5.1 dentro del mix 7.1 de Windows. La
ausencia de LFE solo describe la escena capturada. El perfil tenia
`GstAudio.SpeakerConfiguration 0` (`Auto`, el valor predeterminado), no una seleccion estereo.

Este tap no contiene canales de altura ni el carrier MAT. Sirve para medir la cama entregada por el
juego antes de la salida HDMI, pero no permite decidir si el renderer Dolby agrega metadata despues.

### Captura de Battlefield 1 con Atmos nativo

Con `BF1.exe` y la opcion Dolby Atmos del juego activados (`GstAudio.IsATMOSOptionEnabled 1` y
`GstAudio.YourSoundSystem 3` en el perfil), el comportamiento fue distinto a 2042:

1. El loopback normal del endpoint Hisense fallo al inicializar con `0x8889000A`
   (`AUDCLNT_E_DEVICE_IN_USE`). BF1 mantiene el dispositivo HDMI ocupado en modo exclusivo.
2. El loopback por proceso de Windows 10/11 si pudo seguir el PID de BF1 y recibio 1199040 frames,
   pero todas las muestras fueron silencio efectivo: pico inferior a `-184 dBFS`, sin canales activos
   y sin preambulos IEC 61937.
3. Una captura de control realizada con la misma API y el mismo formato PCM 7.1 recupero correctamente
   el audio de una aplicacion compartida a aproximadamente `-24 dBFS`. El silencio de BF1 no es un
   defecto del capturador.

Los archivos son `captures/battlefield-1-native-atmos-process.wav` y
`captures/process-loopback-control.wav`. En conjunto, las pruebas confirman que el stream Atmos
exclusivo elude tanto el loopback del endpoint como el loopback por proceso. Estas APIs no permiten
leer el carrier que AMD entrega al pin KS/HDMI.

El resultado acota el siguiente prototipo: un endpoint virtual WaveRT/KS que acepte el formato
exclusivo solicitado por BF1 y exponga su ring buffer a user mode. Capturar mas arriba en WASAPI no
sera suficiente; la alternativa de referencia sigue siendo hardware HDMI capaz de HBR/MAT.

### Prueba de GTA V Enhanced

[Rockstar confirma Dolby Atmos en GTA V Enhanced para PC](https://www.rockstargames.com/newswire/article/7551a7k917554a/free-upgrade-for-grand-theft-auto-v-on-pc-now-available)
y especifica un sistema compatible con Windows Spatial Sound. La prueba se realizo en modo sin
BattlEye: `GTA5_Enhanced.exe` tenia `-nobattleye` en su linea de comandos y `BEService` estaba
detenido. No debe repetirse con el anticheat activo mientras el driver sea de prueba.

Con `SinkDescription Sample` como salida predeterminada, cinco segundos de juego entregaron
15,356,928 bytes y 249 rafagas MAT, sin descartes ni gaps. La escena uso una cama estatica con
FL/FR/FC, laterales, traseros y TFL/TFR activos; LFE quedo en silencio y no aparecieron objetos
dinamicos durante esa muestra. Una ejecucion directa de `live-712` proceso 1,503 rafagas durante
30.062 segundos con cero perdidas, rafagas malformadas, clipping o starvation activo, y delta de
reloj de 0.000 ms. El endpoint virtual no suena por si solo: `live-712` debe permanecer ejecutandose
para alimentar las salidas analogicas.

### Prototipo SysVAD

Se fijo el sample oficial SysVAD en `driver/windows-driver-samples`, commit
`2ee527bfeb0aeb6be11f0a8b6dce4011b358ce89`. El endpoint HDMI upstream ya anuncia MAT 2.0 y 2.1
como carrier IEC 61937 de 8 canales/192 kHz y `CSaveData` ya vuelca los bytes renderizados en
`%DriverData%\Audio_Samples\Sysvad\STREAM_HOST_*.wav`. En este equipo `%DriverData%` resuelve a
`C:\Windows\System32\Drivers\DriverData`; no es `C:\DriverData`. La primera prueba confirmo que
Dolby Access crea un dump MAT real en esa ubicacion.

El equipo tiene Secure Boot activo, HVCI/Memory Integrity inactivo, Visual Studio 2022, SDK 26100 y
WDK 10.0.26100.6584. Las pruebas usan Inicio avanzado -> Startup Settings -> opcion 7, que
deshabilita la comprobacion de firma una sola vez. La build cargada actualmente es `oem117.inf`,
version `4.5.27.5`. `TESTSIGNING` persistente requiere desactivar Secure Boot y se
reserva para iteraciones posteriores. Los scripts estan documentados en `driver/README.md`.

BF1 y 2042 usan EA Javelin AntiCheat. No se debe ejecutar ninguno con Test Mode o el driver de prueba
cargado: EA puede rechazar controladores no firmados/no compatibles. La fase de driver se valida con
Dolby Access u otra fuente Atmos sin anticheat; para Battlefield se necesitara una firma de produccion
o captura HDMI externa.

#### Resultado MAT 2.1 y primer extractor 7.1.2

El endpoint `SinkDescription Sample` modificado acepta PCM 7.1 y MAT 1.0/2.0/2.1 en modo exclusivo.
Con `Dolby Atmos para el centro de entretenimiento` activo, Windows expone mascara estatica
`0xC1FFE`, 20 objetos dinamicos y abre MAT 2.1 Profile 3. El volcado real usa 8 canales, 192 kHz,
16 bits y mascara `0x63F` como carrier IEC 61937.

Las capturas controladas establecen la estructura necesaria para el primer decoder:

- 50 rafagas IEC por segundo, separadas exactamente 61440 bytes; `Pc=0x16`.
- El transporte intercambia los dos bytes de cada palabra de 16 bits.
- Cada rafaga contiene dos bloques de 10 ms.
- Cada bloque ofrece 31 slots PCM16 de banda completa a 48 kHz y un LFE PCM16 a 12 kHz.
- Los slots estaticos usados son FL, FR, FC, SL, SR, BL, BR, TFL y TFR; el LFE lleva su bloque
  separado. Los slots 9 y 10 completan la cama estatica 7.1.4 y los slots 11 a 30 corresponden a
  los 20 objetos dinamicos.
- Una prueba con un objeto movil de 900 Hz coloca su audio, sin compresion, en el slot 11.

`dolby-probe extract-mat-712` desempaqueta la cama estatica y los 20 objetos dinamicos para generar
PCM16/48 kHz de diez canales con mascara `0x563F`. Las pruebas de tonos estaticos recuperan los
canales activos a aproximadamente `-25.10 dBFS RMS / -21.93 dBFS peak`; los canales no usados quedan
en cero digital. La ampliacion dinamica conserva el archivo estatico anterior byte por byte. El LFE
se expande provisionalmente de 12 a 48 kHz repitiendo muestras; se reemplazara por un resampler con
filtro antes de la salida en tiempo real.

La metadata de posicion MAT no es un contenedor EMDF/OAMD reconocible: no aparece un sync `0x5838`
valido ni siquiera al buscar en cualquier alineacion de bit. La comparacion controlada permitio
identificar directamente su tabla de objetos:

- Cada bloque de 10 ms contiene 20 registros de 36 bits, uno por cada slot dinamico 11-30.
- Cada registro tiene seis campos de 6 bits. Los tres primeros son `X`, `Z` e `Y`; los tres campos
  auxiliares permanecieron en `4,3,31` en todas las fixtures y aun no tienen semantica confirmada.
- La tabla aparece dos veces por rafaga de 20 ms. Las posiciones fijas fueron estables en 92 de 92
  copias analizadas.
- Los codigos extremos observados son izquierda `X=0`, derecha `X=62`, frente `Z=0`, detras `Z=62`,
  cama `Y=32` y arriba `Y=62`. El renderer aplica una transformacion perceptual antes de MAT; estos
  valores son controles de paneo, no metros cartesianos lineales.
- Las unicas diferencias adicionales entre posiciones son campos de integridad al final de la
  metadata y de cada bloque; el PCM del objeto permanece identico en el slot 11.

El panner provisional usa potencia constante entre los altavoces adyacentes del anillo 7.1 y una
transicion de elevacion hacia TFL/TFR. Las pruebas producen solo `SL` a la izquierda, solo `SR` a la
derecha, `FC` delante, `BL/BR` detras y `TFL/TFR` arriba, sin clipping. Aun falta identificar los
tres campos auxiliares y contrastar el paneo contra un decoder Dolby de referencia.

La salida analogica 7.1.2 tambien esta validada. `dolby-probe play-712` abre en modo compartido float
el endpoint trasero `Altavoces (Realtek(R) Audio)` y el frontal
`Realtek HD Audio 2nd output (Realtek(R) Audio)`, envia FL..SR al primero y TFL/TFR al segundo, y
mide ambos `IAudioClock` al terminar. Una ejecucion silenciosa de 15 repeticiones proceso 1,425,600
frames en 29.702 segundos con una diferencia final de 0.000 ms. Esta orden reproduce por ahora un
WAV ya extraido. Para la ruta directa se usa `live-712`.

El controlador `oem116.inf` reemplaza el archivo como ruta primaria por un ring no paginado de
4 MiB, acotado y no bloqueante. Lo expone solo a administradores como `\\.\DolbyDecoderMat` mediante
IOCTLs para leer, consultar estadisticas y reiniciar contadores. `capture-mat-ring` guarda el flujo
continuo como WAV MAT 2.1 y comprueba secuencias y overflows. Una escena dinamica de cuatro segundos
produjo exactamente 12,294,144 bytes y 200 rafagas, con cero discontinuidades y cero bytes
descartados. Ese archivo se decodifico a 199 bloques 7.1.2, 392 posiciones de objeto y cero muestras
recortadas; la reproduccion analogica posterior avanzo 3.982 segundos en ambos relojes, delta 0 ms.
`live-712` conecta ese mismo parser incremental a una cola PCM comun y a los dos endpoints Realtek.
Con 80 ms de prebuffer proceso en vivo 199 rafagas de la misma escena: cero bytes descartados, cero
saltos de secuencia, cero rafagas malformadas, cero clipping y cero starvation durante el carrier.
Los dos relojes avanzaron 4.172 segundos con una diferencia final de 0.000 ms. Una segunda ejecucion
audible con ganancia 0.10 obtuvo los mismos contadores.
Una prueba prolongada de 30 segundos proceso 92,166,144 bytes y 1,499 rafagas: cero descartes,
gaps, rafagas malformadas, clipping o starvation activo; ambos relojes avanzaron 30.182 segundos.
La validacion posterior al reinicio con `oem117.inf` repitio el recorrido completo y confirmo que
`DoNotCreateDataFiles=1` no crea ningun `STREAM_HOST_*.wav`; el ring permanece operativo.
`live-712` mantiene ahora abiertas las salidas analogicas durante todo el tiempo solicitado aunque
el carrier MAT se interrumpa. Una prueba cambio la salida predeterminada a Realtek durante dos
segundos y regreso a `SinkDescription Sample`: el mismo proceso sobrevivio y reanudo el consumo
de MAT. Esto permite cambiar temporalmente de dispositivo sin tener que reiniciar el puente.

#### Estado de DTS:X para juegos

El protocolo del ring es ahora v2 y conserva el subtipo KS junto con cada lectura. El driver
`oem121.inf` captura MAT 2.0/2.1 y tambien IEC 61937 DTS, DTS-HD y DTS:X E1/E2; al cambiar el formato
reinicia las secuencias para no mezclar carriers. Con `DTS:X para centro de entretenimiento` activo,
una escena espacial 7.1.4 produjo 375 rafagas E1 validas, `Pc=0x0411`, sin gaps ni descartes. Los
payloads extraidos comienzan con el substream DTS-HD `0x64582025` y las seis fixtures de posicion
confirman que el transporte cambia con el objeto.

```powershell
.\build\dolby-probe.exe capture-iec61937-ring 10 `
  '.\captures\dtsx-capture.wav' 2
.\build\dolby-probe.exe analyze-iec61937 `
  '.\captures\dtsx-capture.wav'
.\build\dolby-probe.exe extract-dtshd `
  '.\captures\dtsx-capture.wav' '.\captures\dtsx-capture.dts'
.\build\dolby-probe.exe analyze-dtsx-exss `
  '.\captures\dtsx-capture.dts'
```

El EXSS no contiene core DTS, XBR, XLL ni LBR. Los 375 frames declaran una presentacion de 12
canales, 48 kHz y 24 bits en el modo auxiliar 3, con codec auxiliar ID 1. El asset estandar solo
ocupa cuatro bytes; el resto se divide exactamente, sin padding, en un paquete de configuracion
privado `0x3A429B0A` de 36 bytes y un paquete lossless `0x759A1908` de 72 a 5.832 bytes. Los mismos
sync words aparecen como constantes en `DtsxHdmiEnc.dll` y `DTSXDecoder.dll`: el encoder escribe
ambos paquetes y el decoder los despacha a rutinas separadas. FFmpeg solo implementa los
componentes DTS publicados y por eso no puede decodificar este codec auxiliar.

La ruta analogica DTS:X esta habilitada mediante el MFT oficial `DTSXDecoder`. La prueba de
`DTS:X Decoder` de Sound Unbound esta activa en esta maquina hasta el **31/07/2026**; el AppService
responde `Status=OK`. Esta licencia es independiente de `DTS:X para centro de entretenimiento`, que
solo habilita el encoder de juegos. El decoder no esta registrado como MFT Field-of-Use: aparece
con y sin `MFT_ENUM_FLAG_FIELDOFUSE`, se activa sin invocar `IMFFieldOfUseMFTUnlock` y el callback
registra cero llamadas. No se elude ningun control de licencia.

El MFT anuncia PCM 7.1 y `MFAudioFormat_Float_SpatialObjects`. Como Windows no publica una fabrica
para `IMFSpatialAudioSample`, el probe implementa la muestra y los
`IMFSpatialAudioObjectBuffer` requeridos, con colecciones de metadata creadas por el endpoint. La
captura de referencia produce 12 objetos de 512 frames por muestra: `FL, FR, FC, LFE, BL, BR, SL,
SR, TFL, TFR, TBL, TBR`. Tambien las fixtures de objetos dinamicos salen renderizadas por DTS como
una cama estatica 7.1.4, por lo que la ruta analogica no interpreta metadata privada.

```powershell
# Diagnostico de licencia y decodificacion offline.
.\build\dolby-probe.exe probe-dtsx-license CDTSXDecoder
.\build\dolby-probe.exe probe-dtsx-decode `
  '.\captures\dtsx-spatial-714.wav' 16 spatial
.\build\dolby-probe.exe probe-dtsx-decode `
  '.\captures\dtsx-spatial-714.wav' 32 pcm '.\captures\dtsx-decoded-71.wav'

# Puente de juegos DTS:X E1 a las salidas del perfil 7.1.4.
.\tools\Start-LiveDtsX714.ps1 -DurationSeconds 3600 -Gain 0.25
.\tools\Stop-LiveDtsX714.ps1
```

`live-dtsx-layout` consume el ring IEC 61937, separa los frames E1, decodifica incrementalmente y
entrega los 12 canales a `MultiEndpointRenderer`; conserva trims, delays y correccion de deriva por
endpoint. La prueba integral reinyecto la captura E1 al endpoint virtual y proceso 12.392.448 bytes,
375 bursts y 192.000 frames: cero drops, gaps, bursts malformados, clipping, objetos sin mapear o
starvation. Los errores de fase finales de las salidas secundarias fueron +0,063 ms y -0,367 ms.
La reinyeccion reproducible es:

```powershell
.\build\dolby-probe.exe replay-iec61937 `
  '.\captures\dtsx-spatial-714.wav' 'SinkDescription Sample' 1
```

`tools/Register-DolbyProbePackage.ps1` conserva este diagnostico reproducible: crea y firma el MSIX,
registra el alias `dolby-probe-dtsx.exe` y solicita UAC una sola vez para confiar en el certificado
local dentro de `LocalMachine\TrustedPeople`. No se debe distribuir ese certificado ni intentar
eludir el servicio de licencia. El comando `probe-dtsx-field-of-use` descarta de forma reproducible
esa ruta y `analyze-dtsx-exss` valida el transporte privado. El parche de FFmpeg de marzo de 2025
citado al final de este documento corresponde a objetos Dolby TrueHD, no a DTS:X.

Capturas de referencia:

- `captures/mat21-spatial-bed-71-distinct-tones.wav`
- `captures/mat21-spatial-height-tfl-1khz-tfr-1.5khz.wav`
- `captures/mat21-spatial-712-distinct-tones.wav`
- `captures/mat21-spatial-dynamic-900hz-moving.wav`
- `captures/mat21-spatial-712-silence.wav`
- `captures/mat21-spatial-dynamic-fixed-origin.wav`
- `captures/mat21-spatial-dynamic-fixed-left.wav`
- `captures/mat21-spatial-dynamic-fixed-right.wav`
- `captures/mat21-spatial-dynamic-fixed-above.wav`
- `captures/mat21-spatial-dynamic-fixed-below.wav`
- `captures/mat21-spatial-dynamic-fixed-front.wav`
- `captures/mat21-spatial-dynamic-fixed-behind.wav`
- `captures/mat21-spatial-dynamic-fixed-xquarter.wav`
- `captures/mat21-spatial-dynamic-fixed-yhalf.wav`
- `captures/mat21-spatial-dynamic-fixed-front-half.wav`
- `captures/extracted-mat21-spatial-bed-71-distinct-tones-static-712.wav`
- `captures/extracted-mat21-dynamic-moving-712.wav`
- `captures/mat-ring-live-full.wav`
- `captures/mat-ring-live-full-712.wav`

## Archivos

- `tools/Export-ActiveEdid.ps1`: respalda el EDID de uno o todos los monitores activos.
- `tools/Get-EdidAudio.ps1`: valida checksums y muestra SADs CTA de audio.
- `tools/New-MatEdid.ps1`: conserva video/timings y reemplaza solo audio/speaker allocation.
- `tools/Get-DriverTestReadiness.ps1`: informa Secure Boot, BitLocker, HVCI, BCD y toolchain.
- `tools/Set-TestSigning.ps1`: activa/desactiva Test Mode con guardas para Secure Boot y BitLocker.
- `tools/Install-WdkToolchain.ps1`: instala VS 2022 Community, SDK 26100 y WDK 26100 mediante WinGet.
- `tools/Install-Truehdd.ps1`: instala `truehdd` 0.4.0 para Windows y verifica el SHA-256
  publicado del archivo antes de extraerlo.
- `tools/Prepare-TrueHdAtmos712.ps1`: extrae un fragmento TrueHD Atmos, convierte la presentacion
  de objetos a DAMF y la renderiza como WAV PCM16 7.1.2.
- `tools/Build-DolbyPlayer.ps1`: compila el reproductor de peliculas y valida sus dependencias.
- `tools/Start-DolbyPlayer.ps1`: inicia una pelicula Atmos con video, subtitulos y salida analogica
  Realtek 8+2.
- `tools/DolbyPlayer`: reproductor incremental E-AC-3 JOC/TrueHD Atmos, frontend mpv y reloj WASAPI.
- `third_party/truehdd-src/src/bin/truehd-stream.rs`: decoder TrueHD presentation 3 incremental con
  protocolo binario THDS documentado en `STREAMING_PROTOCOL.md`.
- `tools/Capture-MatSpatialFixtures.ps1`: captura silencio y diez posiciones controladas de un
  objeto dinamico para investigar y calibrar la metadata MAT.
- `tools/Capture-Iec61937SpatialFixture.ps1`: captura una escena DTS:X desde el ring v2 y valida el
  subtipo IEC antes de guardarla.
- `tools/Capture-DtsXPositionFixtures.ps1`: automatiza las seis posiciones DTS:X de referencia.
- `tools/Register-DolbyProbePackage.ps1`: registra el probe como MSIX local para diagnosticar
  extensiones Media Foundation de Microsoft Store.
- `src/dolby_probe.cpp`: CLI y validacion de argumentos; no contiene logica de audio.
- `src/audio_platform.*`: RAII de COM/handles, endpoints, formatos PCM/MAT y dispositivo
  predeterminado.
- `src/wave_io.*`: lectura y escritura RIFF/WAVE compartida por capturas y extractores.
- `src/capture_commands.cpp`: pruebas de transporte, Spatial Sound y loopback WASAPI.
- `src/mat_capture_client.*`: cliente IOCTL del ring `\\.\DolbyDecoderMat`.
- `src/media_foundation_probe.cpp`: enumeracion de MFTs, licencia, decoder incremental y puente
  analogico DTS:X.
- `src/spatial_audio_sample.*`: implementacion caller-owned de muestras y objetos espaciales MF.
- `src/dtsx_analysis.cpp`: parser del EXSS auxiliar y de los paquetes privados del renderer DTS:X.
- `src/mat_format.*`: operaciones comunes sobre transporte y metadata MAT.
- `src/mat_analysis.cpp`: diagnosticos del carrier, posiciones de objetos y PCM multicanal.
- `src/mat_pipeline.cpp`: decodificador MAT 7.1.2 compatible y ruta configurable en vivo.
- `src/speaker_layout.*`: parser/validador de perfiles y paneo segun posiciones fisicas.
- `src/multi_endpoint_renderer.*`: WASAPI multidispositivo, colas, relojes y resampling adaptativo.
- `configs/realtek-c1u-714.ini`: cama Realtek 7.1, techo frontal C-1U y techo trasero Realtek.
- `tools/SpeakerLayoutEditor`: editor grafico WPF del perfil de parlantes y salidas.
- `tools/Start-Live714.ps1` / `tools/Stop-Live714.ps1`: administran el puente MAT 7.1.4.
- `tools/Start-LiveDtsX714.ps1` / `tools/Stop-LiveDtsX714.ps1`: administran el puente DTS:X 7.1.4.
- `build/dolby-probe.exe`: binario Windows ya compilado en esta maquina.

La separacion modular conserva la salida del decoder byte por byte. El fixture estatico
`mat21-spatial-712-distinct-tones.wav` sigue produciendo SHA-256
`c0a2d9fa83ebbd9a99686a724b625a0a96eed5c3392c9b2b7632e5b2def68691`; la salida de `list`
tambien coincide exactamente con el binario monolitico anterior. El anterior se conserva como
`build/dolby-probe-monolith.exe`.

El EDID generado contiene:

```text
LPCM:            0F 54 07  (8ch, 48/96/192 kHz, 16/20/24 bit)
E-AC-3/JOC:      57 04 03  (8ch, 48 kHz, JOC + ACMOD28)
MAT/MLP/TrueHD:  67 54 03  (8ch, object/channel PCM, sin hash obligatorio)
Speakers:        4F 00 00  (7.1)
```

## Uso

PowerShell 5 funciona con los scripts. El backup y el EDID de prueba ya fueron generados, pero el
override no se ha instalado.

```powershell
cd C:\Users\barra\Documents\Desarrollo\dolbyDecoder

.\tools\Export-ActiveEdid.ps1 `
  -InstanceId 'DISPLAY\SAM0F70\7&1892BE41&0&UID520'

.\tools\Get-EdidAudio.ps1 `
  -InputFile '.\generated\edid-backup\DISPLAY_SAM0F70_7_1892BE41_0_UID520.bin'

.\tools\New-MatEdid.ps1 `
  -InputFile '.\generated\edid-backup\DISPLAY_SAM0F70_7_1892BE41_0_UID520.bin' `
  -HardwareId 'MONITOR\SAM0F70'

.\tools\Get-EdidAudio.ps1 `
  -InputFile '.\generated\mat-override\DISPLAY_SAM0F70_7_1892BE41_0_UID520-mat.bin'

.\build\dolby-probe.exe list
.\build\dolby-probe.exe analyze '.\captures\battlefield-2042-atmos-shared.wav'
.\build\dolby-probe.exe analyze-mat '.\captures\mat21-spatial-712-distinct-tones.wav'
.\build\dolby-probe.exe analyze-mat-positions `
  '.\captures\mat21-spatial-dynamic-900hz-moving.wav'
.\build\dolby-probe.exe compare-mat-positions `
  '.\captures\mat21-spatial-dynamic-fixed-origin.wav' `
  '.\captures\mat21-spatial-dynamic-fixed-left.wav' `
  '.\captures\mat21-spatial-dynamic-fixed-right.wav' `
  '.\captures\mat21-spatial-dynamic-fixed-above.wav' `
  '.\captures\mat21-spatial-dynamic-fixed-front.wav' `
  '.\captures\mat21-spatial-dynamic-fixed-behind.wav'
.\tools\Capture-MatSpatialFixtures.ps1
.\build\dolby-probe.exe extract-mat-712 `
  '.\captures\mat21-spatial-712-distinct-tones.wav' `
  '.\captures\mat21-spatial-712-decoded.wav'
.\build\dolby-probe.exe play-712 `
  '.\captures\extracted-mat21-dynamic-moving-712.wav' `
  'Altavoces (Realtek(R) Audio)' '2nd output' 0.25
.\build\dolby-probe.exe capture-mat-ring 10 `
  '.\captures\mat-ring-capture.wav' 2
.\build\dolby-probe.exe live-712 3600 `
  'Altavoces (Realtek(R) Audio)' '2nd output' 0.25 80
.\tools\Start-Live712.ps1 -DurationSeconds 3600 -Gain 0.25
.\tools\Stop-Live712.ps1
.\build\dolby-probe.exe analyze-mat-layout `
  '.\captures\mat21-spatial-dynamic-fixed-above.wav' `
  '.\configs\realtek-c1u-714.ini'
.\tools\Start-Live714.ps1 -DurationSeconds 3600 -Gain 0.25
.\tools\Stop-Live714.ps1
.\build\dolby-probe.exe capture-process 25 (Get-Process bf1).Id `
  '.\captures\battlefield-1-native-atmos-process.wav'
```

El `.inf` generado documenta el override por bloques, pero Windows moderno normalmente exigira un
paquete firmado. Para la primera prueba es mas reversible importar el `.bin` con
[CRU](https://www.monitortests.com/forum/Thread-Custom-Resolution-Utility-CRU), reiniciar el driver
con `restart64.exe` y conservar `reset-all.exe` como recuperacion. No aplicar el archivo a otro
modelo ni modificar los bloques de video manualmente.

Tras reiniciar el driver:

1. Ejecutar `dolby-probe.exe list` y guardar el resultado.
2. Verificar que el S34J55x acepte PCM 7.1 y alguno de MAT 1.0/2.0/2.1 como `exclusive: exact`.
3. Abrir Dolby Access y seleccionar Dolby Atmos for Home Theater para ese endpoint.
4. Repetir el probe; Atmos activo deberia exponer objetos dinamicos, no cero.
5. Reproducir una escena espacial conocida y capturar el endpoint AMD:

```powershell
New-Item -ItemType Directory -Force .\captures
.\build\dolby-probe.exe capture 20 'S34J55x' '.\captures\atmos-on.wav'
```

El probe informa el formato visible para loopback y busca preambulos IEC 61937. Un loopback
`IEEE_FLOAT` solo demuestra que este punto de captura contiene PCM del motor; no descarta MAT en el
tramo posterior. La ausencia de preambulos tampoco es concluyente porque loopback no captura streams
exclusivos. Hay que comparar Atmos off/on y tratar cualquier resultado negativo como inconcluso.

No subir el volumen del monitor durante esta prueba: se esta anunciando deliberadamente una
capacidad que el S34J55x fisico no posee. Para restaurar, importar el backup con CRU o ejecutar
`reset-all.exe` y reiniciar el driver.

## Compilacion

En esta maquina, el camino reproducible no depende de que CMake este en `PATH`:

```powershell
.\tools\Build-DolbyProbe.ps1
.\tools\Build-SpeakerLayoutEditor.ps1
```

Con Visual Studio 2022, CMake y Windows SDK:

```powershell
cmake -S . -B build-vs -A x64
cmake --build build-vs --config Release
```

El binario actual tambien se compilo con MinGW-w64 y paso una captura loopback de humo: 8 canales
float/48 kHz del endpoint Realtek, con frames no silenciosos y cabecera WAV valida.

## Ruta posterior

El ring, el parser incremental y la salida analogica directa ya estan validados sin perdida. Las
siguientes iteraciones se concentran en fidelidad e integracion:

1. Reutilizar el renderer configurable en `DolbyPlayer` para peliculas 7.1.4.
2. Agregar una espera/notificacion cancelable al IOCTL despues de validar el sondeo inicial.
3. Medir latencia y recuperacion ante desconexion/reconexion fisica de DACs secundarios.
4. Comparar el paneo y el LFE contra un decoder de referencia; despues sustituir la repeticion LFE
   por un resampler con filtro.

El driver solo debe transportar bytes: la decodificacion, metadata y mezcla permanecen en user mode
para evitar trabajo pesado o bloqueante en callbacks de audio del kernel.

No se debe copiar codigo de Cavern al prototipo sin aceptar su licencia no comercial/share-alike.
El encoder de `raress96` opera en la direccion contraria (DAMF a E-AC-3 JOC) y el parche FFmpeg de
marzo de 2025 trabaja con el cuarto substream TrueHD; ninguno decodifica MAT 2.0 de juegos.

### Prueba de peliculas E-AC-3 JOC

`tools/Cavern712` usa la copia local de Cavern para inspeccionar Dolby Digital Plus Atmos y
renderizar sus objetos a PCM16 7.1.2. El orden de salida coincide con `play-712`:
`FL FR FC LFE BL BR SL SR TFL TFR`. Esta ruta no usa el endpoint MAT ni Dolby Access.

```powershell
dotnet build .\tools\Cavern712\Cavern712.csproj -c Release

dotnet .\tools\Cavern712\bin\Release\net8.0\Cavern712.dll `
  inspect .\captures\sample.eac3

dotnet .\tools\Cavern712\bin\Release\net8.0\Cavern712.dll `
  render .\captures\sample.eac3 .\captures\sample-712.wav 30

.\build\dolby-probe.exe play-712 .\captures\sample-712.wav `
  "Altavoces (Realtek(R) Audio)" "2nd output" 0.5 1
```

La muestra de *Guardians of the Galaxy Vol. 3* a 11:30 fue detectada como E-AC-3 JOC de
768 kb/s con 16 objetos (15 dinamicos mas LFE). En un render de 30 segundos, ambos canales de
techo tuvieron señal (`TFL` pico -24.1 dBFS, `TFR` pico -21.9 dBFS). Las salidas Realtek 7.1 y
frontal completaron la reproduccion con relojes iguales y delta de 0 ms.

La ruta incremental ya esta integrada en `DolbyPlayer`: Matroska entrega la pista elegida a
`EnhancedAC3Renderer`, Cavern actualiza JOC/OAMD cada 64 muestras y el PCM 7.1.2 alimenta una cola
acotada. Pausa y busqueda reconstruyen todo el estado interframe con 300 ms de preroll. La utilidad
y Cavern quedan sujetas a la licencia no comercial/share-alike incluida en `third_party/Cavern`.

### Prueba de peliculas TrueHD Atmos

Cavern identifica la cabecera MLP y el numero de objetos de una pista TrueHD Atmos, pero su
`MeridianLosslessPackingRenderer` no decodifica muestras. `Cavern712` ahora informa esta limitacion
y rechaza el render directo en vez de dejar un WAV incompleto. La ruta funcional usa
[`truehdd`](https://github.com/truehdd/truehdd) para convertir la presentacion Atmos 3 a DAMF; Cavern
renderiza despues las camas, objetos y metadata de ese DAMF a 7.1.2.

El archivo `28 Years Later The Bone Temple 2026.mkv` contiene una pista TrueHD Atmos a 48 kHz y
aproximadamente 4.15 Mb/s. `truehdd` confirmo cuatro substreams y una presentacion 3 independiente
con 12 elementos. La prueba de 29.94 segundos desde 10:00 genero
`captures/bone-temple-10m-30s-712.wav`; ambos canales superiores contienen senal (`TFL` pico
-35.64 dBFS y `TFR` -33.72 dBFS). La reproduccion analogica avanzo 29.942 segundos en ambos
endpoints Realtek, con delta final de 0.000 ms.

```powershell
.\tools\Install-Truehdd.ps1

.\tools\Prepare-TrueHdAtmos712.ps1 `
  -InputFile 'C:\Users\barra\Desktop\28 years\28 Years Later The Bone Temple 2026.mkv' `
  -StartSeconds 600 -DurationSeconds 30 -AudioStreamIndex 1 `
  -OutputBase '.\captures\bone-temple-10m-30s'

.\build\dolby-probe.exe play-712 `
  '.\captures\bone-temple-10m-30s-712.wav' `
  'Altavoces (Realtek(R) Audio)' '2nd output' 0.25 1
```

El script anterior se conserva como referencia offline. La reproduccion normal ya no crea archivos
temporales DAMF: el fork de `truehdd` incluye `truehd-stream`, que recibe bloques TrueHD desde
Matroska y emite PCM de objetos mas eventos OAMD en el protocolo THDS v1. El lector conserva el PTS
del major sync anterior a una busqueda, aplica el preroll exacto y renderiza cada actualizacion a
7.1.2 con Cavern.

### Reproductor de peliculas Atmos en tiempo real

`DolbyPlayer` reproduce MKV E-AC-3 JOC y TrueHD Atmos. mpv conserva video, HDR, subtitulos,
fullscreen y controles de pausa/busqueda, pero se inicia sin audio. El motor propio selecciona la
pista Atmos, renderiza `FL FR FC LFE BL BR SL SR TFL TFR` y envia los primeros ocho canales al
Realtek trasero y los dos superiores a la salida frontal. No requiere Dolby Access, el endpoint
SysVAD, F7 ni `live-712`; esos componentes solo se usan para Atmos de juegos.

Dependencias instaladas en este equipo: .NET 8, WSL con FFprobe y mpv 0.41. El decoder
`tools/truehdd/truehd-stream.exe` esta incluido junto con su fuente Apache-2.0. Para compilar y
reproducir:

```powershell
cd C:\Users\barra\Documents\Desarrollo\dolbyDecoder
.\tools\Build-DolbyPlayer.ps1

.\tools\Start-DolbyPlayer.ps1 `
  -InputFile 'C:\Users\barra\Desktop\28 years\28 Years Later The Bone Temple 2026.mkv' `
  -StartSeconds 600 -Gain 0.25
```

El primer stream Atmos se selecciona automaticamente. `-AudioStreamIndex N` fuerza el indice
global `0:N`; `-AvDelayMilliseconds N` adelanta el timeline de video cuando es necesario compensar
la latencia de pantalla. Los filtros de endpoint tambien son configurables con `-RearEndpoint` y
`-HeightEndpoint`.

La validacion realizada con *Bone Temple* y *Guardians of the Galaxy Vol. 3* cubre:

- E-AC-3 JOC: 5.06 s decodificados en 0.96 s; prueba analogica de 10.028 s, cero starvation.
- TrueHD Atmos: 5.10 s decodificados en 1.67 s; el render directo coincide con el DAMF de referencia
  por canal, incluidos picos `TFL -35.64 dBFS` y `TFR -37.52 dBFS`.
- Video/subtitulos: IPC mpv, pausa, busqueda exacta y seleccion de las 68 pistas del MKV.
- Integracion: E-AC-3 y TrueHD pasaron pausa, salto, reconstruccion y reanudacion sin starvation;
  TrueHD salto de aproximadamente 602 s a 607.023 s.
- Sincronizacion: drift A/V maximo de 41.7 ms en reproduccion continua y 90.1 ms durante la prueba
  de pausa/busqueda; cero correcciones duras. Los relojes Realtek terminaron a menos de 0.8 ms.

La correccion A/V usa el menor de los dos relojes WASAPI como maestro. Desajustes pequenos ajustan
la velocidad de video entre 0.995x y 1.005x; un desajuste superior a 250 ms busca el video sin mover
el audio. Al buscar manualmente se pausa la salida, se reconstruye el decoder desde un major sync o
frame anterior, se precargan 2.5 s y se reanuda.

## Referencias

- [Windows Spatial Sound](https://learn.microsoft.com/en-us/windows/win32/coreaudio/spatial-sound)
- [Representacion de MAT 1.0/2.0/2.1 en IEC 61937](https://learn.microsoft.com/en-us/windows/win32/coreaudio/representing-formats-for-iec-61937-transmissions)
- [Override EDID oficial de Windows](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/overriding-monitor-edids)
- [WASAPI loopback](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- [SysVAD, virtual audio device sample](https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/sysvad-virtual-audio-device-driver-sample/)
- [Interpretacion CTA del SAD MAT](https://android.googlesource.com/platform/external/edid-decode/+/a004802a68f85992fb92bc0c93b2773d413d7f9e/parse-cta-block.cpp)
- [Dolby MAT 2.0 para soundbars](https://professional.dolby.com/siteassets/tv/home/dolby-atmos/dolby-atmos-for-sound-bar-applications.pdf)
- [Cavern](https://github.com/VoidXH/Cavern)
- [truehdd](https://github.com/truehdd/truehdd)
- [dolby-atmos-encoder](https://github.com/raress96/dolby-atmos-encoder)
- [Parche TrueHD de FFmpeg](https://ffmpeg.org/pipermail/ffmpeg-devel/2025-March/341429.html)
- [DTS Sound Unbound FAQ](https://dts.com/dts-sound-unbound-faq/)
- [ETSI TS 102 114, DTS Core y extensiones](https://www.etsi.org/deliver/etsi_ts/102100_102199/102114/01.06.01_60/ts_102114v010601p.pdf)
- [Parser EXSS de FFmpeg](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/dca_exss.c)
