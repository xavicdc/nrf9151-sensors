# nRF9151-SMA-DK Sensor Monitoring + MQTT over LTE-M

Monitorització d'entorn amb el **Nordic nRF9151-SMA-DK**, construïda amb el **nRF Connect SDK (NCS) / Zephyr** i gestionada des de **PlatformIO**. El dispositiu es connecta a la xarxa **LTE-M** (SIM cel·lular), llegeix els sensors i publica les dades per **MQTT/TLS** a un broker cada 2 segons.

## Estat actual (validat)

- ✅ Connexió **LTE-M** amb SIM Deutsche Telekom (APN `internet.m2mportal.de`).
- ✅ Connexió **MQTT 3.1.1 sobre TLS** (`CONFIG_MQTT_LIB_TLS`), certificat del broker verificat amb CA embegut al mòdem.
- ✅ Publicació cada 2 s a `nrf9151/data` (broker actual: **HiveMQ** `broker.hivemq.com:8883`, validat end-to-end).
- ✅ Sensors: **QMP6988** (pressió + temperatura) funcionant.
- ❌ **SHT30** (humitat): no respon a l'I2C (vegeu [Problemes coneguts](#problemes-coneguts)).
- ✅ LEDs (4) i botons (4) amb control per interrupció i debounce.

Exemple de línia publicada:

```json
{"temperature":30.531,"pressure":1006.00,"humidity":null}
```

## Maquinari

| Element | Detall |
|---------|--------|
| Placa | nRF9151-SMA-DK (nRF9151, LTE-M/NB-IoT) |
| Mòdul sensors | M5Stack ENV III (SHT30 + QMP6988) al bus I2C2 (Arduino header, P0.30=SDA, P0.31=SCL) |
| SIM | Deutsche Telekom IoT, APN `internet.m2mportal.de` |
| Broker MQTT | Configurable a `src/net_config.h` (per defecte: HiveMQ públic) |

## Arquitectura del firmware

```
src/
├── main.c            → lògica principal: sensors, LEDs/botons, JSON, crida mqtt_app_init()
├── qmp6988.c/.h      → driver manual del QMP6988 (no existeix driver a Zephyr)
├── mqtt_app.c/.h     → LTE (lte_lc) + MQTT (mqtt_client) + bucle de publicació
├── credentials.c     → provisiona CA cert + APN al mòdem a l'arrencada
├── net_config.h      → TOTA la configuració: broker, credencials, topic, APN, sec_tag
├── ca_cert.h         → certificat CA (cadena) embegut (generat des de ca-cert.pem)
└── ca-cert.pem       → PEM font de la cadena CA del broker
```

### Flux d'arrencada

1. `main()` crida `mqtt_app_init()` → `nrf_modem_lib_init()` (inicialitza el mòdem).
2. El hook `NRF_MODEM_LIB_ON_INIT` (a `credentials.c`) provisiona el **CA cert** (sec tag 955) i configura l'**APN** via `AT+CGDCONT=1,"IP","internet.m2mportal.de"`, i després crida `mqtt_app_start()`.
3. `mqtt_app_start()` registra el handler de LTE i crida `lte_lc_connect_async()`.
4. Quan LTE es registra (`LTE_LC_NW_REG_REGISTERED_HOME/ROAMING`), el thread MQTT connecta al broker: `mqtt_connect()` → socket TLS del mòdem → `CONNACK`.
5. El thread fa `poll()` + `mqtt_input()` (processa el CONNACK) i, un cop connectat, buida la cua de payloads (`k_msgq`) publicant cada dades que arriba de `main()`.
6. `main()` llegeix QMP6988 (+ SHT30 si funcionés) cada 2 s, construeix el JSON i crida `mqtt_app_publish()`.

### Configuració del broker (`src/net_config.h`)

```c
#define MQTT_BROKER_HOSTNAME "broker.hivemq.com"   // hostname per TLS_HOSTNAME (SNI + verificació)
#define MQTT_BROKER_IP       "3.69.77.221"          // IP IPv4 estàtica (evita el DNS IPv6 del mòdem)
#define MQTT_BROKER_PORT     8883                   // port TLS
#define MQTT_DEVICE_ID       "nrf1"                 // client_id
#define MQTT_TOPIC           "nrf9151/data"         // topic de publicació
#define MQTT_BROKER_USERNAME ""                     // usuari (buit = sense autenticació)
#define MQTT_BROKER_PASSWORD ""                     // password
#define MQTT_TLS_SEC_TAG     955                    // sec_tag on es desa el CA cert al mòdem
#define LTE_APN              "internet.m2mportal.de"
```

