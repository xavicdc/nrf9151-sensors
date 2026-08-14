# PROCESS — Crònica exhaustiva del desenvolupament

Aquest document recull, de manera detallada, tot el procés de posada en marxa del projecte: des de l'estat inicial (blink + sensors) fins a la publicació MQTT sobre LTE-M validada de cap a cap. Inclou els diagnòstics, els errors trobats, les decisions i les evidències (logs) de cada etapa.

---

## 1. Estat inicial

Projecte PlatformIO `pio_nrf9151_blinky` per a la placa **nRF9151-SMA-DK** amb el NCS (Zephyr). Estat inicial (`git log`):

- `466f22b` — nRF9151-SMA-DK sensor monitoring (SHT30 + QMP6988) with LED/button control
- `d39973d` — Add README with project overview

Funcionalitat inicial: lectura del **QMP6988** (pressió/temp) i **SHT30** (temp/humitat) per I2C2, 4 LEDs amb parpelleig i 4 botons amb debounce.

---

## 2. Investigació del SHT30 (no detectat)

**Símptoma:** l'escàner I2C només trobava `0x70` (QMP6988); el SHT30 (0x44) no apareixia i el driver deia `SHT30 not ready`.

**Proves fetes:**

1. **Escàner I2C complet** (0x03–0x7F): només `0x70`.
2. **Prova amb soft reset** (ordre 0x30A2) + lectura de registre d'estat a 0x44 i 0x45: `no ACK` en tots dos.
3. **Baixar la velocitat del bus** a 100 kHz (`clock-frequency = <100000>` a l'overlay): continua `no ACK`.
4. **Voltatge/velocitat descartats**: el QMP6988 funciona al mateix bus, mateixa alimentació, mateixa velocitat — no pot ser tensió ni velocitat.

**Conclusió:** el xip SHT30 d'aquesta unitat del mòdul M5Stack ENV III no respon elèctricament al bus I2C (probablement defectuós). El firmware continua funcionant amb el QMP6988 i publica `"humidity":null`.

**Aprenentatge:** quan dos sensors comparteixen bus, alimentació i velocitat i un funciona i l'altre no, la causa és el xip mateix, no la configuració.

---

## 3. Primer intent de broker: The Things Network (TTN)

L'objectiu era publicar temp/humitat a un broker MQTT. El primer broker provat va ser **TTN** (`eu1.cloud.thethings.network:8883`), amb usuari `insfpstqgat@ttn` i una API key `NNSXS...`.

**Resultat:** la connexió TLS + MQTT es va establir correctament (el firmware deia `MQTT connected`), però **TTN mai mostrava els missatges**.

**Verificació des del PC (paho-mqtt):** en subscriure's i publicar al topic `v3/insfpstqgat/devices/nrf1/up`, el missatge **no es reenviava als subscriptors** (fins i tot amb l'envelope complet `uplink_message`). 

**Conclusió (documentada a la doc oficial de TTN):** el broker MQTT de TTN està pensat perquè les *aplicacions* rebin uplinks de dispositius **LoRaWAN** i enviïn downlinks. **No accepta que un client extern publiqui uplinks** al topic `up`; els descarta en silenci. Un nRF9151 (LTE-M) no pot injectar dades a TTN per MQTT.

---

## 4. El broker propi (Cloudflare Quick Tunnel, WebSocket)

El broker real era un **Mosquitto/EMQX local** exposat via **Cloudflare Quick Tunnel** (`xxx.trycloudflare.com`) com a **MQTT sobre WebSocket** al port 443, amb usuari/password.

**Verificació des del PC (paho `transport="websockets"`):** la connexió i la publicació **funcionaven perfectament** (el broker reemetia el missatge als subscriptors). El broker era correcte; el problema era fer-ho des del nRF91.

---

## 5. Desenvolupament del firmware de connectivitat

### 5.1 LTE

- `CONFIG_LTE_LINK_CONTROL=y` proporciona l'API `lte_lc`.
- **Error inicial:** `lte_lc: Could not get registration status, error: -1`. El mòdem no responia a `AT+CEREG?`.
- **Causa arrel:** cap codi cridava `nrf_modem_lib_init()`. Els samples oficials del NCS el criden explícitament a `main()`. Sense això, el mòdem no s'inicialitza i l'AT falla sempre.
- **Fix:** `mqtt_app_init()` crida `nrf_modem_lib_init()` abans de `lte_lc_connect_async()`.

### 5.2 APN

L'APN de Deutsche Telekom es configura amb l'ordre AT al hook d'init del mòdem:

```
AT+CGDCONT=1,"IP","internet.m2mportal.de"
```

