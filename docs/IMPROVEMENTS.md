# PROPOSTES — Millora i implementació de xarxes LEO/GEO via TTN

Aquest document recull (1) propostes de millora del sistema actual i (2) una proposta d'implementació per a comunicació per satèl·lit **LEO i GEO** amb **TTN (The Things Network)**.

---

## 1. Proposta de millora del sistema actual

### 1.1 Fix GNSS ràpid (prioritat alta)
**Problema:** el fix GNSS triga ~10 min perquè el receptor ha de descodificar l'efemèride dels satèl·lits (l'almanac de fàbrica és antic i no inclou efemèride precisa).

**Millora:** **A-GNSS via nRF Cloud** (efemèride fresca descarregada per la xarxa Onomondo). Fix en segons fins i tot amb senyal feble o pocs satèl·lits.
- Config: `CONFIG_NRF_CLOUD_REST=y` + `CONFIG_NRF_CLOUD_AGNSS=y`.
- Cal: compte nRF Cloud, provisionar el dispositiu (device ID + API key).
- Impacte: TTFF de ~10 min → segons; fix més fiable.

### 1.2 Estalvi d'energia (PSM / eDRX)
- **PSM (Power Saving Mode):** entre publicacions (5 min) el mòdem pot dormir profundament. `CONFIG_LTE_PSM_REQ=y` + valors TAU/active time.
- **eDRX:** reduir la freqüència de paging. `CONFIG_LTE_EDRX_REQ=y`.
- Impacte: bateria (si el dispositiu va amb bateria) molt més llarga.

### 1.3 Optimització de dades
- **Payload binari (CBOR/custom) en lloc de JSON:** estalvia ~50-70% de bytes → menys dades i menys senyalització.
- **Batching:** acumular vàries lectures i enviar-les juntes (ex. 1 cop/hora amb N punts).
- Impacte: consum de dades i RF reduït.

### 1.4 Robustesa de la connexió
- **Monitor de senyal (RSRP/RSRQ):** publicar el nivell de senyal al payload per detectar cobertura deficient.
- **Reconnexió adaptativa:** si MQTT falla reiteradament, fer re-registració LTE (ja implementat) + **reinici del mòdem** si persisteix.
- **MQTT keepalive dinàmic** segons la cadència de publicació.

### 1.5 FOTA (firmware over the air)
- Activar **MCUboot + SMP** per actualitzar el firmware remotament per xarxa (estalvia haver de flashejar per USB).
- Impacte: manteniment del camp molt més fàcil.

### 1.6 Broker propi per a les dades finals
- El broker HiveMQ és públic (dades visibles). Per a producció, usar un **broker propi** (Mosquitto/EMQX) amb TLS i autenticació (el codi ja ho suporta — només cal canviar `net_config.h` + CA).

### 1.7 Cadència adaptativa
- Publicar cada 5 min normalment, però **reduir a 1 cop/hora** si la posició no canvia (dispositiu estàtic) o si el senyal és dolent.
- Impacte: estalvi de dades i energia.

---

## 2. Proposta d'implementació per a xarxes LEO i GEO via TTN

### 2.1 Context tècnic (important)

| Tecnologia | Dispositiu | Comunicació satel·lital |
|------------|-----------|------------------------|
| **TTN** | LoRaWAN (SX1262/SX1261) | Via gateways satel·litals LoRaWAN |
| **nRF9151** | LTE-M / NB-IoT | Via **NTN NB-IoT** (GEO/LEO) — NO és TTN |

**Conclusió:** TTN és una xarxa **LoRaWAN**. El nRF9151 (LTE-M/NB-IoT) **no parla LoRaWAN**, així que per a TTN caldria un ràdio LoRaWAN addicional. Hi ha **dues vies** possibles:

---

### 2.2 Via A — NTN NB-IoT (directa amb el nRF9151 actual)

Recomanada perquè **no cal maquinari nou** — només firmware i un pla satel·lital.

**Requisits:**
1. **Firmware de mòdem NTN:** re-flashejar `mfw_nrf9151-ntn` (variant de mòdem per a satèl·lit).
2. **Pla satel·lital NB-IoT:** contractar amb un operador que doni NB-IoT satel·lital:
   - **Skylo** (ara Skyvera) — LEO/GEO NB-IoT (via Viasat, Terrestar, etc.).
   - **EchoStar / Terrestar** — NB-IoT GEO a Europa.
   - **SES / Eutelsat OneWeb** — constel·lacions LEO amb serveis IoT.
