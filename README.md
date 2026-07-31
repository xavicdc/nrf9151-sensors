# nRF9151-SMA-DK Sensor Monitoring

Projecte de monitorització d'entorn per al kit de desenvolupament **Nordic nRF9151-SMA-DK**, construït amb el **nRF Connect SDK (NCS) / Zephyr** i gestionat des de **PlatformIO**.

## Funcionalitats

### Sensors d'entorn (I2C)

El programa llegeix el mòdul **M5Stack ENV III** (connectat al bus I2C2, pins SDA=P0.30, SCL=P0.31) que incorpora dos sensors:

| Sensor | Magnituds | Adreça I2C |
|--------|-----------|------------|
| **SHT30** | Temperatura i humitat | `0x44` |
| **QMP6988** | Temperatura i pressió baromètrica | `0x70` |

Cada 2 segons envia per UART (VCOM0 / COM19, 115200 bauds) un línia semblant a:

```
QMP6988 temp=23.48 C  pressure=999.91 hPa
SHT30 temp=25.31 C  hum=45.20 %
```

### Control de LEDs i botons

- Els **4 LEDs** parpellegen a cadències diferents: 0,5s / 1s / 1,5s / 2s.
- Cada botó (**SW0**–**SW3**) activa o desactiva el LED corresponent.
- Les transicions s'antireboten (debounce de 50 ms) i es mostren per UART (`LED0 ON/OFF`, ...).

## Estructura del projecte

```
├── boards/
│   └── nrf9151dk_nrf9151_ns.overlay   # Nodes I2C dels sensors
├── src/
│   ├── main.c                         # Lògica principal (sensors + LEDs/botons)
│   ├── qmp6988.c / qmp6988.h          # Driver manual del QMP6988 (no hi ha driver a Zephyr)
├── CMakeLists.txt
├── platformio.ini                     # Config PlatformIO (plataforma nordicnrf91)
└── prj.conf                           # Config Zephyr (I2C, sensor, GPIO)
```

## Requisits

- **nRF Connect SDK (NCS)** v3.2.x instal·lat i workspace inicialitzat
- Toolchain NCS (`C:\ncs\toolchains\...`)
- Entorn `west` operatiu (ex. `~\.zephyrtools`)
- **PlatformIO Core** amb la plataforma personalitzada `nordicnrf91`

## Compilar i programar

```bash
pio run              # Compila
pio run -t upload    # Compila i programa la placa
```

> La plataforma `nordicnrf91` delega la compilació a `west build` usant el NCS del sistema. Els camins del workspace/toolchain es configuren a `platformio.ini`.

## Configuració

A `platformio.ini`:

| Opció | Descripció | Valor per defecte |
|-------|------------|-------------------|
| `ncs_workspace` | Workspace del NCS (conté zephyr/, nrf/, nrfxlib/) | `C:/Users/jcano23/nRF1` |
| `ncs_toolchain` | Directori del toolchain NCS | `C:/ncs/toolchains/936afb6332` |
| `nrf91_board` | Board target Zephyr | `nrf9151dk/nrf9151/ns` |

## Nota sobre el SHT30

Si el SHT30 no es detecta (l'escàner I2C només mostra `0x70`), el programa continua funcionant amb el QMP6988 i avisa per UART que la humitat no està disponible. Verifica el cablatge o que el mòdul no sigui defectuós.
