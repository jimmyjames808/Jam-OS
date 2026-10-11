# Rozšíření testů SSH protokolu

Stav plánu: první realizační vlna byla dokončena 2026-09-05. Stávající SSH testy jsou migrované do `tests/` a sedm navazujících protokolových oblastí má aktivní regresní testy. Podrobný katalog v §4 zůstává širším backlogem; položka v katalogu je splněná jen tehdy, pokud ji pokrývá níže uvedená aktivní sada nebo pozdější evidovaný test.

### Stav první realizační vlny

| Krok | Aktivní sada | Realizované pokrytí | Commit |
| --- | --- | --- | --- |
| Migrace testů | `make test-ssh`, `make -C tests test` | Samostatný build a runner v `tests/` podle vzoru `../pok/tests/`; produkční zdroje jsou připojené relativními symlinky. | `ae2a954` |
| Parser a rámcování | `test-packet-protocol.sh` | Hranice SSH datových typů, zkrácené a přebytečné payloady, plaintext rámce, padding, délkové limity, fragmentace a více paketů v bufferu. | `f5ec39e` |
| Pořadová čísla | `test-packet-sequence.sh` | Send/receive čítače, neúplný paket, IGNORE/DEBUG, strict/non-strict NEWKEYS, wrap a neautentizovaný limit. | `fd26a22` |
| KEX protokol | `test-kex-protocol.sh` | KEXINIT seznamy a směry, komprese, guess, KEXDH_INIT, NEWKEYS, strict KEX a chybné pořadí zpráv. | `6396a8d` |
| Autentizace | `test-auth-protocol.sh` | Service request, `none`, publickey probe a podpisová větev, identity a NUL, neplatné algoritmy/bloby, limity a stavové přechody. | `6901d44` |
| Session channel | `test-channel-protocol.sh` | Otevření kanálu, globální a channel requesty, shell/exec/subsystem, env, PTY, data, extended data, EOF a CLOSE. | `ebc84be` |
| Tok a uzavření kanálu | `test-channel-flow.sh` | Okna, WINDOW_ADJUST, stdout/stderr kredit, overflow, EPIPE, EOF/CLOSE pořadí a ukončení dítěte. | `287caaa` |
| Rekey | `test-rekey-protocol.sh` | Opakovaný rekey, nová nabídka algoritmů, zachování stavu a pořadových čísel a chybné rekey payloady. | `39445c9` |

SSH sada nyní spouští 10 protokolových skriptů. Úplný cíl `make test` navíc spouští sedm migrovaných crypto testů a dva testy utilit; celkem je v `tests/` 19 aktivních skriptů. Test závislý na externím OpenSSH klientovi není součástí sestavení ani žádné sady.

## 1. Rozsah

Cílem je důkladně otestovat SSH transport, identifikaci, vyjednávání, pořadí zpráv, přechody NEWKEYS a rekey, autentizační protokol, globální požadavky a jediný podporovaný session kanál včetně requestů, dat, oken, EOF a CLOSE.

Kryptografie slouží pouze jako existující prostředek pro vytvoření platného SSH spojení. Neplánují se testovací vektory šifer, hashů, podpisů, KEM, náhodnosti, konstantního času ani matematických vlastností klíčů. Název algoritmu, délka SSH blobu, vazba autentizační zprávy na relaci a přechod mezi transportními epochami patří do rozsahu. Interní implementaci kryptografických funkcí netestovat. U testu podpisové vazby lze zachytit vstup předaný existujícímu ověřovači; nepsat nový ověřovač.

Mimo rozsah jsou také CLI samo o sobě, instalace, systémové služby, obecné testování souborových práv, generování/export klíčů, benchmarky a protokoly uvnitř kanálu (SFTP, SCP apod.). Volby `-e`, `-x`, `-S`, `-P` a izolované účty jsou jen fixture pro ověření výsledného SSH chování. Forwarding, X11, agent forwarding a komprese se testují jako nepodporované požadavky; jejich implementace není cílem.

## 2. Co už existuje a co chybí

| Oblast | Zjištěný stav | Důsledek pro plán |
| --- | --- | --- |
| `test-tinysshd.sh` | Tři základní negativní hello scénáře a výpisy KEX nabídky v kombinacích voleb a dostupnosti host klíčů. | Doplnit skutečný výsledek vyjednávání a kompletní relaci. Výpis nabídky sám není test dokončeného handshake. |
| Vypnuté testy v témže skriptu | Limit neautentizovaných zpráv je zakomentovaný; hello/KEX helpery za `#temporary removed` a `exit 0` se nespouštějí. | Obnovovat podle významu testu, ne mechanicky odstraňovat `exit 0`. |
| `test-tinysshd-ignore.sh`, `_tinysshd-test-ignore.c` | Devět kombinací IGNORE/DEBUG před KEXINIT nebo před KEXDH_INIT, strict i non-strict. Úspěšná větev končí po KEXDH_REPLY. | Chybí čekání na NEWKEYS, provoz po něm, rekey a úplná kontrola odpovědí/ukončení. |
| `test-packet-global-request.sh`, `_tinysshd-test-global-request.c` | Přímý handler: want-reply 0/1, payload u true, jeden zkrácený paket. | Doplnit varianty parseru a zapojení do skutečné autentizované smyčky. |
| `test-tinysshnoneauthd.sh` | Původní prázdný test i pozdější varianta závislá na OpenSSH klientovi byly odstraněny. | Úplná E relace zůstává backlogem pro samostatné prostředí s explicitně zajištěným klientem. |
| `old/tinyssh-tests/` | Starší testy parseru, seznamů a kanálu; nejsou v aktuálním `TESTOUT`. Část pozitivních testů `channeltest.c` leží za `_exit(0)`. | Převzít užitečné scénáře, přepsat zastaralé předpoklady a zapojit do aktivní sady. |
| `tests/test.sh` | Společný runner kontroluje návratový kód skriptu i shodu stdout s `.exp` a umí dostat explicitní seznam testů. | Stejný runner obsluhuje úplnou i SSH sadu bez maskování chyby shodným stdout. |
| `tests/Makefile`, `tests/makefilegen.sh` | Samostatně sestavují všechny testy; cíle `test` a `test-ssh` oddělují úplnou a čistě SSH sadu. | Rozšíření udržovat v generátoru i generovaném Makefile. |

### Konkrétní místa s vysokou hodnotou nových testů

Jde o zjištění ze statického čtení, nikoli o reprodukované chyby. Očekávané výsledky se nesmějí automaticky odvodit z dnešního chování.

| Místo | Zjištění / hypotéza | Navazující skupiny |
| --- | --- | --- |
| `packet_hello.c` | Příjem kontroluje hlavně minimální délku a prefix `SSH-`; úplná syntaxe a verze nejsou výslovně ověřeny. `getln.c` už odmítá NUL. | HEL |
| `packet_get.c`, `packet_put.c` | Pořadová čísla, limit 30 neautentizovaných zpráv, zvláštní větev NEWKEYS a strict reset. NEWKEYS obchází obecné porovnání očekávaného typu. | FRM, SEQ, ORD, SKX |
| `packet_kex.c` | Směry šifer i kompresní seznamy se validují; aktivní KEX testy obsahují rozdílné a nepodporované nabídky. | NEG |
| `packet_kexdh.c` | Chybný guess zahazuje paket přes čtení očekávající KEXDH_INIT. Non-strict čekání na NEWKEYS zahazuje jiné zprávy. | GSS, KEY, RK |
| `main_tinysshd.c`, `packet_kex.c`, `sshcrypto_kex.c` | Rekey znovu parsuje KEXINIT a vybírá algoritmy z nové nabídky; regresní test hlídá odmítnutí neplatné nabídky i zachování stavu relace. | RK |
| `packet_auth.c` | Publickey probe, úplnost payloadu a NUL v identitě pokrývá aktivní autentizační sada. | AUT, PUB, NON |
| `packet_channel_open.c` | Jediný session kanál; stejné lokální i vzdálené ID; maxpacket se omezí do 32…32768, inzerováno 16384. | OPN, DAT, WIN |
| `packet_channel_recv.c` | Příchozí EXTENDED_DATA validuje ID, datový typ, délku a společné okno; aktivní channel testy pokrývají chybné varianty. | EXT |
| `packet_channel_request.c` | Převod stringů na C stringy; `pty-req` má TODO pro terminal modes. `env` po startu končí v interní chybové větvi `channel_env()`. | REQ, ENV, PTY |
| `channel.c` | Existují větve pro pozdní data a EPIPE; oba výstupy sdílejí remote window; přetečení okna je kontrolováno. | DAT, WIN, END |
| `main_tinysshd.c` | Po DISCONNECT se v hlavní smyčce přechází na `finished`; čtení, vyprázdnění bufferů a ukončení dítěte jsou časově provázané. | CTL, END, IO |
| `0005-send-auth-disconnect.patch` vs. `packet_auth.c` | Patch navrhuje diagnostický DISCONNECT při chybách autentizační fáze; v přečteném `.c` příslušný helper není. | SRV, ORD: evidovat jako návrh, ne jako hotové chování. |

## 3. Způsob implementace testů

### Vrstvy

- **U – přímý protokolový test v C:** parser, handler, serializovaná odpověď, okna a stavové příznaky. Fatal větve běží v odděleném procesu. Vždy kontrolovat `WIFEXITED` a očekávaný exit code; SIGSEGV není úspěšné odmítnutí.
- **W – scénář na drátu:** spustit skutečný daemon přes pipes nebo `socketpair`, posílat vlastní SSH pakety, číst a nezávisle dekódovat odpovědi. Umět fragmentaci, slepení paketů, half-close a přesné pořadí zpráv.
- **E – úplná SSH relace:** klient s existující podporou kryptografie a možností injektovat protokolové zprávy po NEWKEYS; OpenSSH jako další nezávislý klient. `tinysshnoneauthd -e` pod neprivilegovaným testovacím účtem usnadní connection testy, ale nenahradí testy publickey autentizace.
- **F – generativní test:** strukturované mutace platných zpráv a stavových sekvencí. Primární oracle je protokol a invarianty, nikoli jen absence pádu.

V pozitivních E testech kontrolovat obsah stdout/stderr, přesný exit status, odpovědi na requesty a korektní dokončení kanálu. Změny dat na drátu po NEWKEYS musí provádět klient ještě před použitím existujícího transportního encoderu; pouhá změna ciphertextu obvykle nedosáhne parseru, který chceme testovat.

### Fixture a determinismus

1. Každý scénář má nový proces a oddělený dočasný adresář; žádný sdílený `keydir` mezi paralelními testy. Zachovat oddělení logu od SSH stdout.
2. Testovací publickey účet a jeho autorizace vznikají v izolovaném prostředí. Neměnit skutečné `~/.ssh/authorized_keys`. V U testech lze nahradit pouze rozhodnutí autorizace a zaznamenat argumenty; alespoň pozitivní/negativní E dvojice musí používat skutečnou cestu.
3. Připravit malé deterministické programy pro echo binárních dat, oddělený stdout/stderr, výpis přijatých env, měření PTY, řízené zavření stdin a řízený exit. Netestovat jejich shellovou implementaci.
4. Synchronizovat přes explicitní značky a readiness, ne přes nahodilé `sleep`. Každé čekání má deadline, cleanup ukončí a sklidí daemon i potomky. Timeout je FAIL, nikdy očekávané odmítnutí malformed paketu.
5. Náhodný cookie, padding, log ID, PID a čas neporovnávat byte-for-byte. Dekódovat strukturu, délky, typy, příjemce a obsah relevantních polí.
6. Odeslanou odpověď dekódovat jednoduchým nezávislým dekodérem. Nepoužívat pouze roundtrip přes stejný chybný parser/serializer.
7. Ve výsledku uvést ID, seed, fázi, přijaté typy, dekódovaná pole, stav procesu a důvod selhání. Každé SKIP uvádí konkrétní chybějící předpoklad; požadované CI prostředí žádný povinný test nepřeskakuje.
8. `make test-ssh` a přímý `make -C tests test-ssh` spouštějí migrovanou základní SSH sadu. `make test` a `make -C tests test` spouštějí všechny testy včetně crypto a utilit. `make test-ssh-integration` a `make test-ssh-fuzz-smoke` zůstávají navrhované cíle pro budoucí rozšíření. Základní SSH cíl nespouští ani nevyžaduje binárku `test-crypto`; linkování existující kryptografie pro handshake je v pořádku.

### Význam priorit a očekávání

**P0:** hranice autentizace, parsery, pořadí KEX, okna a ztráta dat. **P1:** úplnost podporovaných zpráv a běžná kompatibilita. **P2:** širší kombinace, dlouhé běhy a vzácné interakce. Priorita platí pro celou skupinu, pokud řádek neurčuje jinak.

Očekávání bez značky je navržený cílový kontrakt. **[R]** výslovně označuje regresi současné lokální politiky. **[N]** je kandidát na test odhalující chybějící validaci nebo nesoulad; může být zpočátku červený. **[D]** vyžaduje před implementací konkrétního assertionu rozhodnout kompatibilní politiku nebo vyložit příslušnou normu. U [D] je vždy uveden alespoň minimální invariant. Nepřevádět [N] na zelený test změnou očekávání podle nalezené chyby; dočasné XFAIL musí mít konkrétní důvod a vlastní evidenci.

„Odmítnout“ znamená definovanou protokolovou odpověď uvedenou v řádku, nebo řízené ukončení spojení bez provedení zakázané operace. Nelze zaměňovat CHANNEL_FAILURE, REQUEST_FAILURE, USERAUTH_FAILURE, UNIMPLEMENTED a DISCONNECT. Přesný SSH disconnect reason požadovat tam, kde jej kontrakt stanoví; procesní exit 111 není náhradou testu odpovědi na drátu.