(`credentials.c`, dins de `NRF_MODEM_LIB_ON_INIT`).

### 5.3 Credencials TLS

- El CA cert del broker s'embeu al firmware (`ca_cert.h`, generat des de `ca-cert.pem`) i es provisiona al mòdem amb `modem_key_mgmt_write(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, ...)`.
- `sec_tag = 955` (qualsevol valor < 2147483647 vàlid).

### 5.4 MQTT

S'usa el `mqtt_client` de Zephyr directament (`CONFIG_MQTT_LIB`), amb transport **TLS** (`MQTT_TRANSPORT_SECURE`) i el socket TLS del mòdem (`IPPROTO_TLS_1_2`, `TLS_SEC_TAG_LIST`, `TLS_HOSTNAME`).

**Error important resolt:** després de `mqtt_connect()`, cal processar el CONNACK cridant `mqtt_input()` en un bucle de `poll()`. Sense això, l'espera del CONNACK s'esgotava i el firmware deia `MQTT connection not established`.

### 5.5 DNS / IPv6

El DNS del mòdem retornava adreces **AF_INET6** per hostnames amb només IPv4 (fins i tot IPv4-mapped). `zsock_connect` a aquestes adreces fallava (`-EPROTO`). **Solució:** usar una **IP IPv4 estàtica** del broker (resolta des del PC) a `MQTT_BROKER_IP`, mantenint el hostname només per a `TLS_HOSTNAME` (SNI + verificació).

---

## 6. L'intent de WebSocket i el mur SHA1/TF-M

Per connectar al broker del Cloudflare (WebSocket), es va provar el transport WebSocket de Zephyr (`MQTT_TRANSPORT_SECURE_WEBSOCKET` + `CONFIG_MQTT_LIB_WEBSOCKET` + `CONFIG_WEBSOCKET_CLIENT`).

**Diagnòstic amb logs de debug** (`CONFIG_NET_LOG=y` + `CONFIG_NET_WEBSOCKET_LOG_LEVEL_DBG=y`):

```
[dbg] net_mqtt_sock_tls: mqtt_client_tls_connect: Created socket 3
[dbg] net_mqtt_sock_tls: mqtt_client_tls_connect: Connect completed      <- el TLS amb Cloudflare OK
[dbg] net_websocket: websocket_connect: Cannot calculate sha1 (-134)     <- PSA_ERROR_NOT_SUPPORTED
[err] net_mqtt_websocket: Websocket connect failed (-71)
```

**Troballes clau:**

1. El TLS del mòdem **sí completa el handshake amb Cloudflare** (cert ECDSA). El problema no era el certificat.
2. El handshake WebSocket necessita calcular **SHA1** (`Sec-WebSocket-Accept`), però `psa_hash_compute(PSA_ALG_SHA_1)` retorna **`PSA_ERROR_NOT_SUPPORTED` (-134)**: la partició cripto **TF-M no implementa SHA1**.
3. Es va habilitar `PSA_WANT_ALG_SHA_1` (definit als configs generats) però **TF-M segueix sense SHA1 a runtime**. `CONFIG_PSA_CRYPTO_ENABLE_ALL` trenca el build (RSA sense mida de clau).
4. **Conclusió:** el mòdem nRF91 **no pot fer MQTT sobre WebSocket** en aquesta configuració. Com que els *Cloudflare Quick Tunnels* només passen HTTP/WS, un broker exposat així **no és accessible** des del dispositiu.

**Verificació complementària:** amb **TLS pla** (sense WebSocket) cap a Cloudflare, el mòdem completava el TLS i Cloudflare responia amb un error HTTP que es malparsava com `MQTT CONNACK refused, code: 113` — confirmant que el TLS sí funcionava i que el trànsit no era WebSocket.

---

## 7. Solució final: MQTT/TLS pla amb cert RSA

Es va descartar el WebSocket i es va usar **MQTT/TLS pla** directe. Per validar la pipeline completa sense dependre del broker propi, es va triar un broker públic amb **cert RSA**: **HiveMQ** (`broker.hivemq.com:8883`).

**Verificació final (logs del dispositiu):**

```
LTE: searching for network...
LTE network registered
Connecting to MQTT broker broker.hivemq.com (TLS)
CA cert present in modem
Using static broker IP 3.69.77.221:8883
MQTT connected
Published: {"temperature":30.507,"pressure":1005.99,"humidity":null}
...
```

**Verificació end-to-end des del PC** (paho-mqtt subscrit a `nrf9151/data`): es van rebre **6 missatges reals** del dispositiu.

---

## 8. Windows MAX_PATH (build del TF-M)

**Símptoma:** el build del TF-M fallava amb:

