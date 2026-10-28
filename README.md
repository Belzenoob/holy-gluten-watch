# Orologio Holy Gluten

Il lievito madre al polso. Questo repository contiene il **firmware** e la
**custodia stampabile** dell'orologio di [Holy Gluten](https://holygluten.app),
l'app per la panificazione: chi ha la scheda può costruirsi il proprio.

Sull'orologio vive la mascotte del lievito madre: gli stessi occhi della
scheda Lievito dell'app, con stati d'animo che dipendono dai rinfreschi,
animazioni, suoni e gesti. Dal firmware 0.3.0 l'orologio disegna anche il
**compagno** dell'account (Gufetto, Lumachina, Ranocchio…), come l'app. L'app manda all'orologio la cronologia dei
rinfreschi, il nome del lievito e l'ora via Bluetooth LE; l'orologio non ha
Wi-Fi, non ha account e non parla con nessun server.

- `firmware/`: progetto ESP-IDF (C++, core Arduino come componente)
- `case/stl/`: custodie in TPU, pronte da stampare

## Cosa serve

- Scheda **Waveshare ESP32-C6-Touch-AMOLED-2.06** (schermo AMOLED 410×502,
  touch, accelerometro, RTC, codec audio). Con la sua batteria al litio.
- Un cavo USB-C dati.
- Filamento **TPU** (95A o 85A) per la custodia.
- L'app Holy Gluten su Android per sincronizzare i rinfreschi.

## Installare il firmware già compilato

Scarica dalla pagina [Releases](../../releases/latest) due file:

- `holy-gluten-watch-v1.1.0.bin`: l'immagine completa (bootloader, tabella
  delle partizioni, app), da scrivere all'indirizzo `0x0`;
- `companions-pack.bin`: i disegni dei compagni, da scrivere all'indirizzo
  `0x610000` (partizione `cmp`). Senza questo file l'orologio funziona e
  mostra sempre Madre.

Aggiornando da una versione 1.0.0 vanno scritti **entrambi**: la tabella delle
partizioni è cambiata (la partizione `ffat`, mai usata, ora è `cmp`). Scrivere
l'immagine a `0x0` azzera lo stato salvato sull'orologio (rinfreschi, umore):
l'app lo rimanda alla prossima sincronizzazione.

### Dal browser (più semplice)

1. Apri [ESP Tool](https://espressif.github.io/esptool-js/) con Chrome o Edge
   sul computer.
2. Collega la scheda con il cavo USB-C e premi **Connect**; scegli la porta
   della scheda (`USB JTAG/serial debug unit`).
3. In **Flash Address** lascia `0x0` e scegli `holy-gluten-watch-v1.1.0.bin`.
   Con **Add File** aggiungi una seconda riga: indirizzo `0x610000`, file
   `companions-pack.bin`. Premi **Program**.
4. Alla fine la scheda si riavvia e compare la mascotte; se lo schermo resta
   nero, premi **Hard reset** nella pagina.

### Da terminale, con esptool

```bash
pip install esptool
esptool --chip esp32c6 -p /dev/cu.usbmodem101 -b 460800 write-flash \
  0x0 holy-gluten-watch-v1.1.0.bin 0x610000 companions-pack.bin
```

Su Windows la porta è del tipo `COM5`, su Linux `/dev/ttyACM0`. Con esptool
4.x il comando si scrive `write_flash`.

Se la scheda non viene vista, mettila in modalità download: scollega la USB,
tieni premuto il tasto **BOOT**, ricollega la USB e poi rilascia BOOT.

### File separati

Nella release ci sono anche i singoli file, per chi preferisce scriverli a
parte:

| File | Indirizzo |
|---|---|
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xe000` |
| `madre.bin` | `0x10000` |
| `companions-pack.bin` | `0x610000` |

Impostazioni flash: 16 MB, modalità DIO/QIO, 80 MHz.

## Compilare il firmware

Serve **ESP-IDF 5.5** (provato con la 5.5.6), installato come da
[guida Espressif](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32c6/get-started/).

```bash
cd firmware
./fetch-board-libs.sh            # librerie Waveshare della scheda, in firmware/vendor
idf.py build                     # il target esp32c6 è già in sdkconfig.defaults
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Al primo `build` il component manager di ESP-IDF scarica le dipendenze
fissate in `dependencies.lock` (core Arduino 3.3.5, LVGL 9.3.0,
ArduinoJson 7.4.2) in `managed_components/`.

Sulla porta USB (115200 baud) c'è una piccola console di diagnosi:
`stato` (riepilogo e tempi dei fotogrammi), `tocca`, `scuoti`, `pasto`,
`premi` (simulano i gesti), `batteria` (registro dei consumi).

### Struttura

| File | Contenuto |
|---|---|
| `brain.*` | stati d'animo, sazietà, vitalità (stessi calcoli dell'app) |
| `face.*`, `stage.*` | espressioni e disegno degli occhi, interi a virgola fissa |
| `pet.*` | comportamento: sguardi, respiro, rimbalzi, reazioni ai gesti |
| `ui.*` | pannello Sazietà / Vitalità (LVGL) |
| `hw.*` | orologio RTC, accelerometro, batteria |
| `audio.*`, `sounds.*` | suoni sul codec ES8311 |
| `store.*` | stato salvato in NVS |
| `link.*`, `protocol.h` | servizio Bluetooth LE (NimBLE) |
| `diag.*` | diagnosi dei consumi |