## 4. Katalog scénářů

### HEL – identifikace (P1; U/W)

Kód: `packet_hello.c`, `getln.c`. P0 jsou HEL-09, HEL-10 a HEL-14.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| HEL-01 | `SSH-2.0-client\r\n` | Přechod do KEX. |
| HEL-02 | Platný banner s komentářem a mezerami v komentáři | Komentář neovlivní výběr protokolu. |
| HEL-03 | Platný banner ukončený pouze LF | [R] Zachovat nynější toleranci. |
| HEL-04 | Délky celého řádku 254, 255, 256 včetně CRLF | Přijmout podporované hranice, nad limit řízeně odmítnout; explicitně počítat i CRLF. |
| HEL-05 | Každé možné rozdělení banneru mezi dva zápisy | Stejný výsledek jako jediný zápis. |
| HEL-06 | Banner po jednotlivých bytech, zvlášť CR a LF | Bez ztráty/zdvojení a předčasného KEX. |
| HEL-07 | Prázdný řádek, `SSH`, `ssh-2.0-client` | Odmítnout. |
| HEL-08 | Jiný prefix, HTTP request, náhodná binární data | Odmítnout bez SSH autentizace. |
| HEL-09 | NUL na začátku, uprostřed verze, v komentáři, před LF | [R] Odmítnout celý řádek, nikoli přijmout prefix. |
| HEL-10 | `SSH-1.5-client`, `SSH-3.0-client`, `SSH-x-client` | [N] Nepokračovat jako SSHv2 jen podle prefixu. |
| HEL-11 | `SSH-1.99-client` | [D] Vymezit kompatibilitu této role serveru; nesmí se zapnout SSH1. |
| HEL-12 | Chybějící softwareversion, oddělovač, prázdná verze | [N] Ověřit úplnou gramatiku identifikace. |
| HEL-13 | Řídicí znaky a vysoké byty v softwareversion | [D] Definovat validaci textové identifikace; nevytvořit jiné rozdělení paketů. |
| HEL-14 | Platný banner a kompletní KEXINIT v jednom zápisu | KEXINIT zůstane celý dostupný transportu. |
| HEL-15 | Textový řádek před klientským bannerem | [R] Nyní odmítnout; nezaměnit s tolerancí klienta k serverovému pre-banneru. |
| HEL-16 | EOF bez dat / uprostřed banneru / po CR bez LF | Ukončit bez KEX a bez čekání na plný timeout. |
| HEL-17 | Druhý banner po prvním | Nedostat druhou textovou identifikaci do nové relace. |
| HEL-18 | Klient zavře čtecí konec při odesílání serverového hello | Řízené ukončení, žádný nekonečný retry. |
| HEL-19 | Dekódování serverového banneru | Prefix SSH-2.0, neprázdná implementace, CRLF, přijatelná délka. |
| HEL-20 | Úplný handshake s různými komentáři banneru | E test klienta projde; server používá skutečnou identifikaci relace. |

### PAR – SSH datové typy a parsery (P0; U/F)

Kód: `packetparser.c`, `stringparser.c`; pouze použití při dekódování SSH zpráv. Obnovit relevantní případy ze starých testů.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| PAR-01 | uint8 na poslední platné pozici | Správná hodnota i nová pozice. |
| PAR-02 | uint8 za koncem zprávy | Řízené odmítnutí. |
| PAR-03 | uint32: 0, 1, 255, 256, 65535, 65536, UINT32_MAX | Správné big-endian dekódování. |
| PAR-04 | uint32 s pouze 0/1/2/3 dostupnými byty | Odmítnout každou variantu. |
| PAR-05 | SSH string délky 0 u pole, které ji dovoluje | Správná pozice dalšího pole. |
| PAR-06 | Délka stringu přesně odpovídá zbytku | Čtení skončí přesně na hranici. |
| PAR-07 | Deklarovaná délka o jeden větší než zbytek | Odmítnout. |
| PAR-08 | Deklarace 0x7fffffff, 0x80000000, 0xffffffff v malém paketu | Odmítnout bez aritmetického přetečení nebo velké alokace. |
| PAR-09 | Nulový skip/copy mezi skutečnými poli | Žádný posun ani poškození sousedního pole. |
| PAR-10 | `packetparser_end` na konci / byte před / byte za koncem | Přijmout pouze přesný konec. |
| PAR-11 | Binární SSH string s NUL a všemi hodnotami byte | Délka řídí čtení; žádný implicitní strlen u binárních polí. |
| PAR-12 | Zkrácení každého známého payloadu na každém offsetu | Neprovést neúplný request; kontrolovat typ odpovědi/ukončení. |
| PAR-13 | Přebytečné byty za známým payloadem | Odmítnout tam, kde zpráva nemá extension payload. |
| PAR-14 | Neznámý request s libovolným specifickým payloadem | Neaplikovat PAR-13 na data, jejichž formát server nezná. |
| PAR-15 | Boolean 0, 1, 2, 255 | Nula je false, ostatní true; samostatně ověřit want-reply, guess a signature-present. |
| PAR-16 | Vnořený string deklaruje délku přes hranici vnějšího blobu | Odmítnout i při dostatku bytů za vnějším blobem. |
| PAR-17 | Sentinel před/za cílovým bufferem při platném i chybném copy | Sentinel zachován; žádné přepsání okolí. |
| PAR-18 | Slepování více stringů: prázdný, binární, maximální | Každý následující field má správný offset. |

### FRM – transportní rámcování (P0; U/W)

Kód: `packet_get.c`, `packet_put.c`, `packet_recv.c`, `packet_send.c`; po NEWKEYS také protokolová obálka `sshcrypto_cipher_chachapoly.c`. Bez testů výpočtu šifry či tagu.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| FRM-01 | Minimální platný payload s platným paddingem | Vydat právě jeden payload bez paddingu. |
| FRM-02 | Hlavička rozdělená po 1/2/3 bytech | Nevydat paket před úplnou hlavičkou. |
| FRM-03 | Úplná hlavička, neúplné tělo | Nevydat payload a neposunout sequence number. |
| FRM-04 | Tělo doručené po jednom bytu | Jediné doručení po dokončení. |
| FRM-05 | Dva/tři celé pakety v jediném read | Doručit všechny ve správném pořadí. |
| FRM-06 | Celý paket a část dalšího | Zbytek uchovat pro příští read. |
| FRM-07 | packet_length 0, 1, 4, 5 | Odmítnout neplatný vztah délky, paddingu a payloadu. |
| FRM-08 | padding_length 0, 1, 2, 3 | [R] Odmítnout před i po NEWKEYS. |
| FRM-09 | padding_length 4 a další platné délky včetně 255 | Přijmout při platné délce a zarovnání paketu. |
| FRM-10 | Padding zabere celý payload / přesáhne packet_length | Odmítnout prázdný nebo záporný payload. |
| FRM-11 | packet_length 32767, 32768, 32769 | [R] Prověřit hranici PACKET_LIMIT; ostatní pole připravit validní, kde to zarovnání dovolí. |
| FRM-12 | packet_length UINT32_MAX | Odmítnout hned po hlavičce, nečekat na obří tělo. |
| FRM-13 | Nezarovnaný packet_length při jinak konzistentním payloadu | [N] Validovat podle transportního režimu, nikoli jedním pravidlem pro všechny režimy. |
| FRM-14 | Celý plaintext paket bez posledního padding byte + EOF | Žádné doručení neúplného paketu. |
| FRM-15 | Platný šifrovaný paket bez posledního byte transportní obálky | Žádné doručení před kompletním rámcem. |
| FRM-16 | Střídání velmi krátkých a velkých paketů | Správně posouvat recvbuf, žádné staré byty v novém payloadu. |
| FRM-17 | Velikosti kolem PACKET_FULLLIMIT a kapacity recvbuf | Zastavit čtení při nedostatku místa a znovu pokračovat po zpracování. |
| FRM-18 | Částečně přijatý šifrovaný paket a opakovaný packet_get | Cache `packet.packet_length` nemění výsledek ani pořadové číslo. |
| FRM-19 | Další šifrovaný paket má jinou délku | Po prvním paketu je cache délky resetována. |
| FRM-20 | Serializace payloadů kolem každé hranice paddingu | Nezávisle ověřit délku, počet padding bytů a přesný payload. |
| FRM-21 | Serverový KEXINIT s mnoha položkami | Vejde se do skutečné kapacity kexsend a vytvoří celý rámec. |
| FRM-22 | Payload 32768 versus packet_length 32768 | [N] Samostatně prověřit minimální přijímané velikosti protokolu; nezaměnit limit payloadu s limitem rámce. |

### SEQ – čítače a limit před autentizací (P0; U/W)

Kód: `packet_get.c`, `packet_put.c`, `packet.h`, `packet_auth.c`.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| SEQ-01 | Čerstvé spojení | Oba sequence numbers začínají nulou. |
| SEQ-02 | Přijetí / odeslání běžného paketu | Inkrement právě jednou za úplný paket. |
| SEQ-03 | IGNORE a DEBUG mezi pakety v povolené fázi | Počítají se do pořadí, i když handler nedostane payload. |
| SEQ-04 | Neúplný paket opakovaně dotazovaný parserem | Čítač se nemění. |
| SEQ-05 | Strict NEWKEYS při příjmu | Další přijímaný paket používá pořadí 0. |
| SEQ-06 | Strict NEWKEYS při odeslání | NEWKEYS patří staré epoše, další odesílaný paket má pořadí 0. |
| SEQ-07 | Non-strict NEWKEYS | Čítače pokračují bez resetu. |
| SEQ-08 | Nastavení čítače těsně před UINT32_MAX přes U fixture | [R] Současná ochrana ukončí spojení při wrapu; neposílat miliardy paketů. |
| SEQ-09 | 29, 30, 31 přijatých paketů v neautentizované epoše | [R] Limit platí pro skutečný receivepacketid; 31. nepřejde do handleru. |
| SEQ-10 | Do limitu vložit IGNORE/DEBUG nebo publickey probes | Započítat i zprávy bez USERAUTH_FAILURE. |
| SEQ-11 | Stejný počet požadavků ve strict/non-strict relaci | Explicitně zachytit vliv resetu při NEWKEYS; neoznačit limit jako celoživotní počet zpráv. |
| SEQ-12 | USERAUTH_SUCCESS těsně před limitem a pak dlouhý přenos | Po autentizaci limit 30 nezabije relaci. |
| SEQ-13 | Smyčka 32 auth pokusů versus transportní limit 30 | Oddělit U test smyčky od dosažitelnosti přes drát; transport může skončit dříve. |
| SEQ-14 | Neúspěšné či nečekané NEWKEYS mimo KEX | [N] Nesmí umožnit obejít limit pomocí neoprávněného resetu čítače. |

### NEG – KEXINIT a vyjednávání (P0; U/W/E)

Kód: `packet_kex.c`, výběr jmen v `sshcrypto_kex.c`, `sshcrypto_key.c`, `sshcrypto_cipher.c`, `stringparser.c`.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| NEG-01 | Výchozí serverový KEXINIT | Dekódovat cookie 16 B, deset name-listů, boolean a reserved; žádný zbytek. |
| NEG-02 | Každé ze čtyř aktuálních KEX jmen samostatně | Úplný handshake, zvolen přesně nabízený podporovaný název. |
| NEG-03 | Dva společné algoritmy v obráceném klientském pořadí | Výběr respektuje klientskou preferenci. |
| NEG-04 | Neznámý algoritmus před společným / za společným | Najít společný; nezaměnit s first-packet guess. |
| NEG-05 | Žádný společný KEX | Neodeslat KEXDH_REPLY pro jiný algoritmus. |
| NEG-06 | Pouze strict/ext-info pseudojména bez skutečného KEX | Vyjednávání selže. |
| NEG-07 | Prázdný povinný name-list | Odmítnout nedohodnutou kategorii. |
| NEG-08 | Žádný společný host-key algoritmus | Handshake skončí před autentizací. |
| NEG-09 | Podporovaný host key chybí ve fixture serveru | Neinzerovat jej jako použitelný; nevytvořit falešný úspěch. |
| NEG-10 | Žádná společná C→S šifra | Vyjednávání selže. |
| NEG-11 | C→S společná, S→C pouze nepodporovaná šifra | [N] Vyjednávání selže; nepředpokládat shodu směrů. |
| NEG-12 | C→S společná, S→C prázdný seznam | [N] Stejná kontrola samostatného směru. |
| NEG-13 | S→C má společnou šifru až za neznámou položkou | Úspěch, obě strany skutečně používají společnou šifru. |
| NEG-14 | Komprese `none` v obou směrech | Úspěch bez komprese. |
| NEG-15 | Komprese pouze `zlib`, samostatně každý směr | [N] Odmítnout; nesmí tiše zvolit neinzerované `none`. |
| NEG-16 | `zlib,none` / `none,zlib` | Zvolit společné `none`. |
| NEG-17 | Různé MAC seznamy při stávající AEAD šifře | [R] Testovat kompatibilitu ignorovaného samostatného MAC, ne požadovat jeho výpočet. |
| NEG-18 | Prázdné jazykové seznamy / neprázdné jazyky | Handshake bez změny aplikovaných algoritmů. |
| NEG-19 | NUL uvnitř jména, ne-ASCII, změna velikosti písmen | Nenajít falešnou shodu s podporovaným jménem. |
| NEG-20 | Podporované jméno jako prefix delšího jména | Žádná částečná shoda. |
| NEG-21 | Duplicitní podporované jméno | Deterministický výběr, žádný duplicitní handshake. |
| NEG-22 | Úvodní/koncová čárka a dvě čárky vedle sebe | [D] Současný parser ignoruje prázdné položky; rozhodnout validaci name-listu a nezaměnit s prázdným celým seznamem. |
| NEG-23 | Useknutý cookie nebo libovolný z deseti seznamů | Odmítnout, nepoužít částečný výběr. |
| NEG-24 | Chybí boolean / reserved / přebývá byte | Odmítnout neúplnou nebo prodlouženou známou zprávu. |
| NEG-25 | Reserved je nenulové | [D] Ověřit pravidlo příjemce; minimálně žádný vliv na algoritmus/stav. |
| NEG-26 | Velký seznam s mnoha neznámými jmény a společným posledním | Ukončit výběr v omezeném čase a bez překročení bufferu. |
| NEG-27 | Vypnutá skupina algoritmů ve fixture | Jména chybí v nabídce a klient si je nemůže vynutit. |
| NEG-28 | Žádný povolený skutečný KEX, pouze pseudojméno | Čisté selhání, žádný přechod na nevybranou funkci. |