```
ninja: error: mkdir(secure_fw/.../nrf_oberon/CMakeFiles/oberon_psa_driver.dir/C_/Users/.../oberon-psa-crypto/oberon/drivers): No such file or directory
```

**Causa:** en habilitar SHA1, el driver `oberon_psa_driver` del TF-M compila fitxers de `oberon-psa-crypto/oberon/drivers`, i el path de l'objecte superava els 260 caràcters. Tot i que `LongPathsEnabled` ja era 1, **Ninja no és long-path-aware**.

**Solució:** redirigir el build a un directori curt amb `platformio.ini`:

```ini
[platformio]
build_dir = C:/N/build
```

Això escurça prou el path complet perquè Ninja el creï.

---

## 9. Aprenentatges tècnics (resum)

| Tema | Troballa |
|------|----------|
| SHT30 | Dos sensors al mateix bus/cable/alimentació: si un funciona i l'altre no, el xip és el sospitós |
| `nrf_modem_lib_init()` | S'ha de cridar explícitament (els samples del NCS ho fan a `main()`) |
| CONNACK MQTT | Després de `mqtt_connect()`, cal `poll()` + `mqtt_input()` per processar el CONNACK |
| DNS mòdem | Retorna IPv6 (incloent IPv4-mapped) → usar IP IPv4 estàtica per connectar |
| TLS mòdem | Funciona amb certs **RSA**; amb ECDSA el handshake TLS sí es completa |
| WebSocket nRF91 | Impossible: el handshake requereix SHA1 i la partició TF-M no el implementa |
| Cloudflare Quick Tunnel | Només HTTP/HTTPS/WS (no TCP pur) → inútil per a MQTT pla |
| Windows Ninja | No és long-path-aware → `build_dir` curt |

---

## 10. Com reproduir la generació del CA cert (`ca_cert.h`)

El fitxer `ca_cert.h` és un array C generat des de `ca-cert.pem` (la cadena PEM del broker). Per a un broker nou:

1. Desa la cadena CA (intermedis + root) a `src/ca-cert.pem`.
2. Converteix el PEM a array C (exemple PowerShell):