## Gesti

| Gesto | Reazione |
|---|---|
| Tocco | risatina |
| Doppio tocco | coccola: occhi felici e cuori |
| Swipe in giù | si appisola e si riapre |
| Swipe in su | sveglia, poi entusiasta |
| Swipe laterale | sguardo furbo da quella parte |
| Tanti tocchi di fila | si arrabbia |
| Pressione di 2 s | Sazietà e Vitalità |
| Scossone | sorpresa, poi arrabbiata |
| Rinfresco dal telefono | l'animazione del pasto |

Lo schermo si abbassa dopo 30 s e si spegne dopo 2 minuti; un tocco lo
riaccende. A schermo spento il processore dorme per risparmiare batteria.

## Abbinare l'orologio all'app

1. Nell'app Holy Gluten apri la scheda **Lievito** e tocca l'icona
   dell'orologio.
2. Tocca l'orologio per accenderne lo schermo: il Bluetooth resta acceso
   solo a schermo acceso e per 20 secondi dopo l'ultimo tocco.
3. Premi **Cerca orologio**, poi **Abbina** accanto a `MADRE`.

Da lì l'app sincronizza da sola rinfreschi, nome e ora. Se l'orologio dorme,
l'app chiede di toccarlo.

## Stampare la custodia

La custodia si infila sul fronte: il retro è chiuso, il labbro frontale
trattiene la scheda senza viti. Ci sono cinque mascotte, ognuna in due
versioni secondo la durezza del TPU:

| Mascotte | File |
|---|---|
| Diavoletto (corna e occhiello) | `custodia-diavolo-tpu95A.stl`, `…-tpu85A.stl` |
| Micio | `custodia-gatto-…` |
| Boo, il fantasma | `custodia-fantasma-…` |
| Polpetto | `custodia-polpo-…` |
| Telly, la TV anni '60 | `custodia-tv-…` |

- **95A** (il TPU più comune): gioco 0,15 mm per lato, labbro spesso 2 mm.
- **85A** (più morbido): gioco 0,10 mm, labbro spesso 2,4 mm per trattenere
  meglio.

Impostazioni consigliate:

- Orientamento: **retro sul piatto**, come nel file (Z = 0 è il retro).
- Strato 0,2 mm, 3 perimetri, riempimento 20–30 % (30–40 % per l'85A).
- Velocità bassa: 20–30 mm/s; retrazione corta o nulla.
- Ugello 220–235 °C, piatto 40–60 °C, ventola al 30–50 %.
- Supporti: di solito non servono; se le parti della mascotte che sporgono
  si afflosciano, aggiungi supporti solo dal piatto.

I fori lasciano liberi la USB-C, i due tasti (con bottoncini in rilievo) e i
microfoni. Misure prese dal disegno quotato ufficiale della scheda.

## Sicurezza e privacy

- L'orologio non contiene credenziali, chiavi o indirizzi di server: non ha
  Wi-Fi e parla solo Bluetooth LE con il telefono abbinato.
- Riceve la cronologia dei rinfreschi, il nome del lievito, l'ora e (dal
  firmware 0.3.0) il nome del compagno scelto nell'app; non
  manda all'app altro che versione, batteria ed esito dell'ultima
  sincronizzazione.
- Il protocollo è descritto in `firmware/main/protocol.h`.

## Licenze

- **Firmware** (`firmware/`): [MIT](LICENSE).
- **Custodia** (`case/`): [CC BY-NC-SA 4.0](case/LICENSE). Puoi stamparla,
  modificarla e condividerla citando Holy Gluten, non venderla; le versioni
  modificate vanno condivise con la stessa licenza.
- **Disegni dei compagni** (`companions-pack.bin`, allegato alle release): i
  disegni di Holy Gluten, con la stessa licenza della custodia
  ([CC BY-NC-SA 4.0](case/LICENSE)).

Componenti di terze parti (scaricati al momento della compilazione, non
inclusi qui): librerie della scheda Waveshare (Apache-2.0), core Arduino per
ESP32 (LGPL-2.1), ESP-IDF e NimBLE (Apache-2.0), LVGL (MIT),
ArduinoJson (MIT). Il carattere in `font_medium_14.c` è
[Inter](https://rsms.me/inter/) (SIL Open Font License 1.1). I suoni sono
originali, sintetizzati per questo progetto.