### GSS – first_kex_packet_follows (P0; U/W/E)

Kód: `packet_kex.c`, `packet_kexdh.c`, `sshcrypto_kex.c`.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| GSS-01 | follows=false, první společný algoritmus | Číst jediný skutečný KEX init. |
| GSS-02 | follows=false, první algoritmus nepodporovaný | Nezahazovat následný správný init. |
| GSS-03 | follows=true, správný guess KEX i host key | Guessed paket je skutečným init, bez druhého čekání. |
| GSS-04 | follows=true, chybný KEX guess | Zahodit právě jeden guessed paket, pak zpracovat správný init. |
| GSS-05 | KEX guess správný, host-key guess chybný | [N] Zohlednit i host-key preferenci. |
| GSS-06 | Chybný guess používá jiný message number 30–49 | [N] Nevyžadovat, aby zahozený paket měl číslo KEXDH_INIT. |
| GSS-07 | Zahozený paket má obsah neodpovídající zvolenému KEX | Neparsovat jej jako zvolenou metodu; transportní rámec musí být platný. |
| GSS-08 | Guessed a skutečný init v jednom read | Zahodit přesně první; druhý neztratit. |
| GSS-09 | Chybný guess bez druhého init, následně EOF | Ukončit, žádný falešný KEXDH_REPLY. |
| GSS-10 | follows=2 nebo 255 | Stejná boolean sémantika jako true. |
| GSS-11 | Pseudojméno před prvním skutečným KEX | [D] Zkontrolovat pravidlo pro guess rozšíření; žádné náhodné zahození správného paketu. |
| GSS-12 | Stejné scénáře s vyjednaným strict režimem | Výjimka pro legitimní chybný guess se neposuzuje jako svévolná injekce; nezahazovat DISCONNECT. |

### KEY – KEX zprávy a přechod NEWKEYS (P0; U/W/E)

Kód: `packet_kexdh.c`, `packet_get.c`, `main_tinysshd.c`.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| KEY-01 | Správný init pro zvolený KEX | Právě KEXDH_REPLY a NEWKEYS ve správném pořadí. |
| KEY-02 | Délka klientského KEX blobu L−1, L, L+1 | Přijmout jen L zvoleného wire formátu; žádné matematické testy blobu. |
| KEY-03 | Prázdný / useknutý / přebytečný KEX blob | Řízeně odmítnout. |
| KEY-04 | Délka blobu jiného podporovaného KEX | Nepřepnout algoritmus podle délky. |
| KEY-05 | Nezávislé dekódování KEXDH_REPLY | Správné vnořené stringy host key, serverového KEX blobu a signature blobu. |
| KEY-06 | Klient NEWKEYS po KEXDH_REPLY | Autentizační fáze začne pouze po platném přechodu. |
| KEY-07 | NEWKEYS s přebytečnými daty | [N] Kontrolovat přesný jedno-byte payload. |
| KEY-08 | NEWKEYS ještě před KEXINIT / místo KEX init | [N] Nezapnout klíče a neobejít očekávaný typ zprávy. |
| KEY-09 | Druhé NEWKEYS bez nového KEX | [N] Žádný svévolný reset či změna epochy. |
| KEY-10 | NEWKEYS a první zašifrovaný SERVICE_REQUEST v jednom read | Každý paket dekódovat ve správné epoše. |
| KEY-11 | NEWKEYS a jen část další hlavičky | Zbytek bufferu uchovat při přepnutí transportu. |
| KEY-12 | Klient odkládá NEWKEYS, ale server už své odeslal | Směrové přechody zůstávají konzistentní; žádné předčasné app zprávy. |
| KEY-13 | Klient místo NEWKEYS odpojí transport | Žádná autentizace, řízené dokončení. |
| KEY-14 | Slepování KEXINIT, KEX init a dalších platných kroků | Nezávislost na počtu read; pořadí závislostí zůstává vynucené. |

### SKX – strict KEX (P0; W/E, čítače U)

Kód: `packet_get.c`, `packet_put.c`, `packet_kex.c`, `packet_kexdh.c`.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| SKX-01 | Klient nabízí `kex-strict-c-v00@openssh.com` | Strict se aktivuje pro relaci. |
| SKX-02 | Klient nabízí pouze serverové strict-s jméno | Nezaměnit role a neaktivovat podle nesprávného tokenu. |
| SKX-03 | Token strict se v seznamu opakuje | Stejný výsledek jako jediný token. |
| SKX-04 | IGNORE před prvním KEXINIT, který pak vyjedná strict | Odmítnout, protože KEXINIT nebyl první paket. |
| SKX-05 | DEBUG před prvním KEXINIT | Stejná kontrola jako SKX-04. |
| SKX-06 | Více IGNORE/DEBUG před KEXINIT v jednom read | Odmítnout bez ohledu na fragmentaci. |
| SKX-07 | IGNORE mezi KEXINIT a KEX init | Odmítnout. |
| SKX-08 | DEBUG mezi KEXINIT a KEX init | Odmítnout pro always-display false i true. |
| SKX-09 | IGNORE/DEBUG při čekání na první NEWKEYS | Odmítnout každou variantu. |
| SKX-10 | SERVICE_REQUEST, USERAUTH_REQUEST, CHANNEL_OPEN během prvního KEX | Odmítnout před jakýmkoliv aplikačním účinkem. |
| SKX-11 | UNIMPLEMENTED nebo neznámý typ při prvním KEX | Odmítnout nečekanou zprávu. |
| SKX-12 | DISCONNECT v každém kroku prvního KEX | Respektovat ukončení; nepokračovat handshake. |
| SKX-13 | Stejné IGNORE/DEBUG scénáře bez vyjednaného strict | Povolené generické zprávy přeskočit, pokračovat handshake. |
| SKX-14 | IGNORE/DEBUG po dokončení prvního strict KEX | Relace pokračuje; zákaz prvního KEX nepřenést na běžný provoz. |
| SKX-15 | Rekey ve strict relaci | Znovu resetovat oba sequence numbers, zachovat session ID. |
| SKX-16 | IGNORE/DEBUG v různých krocích rekey | [N] Rozlišit pravidla prvního KEX a rekey; nevynucovat rozdílné chování jen podle místa ve smyčce. |
| SKX-17 | Strict token vynechaný v pozdějším KEXINIT | Strict zůstane vlastností již vyjednané relace. |
| SKX-18 | Strict token poprvé až při rekey | Neaktivovat rozšíření zpětně. |

### RK – opakovaná výměna klíčů (P0; W/E)

Kód: label `rekeying` a dispatcher v `main_tinysshd.c`, `packet_kexdh.c`, selektory. Crypto výpočty se považují za existující závislost.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| RK-01 | Rekey po autentizaci před otevřením kanálu | Po výměně lze otevřít session bez další autentizace. |
| RK-02 | Rekey s otevřeným kanálem před exec | Zachovat ID, okna, env a připravené PTY. |
| RK-03 | Rekey během přenosu stdin/stdout | Přesný obsah před i po výměně; žádná ztráta/duplicita. |
| RK-04 | Rekey při současném stdout/stderr | Zachovat oba proudy i společný kredit. |
| RK-05 | Rekey při nulovém remote window | Po výměně a WINDOW_ADJUST přenos pokračuje. |
| RK-06 | Rekey při zaplněném sendbuf/recvbuf | Bez poškození bufferu a ztráty čekajících odpovědí. |
| RK-07 | Dvě a více po sobě jdoucích výměn | Každá dokončena, bez kumulovaného stavu. |
| RK-08 | Nový KEXINIT má useknuté nebo chybné fieldy | [N] Rekey validuje formát stejně důkladně jako první vyjednávání. |
| RK-09 | Rekey klient přestane nabízet původní algoritmus, nabízí jiný společný | [N] Znovu vyjednat podporovaný průnik nebo explicitně skončit; neignorovat nabídku. |
| RK-10 | Rekey bez jakéhokoli společného KEX | [N] Neprovést starý KEX v rozporu s novým seznamem. |
| RK-11 | Rekey mění nabídku host key, S→C šifry nebo komprese | [N] Prověřit každou kategorii, ne pouze KEX. |
| RK-12 | Rekey mění follows false→true a true→false | [N] Nepoužít příznaky z prvního KEXINIT. |
| RK-13 | Rekey mění správný guess na chybný | [N] Zahodit správný počet guessed paketů podle nové nabídky. |
| RK-14 | Session ID zachycené před a po několika rekey | Nemění se; nejde o novou autentizovanou relaci. |
| RK-15 | Aplikační paket již odeslaný klientem před jeho KEXINIT | Zpracovat před výměnou, neoznačit automaticky za zakázaný paket během KEX. |
| RK-16 | App paket poslaný mezi klientským KEXINIT a jeho NEWKEYS | Nevykonat nelegální operaci během pozastaveného connection provozu. |
| RK-17 | Druhý KEXINIT ještě před dokončením předchozí výměny | Žádná rekurzivní/nested výměna ani přepsání aktivního stavu. |
| RK-18 | Rekey během USERAUTH před úspěchem | [N/D] Ověřit podporu transportního rekey i v této fázi; nikdy neobejít autentizaci. |
| RK-19 | EOF/CLOSE kanálu těsně před a po rekey | Zachovat pořadí, žádné obnovení uzavřeného směru. |
| RK-20 | DISCONNECT nebo EOF transportu v každém kroku rekey | Ukončit bez pokračování se smíšeným stavem. |

### CTL – generické zprávy a ukončení transportu (P0; W/E)

Kód: `packet_get.c`, `packet_unimplemented.c`, `main_tinysshd.c`.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| CTL-01 | IGNORE s prázdným / binárním / velkým stringem v povolené fázi | Bez odpovědi, následující zpráva zpracována. |
| CTL-02 | DEBUG always-display 0/1/255, neprázdný jazyk | Bez SSH odpovědi a bez změny kanálu. |
| CTL-03 | Více IGNORE/DEBUG následovaných běžným paketem v jednom read | Žádné čekání na další síťový byte. |
| CTL-04 | Useknutý IGNORE nebo DEBUG | [D] Nyní se payload neparsuje; rozhodnout validaci, v každém případě žádný účinek obsahu. |
| CTL-05 | Platný DISCONNECT v KEX, auth, idle session, přenosu, rekey | Bez dalšího zpracování aplikačních zpráv. |
| CTL-06 | DISCONNECT a CHANNEL_REQUEST exec v jednom read | Exec za DISCONNECT se nikdy nespustí. |
| CTL-07 | DISCONNECT a KEXINIT v jednom read | Žádný nový handshake. |
| CTL-08 | DISCONNECT s neznámým reason a binárním popisem | Ukončit, obsah nepoužít jako příkaz nebo další paket. |
| CTL-09 | DISCONNECT pouze s typem / useknutými stringy | [D] Validace nebo okamžité uzavření; vždy žádné pokračování. |
| CTL-10 | Neznámý message number po autentizaci | UNIMPLEMENTED s pořadovým číslem konkrétního přijatého paketu. |
| CTL-11 | Neznámý paket po IGNORE a DEBUG | UNIMPLEMENTED zahrnuje v číslování i přeskočené pakety. |
| CTL-12 | Neznámý paket jako první po strict NEWKEYS | Ověřit správné číslování nové epochy. |
| CTL-13 | Dva neznámé pakety za sebou | Dvě samostatné správně přiřazené odpovědi. |
| CTL-14 | Příchozí UNIMPLEMENTED | [N/D] Nepřipustit nekonečné odpovídání UNIMPLEMENTED na UNIMPLEMENTED. |
| CTL-15 | Směrově nesmyslné odpovědi SERVICE_ACCEPT, PK_OK, CHANNEL_SUCCESS | Nezměnit autorizaci ani stav kanálu; konkrétní reakce podle fáze. |
| CTL-16 | Nepodporované transportní extension zprávy | Nepřisoudit jim význam globálního requestu; řízená reakce podle čísla/fáze. |

### SRV – služba a vstup do autentizace (P0; U/W/E)