3. **Config al firmware (ja preparada):**
   - `net_config.h`: `MQTT_USE_NTN_NBIOT=1`, `MQTT_PUBLISH_INTERVAL_SECONDS_NTN=10800` (3 h — pla típic 50 KB/mes).
   - `prj.conf`: `CONFIG_LTE_NETWORK_MODE_NTN_NBIOT=y`.
   - El codi ja selecciona `LTE_LC_SYSTEM_MODE_NTN_NBIOT` quan `MQTT_USE_NTN_NBIOT=1`.

**Flux:** sensors + GNSS → payload → NTN NB-IoT (satèl·lit) → operador → Internet → broker MQTT.

**Limitacions:**
- NB-IoT satel·lital té **baixa velocitat** i **alta latència** (els sats no estan sempre a la vista).
- Cost per missatge relativament alt; per això la cadència de 3 h.
- Cobertura depèn del satèl·lit (LEO = finestres de visibilitat, GEO = cobertura fixa).

---

### 2.3 Via B — TTN + LoRaWAN satel·lital (LEO/GEO)

Aquesta és la via que realment implica **TTN**. Requereix **maquinari addicional** perquè el nRF9151 no és LoRaWAN.

**Arquitectura proposada:**

```
[nRF9151] -- UART/SPI -- [MCU LoRaWAN + ràdio SX1262 + mòdem satel·lital]
        |                                                        |
        | sensor/GNSS/MQTT (LTE-M)                          uplink LoRaWAN
        |                                                     via satèl·lit
        v                                                        v
    [Internet/LTE-M]                              [gateway satel·lital LEO/GEO]
                                                          |
                                                          v
                                                     [TTN (Network Server)]
                                                          |
                                      [MQTT / Webhook] -- v3/app/devices/dev/up
                                                          |
                                                          v
                                                    [teu backend]
```

**Passos d'implementació:**

1. **Maquinari:**
   - Afegir un **ràdio LoRaWAN** (ex. SX1262) + un **mòdem satel·lital LoRaWAN** (o un mòdul integrat tipus Swarm M138, EchoStar/Eutelsat LoRaWAN, Lacuna/Swarm M2M).
   - El nRF9151 faria de "brain" (sensors + GNSS + LoRaWAN via UART/SPI), o es delega a un MCU LoRaWAN dedicat.

2. **Serveis satel·litals LoRaWAN:**
   - **LEO:** Swarm (SpaceX) — mòduls M138/M138X + Swarm M2M; Lacuna (ara Swarm) — ràdios SX1262 + servei satel·lital.
   - **GEO:** EchoStar Mobile (EchoStar Global, banda S, Europa); Eutelsat (LoRaWAN GEO via OneWeb/Eutelsat OneWeb); Terrestar.
   - Cada servei té el seu gateway satel·lital que encamina els uplinks a **TTN** (Network Server).

3. **TTN:**
   - Crear el dispositiu a TTN (App + Device) — ex. l'app `insfpstqgat` que ja tens.
   - Registrar el device amb els identificadors LoRaWAN (DevEUI/JoinEUI/AppKey).
   - Rebre els uplinks via **MQTT** (`v3/{app}/devices/{dev}/up`) o **Webhooks** — el mateix mecanisme que vam investigar (recorda: TTN **no accepta** que un client publiqui uplinks; aquí el device real envia per LoRaWAN i TTN els publica).

4. **Payload:** mateix JSON (temp/pressió/humitat/posició), però **comprimit** per adaptar-se al límit de mida dels paquets LoRaWAN (SF7 ~222 bytes, SF12 ~51 bytes) i al duty cycle.

**Limitacions de la Via B:**
- Cost i complexitat de maquinari addicional.
- **Duty cycle** LoRaWAN (1% per canal) — poques transmissions al dia.
- Mida de payload molt limitada → cal un codificador binari eficient.
- Cost per missatge dels serveis satel·litals.
- Per a un dispositiu LTE-M, la Via A (NTN) és més natural i senzilla.

---

### 2.4 Recomanació

- **Si es vol mínima complexitat i aprofitar el nRF9151:** **Via A (NTN NB-IoT)** — només canviar el firmware de mòdem i contractar el pla satel·lital. TTN no hi participa.
- **Si es vol específicament TTN + satèl·lit (LEO/GEO):** **Via B** — cal afegir ràdio LoRaWAN + servei satel·lital, i integrar l'app amb TTN per MQTT/webhook. És un projecte amb més maquinari i costos.

**Suggeriment híbrid:** mantenir el nRF9151 amb LTE-M terrestre (Onomondo) com a via principal i usar el satèl·lit (Via A o B) només com a **fallback** quan no hi ha cobertura terrestre. El firmware ja té el flag `MQTT_USE_NTN_NBIOT` preparat per activar la via satel·lital.