> ⚠️ **Credencials:** `net_config.h` és el fitxer on van usuari/password del broker. **No commitis mai credencials reals.** Aquest repositori es commiteja amb el broker públic HiveMQ (credencials buides).

## Requisits i construcció

- **NCS v3.2.x** inicialitzat (workspace `C:/Users/jcano23/nRF1`).
- Toolchain NCS (`C:/ncs/toolchains/936afb6332`).
- **PlatformIO Core** amb la plataforma personalitzada `nordicnrf91`.

```bash
pio run              # Compila
pio run -t upload    # Compila i programa (via west flash / nrfutil)
pio device monitor -p COM19 -b 115200   # Consola (COM20 = boot TF-M)
```

> **Windows / MAX_PATH:** el build del TF-M + driver oberon genera paths >260 caràcters que Ninja no pot crear encara amb els *long paths* de Windows activats. Per això `platformio.ini` redirigeix el build a un directori curt:
> ```ini
> [platformio]
> build_dir = C:/N/build
> ```
> Ajusta aquest camí a un directori curt del teu sistema si cal.

## Configuració Kconfig (`prj.conf`)

| Bloc | Opcions clau |
|------|--------------|
| Sensors | `CONFIG_I2C`, `CONFIG_SENSOR`, `CONFIG_SHT3XD` |
| Mòdem | `CONFIG_NRF_MODEM_LIB`, `CONFIG_LTE_LINK_CONTROL`, `CONFIG_LTE_NETWORK_MODE_LTE_M` |
| Xarxa | `CONFIG_NET_SOCKETS_OFFLOAD`, `CONFIG_NRF_MODEM_LIB_NET_IF`, `CONFIG_NET_CONNECTION_MANAGER` |
| MQTT | `CONFIG_MQTT_LIB`, `CONFIG_MQTT_LIB_TLS`, `CONFIG_MQTT_KEEPALIVE=30` |
| TLS | `CONFIG_MODEM_KEY_MGMT` (credencials al mòdem) |

## Canviar de broker

1. Edita `src/net_config.h`: hostname, IP, port, usuari/password.
2. Substituïu la cadena CA de `src/ca-cert.pem` per la del nou broker i regenereu `src/ca_cert.h` (vegeu [docs/PROCESS.md](docs/PROCESS.md) per la generació).
3. Recompila i flasheja.

**Requisits del broker per al nRF91 (importants):**

- **MQTT/TCP pla (no WebSocket).** El mòdem no pot completar el handshake WebSocket perquè la partició cripto **TF-M no implementa SHA1** (necessari pel `Sec-WebSocket-Accept`). Per això el broker ha d'exposar MQTT per TCP directe, **no** darrere un túnel WebSocket (ex. Cloudflare Quick Tunnel, que només passa HTTP/WS).
- **Certificat TLS del broker:** el TLS del mòdem ha verificat correctament amb **cert RSA** (HiveMQ, TTN). Amb certificats **ECDSA** (ex. túnels Cloudflare) el mòdem **sí completa el handshake TLS**, però no s'ha pogut validar de cap a cap — es recomana cert RSA o MQTT sense TLS.

## Problemes coneguts

### SHT30 no detectat
L'escàner I2C només troba `0x70` (QMP6988). El SHT30 (0x44/0x45) no respon ni amb *soft reset* (prova a `sht30_probe()`), ni baixant el bus a 100 kHz, ni en cap altra placa. Conclusió: el xip SHT30 d'aquest mòdul ENV III concret no respon elèctricament al bus; el QMP6988 (mateix bus/cable) funciona perfectament. El firmware continua publicant amb `"humidity":null`.

### Broker propi darrere Cloudflare Quick Tunnel (WebSocket)
El túnel *quick* de `trycloudflare.com` només encamina HTTP/HTTPS/WebSocket (no TCP pur). El mòdem nRF91 no pot fer MQTT-over-WebSocket (limitació SHA1 a TF-M), per tant un broker exposat així **no és accessible** des del dispositiu. Solucions: túnel TCP pur (`bore`, `rathole`, `frp`), broker amb IP pública, o un broker VPS.

## Verificació end-to-end

Subscriu-te al topic des de qualsevol client MQTT per veure els missatges:

```bash
mosquitto_sub -h broker.hivemq.com -p 8883 --cafile src/ca-cert.pem -t 'nrf9151/data'
```

## Historial del procés

Vegeu [docs/PROCESS.md](docs/PROCESS.md) per a la crònica detallada i exhaustiva del desenvolupament, els diagnòstics i els aprenentatges (SHT30, LTE, certificats TLS, WebSocket, MAX_PATH, etc.).