Kód: `packet_auth.c`, `main_tinysshd.c`. Případy diagnostického DISCONNECT záměrně nezaměňují existující patch s aplikovaným kódem.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| SRV-01 | SERVICE_REQUEST `ssh-userauth` po NEWKEYS | Právě SERVICE_ACCEPT se stejným názvem. |
| SRV-02 | SERVICE_REQUEST `ssh-connection` místo `ssh-userauth` | [N] Ukončit; cílově DISCONNECT SERVICE_NOT_AVAILABLE. |
| SRV-03 | Prázdný nebo neznámý název služby | [N] Stejná diagnostika nedostupné služby, žádná autentizace. |
| SRV-04 | `ssh-userauth` s příponou, změnou case nebo NUL | Žádná falešná shoda. |
| SRV-05 | Useknutá délka nebo název služby | Protokolová chyba, nikoli SERVICE_ACCEPT. |
| SRV-06 | SERVICE_REQUEST s přebytečnými byty | Odmítnout. |
| SRV-07 | USERAUTH_REQUEST před SERVICE_REQUEST | Neposlat USERAUTH_SUCCESS. |
| SRV-08 | Opakovaný SERVICE_REQUEST během auth | [D] Určit reakci na opakované vyžádání; žádný reset počtu pokusů. |
| SRV-09 | SERVICE_REQUEST a USERAUTH_REQUEST slepené v read | ACCEPT před odpovědí autentizace. |
| SRV-10 | IGNORE/DEBUG před SERVICE_REQUEST po dokončeném KEX | Pokračovat do služby v strict i non-strict relaci. |
| SRV-11 | Channel/global zpráva před SERVICE_REQUEST | [N] Žádná connection operace; cílově DISCONNECT PROTOCOL_ERROR. |
| SRV-12 | Channel/global zpráva po ACCEPT před SUCCESS | [N] Stejné odmítnutí a diagnostika. |

### AUT – společná autentizační pravidla (P0; U/E)

Kód: `packet_auth.c`. Rozhodnutí autorizace je fixture; testujeme jeho dopad na SSH stav a odpovědi.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| AUT-01 | `none` na běžném tinysshd | USERAUTH_FAILURE, seznam `publickey`, partial-success=false. |
| AUT-02 | `password` včetně change-password boolean a dalších dat | Žádné přihlášení, standardní selhání metody. |
| AUT-03 | `hostbased` s korektně sestavenými fieldy | Žádné přihlášení. |
| AUT-04 | `keyboard-interactive` | Selhání, žádný autentizační challenge. |
| AUT-05 | Neznámá metoda a její libovolný payload | Selhání metody, ne interpretace payloadu jako publickey. |
| AUT-06 | Prázdný název metody | Neúspěch, žádný SUCCESS. |
| AUT-07 | Metoda obsahuje podporovaný prefix + NUL/suffix | Žádná falešná shoda. |
| AUT-08 | Služba v USERAUTH je `ssh-connection` | Pokračovat pouze s korektní metodou. |
| AUT-09 | Jiná služba v USERAUTH_REQUEST | Odmítnout, nespustit session. |
| AUT-10 | Prázdné uživatelské jméno | Žádná náhradní identita podle UID procesu. |
| AUT-11 | Délka jména LOGIN_NAME_MAX−1, MAX, MAX+1 | Ověřit skutečnou mez `packet.name`, žádné zkrácení na jiný účet. |
| AUT-12 | Existující účet + NUL + jiný suffix | [N] Neautorizovat pouze prefix před NUL. |
| AUT-13 | Neexistující účet s jinak platným požadavkem | Žádný SUCCESS ani vznik kanálu. |
| AUT-14 | Změna uživatele mezi pokusy | Žádné přenesení autorizace/PK_OK předchozí identity. |
| AUT-15 | Změna služby nebo metody mezi pokusy | Nový požadavek kompletně posouzen, žádný zbytek starého stavu. |
| AUT-16 | Selhávající metoda, potom platná publickey autentizace | Úspěch, pokud ještě nebyl vyčerpán limit. |
| AUT-17 | Zkrácení před každým ze tří společných stringů | Ukončení bez částečné autentizace. |
| AUT-18 | USERAUTH_FAILURE na drátu | Přesně typ, name-list a false boolean; bez přebytečných bytů. |
| AUT-19 | USERAUTH_SUCCESS na drátu | Přesně jedno-byte payload; stav autorizace dostupný následující zprávě. |
| AUT-20 | Další USERAUTH_REQUEST po SUCCESS | [N] Nepřepnout uživatele; prověřit ignorování dalších auth requestů podle pravidel autentizační vrstvy. |
| AUT-21 | EOF nebo DISCONNECT během série pokusů | Okamžitý konec, žádný dodatečný SUCCESS. |
| AUT-22 | USERAUTH_SUCCESS zaslaný klientem serveru | Nikdy nenastavit `flagauthorized` podle klientské odpovědi. |

### PUB – publickey protokol a SSH bloby (P0; U/E)

Kód: `packet_auth.c`, pouze wire parser/serializer v `sshcrypto_key_ed25519.c`. Žádné Ed25519 testovací vektory.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| PUB-01 | Podporovaný publickey probe bez podpisu | PK_OK obsahuje přesný algoritmus a klíč, nikoli SUCCESS. |
| PUB-02 | Probe pro klíč neautorizovaný fixture | [R] Současný server může vrátit PK_OK; definitivní autorizaci ověřit až podepsaným requestem. |
| PUB-03 | Probe s nepodporovaným algoritmem | USERAUTH_FAILURE, žádný PK_OK pro jiné jméno. |
| PUB-04 | Více různých probes před podpisovým requestem | Bez přenesení klíče či účtu mezi zprávami. |
| PUB-05 | Autorizovaný podepsaný request bez předchozího probe | SUCCESS; probe není povinný. |
| PUB-06 | Probe následovaný odpovídajícím autorizovaným requestem | Jediný SUCCESS, pak lze otevřít kanál. |
| PUB-07 | Korektně podepsaný request s neautorizovaným klíčem | USERAUTH_FAILURE, žádný session proces. |
| PUB-08 | Probe A, následně podepsaný request B | Rozhodnutí podle B; PK_OK pro A nic neautorizuje. |
| PUB-09 | Signature-present=false, ale připojen další blob | [N] Ověřit chybějící kontrolu konce probe větve. |
| PUB-10 | Signature-present=true, signature blob chybí | Odmítnout. |
| PUB-11 | Signature-present=2 nebo 255 | Boolean true; musí projít stejnými kontrolami jako 1. |
| PUB-12 | Prázdný / zkrácený public-key blob | Odmítnout bez čtení za vnější hranicí. |
| PUB-13 | Algoritmus v requestu nesouhlasí s algoritmem uvnitř blobu | Odmítnout; žádné přepnutí podle vnitřního jména. |
| PUB-14 | Délka veřejného klíče 31, 32, 33 bytů | Pro stávající wire formát přijmout jen 32; obsah řeší existující crypto. |
| PUB-15 | Přebytečné byty za vnitřním klíčem | Odmítnout celý blob. |
| PUB-16 | Prázdný / useknutý signature blob | Odmítnout. |
| PUB-17 | Jméno algoritmu v signature blobu je jiné | Odmítnout bez nechtěné fallback metody. |
| PUB-18 | Délka podpisového pole 63, 64, 65 bytů | Strukturální validace délky 64 pro stávající formát. |
| PUB-19 | Přebytečné byty uvnitř podpisového blobu / za requestem | Odmítnout obě varianty. |
| PUB-20 | Zachycení dat předaných ověřovači | Přesná session ID jako SSH string a správná serializace USERAUTH_REQUEST bez signature blobu. |
| PUB-21 | Request připravený pro relaci A použitý v relaci B | Žádný SUCCESS; test protokolové vazby pomocí existující kryptografie. |
| PUB-22 | Po přípravě podpisu změnit username / service / key | Každou změnu zamítnout; žádná autorizace jiného požadavku. |
| PUB-23 | Autorizátor ve fixture vrátí chybu místo deny | Žádný SUCCESS ani částečné přihlášení. |
| PUB-24 | Neznámý algoritmus s velmi dlouhým následujícím blobem | Bez přetečení, falešné shody nebo čtení C stringu za hranicí. |

### NON – autentizace none ve vyhrazeném režimu (P0; U/E)

Kód: `packet_auth.c`, `main_tinysshd.c`; fixture neprivilegovaný proces s `-e`. Testování startovacích voleb samotných sem nepatří.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| NON-01 | `none` pro účet odpovídající effective UID | SUCCESS a možnost otevřít session. |
| NON-02 | Existující uživatel s jiným UID | FAILURE, žádný start programu. |
| NON-03 | Neexistující uživatel | FAILURE. |
| NON-04 | Prázdné jméno nebo jméno s vnitřním NUL | [N] Žádná autorizace zkrácené či implicitní identity. |
| NON-05 | Přebytečné byty za metodou none | [R] Odmítnout i pro správného uživatele. |
| NON-06 | Chybná služba se správným UID | Bez SUCCESS. |
| NON-07 | Neúspěšný none pro jiný účet, pak správný účet | Vyhodnotit znovu aktuální identitu, respektovat limit. |
| NON-08 | Po úspěchu session + shell request | Spustit pouze fixture programu z `-e`. |
| NON-09 | Po úspěchu exec / subsystem request | CHANNEL_FAILURE při want-reply, žádný spuštěný klientský program. |
| NON-10 | Stejný none request na standardním tinysshd | Zůstane zamítnut; režim není vlastnost přenesená klientem. |

### GLB – globální požadavky (P1; U/E)

Kód: `packet_global_request.c`, dispatcher v `main_tinysshd.c`. GLB-09 až GLB-12 mají P0.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| GLB-01 | `keepalive@openssh.com`, want-reply=true | Právě REQUEST_FAILURE, relace pokračuje. |
| GLB-02 | Totéž, want-reply=false | Žádná odpověď. |
| GLB-03 | Neznámý request, true/false | Stejná sémantika podle want-reply. |
| GLB-04 | Neznámý request s binárním extension payloadem | Payload je ignorován, neověřovat neznámou strukturu. |
| GLB-05 | want-reply=2/255 | Právě jedna REQUEST_FAILURE. |
| GLB-06 | `tcpip-forward` a `cancel-tcpip-forward` | Zamítnout dle want-reply, nevytvořit forwarding. |
| GLB-07 | `streamlocal-forward@openssh.com` a zrušení | Stejná nepodporovaná větev. |
| GLB-08 | Prázdný nebo binární název requestu | [D] Definovat validaci názvu; nikdy jej nesplést s podporovanou operací. |
| GLB-09 | Chybí část uint32 délky názvu | Odmítnout. |
| GLB-10 | Deklarovaná délka názvu přes konec | Odmítnout. |
| GLB-11 | Chybí want-reply za kompletním názvem | Odmítnout. |
| GLB-12 | Velikost requestu na transportní hranici | Bez přepsání bufferu a bez příliš dlouhého čekání. |
| GLB-13 | Více požadavků se střídáním false/true v jednom read | Odpovědi pouze na true, v přijatém pořadí. |
| GLB-14 | Request před otevřením kanálu / během exec / po EOF | Nezávislost globálního requestu na stavu session. |
| GLB-15 | Request během velkého výstupu při nulovém channel window | Odpověď není blokována kreditem kanálu; transport může být dočasně vytížen. |
| GLB-16 | REQUEST_SUCCESS/FAILURE bez serverového requestu | [D] Žádná odpovědní smyčka ani změna stavu kanálu. |

### OPN – otevření a identifikátory kanálu (P0; U/E)

Kód: `packet_channel_open.c`, `channel.c`, `channel.h`.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| OPN-01 | První `session` s ID 0 | OPEN_CONFIRMATION se správným recipient, sender, oknem a maxpacket. |
| OPN-02 | První session s ID 1, 255, 65536, UINT32_MAX | Žádné znaménkové zúžení ID; další zprávy míří na potvrzené serverové ID. |
| OPN-03 | Initial window=0 | Otevření dovoleno, server neposílá data do přidání kreditu. |
| OPN-04 | Initial window=1 / UINT32_MAX | Správná práce s celým rozsahem. |
| OPN-05 | Druhá session s jiným ID | [R] OPEN_FAILURE ADMINISTRATIVELY_PROHIBITED; první kanál dál funguje. |
| OPN-06 | Druhá session se stejným ID | Žádné přepsání běžícího kanálu. |
| OPN-07 | Neznámý typ kanálu | [R] OPEN_FAILURE UNKNOWN_CHANNEL_TYPE, ne administrativní důvod. |
| OPN-08 | `direct-tcpip`, `forwarded-tcpip`, `x11`, agent channel | Každý samostatně odmítnout jako nepodporovaný typ. |
| OPN-09 | `direct-streamlocal@openssh.com` a forwarded varianta | Stejně odmítnout, žádné vytvoření socketu. |
| OPN-10 | Neznámý typ s vlastním payloadem | Odmítnout typ bez chybné interpretace specifických dat. |
| OPN-11 | session s dodatečným payloadem | Odmítnout známou zprávu s přebytkem. |
| OPN-12 | Prázdný typ, `session` s NUL/suffix/case změnou | Žádná falešná session; odpověď na neznámý typ nebo validované odmítnutí. |
| OPN-13 | Useknuté ID / window / maxpacket | Bez otevření částečně inicializovaného kanálu. |
| OPN-14 | Maxpacket 0, 1, 8, 9, 13, 31, 32 | [R/N] Zachytit nynější clamp; zvlášť ověřit, že výsledný přenos nepřekročí klientský limit. |
| OPN-15 | Maxpacket 32767, 32768, 32769, UINT32_MAX | Lokální limit bezpečný, žádné přetečení. |
| OPN-16 | OPEN_CONFIRMATION inzeruje window=131072 a maxpacket=16384 | [R] Ověřit aktuální deklarované hodnoty a reálnou schopnost přijmout odpovídající data. |
| OPN-17 | OPEN_FAILURE struktura | Recipient je požadované klientské ID, reason správný, description/language platné stringy. |
| OPN-18 | Neúspěšné otevření neznámého typu, pak session | Předchozí odmítnutí nezabralo jediný slot. |
| OPN-19 | Session po uzavření předchozí session | [R/D] Server dnes nemá reset/reuse kanálu; určit ukončení relace nebo explicitní odmítnutí, nikdy použít starý stav. |
| OPN-20 | Klient pošle OPEN_CONFIRMATION bez serverového OPEN | Neotevřít kanál podle nevyžádané odpovědi. |