```powershell
$pem = Get-Content src/ca-cert.pem -Raw
$bytes = [System.Text.Encoding]::ASCII.GetBytes($pem)
$sb = "static const unsigned char ca_certificate[] = {`n"
for ($i = 0; $i -lt $bytes.Length; $i += 12) {
    $len = [Math]::Min(12, $bytes.Length - $i)
    $sb += "  " + ((0..($len-1) | ForEach-Object { "0x{0:x2}" -f $bytes[$i+$_] }) -join ", ") + ",`n"
}
$sb += "};"
Set-Content src/ca_cert.h $sb -Encoding ascii
```

> ⚠️ Incloure sempre la **cadena sencera** (intermedis + root) perquè alguns servidors no envien l'intermediari al handshake.

---

## 11. Estat dels fitxers rellevants (commit actual)

| Fitxer | Rol |
|--------|-----|
| `src/main.c` | Sensors + LEDs/botons + JSON + `mqtt_app_init()`/`mqtt_app_publish()` |
| `src/mqtt_app.c` | LTE (`lte_lc`), MQTT (`mqtt_client`), bucle poll/keepalive/publicació |
| `src/credentials.c` | Provisionament CA + APN al mòdem |
| `src/net_config.h` | Broker, credencials, topic, APN, sec_tag |
| `src/ca_cert.h` / `ca-cert.pem` | Cadena CA del broker |
| `prj.conf` | Kconfig: sensors, mòdem, xarxa, MQTT/TLS |
| `platformio.ini` | Plataforma `nordicnrf91`, paths NCS, `build_dir` curt |
| `boards/nrf9151dk_nrf9151_ns.overlay` | Node SHT30 + `clock-frequency = 100000` al I2C2 |

---

## 12. GNSS, pla de dades i NTN

### 12.1 GNSS / GPS

Es va activar el mode del sistema **LTE-M + GPS** (`LTE_LC_SYSTEM_MODE_LTEM_GPS`, `AT%XSYSTEMMODE=1,0,1`) i el receptor GNSS del mòdem (`nrf_modem_gnss_*`). Punts clau:

- `nrf_modem_gnss_event_handler_set()` + `nrf_modem_gnss_start()`.
- Al event `NRF_MODEM_GNSS_EVT_PVT` es llegeix `struct nrf_modem_gnss_pvt_data_frame`; només es considera fix vàlid si `pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID`.
- El fix necessita **cel obert**; a l'interior només apareix `GNSS: searching satellites...`.
- `gnss_init()` ha de córrer DESPRÉS que el mode del sistema inclogui GPS (el thread MQTT el fixa); per això `main` el crida cada cicle fins que arrenca (idempotent).

### 12.2 Pla de dades (terrestre 6.5 MB/mes)

Cada publicació costa ~160 B (payload + MQTT/TCP/IP/TLS) + senyalització LTE-M. La cadència inicial de **2 s** implicava ~43.200 missatges/mes ≈ **6.5 MB/mes** — al límit del pla. L'evidència (rebuig **EMM cause 15** "no suitable cells in tracking area") apunta que **el pla es va esgotar**, no a un problema del mòdem.

Mesures preses:
- Cadència de publicació baixada a **60 s** (`MQTT_PUBLISH_INTERVAL_SECONDS`) ≈ 230 KB/mes.
- `CONFIG_MQTT_KEEPALIVE` pujat a **120 s** i supressió de PING just després d'un PUBLISH (els PUBLISH ja compten com a activitat).

### 12.3 NTN NB-IoT (satèl·lit GEO/LEO) — preparat

La SIM té un pla satel·lital de **50 KB/mes**. S'ha preparat el codi per a NTN (`MQTT_USE_NTN_NBIOT` + cadència `MQTT_PUBLISH_INTERVAL_SECONDS_NTN` = 3 h ≈ 38 KB/mes), però:

- ⚠️ NTN requereix el **firmware de mòdem `mfw_nrf9151-ntn`**, un firmware DIFERENT del terrestre (no es poden usar tots dos alhora). No està disponible al sistema; cal obtenir-lo i flashejar-lo.
- Requereix que el pla satel·lital estigui actiu i cobertura de satèl·lit.
- Es deixa `MQTT_USE_NTN_NBIOT=0` (terrestre) fins que calgui canviar.

### 12.4 Aprenentatge de diagnòstic

En aquesta versió del NCS, els enums de mode són: `LTE_LC_LTE_MODE_LTEM = 7` i `LTE_LC_LTE_MODE_NBIOT = 9` (no 1 i 2). Interpretar-los amb els valors d'una altra versió va portar a una diagnosi errònia (pensar que el mòdem estava en "multimode") quan en realitat estava correctament en LTE-M i el rebuig era de la xarxa.

---

## 13. Evolució posterior: Onomondo, GNSS i A-GNSS

### 13.1 Canvi a SIM Onomondo
- APN **`onomondo`** (operador virtual; registra com a *roaming*).
- Pla **50 MB/mes** LTE-M/NB-IoT. Cadència de publicació baixada a **5 min** (≈46 KB/mes).
- Es va provar LTE-M+NB-IoT+GPS, però el GNSS no generava events amb NB-IoT; es va fixar en **LTE-M+GPS**.

### 13.2 Descobriment de l'SHT30 (resolt)
- El SHT30 no responia a 3.3V. **Causa: el mòdul requereix 5V.** Alimentant-lo a 5V respon i publica la humitat.

### 13.3 El repte del fix GNSS
- **Stack overflow** resolt: el `struct nrf_modem_gnss_pvt_data_frame` (gran) s'havia de declarar estàtic a l'event handler; es va treure el printf amb floats del thread del mòdem; `CONFIG_FPU_SHARING` + `CONFIG_MAIN_STACK_SIZE=4096`.
- El GNSS no arrencava mentre LTE es registrava (contenció RF) → **retry de `gnss_init()`** cada 5 s fins que LTE quedés idle.
- El GNSS veia satèl·lits però **no fixava amb LTE actiu** (contenció RF) → es va dissenyar l'**alternança de modes**: burst GNSS-únic per obtenir fix, després LTE per transmetre.
- La primera alternança (cada 5 min) **interrompia el GNSS** abans de descodificar l'efemèride → **finestra llarga (15 min) cada hora** + retenció de l'última posició.
- **Antena**: canviar a una antena GNSS adequada va pujar el cn0 de ~30-35 a **40-49 dBHz**.
- **A-GNSS minimal** implementat (sense compte de núvol): escriptura de l'almanac de fàbrica al mòdem + injecció d'hora (`AT+CCLK`) i ubicació (MCC via `AT%XMONITOR`) quan LTE es registra. La injecció corre en un thread dedicat (les ordres AT bloquegen).
- **Resultat:** amb GNSS-únic ininterromput + assistència, el fix arriba en ~10 min: `GPS fix: 41.4840, 2.1526 (alt 115.1 m, acc 11 m)`.

### 13.4 Conclusió clau
El GNSS **sí funciona**; necessita una finestra llarga sense interrupcions i (idealment) A-GNSS amb efemèride fresca per fixar ràpid.