### REQ – dispatch a spuštění session (P0; U/E)

Kód: `packet_channel_request.c`, `channel.c`, `channel_subsystem.c`. Kombinace shell/exec/subsystem musí tvořit úplnou matici 3×3 prvního a druhého startu.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| REQ-01 | shell na otevřeném kanálu bez `-e` | Spustit fixture přihlašovacího shellu, SUCCESS při want-reply. |
| REQ-02 | exec s jednoduchým příkazem | Přesný očekávaný výstup a exit status. |
| REQ-03 | exec s mezerami, uvozovkami a metaznaky | Předání podle shellové sémantiky, ne nechtěné rozdělení SSH stringu. |
| REQ-04 | Prázdný exec string | [D] Definovat běh prázdného příkazu; žádné nahrazení interaktivním shellem záměnou NULL/string. |
| REQ-05 | exec string s vnitřním NUL | [N] Nevykonat skrytě jen prefix příkazu. |
| REQ-06 | Shell request s přebytečným stringem | Odmítnout. |
| REQ-07 | Registrovaný subsystem | Spustit příslušný fixture program; netestovat jeho aplikační protokol. |
| REQ-08 | Neznámý subsystem | FAILURE, pak stále lze zahájit shell/exec. |
| REQ-09 | Subsystem jméno je prefix jiného / obsahuje NUL | [N] Jen přesná shoda celého jména. |
| REQ-10 | Přebytek nebo truncation u exec/subsystem | Žádný start procesu. |
| REQ-11 | Shell s nastaveným `-e` | Spustit přesně serverový custom program. |
| REQ-12 | Exec s `-e` | FAILURE, žádný klientský příkaz. |
| REQ-13 | Subsystem s `-e` | FAILURE i pro registrovaný subsystem. |
| REQ-14 | Druhý start session po prvním startu (3×3) | [R] Odmítnout všechny kombinace, původní proces dál funguje. |
| REQ-15 | Druhý start po skončení dítěte, před CLOSE handshake | Nevytvořit nový proces na starém kanálu. |
| REQ-16 | Přijatý request, want-reply=false | Účinek proběhne, žádné CHANNEL_SUCCESS. |
| REQ-17 | Odmítnutý request, want-reply=false | Bez CHANNEL_FAILURE i bez vedlejšího účinku. |
| REQ-18 | want-reply=2/255 | Právě jedna odpověď odpovídající výsledku. |
| REQ-19 | Neznámý channel request s vlastním payloadem | FAILURE při true, jinak ticho; relace pokračuje. |
| REQ-20 | `signal`, `break`, `xon-xoff`, `x11-req`, `auth-agent-req@openssh.com` | Samostatné negativní scénáře nepodporovaných requestů. |
| REQ-21 | Chybné recipient ID u každého podporovaného requestu | Odmítnout, žádný účinek na existující kanál. |
| REQ-22 | Chybí want-reply nebo část request name | Odmítnout bez čtení za buffer a bez odpovědi pro náhodné ID. |
| REQ-23 | Request po otevření, ale před odesláním potvrzení klientovi | Při platném pipeliningu zachovat pořadí potvrzení a výsledku requestu. |
| REQ-24 | Selhání startu fixture programu | Žádný hang; kontrolovat rozdíl přijetí requestu a následného exit-status dítěte. |

### ENV – channel request env (P1; U/E)

Kód: `packet_channel_request.c`, `channel_env` v `channel.c`, `newenv.c`. Testuje se jen to, co vyvolá SSH request a co vidí spuštěný proces. ENV-06, ENV-07 a ENV-12 mají P0.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| ENV-01 | Jedna proměnná před startem | Dítě vidí přesné jméno/hodnotu. |
| ENV-02 | Více proměnných a následný exec | Všechny přijaté hodnoty dostupné. |
| ENV-03 | Stejná proměnná zapsaná podruhé | [R] Poslední přijatá hodnota, bez duplicitních či ztracených jiných položek. |
| ENV-04 | Prázdná hodnota | Přenesena jako prázdná, ne jako chybějící proměnná. |
| ENV-05 | UTF-8, mezery, CR/LF a `=` v hodnotě | Přesný přenos hodnoty, žádné rozdělení na více proměnných. |
| ENV-06 | NUL ve jménu/hodnotě | [N] Žádné tiché zkrácení na jinou proměnnou nebo hodnotu. |
| ENV-07 | Prázdné jméno / `=` ve jménu | [D] Definovat bezpečný kontrakt odmítnutí; nesmí přepsat jinou identitu proměnné. |
| ENV-08 | Vyčerpání povoleného počtu env requestů | FAILURE při limitu; dříve přijaté proměnné zůstávají konzistentní. |
| ENV-09 | Vyčerpání textového prostoru env | FAILURE bez poškození paměti; počítat i serverem vložené proměnné. |
| ENV-10 | Opakované přepisování jediné proměnné až k limitu | [R/N] Ověřit spotřebu prostoru a zachování staré hodnoty při selhání nahrazení. |
| ENV-11 | Env want-reply false při úspěchu i limitu | Žádná odpověď; účinek odpovídá výsledku. |
| ENV-12 | Env až po shell/exec/subsystem startu | [N/D] Cílově request odmítnout bez zničení běžící relace; dítěti prostředí zpětně neměnit. |
| ENV-13 | Klient posílá TERM před/po pty-req | [D] Explicitně určit prioritu; dítě vidí konzistentní TERM. |
| ENV-14 | Klient posílá SSH_CONNECTION, USER, HOME nebo PATH | [D] Zdokumentovat politiku serverových proměnných jako SSH pozorovatelný kontrakt; neodvozovat ji z OpenSSH. |
| ENV-15 | Env request s vadnou délkou prvního/druhého stringu nebo přebytkem | Odmítnout bez částečného přidání proměnné. |

### PTY – terminál a změny rozměrů (P1; U/E)

Kód: `packet_channel_request.c`, `channel_openterminal`, `channel_ptyresize`, `channel_forkpty.c`. PTY-03 a PTY-13 až PTY-16 mají P0. PTY závislé E testy patří do explicitně vybaveného prostředí.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| PTY-01 | pty-req před startem s TERM a běžnými rozměry | SUCCESS; dítě má PTY, správný TERM a rozměry. |
| PTY-02 | pty-req want-reply=false | PTY vznikne bez odpovědi. |
| PTY-03 | Druhý pty-req před startem | [R] FAILURE, původní PTY zůstane funkční. |
| PTY-04 | pty-req po spuštění shell/exec/subsystem | [R] FAILURE, žádný druhý terminál. |
| PTY-05 | Selhání dostupnosti PTY ve fixture | FAILURE, otevřený kanál lze dále využít bez PTY. |
| PTY-06 | Nulové rozměry všech čtyř polí | [R/D] Ověřit politiku ponechání výchozí velikosti. |
| PTY-07 | Nulové znakové rozměry, nenulové pixely a obráceně | Správné předání nezávislých polí. |
| PTY-08 | Rozměry 65535, 65536, UINT32_MAX | [D] Vymezit převod do systémového winsize; žádné přetečení vedoucí k jiné operaci. |
| PTY-09 | window-change po spuštění PTY | Dítě pozoruje novou velikost; signalizaci měřit deterministicky. |
| PTY-10 | Více window-change za sebou | Konečná velikost odpovídá poslednímu requestu. |
| PTY-11 | window-change před startem dítěte / bez PTY | [R] Odmítnutí nebo ticho podle want-reply, žádný ioctl na jiný descriptor. |
| PTY-12 | window-change want-reply=true | [R] Ověřit současnou odpověď pro přijatý i odmítnutý request; běžná varianta false je bez odpovědi. |
| PTY-13 | TERM obsahuje NUL | [N] Nepřijmout tiše pouze prefix. |
| PTY-14 | Zkrácení každého uint32 pole pty-req/window-change | Odmítnout bez částečné změny terminálu. |
| PTY-15 | Vadná vnější délka terminal-modes stringu | Odmítnout ještě před alokací PTY. |
| PTY-16 | Data za kompletním pty-req/window-change | Odmítnout. |
| PTY-17 | Prázdné modes / samotný TTY_OP_END | Ověřit přijetí základního požadavku. |
| PTY-18 | Jedna známá změna režimu a řádný END | [N] Test dosud neimplementované interpretace; nezafixovat TODO jako shodu s protokolem. |
| PTY-19 | Více známých modes, opakovaný opcode | [N/D] Definovat výsledný terminálový režim podle wire pravidel. |
| PTY-20 | Neznámý opcode s hodnotou / opcode ukončující parsování | [N] Respektovat pravidla terminálového streamu bez desynchronizace. |
| PTY-21 | Useknutá hodnota opcode / chybějící END | [N/D] Ověřit formát modes; minimálně bez čtení mimo string a částečně rozbitého PTY. |
| PTY-22 | Shell/exec s PTY produkuje stdout i stderr | Ověřit skutečnou terminálovou topologii; nepožadovat oddělený stderr jako u pipes. |

### DAT – běžná data kanálu (P0; U/E)

Kód: `packet_channel_recv.c`, `packet_channel_send.c`, datové funkce v `channel.c`.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| DAT-01 | Jeden byte tam a zpět přes echo fixture | Přesná shoda a správné recipient ID. |
| DAT-02 | Nulová délka CHANNEL_DATA | Žádné EOF, změna kreditu ani falešný byte. |
| DAT-03 | Všechny hodnoty byte včetně NUL a 0xff | Binární transparentnost. |
| DAT-04 | Opakované bloky větší než CHANNEL_BUFSIZE | Úplný přenos s průběžným doplňováním oken. |
| DAT-05 | Payload kolem hranice inzerovaných 16384 bytů | [N] Oddělit přípustnou délku channel dat od celého transportního rámce. |
| DAT-06 | Data překročí inzerovaný maxpacket, ale vejdou se do okna | [N/D] Ověřit serverovou politiku porušení limitu; žádné přepsání bufferu. |
| DAT-07 | Data přesně vyčerpají localwindow | Přijmout přesný počet a snížit kredit na nulu. |
| DAT-08 | Data o jeden byte přes localwindow | [R] Protokolová chyba bez zápisu nad povolený rozsah. |
| DAT-09 | Několik datových paketů v jediném read | Zapsat dítěti ve stejném pořadí, neslévat hranice chybně. |
| DAT-10 | Useknutá délka stringu / tělo kratší než deklarace | Žádný zápis neúplných dat do dítěte. |
| DAT-11 | Přebytečné byty za CHANNEL_DATA | Odmítnout, nepřidat je do streamu. |
| DAT-12 | Neexistující / cizí recipient ID | Odmítnout bez doručení do aktivního kanálu. |
| DAT-13 | Data po OPEN před startem procesu | [R/D] Současný server odmítá; nikdy nezapisovat do neotevřeného child descriptoru. |
| DAT-14 | Data ještě před CHANNEL_OPEN, zejména ID 0 | Nevyužít nulově inicializované channel.id jako platný kanál. |
| DAT-15 | Pomalý čtenář stdin dítěte | Bounded buffering a korektní backpressure, žádná ztráta. |
| DAT-16 | Pomalý klient přijímající stdout | Server nečte nekonečně dopředu a nepřekročí remote window. |
| DAT-17 | Současný velký upload a download | Žádný oboustranný deadlock. |
| DAT-18 | Dítě zavře stdin, dále produkuje stdout | [R] Pozdní klientská data nezabrání doručení výstupu a statusu. |
| DAT-19 | Dítě skončí v okamžiku přijetí dalších dat | [R] Pozdní vstup neukončí transport před předáním výsledku. |
| DAT-20 | EPIPE při částečně zapsaném pending stdin | [R] Zahodit zbývající vstup, zachovat výstup a exit status. |
| DAT-21 | Server posílá data při malém klientském maxpacket | Každý packet respektuje skutečně inzerovaný limit; pokrýt i clamp pod 32. |
| DAT-22 | Payloads mění velikost přes hranice paddingu | Přesný výstup, bez paddingu uvnitř dat. |

### EXT – extended data (P0; U/E)

Kód: `packet_channel_recv_extendeddata`, `packet_channel_send_extendeddata`, `channel_extendedread`.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| EXT-01 | Dítě píše pouze na stderr bez PTY | EXTENDED_DATA s data_type_code=1. |
| EXT-02 | Dítě střídá stdout a stderr | Každý proud zachová vlastní pořadí; nevyžadovat globální pořadí nezávislých pipes. |
| EXT-03 | Velký stderr přes několik oken | Žádná ztráta a stejný kredit jako stdout. |
| EXT-04 | Malý maxpacket při stderr | Zohlednit delší extended-data hlavičku; žádné překročení limitu. |
| EXT-05 | Příchozí platný EXTENDED_DATA typu 1 | [R/N] Obsah se nyní zahazuje; přesto prověřit validaci a účtování okna. |
| EXT-06 | Příchozí neznámý data_type_code | [D] Obsah ignorovat podle zvolené politiky bez změny formátu dalších zpráv. |
| EXT-07 | Příchozí EXTENDED_DATA s cizím ID | [N] Neignorovat kontrolu příslušnosti kanálu. |
| EXT-08 | Příchozí paket bez ID / typu / délky | [N] Odhalit dnešní chybějící parser. |
| EXT-09 | Délka dat přesahuje rámec / přebytečná data | [N] Žádné přijetí strukturálně vadné známé zprávy. |
| EXT-10 | Příchozí extended data přes povolené localwindow | [N] Nelze obejít řízení toku jiným message type. |
| EXT-11 | Směs příchozích DATA a EXTENDED_DATA vyčerpá kredit | [N] Oba typy spotřebovávají stejný kredit i při zahazování obsahu. |
| EXT-12 | Příchozí extended data před OPEN a po CLOSE | [N/D] Stavová validace; nesmí oživit kanál nebo obejít limity. |

### WIN – řízení toku a WINDOW_ADJUST (P0; U/E)

Kód: `packet_channel_recv_windowadjust`, `packet_channel_send_windowadjust`, `channel.c`; CHANNEL_BUFSIZE=131072, práh poloviny=65536.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| WIN-01 | Remote window 0, dítě má připravená data | Žádná DATA/EXTENDED_DATA bez kreditu. |
| WIN-02 | WINDOW_ADJUST +1 | Odeslat nejvýše jeden datový byte. |
| WIN-03 | Několik menších adjustů | Kredit se sčítá přesně. |
| WIN-04 | WINDOW_ADJUST +0 | Bez underflow, bez výstupu nad stávající kredit. |
| WIN-05 | Přidání přesně do UINT32_MAX | Přijmout přesný součet. |
| WIN-06 | Přidání o jeden přes UINT32_MAX | [R] Odmítnout přetečení remote window. |
| WIN-07 | +UINT32_MAX do nulového / nenulového okna | První platné, druhé přetečení. |
| WIN-08 | Window-adjust s chybným ID | Žádný kredit aktivnímu kanálu. |
| WIN-09 | Useknutý increment / dodatečné byty | Odmítnout bez změny okna. |
| WIN-10 | Současný stdout a stderr a jediný malý kredit | Součet dat obou proudů nepřekročí kredit. |
| WIN-11 | Prázdné DATA/EXTENDED_DATA | Nespotřebovat kredit. |
| WIN-12 | Lokální buffer kolem 65535, 65536, 65537 bytů | [R] Prověřit přesné podmínky odeslání doplnění. |
| WIN-13 | Localwindow kolem 65535, 65536, 65537 | [R] Druhá nezávislá podmínka prahu. |
| WIN-14 | Částečný zápis pending dat dítěti | Doplnění podle skutečně uvolněného místa. |
| WIN-15 | Opakované volání bez změny stavu | Žádné duplicitní nebo nulové WINDOW_ADJUST. |
| WIN-16 | Dítě vůbec nečte stdin | Nevracet kredit za dosud nevyprázdněný buffer. |
| WIN-17 | Výpočet doplnění | `plus = CHANNEL_BUFSIZE − pending − localwindow`, žádný záporný či přetečený výsledek. |
| WIN-18 | FIFO více adjustů a dat v jediném read | Zpracování v pořadí, ne souhrnná autorizace dřívějšího nadlimitního paketu pozdějším adjustem. |
| WIN-19 | EOF/CLOSE při nulovém remote window | Řídicí zprávy nejsou channel datový kredit. |
| WIN-20 | Dlouhý přenos s malým oknem a maxpacket | Trvalý postup bez ztráty a bez nekonečného přidávání kreditu. |
| WIN-21 | Adjust před OPEN / po CLOSE | [N/D] Žádná změna neexistujícího kanálu. |
| WIN-22 | Pozdní zahozená data po zavření stdin | [N/D] Vymezit účtování a životnost kreditu; žádný neomezený flood obcházející stav. |

### END – EOF, CLOSE a výsledek procesu (P0; U/E)

Kód: `packet_channel_recv.c`, `packet_channel_send.c`, `channel_puteof`, `channel_write`, `channel_iseof`, `channel_waitnohang`, smyčka v `main_tinysshd.c`.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| END-01 | Klient EOF při prázdném pending stdin bez PTY | Dítě vidí EOF; server může dále posílat stdout/stderr. |
| END-02 | Klient EOF při nevyprázdněném pending stdin | Nejprve doručit data, teprve pak zavřít vstup. |
| END-03 | Opakovaný EOF | Idempotentní, žádné dvojí zavření descriptoru. |
| END-04 | DATA po klientském EOF | [N/D] Nepokračovat v doručování po ukončení směru; zvolit odmítnutí nebo zahazování. |
| END-05 | EOF před OPEN / před spuštěním dítěte | [R/D] Řízená reakce bez zásahu do neplatného descriptoru. |
| END-06 | EOF se špatným ID / zkráceným ID / přebytkem | Odmítnout bez ukončení cizího směru. |
| END-07 | Dítě zavře jen stdout, stderr pokračuje | Neodeslat předčasné EOF pro celý kanál. |
| END-08 | Dítě zavře jen stderr, stdout pokračuje | Doručit zbytek stdout. |
| END-09 | Dítě skončí s velkým výstupem čekajícím v pipes | Veškerý výstup před konečným EOF/CLOSE. |
| END-10 | Normální exit 0, 1, 42, 255 | Přesný uint32 v exit-status, want-reply=false. |
| END-11 | Ukončení signálem TERM / KILL / INT | exit-signal se jménem bez prefixu SIG, bez exit-status místo něj. |
| END-12 | Serializace exit-signal | Recipient, false want-reply, core flag, description a language jsou strukturálně platné. |
| END-13 | Opakované volání send_eof/send_close | [R] Každá závěrečná zpráva nejvýše jednou. |
| END-14 | Serverová normální závěrečná sekvence | [R] Data, EOF, exit-status/exit-signal, CLOSE; klientův CLOSE relaci dokončí. |
| END-15 | Klient pošle CLOSE bez předchozího EOF během běhu dítěte | [N] Včasné CLOSE handshake; nečekat nekonečně na proces čtoucí stdin. |
| END-16 | Klient CLOSE na otevřeném, dosud nespuštěném kanálu | [N] Uzavření bez potřeby existujícího dítěte. |
| END-17 | Obě strany pošlou CLOSE současně | Jediné ukončení, žádné opakované odpovědi. |
| END-18 | Duplicitní CLOSE | Idempotentní nebo již uzavřený transport; žádné oživení. |
| END-19 | CLOSE + exec/data v témže read | [N] Neprovést žádný požadavek na již uzavřeném kanálu. |
| END-20 | CLOSE se špatným ID / truncation / přebytkem | Řízené odmítnutí. |
| END-21 | Klient neodpoví CLOSE | Bounded čekání podle zvoleného timeout kontraktu; harness nesmí viset. |
| END-22 | Transport EOF po dokončené závěrečné sekvenci | Bez falešného selhání úspěšně dokončené relace. |
| END-23 | Transport EOF během uploadu/downloadu | Žádný falešný úspěšný úplný přenos; cleanup bez osiřelých testovacích procesů. |
| END-24 | Child EOF a exit přijdou v opačném časovém pořadí | Výstup i status správně doručeny v obou variantách. |
| END-25 | Child exit těsně před instalací SIGCHLD obsluhy | Žádné čekání na již ztracený signál; stav zjistit také přes waitpid. |
| END-26 | EOF u PTY relace | [R/D] Samostatný kontrakt odlišný od pipes; neočekávat zavření společného PTY fd, které by zničilo výstup. |

### ORD – stavový automat napříč vrstvami (P0; W/E/F)

Kód: `packet_get.c`, `packet_auth.c`, dispatcher a rekey v `main_tinysshd.c`. Tyto scénáře ověřují zapojení handlerů do celého serveru, nikoli pouze jejich přímé volání.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| ORD-01 | Každé message number 0…255 před klientským KEXINIT | Rozlišit KEXINIT, DISCONNECT, povolené generické zprávy a neočekávané typy; žádná autorizace. |
| ORD-02 | Každé číslo 0…255 při čekání na KEX init | Jen platný krok zvolené metody postoupí dál; zohlednit strict a guessed paket. |
| ORD-03 | Každé číslo 0…255 při čekání na NEWKEYS | Žádný aplikační účinek; generické zprávy posoudit podle fáze a strict režimu. |
| ORD-04 | Každé číslo 0…255 při čekání na SERVICE_REQUEST | Žádné otevření kanálu před autentizací. |
| ORD-05 | Každé číslo 0…255 mezi ACCEPT a SUCCESS | Jen autentizační a povolené transportní operace; klientský SUCCESS nic nepovolí. |
| ORD-06 | Každé číslo 0…255 po SUCCESS před OPEN | Platné globální requesty, otevření, rekey a generické zprávy; ostatní nezmění kanál. |
| ORD-07 | Request/data/window/eof/close při channel.id=0, ale bez OPEN | [N] Nulové ID není důkaz existence kanálu. |
| ORD-08 | Channel zprávy po OPEN před startem | Rozlišit přípravné env/PTY/start od datových a závěrečných operací. |
| ORD-09 | Stejné zprávy při běžícím dítěti | Žádný druhý start ani nepovolená změna identity. |
| ORD-10 | Stejné zprávy po remote EOF | Neukončit nesprávný směr; neobnovit uzavřený vstup. |
| ORD-11 | Stejné zprávy po lokálním EOF při živém dítěti | Doručit případný konečný status bez restartu výstupu. |
| ORD-12 | Stejné zprávy po client CLOSE před server CLOSE | Žádné nové požadavky na uzavřeném kanálu. |
| ORD-13 | Stejné zprávy po obou CLOSE | Nezůstane aktivní kanál ani druhé použití starého ID. |
| ORD-14 | Platné payloady s jiným message number | Tělo nezmění dispatch a neobejde kontrolu typu. |
| ORD-15 | Nepodporované versus nečekané známé message number | Rozlišit UNIMPLEMENTED od selhání konkrétního requestu a od chyby pořadí. |
| ORD-16 | Globální request proložený channel requesty | Odpovědi mají správnou úroveň a zachovají pořadí ve své třídě. |
| ORD-17 | SUCCESS autentizace, OPEN, exec v bezprostředním sledu | Přechody proběhnou přesně jednou; žádná náhodná závislost na další poll události. |
| ORD-18 | Neplatná zpráva následovaná platným exec v jednom read | Po fatální chybě se exec nezpracuje. |
| ORD-19 | Odmítnutý nefatální request následovaný platným exec | Naopak zachovat možnost dalšího platného kroku. |
| ORD-20 | Povolovací stav před/po přímém handleru a přes dispatcher | Stejná pozorovatelná pravidla; U fixture nesmí obejít chybějící guard v reálné smyčce. |

### IO – fragmentace, backpressure a životnost SSH relace (P1; W/E)

Kód: `packet_recv.c`, `packet_send.c`, `packet_getall`, `getln.c`, I/O větve `channel.c`, poll smyčka `main_tinysshd.c`. IO-04 až IO-08 a IO-15 mají P0. Nejde o obecné testy POSIX funkcí: vždy se kontroluje dopad na konkrétní SSH scénář.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| IO-01 | Celá krátká relace po jednom bytu | Stejný SSH výsledek jako bez fragmentace. |
| IO-02 | Fragmenty 2, 3, 4, 7, 8, 9, 31, 32, 33, 16384 bytů | Hranice read se nepromítnou do obsahu či pořadí zpráv. |
| IO-03 | Náhodné fragmenty s pevným seedem | Reprodukovatelný výsledek a transcript. |
| IO-04 | Short write při odesílání SSH paketu | Dopsat pouze zbývající suffix, bez duplikace hlavičky. |
| IO-05 | EAGAIN/EWOULDBLOCK při transportním read/write | Zachovat pending data a navázat při readiness. |
| IO-06 | EINTR při read/write/poll v handshake i connection fázi | Bez ztracené zprávy, nesprávného čítače nebo falešného EOF. |
| IO-07 | Čtenář nejprve nečte, potom obnoví čtení | Vyprázdnit sendbuf a obnovit přenos bez změny protokolového stavu. |
| IO-08 | recvbuf/sendbuf blízko plné kapacity během obousměrného přenosu | Žádný overwrite, pevně omezená paměť, po odblokování postup. |
| IO-09 | Transportní half-close klientského vstupu | Odlišit od SSH CHANNEL_EOF, zachytit přerušení relace. |
| IO-10 | Klient zavře jen přijímání serverových dat | Řízené selhání zápisu, žádný nekonečný flush. |
| IO-11 | EOF v každém poli handshake zprávy | Žádné dokončení neúplné zprávy. |
| IO-12 | Peer přestane posílat během identifikace/KEX/auth | [R] Ověřit nynější pre-auth timeout; rychlá sada přes časovou fixture, skutečný dlouhý běh samostatně. |
| IO-13 | Peer přestane posílat během rekey | [R] Ukončení v rekey timeoutu, ne návrat k nedokončené epoše. |
| IO-14 | Neaktivní autentizované spojení a přijetí nových dat | [R] Ověřit reset současného idle alarmu při network read; neoznačit automaticky za timeout bez aplikační aktivity. |
| IO-15 | Série ignorovaných paketů už je celá v recvbuf | Zpracovat bez čekání na nový POLLIN. |
| IO-16 | Neznámá zpráva při plném sendbuf | UNIMPLEMENTED se neztratí a flush nezablokuje relaci natrvalo. |
| IO-17 | Dítě střídá krátké výstupy a pauzy při malém okně | Žádná nepřiměřená latence způsobená čekáním na zaplnění bufferu. |
| IO-18 | Síť, child stdout, stderr a SIGCHLD připravené současně | Všechny výstupy a status doručeny bez závodů. |
| IO-19 | Mnoho krátkých samostatných spojení po sobě | Žádný přenos channel ID, autorizace nebo nevyřízených dat do nové relace. |
| IO-20 | Dvě izolovaná spojení současně, jedno chybné | Chyba nebo timeout jedné relace nepoškodí druhou. |

### INT – nezávislí klienti a úplné scénáře (P1; E)

Použít verze klientů zaznamenané ve výsledku a feature detection dostupných algoritmů. Vlastní W klient je nutný pro malformed zprávy; běžný klient je obvykle odmítne sám před odesláním. OpenSSH a druhý nezávislý klient ověřují interoperabilitu, nikoli univerzální oracle všech lokálních politik.

| ID | Vstup / situace | Očekávání |
| --- | --- | --- |
| INT-01 | OpenSSH publickey + exec `exit 0` | Úspěšné spojení, status 0. |
| INT-02 | OpenSSH publickey + exec s nenulovým statusem | Klient vrátí přesný vzdálený status. |
| INT-03 | OpenSSH binární stdin/stdout bez PTY | Byte-for-byte shoda dlouhého obsahu. |
| INT-04 | OpenSSH oddělený stdout/stderr | Přesné dva výstupy a status. |
| INT-05 | OpenSSH interaktivní PTY a resize | Funkční terminál a přenos nové velikosti. |
| INT-06 | OpenSSH subsystem mapovaný na echo fixture | Korektní session request a binární přenos bez testování SFTP. |
| INT-07 | OpenSSH keepalive při otevřené session | REQUEST_FAILURE pro keepalive spojení nezabije. |
| INT-08 | OpenSSH vynucený malý rekey limit + velký přenos | Alespoň jedna prokazatelně dokončená výměna, stejná data/status. |
| INT-09 | OpenSSH s každým dostupným podporovaným KEX jménem | Celá relace projde, ne pouze výpis nabídky. |
| INT-10 | Nezávislý klient bez strict KEX podpory | Úplná non-strict relace s povoleným IGNORE/DEBUG. |
| INT-11 | Nezávislý klient se strict KEX podporou | Úplná strict relace včetně rekey. |
| INT-12 | Klient s více identitami: probe, neautorizovaný, autorizovaný klíč | Správné pokračování v rámci limitu; žádný falešný SUCCESS. |
| INT-13 | Druhý session kanál na témže transportu | Explicitní odmítnutí druhého, první zůstává funkční. |
| INT-14 | Klient nabízí ext-info-c a další neznámá pseudojména | Nabídka neznámého rozšíření sama nerozbije společný KEX; server nepředstírá neimplementovanou podporu. |
| INT-15 | Stejná sada exec/EOF/rekey přes pipes a socketpair/TCP loopback | Stejná SSH sémantika; rozdíly v transportních informacích nejsou porovnávány jako obsah protokolu. |

### FUZ – strukturované mutace a invarianty (P2; U/W/F)

Fuzzery dostávají pouze protokolové vstupy. Kryptografické backendy nepoužívat jako fuzz target. Pro dosažení connection vrstvy použít připravený autorizovaný stav nebo klientský encoder s platnou transportní obálkou; klíčové nalezené chyby vždy reprodukovat i přes skutečný daemon.

| ID | Generátor / vlastnost | Očekávání |
| --- | --- | --- |
| FUZ-01 | Truncation každého seed payloadu na všech offsetech | Žádná částečně provedená operace; řízená chyba. |
| FUZ-02 | Mutace všech uint32 délek na mezní hodnoty | Žádné čtení mimo rámec, přetečení ani nekonečné čekání. |
| FUZ-03 | Mutace booleans na všechny hodnoty 0…255 | Jednotná SSH boolean sémantika a správný počet odpovědí. |
| FUZ-04 | Mutace recipient ID pro všechny channel typy | Žádný účinek na nesprávný či neexistující kanál. |
| FUZ-05 | Přidávání suffixů k úplným známým payloadům | Odmítnutí tam, kde formát neobsahuje extension data. |
| FUZ-06 | Vložení NUL do každého textového pole | Žádná identita/request/command interpretovaná jen podle nečekaného prefixu. |
| FUZ-07 | Generování name-listů s duplicitami a neznámými názvy | Výběr jen ze skutečného povoleného průniku nebo explicitní selhání. |
| FUZ-08 | Permutace handshake kroků a vložení zprávy do každé mezery | Autorizace vznikne jen platnou cestou. |
| FUZ-09 | Modelové sekvence OPEN/request/data/adjust/EOF/CLOSE | Stav odpovídá povoleným přechodům a uzavřený kanál neožije. |
| FUZ-10 | Stejný transcript při různém rozdělení do read | Stejný dekódovaný výsledek, ignorovat náhodný cookie/padding. |
| FUZ-11 | Stejná data rozdělená do různých CHANNEL_DATA paketů | Stejný výsledný byte stream při dodržení maxpacket a oken. |
| FUZ-12 | Want-reply false/true dvojice téhož requestu | Stejný účinek; rozdíl pouze v definované odpovědi. |
| FUZ-13 | Počítání oken v nezávislém modelu | Žádný záporný kredit, overflow či odeslání nad kredit; počítat oba datové typy. |
| FUZ-14 | Vložený rekey v každém legálním místě přenosu | Stejná aplikační data, identita, status a životnost kanálu. |
| FUZ-15 | Duplikace terminálních zpráv EOF/CLOSE/DISCONNECT | Žádný druhý start, dvojité uvolnění nebo nekonečná odpovědní smyčka. |
| FUZ-16 | Minimalizace nalezeného chybového transcriptu | Uložit seed, fázi a nejmenší reprodukci jako samostatný deterministický regresní test. |

## 5. Systematické rozšíření parametrů

Katalog obsahuje **500 ID scénářů**. Některé řádky jsou rodiny parametrizovaných testů, nikoli tvrzení o přesně 500 budoucích spuštěních. Například ORD-01 až ORD-06 znamenají 6×256 základních kombinací fáze a čísla zprávy ještě před rozlišením strict režimu. Tato čísla nesčítat s jednotlivými případy jako unikátní pokrytí bez deduplikace.

Pro každý generovaný případ ukládat identifikaci například `ORD-05/type=94/strict=1/fragment=3`. U typů se známým formátem generovat nejprve platný payload odpovídající danému typu; samotný jeden byte by často otestoval jen truncation, nikoli stavový automat.

| Osa | Doporučené hodnoty | Použití |
| --- | --- | --- |
| Fáze | před KEXINIT, KEX init, NEWKEYS, service, auth, authorized bez kanálu, open bez procesu, running, EOF, CLOSE, rekey | ORD a každý handler, kde má fáze význam. |
| Strict | nevyjednaný / vyjednaný | První KEX, přechody NEWKEYS, obecné zprávy a rekey. |
| Směr | C→S / S→C | Nezávislá nabídka, serializace, čítače a flow control; nezasílat server-only zprávy jako pozitivní client scénáře. |
| uint32 | 0, 1, 31, 32, 255, 256, 16383, 16384, 16385, 32767, 32768, 32769, 65535, 65536, 131071, 131072, 131073, 0x7fffffff, 0x80000000, 0xffffffff | Pouze relevantní hranice daného pole; držet ostatní pole platná. |
| String | prázdný, délka 1, limit−1, limit, limit+1, vnitřní NUL, prefix/suffix, binární obsah | Rozlišovat binární data od jmen a C-string adaptací. |
| Framing | celý paket, každé dělení na dva fragmenty, byte-wise, více paketů, celý+část dalšího | Každá důležitá hranice parseru a změna epochy. |
| Want-reply | 0, 1, 2, 255 | Global a channel requesty; pozitivní i negativní výsledek. |
| Channel ID | 0, 1, nenulové vysoké ID, UINT32_MAX, validní ID±1 | ID−1/+1 počítat bez náhodného wrapu na platné ID. |
| Okno | nula, jeden byte, přesný zbývající kredit, kredit+1, maximum | Oba směry a oba datové proudy. |
| Dítě | bez procesu, běží/čte, běží/nečte, stdin zavřen, stdout EOF, stderr EOF, skončilo | Datové a závěrečné sekvence. |
| Start requesty | shell, exec, subsystem × stejná trojice druhého requestu | Všech devět kombinací druhého spuštění. |

Úplný kartézský součin všech os by vytvářel mnoho nesmyslných vstupů. Použít úplné kombinace pro hranice autentizace, identity, strict KEX a overflow; pro ostatní pairwise plus cílené trojice odvozené z konkrétních větví. U každého negativního případu měnit nejprve jedinou vlastnost platného základu. Vícenásobně vadné zprávy přidat až poté, aby bylo zřejmé, která kontrola je skutečně pokrytá.

## 6. Doporučené pořadí realizace

| Etapa | Konkrétní výstup | Závislosti a hotovo znamená |
| --- | --- | --- |
| 1. Migrace do `tests/` a spolehlivý runner | Přesun existujících SSH testů/helperů, vlastní `tests/makefilegen.sh`, `tests/Makefile` a `tests/test.sh`, relativní symlinky na produkční zdroje, kořenové delegující cíle, timeout/cleanup a izolované fixture. | Nejprve zaznamenat výchozí výsledky a po přesunu porovnat stejnou sadu. Vypnuté položky jsou chybějící pokrytí, ne PASS. Migraci provést před přidáváním nových scénářů. |
| 2. Přímé protokolové testy | Skupiny PAR, FRM, SEQ a strukturální části GLB/OPN/REQ/EXT/WIN/END. | Nezávislé dekódování odpovědí, fatal testy v child procesu, žádný root pro základní U sadu. |
| 3. Handshake na drátu | HEL, NEG, GSS, KEY, SKX, CTL a první ORD. | W klient umí zprávy řetězit a fragmentovat; pozitivní scénář skutečně dokončí NEWKEYS. |
| 4. Autentizace | SRV, AUT, PUB, NON včetně identity/NUL, nevyžádaného SUCCESS a limitů. | Funkční fixture autorizace a nejméně jedna skutečná pozitivní a negativní publickey E relace. |
| 5. Session a data | OPN, REQ, ENV, DAT, EXT, WIN, END, klíčové IO. | Binární přenos větší než buffery, malá okna, EPIPE, obousměrné zavření a přesné statusy. |
| 6. Rekey a terminál | RK, zbývající SKX, PTY, všechny stavové přechody ORD. | Přenos přes opakovaný rekey bez ztráty; testy změny nabídky a first-packet flags odhalují znovupoužitý stav. |
| 7. Šířka a dlouhodobé pokrytí | INT, FUZ, širší parametry IO a noční běhy. | Reprodukovatelné seedy, model oken a stavů, samostatné deterministické regrese pro nálezy. |

První malá dodávka s největší hodnotou: PAR-07/08/12, FRM-08/10, SEQ-09/14, NEG-11/15, KEY-07/08, SKX-04/07/09, SRV-11/12, AUT-12/22, PUB-09, NON-01/02, EXT-07/08/10, WIN-06, END-15/16/19 a CTL-06. U [N] nejprve přidat reprodukci a samostatně rozhodnout opravu; plán samotný není souhlas s rozšířením implementovaných SSH funkcí.

### Migrace podle `../pok/tests/`

Prostudovaný vzor má plochý adresář s testovacími `.c`, dvojicemi `.sh`/`.exp`, vlastním `makefilegen.sh`, generovaným `Makefile` a runnerem `test.sh`. Například `../pok/tests/packet.c`, `byte.c` a `pok-server.c` jsou relativní symlinky na produkční zdroje o adresář výše. Objekty i testovací binárky se sestavují uvnitř `tests/`; kořenový `makefilegen.sh` generuje delegaci `$(MAKE) test -C tests` a `$(MAKE) clean -C tests`.

Převzít tento způsob organizace, sdílení zdrojů a samostatného sestavení. Testy TinySSH zůstanou přizpůsobené zdejšímu API, loggeru, licencím a feature detection. Nekopírovat specifické linkovací knihovny ani crypto testy POK; do společného adresáře byly později přesunuty vlastní existující crypto testy TinySSH. Nepřebírat ani případné vady runneru: `pok/tests/test.sh` primárně porovnává výstup a neřeší samostatně každý návratový kód skriptu.

### Přesun existujících souborů

Názvy při prvním přesunu byly zachovány, aby byl diff snadno kontrolovatelný. Skutečné testovací zdroje, skripty a očekávané výsledky jsou v `tests/`; v kořeni nezůstávají jejich druhé aktivní kopie.

| Současné soubory v kořeni | Cílové umístění / zacházení |
| --- | --- |
| `test-tinysshd.sh`, `test-tinysshd.exp` | `tests/test-tinysshd.sh`, `tests/test-tinysshd.exp`; při čisté migraci zachovat rozsah aktivních scénářů. |
| `test-tinysshd-ignore.sh`, `.exp`, `_tinysshd-test-ignore.c` | Stejná jména v `tests/`. |
| `test-packet-global-request.sh`, `.exp`, `_tinysshd-test-global-request.c` | Stejná jména v `tests/`. |
| `_tinysshd-test-hello1.c`, `_tinysshd-test-hello2.c`, `_tinysshd-test-kex1.c`, `_tinysshd-test-kex2.c` | Přesun do `tests/`; jejich vypnuté scénáře obnovovat až samostatným krokem. |
| `_tinysshd-unauthenticated.c`, `_tinysshd-printkex.c` | Přesun do `tests/` jako SSH testovací helpery. |
| `runtest.sh` | Po přesunu všech testů jej nahradil společný `tests/test.sh`; kořenový runner byl odstraněn. |
| Relevantní scénáře z `old/tinyssh-tests/` | Při jejich obnově portovat přímo do aktivních souborů `tests/`; celý archiv nekopírovat ani automaticky nespouštět. |
| Produkční `.c`/`.h` potřebné pro sestavení testů | Relativní symlinky `tests/name.c -> ../name.c`, `tests/name.h -> ../name.h`; produkční originály zůstávají v kořeni. |

Původní SSH etapa nepřesouvala `test-crypto*`, jejich includy ani makekey/printkey testy. Následující konsolidační krok je přesunul beze změny obsahu do `tests/`, aby všechny aktivní testy používaly jediný build a runner. Benchmark `_tinysshd-speed.c` zůstává produkčním pomocným programem v kořeni.

### Cílová struktura

```text
tests/
  makefilegen.sh                # generuje zdejší Makefile
  Makefile                     # samostatný testovací build
  test.sh                      # spouštění a kontrola výsledků
  test-tinysshd.sh / .exp       # migrované skripty a očekávání
  test-tinysshd-ignore.sh / .exp
  test-packet-global-request.sh / .exp
  _tinysshd-test-*.c            # migrované i nové testovací programy
  _tinysshd-printkex.c
  _tinysshd-unauthenticated.c
  test-packet-protocol.sh / .exp
  test-ssh-handshake.sh / .exp
  test-ssh-auth.sh / .exp
  test-ssh-channel.sh / .exp
  test-ssh-integration.sh
  ssh-client*.c / .h           # společný scénářový klient a decoder
  ssh-fixture*.c / .h          # řízené testovací programy
  corpus/                     # strukturované seedy a reprodukce
  packet.c -> ../packet.c      # obdobně potřebné produkční moduly
  packet.h -> ../packet.h
  ...
```

Jádro sady bude ploché jako v POK; původně navrhovaná samostatná hierarchie `tests/ssh/` se nepoužije. Pomocné podadresáře jsou vhodné pro corpus a data, nikoli pro druhý nezávislý runner téže sady. `.o`, `.out`, testovací binárky, testovací varianty daemonu a generované feature soubory patří do `tests/`, nikoli mezi kořenové produkční artefakty. Dočasné keydir a další fixture mají unikátní pracovní adresář mimo vyhledávání testů.

### Sestavení, runner a kořenové cíle

1. Přidat `tests/makefilegen.sh`, který podle zdejších zdrojů a symlinků generuje `tests/Makefile`. Změny pravidel provádět v generátoru a regenerovat Makefile; upravit také kořenový generátor, ne pouze jeho výstup. Regenerace při stejných vstupech je deterministická.
2. Testovací binárky a potřebné utility sestavovat z lokálních objektů v `tests/`, včetně symlinku `tinysshd-makekey`, který používají fixture. Nesdílet `.o` s produkčním buildem; testovací makra a sanitizer flags nesmějí zneplatnit produkční artefakty ani naopak. Testovací seam nesmí být podmínkou jediného E ověření: klíčové E scénáře spustit i proti produkční variantě bez mocků.
3. Pro závislosti zachovat TinySSH feature detection, `tryfeature.sh`, `trylibs.sh`, potřebné `has*.c`, generované `has*.h`, `libs`, logy a `randombytes.o`. Skripty a zdrojové sondy sdílet relativními odkazy; generované výsledky vytvářet lokálně. Závislosti hlaviček generovat se správnou cestou `-I../cryptoint`; include cesty, `CC`, `CFLAGS`, `CPPFLAGS` a `LDFLAGS` musí fungovat i při přímém `make -C tests` v čistém checkoutu.
4. `tests/test.sh` vybírá stabilně seřazené dvojice `.sh`/`.exp` základní SSH sady a explicitně odlišuje integrační či dlouhé scénáře. Samotný runner a generátor se nespouštějí jako testy. Chybějící `.exp` u registrovaného golden testu je chyba, ne tiché přeskočení. Integrační test bez `.exp` má explicitní registraci a kontrolu návratového kódu.
5. Runner kontroluje **návratový kód skriptu i shodu očekávání**. Očekávané neúspěchy daemonu posuzuje konkrétní skript/harness a při splnění kontraktu sám vrací 0. Normalizace přes `sed` nesmí zakrýt neúspěch předchozí části pipeline. Při nesouladu zachovat `.out` a diagnostiku.
6. Skripty se spouštějí s pracovním adresářem `tests/`. Upravovat cesty k helperům, binárkám, fixture a referenčním souborům vědomě; nespoléhat na náhodný cwd volajícího. Přímé `sh tests/test.sh` z kořene se má samo přepnout do svého adresáře stejně jako běh přes Makefile.
7. Kořenový `test-ssh` deleguje `$(MAKE) -C tests test-ssh`; `test-ssh-integration` deleguje `$(MAKE) -C tests test-integration`; `test-ssh-fuzz-smoke` deleguje `$(MAKE) -C tests test-fuzz-smoke`. Používat rekurzivní `$(MAKE)`, aby fungovaly build proměnné a jobserver. Integrace není automaticky součástí rychlé sady.
8. Kořenový `make test` deleguje úplnou sadu do `tests/`; v kořeni nezůstává druhý runner, `TESTOUT` ani testovací binárka. `make test-ssh` deleguje na `make -C tests test-ssh` a nekompiluje ani nespouští `test-crypto`. Nevytvořit dvojí spouštění stejného testu.
9. Kořenový `clean` deleguje také `$(MAKE) -C tests clean`. Testovací clean odstraní lokální artefakty, nikoli produkční zdroje dosažitelné přes symlinky, `.exp` či corpus. Upravit pravidla ignorování generovaných souborů; neignorovat celou složku `tests/`.
10. Relativní symlinky evidovat v repozitáři a zkontrolovat v čistém checkoutu. Nevytvářet absolutní odkazy do pracovního prostředí ani kopie produkčního kódu, které by se časem rozešly. Seznam potřebných modulů musí zůstat explicitně kontrolovatelný a nemá automaticky linkovat test-crypto ani všechny programové `main()` do jedné binárky.

### Nové testovací soubory po migraci

Navázat na přestěhované `_tinysshd-test-*.c` a dvojice `.sh`/`.exp`. Všechny následující názvy jsou relativní vůči `tests/`:

- `_tinysshd-test-packet.c` + `test-packet-protocol.sh/.exp`: PAR, FRM, SEQ a přímé CTL.
- `_tinysshd-test-handshake.c` + `test-ssh-handshake.sh/.exp`: HEL, NEG, GSS, KEY, SKX.
- `_tinysshd-test-auth.c` + `test-ssh-auth.sh/.exp`: SRV, AUT, PUB, NON.
- `_tinysshd-test-channel.c` + `test-ssh-channel.sh/.exp`: OPN, REQ, ENV, EXT, WIN, END; scénáře přímých handlerů.
- Rozšířit migrovaný `_tinysshd-test-global-request.c` a `test-packet-global-request.sh/.exp` pro GLB; neduplikovat čtyři stávající kontroly.
- Společný scénářový klient, nezávislý decoder a fixture programy pro DAT/PTY/RK/IO/INT/ORD; seedy v `corpus/`.
- `test-ssh-integration.sh` a explicitní integrační cíl: knihovny a klienty pinovat v testovacím prostředí, běhové testy nemají nic stahovat.

Názvy nových testovacích souborů v tomto pododdílu jsou návrh. Migrované soubory a infrastruktura `tests/Makefile`, `tests/makefilegen.sh` a `tests/test.sh` už existují. Přímé testy a nezávislý klient mohou sdílet definice message numbers, ale ne bezvýhradně stejný parser/serializer jako server.

### Ověření dokončené migrace

- Před přesunem zaznamenat seznam skutečně aktivních SSH scénářů, výsledky a vypnuté části. Po samotné migraci projde stejná aktivní sada se stejnou sémantikou; nové scénáře a změny očekávání přidávat v oddělených kontrolovatelných krocích.
- Z čistého checkoutu funguje `make -C tests test` i `make test-ssh`; obě cesty vyberou tutéž sadu a nic nevyžadují z předchozího produkčního buildu. `make -j test-ssh` nesdílí fixture mezi testy.
- Ověřit přenos nastaveného kompilátoru a flags, opakovanou regeneraci obou Makefile a rebuild po změně produkčního `.c` i `.h` přes symlink. Test nesmí běžet proti zastaralému objektu.
- Řízené selhání testovacího skriptu při shodném stdout musí způsobit selhání celé sady; totéž platí pro rozdílný `.out` při návratovém kódu 0 a chybějící povinný `.exp`.
- V kořeni nezůstávají aktivní kopie migrovaných SSH testů ani jejich pravidla sestavení. Nemigrované testy mají stále funkční původní cesty a runner; jejich obsah se nemění.
- `make -C tests clean` a kořenový `make clean` bezpečně odstraňují odpovídající artefakty; kontrolovat, že symlinky, produkční zdroje a očekávané výsledky zůstaly zachovány.

## 7. Kritéria přijetí a kontrola pokrytí

1. Každá implementovaná položka eviduje ID, vrstvu U/W/E/F, skutečný vstup, očekávané zprávy, očekávané účinky a stav PASS/FAIL/XFAIL/SKIP. Plánované ID se do splněného pokrytí nezapočítává.
2. Všechny podporované zprávy mají pozitivní scénář, truncation všech povinných polí, délkovou hranici a neplatnou fázi. Všechny channel zprávy mají chybné ID. Známé bezextension payloady mají test přebytku; neznámé requesty mají test tolerovaného specifického payloadu.
3. Pozitivní scénáře nekončí jen tím, že server ještě běží: po jejich dokončení proběhne další platná operace nebo úplné ukončení relace. Nefatální odmítnutí nesmí poškodit následující platnou operaci.
4. Při negativním scénáři kontrolovat i nepřítomnost účinku: žádný marker spuštěného exec, žádný SUCCESS, žádné přidání kreditu cizímu kanálu. Již dříve legitimně zařazené výstupy neoznačit automaticky za porušení zákazu nové operace po DISCONNECT.
5. Základní protokolová sada běží při každé změně příslušných souborů. E sada má explicitní prostředí s účtem a PTY. Dlouhé timeout scénáře a fuzzing mají samostatný časově omezený běh.
6. ASan/UBSan použít na parserové a protokolové harnessy; sledovat zejména hranice bufferů, délkové konverze a uint32 účetnictví. Nevyhlašovat pokrytí či správnost kryptografických primitiv z těchto běhů.
7. Coverage report sledovat po větvích uvedených funkcí, zvlášť receive/send, před/po NEWKEYS a první KEX/rekey. Nepoužívat jedinou procentní metriku pro celý repozitář včetně crypto jako cíl tohoto plánu.
8. Každá oprava nalezená při implementaci má minimální deterministický test, který na původním kódu selže ze správného důvodu a po opravě projde. Broad suite opakovat při relevantní změně, ne maskovat nestabilitu automatickými retry.

## 8. Podklady pro rozhodování o protokolové shodě

Hlavním podkladem katalogu je přečtený lokální kód a stávající testy. Následující primární dokumenty určují, kde je potřeba odlišit protokolové pravidlo od lokální politiky nebo neimplementovaného rozšíření:

- [RFC 4251, §5 – datové typy](https://www.rfc-editor.org/rfc/rfc4251.txt): reprezentace boolean, uint32, string a name-list; podklad pro PAR a parametrické mutace.
- [RFC 4253 – transport](https://www.rfc-editor.org/rfc/rfc4253): §4.2 identifikace; §6 rámcování; §7.1 vyjednávání a guess; §7.3 NEWKEYS; §9 rekey; §10 služba; §11 generické zprávy. Hranice payloadu a celého packet length se posuzují odděleně.
- [RFC 4252 – autentizace](https://www.rfc-editor.org/rfc/rfc4252.txt): §5 společné requesty a odpovědi, §5.1 none, §6 rozsahy zpráv a §7 publickey. PK_OK není důkaz úspěšné autentizace; seznam příštích metod a partial-success mají vlastní význam.
- [RFC 4254 – connection protokol](https://www.rfc-editor.org/rfc/rfc4254): §4 globální requesty, §5 kanály/okna/EOF/CLOSE, §6 session a §8 terminal modes. Rozlišovat identifikátory obou stran, společný kredit DATA/EXTENDED_DATA a ukončení směru versus kanálu.
- [OpenSSH PROTOCOL](https://raw.githubusercontent.com/openssh/openssh-portable/master/PROTOCOL) odkazuje u strict KEX na [draft-miller-sshm-strict-kex-01](https://datatracker.ietf.org/doc/html/draft-miller-sshm-strict-kex-01). Jde o pracovní návrh, nikoli vydané RFC. Pro zdejší tokeny ověřovat zvlášť restrikce prvního KEX a reset sequence numbers při každém KEX; pravidla mechanicky nerozšiřovat na všechny fáze rekey.
- [RFC 8308 – extension negotiation](https://www.rfc-editor.org/rfc/rfc8308.txt): podklad pro interoperabilitu nabídky ext-info pseudojmen. Tento plán nepožaduje implementaci EXT_INFO ani server-sig-algs, které současný server neinzeruje.

U každé [D] položky před napsáním finálního assertionu zaznamenat konkrétní sekci a zvolenou lokální politiku, pokud standard připouští více reakcí. U MUST, SHOULD a MAY zachovat rozdíl síly požadavku. Interoperabilita s jedním klientem sama nepotvrzuje protokolovou shodu.
