# OpenMW-3DS na Old 3DS — odkrycia (17.11.2026)

Próba uruchomienia portu `openmw-3ds` (0.2.0, tylko New 3DS) na zwykłym 3DS. Repo: `E:\openmw-o3ds`, gałąź
`old3ds`. Wniosek: **pełny OpenMW nie zmieści się w pamięci Old 3DS samym odchudzaniem**. Brakuje ~10–14 MB
po wszystkich realnych cięciach, a procesor dałby ~7–10 fps.

## Budżet pamięci

| | New 3DS (tryb 178 MB) | Old 3DS (tryb 80 MB) |
|---|---|---|
| Obraz programu (kod) | 30,3 MB | 30,3 MB |
| Sterta | ~120 MB | **38,5 MB** |
| Linear (GPU/DSP) | 24–48 MB | 12 MB |

- **Tryb 80 MB to maksimum Old 3DS** (Gothic 3DS: 80 MB działa, 96 MB wywraca konsolę).
- **Stary log z konsoli (0.2.0, tryb 64 MB):** sterta 17 MB, z czego 8 MB zarezerwowane na Lua, a 8,6 MB pamięci linear leżało bezczynnie. Abort przy ładowaniu ESM.

## Pomiar zużycia (Azahar, tryb New 3DS, Seyda Neen)

| Co | MB |
|---|---|
| Stan gry w Seyda Neen (przed poprawką INFO) | 80,6 (83,6 po rozmowie) |
| po poprawce INFO, siatka 3×3 | 73,2 |
| po poprawce INFO, tylko komórka gracza (1×1) | 70,0 |

Rozkład (memprobe, memstage):

| Pozycja | MB | Uwagi |
|---|---|---|
| Rekordy ESM razem | 23,1 | |
| ↳ INFO (dialogi) | 9,0 | 23 693 × 400 B; teksty już leniwe (`LazyString`), obiekty nie |
| ↳ NPC_ | 4,0 | 2 675 × 1 552 B |
| ↳ STAT | 2,2 | 821 B/szt. — podejrzane, pewnie rehash puli napisów |
| ↳ PGRD | 1,6 | siatki ścieżek całego świata |
| ↳ CELL | 1,3 | |
| `store setUp` | 6,1 | **błąd: kopia wszystkich INFO** (naprawione) |
| Okna interfejsu tworzone przy starcie | 8,4 | `settingsWindow` 1,8; reszta 0,1–0,4 na okno |
| Indeks plików VFS (BSA) | 4,1 | ~16 tys. plików × ~250 B (`std::map` ze ścieżką) |
| Lua | 4,5–5,5 | |
| Pierwsza komórka (`insertCell`) | 6,9 | szablony: animacje NPC 2,8 (7 NPC), modele 2,7 |
| Każda kolejna komórka | ~0,4 | 8 sąsiadów razem ~3 MB |
| Wolne dziury < 1 KB w stercie newlib | 5,8 | fragmentacja drobnych alokacji |

Inne pomiary:
- **Graf sceny OSG jest mały:** 1 174 węzły na 1 849 obiektów w 9 komórkach. Teoria „tysiąca beczek” się nie potwierdziła.
- **Rozmowa z NPC:** +0,5 MB, 81 ms. Start rozmowy przegląda **wszystkie** tematy gry (`updateActorKnownTopics`), więc leniwe ładowanie tematów bez nagłówka w RAM nic nie da.
- **OpenMW już spłaszcza modele przy wczytaniu** (`FLATTEN_STATIC_TRANSFORMS`, `MERGE_GEOMETRY`, `SHARE_DUPLICATE_STATE`). Optymalizator na PC skróciłby czas ładowania, ale pamięci by nie oszczędził.

## Znaleziony błąd w OpenMW

`components/esm3/infoorder.hpp`, `extractOrderedInfo`: `info = mOrderedInfo` **kopiował** listę wszystkich INFO
i zostawiał oryginał. Każda odpowiedź dialogowa była w pamięci dwa razy przez całą grę. Przeniesienie zamiast
kopii: **−6,1 MB** (commit `974290a`). Dotyczy też PC i New 3DS — warto zgłosić autorowi portu i upstreamowi OpenMW.

## Azahar

- **Tryb Old 3DS Azahara wywraca sam emulator** (naruszenie dostępu w `azahar.exe`) po `createWindow`, także w trybie 64 MB i jeszcze przed pierwszym wątkiem. Na prawdziwym O3DS ten moment przechodzi. Tryb New 3DS działa.
- **Obejście:** plik `sdmc:/openmw-test/o3ds` w trybie New 3DS włącza profil O3DS i przycina stertę oraz linear do wartości z trybu 80 MB. Gra pada wtedy tak jak konsola (emulacja jest ~2 MB hojniejsza od prawdziwego O3DS).

## Co zostało zrobione (gałąź `old3ds`)

- **Drugi CIA `openmw_o3ds.cia`:** ten sam ELF, SystemMode 80 MB, AffinityMask 3, UniqueId o 2 niżej.
- **Profil O3DS** (`p3ds::info().lowMemory`):
  - linear 12 MB, pula Lua 3 MB, wątki na rdzeniu 1,
  - tekstury od 2. mipmapy,
  - 2 komórki w pamięci poza aktywnymi,
  - lżejsze ustawienia (`n3ds/o3dsprofile.cpp`).
- **Wycięte wtyczki OSG** `osgdb_osg` i `osgdb_serializers_osg`; znacznik błędu na 3DS to pusty węzeł.
- **Pomiary:**
  - spis sceny po zmianie komórki,
  - pamięć i czas startu rozmowy,
  - przełączniki plikowe `o3ds`, `o3ds-nocut`, `grid0`, `no-core1`.
- **Narzędzia:** `tools-o3ds/azahar-test.sh` (pojedynczy test z przywróceniem trybu Azahara), `build-lto.sh`.

## Ile da się jeszcze wyciąć (szacunek z pomiarów)

| Krok | MB |
|---|---|
| INFO jako zwarty nagłówek w RAM + reszta z karty | −4…5 |
| Zbędne okna interfejsu (lub tworzone przy otwarciu) | −4…6 |
| Zwarty indeks VFS | −2 |
| Siatka 2×2 zamiast 3×3 | −2 |
| PGRD tylko dla aktywnych komórek, NPC_ | −3 |
| Lua | −1…2 |
| **Razem** | **~16–20 MB** |

Potrzeba ~30–34 MB, więc nie wystarczy.

## Wnioski dla Morrowinda na Old 3DS (metoda Gothic3DS w 3dsOpenworld)

- **Dane trzeba spakować na PC**, a nie trzymać w obiektach C++. Kandydaci według wagi: INFO (nagłówek filtra ~20–30 B na odpowiedź + warunki i tekst z karty), NPC_, PGRD, indeks plików.
- **Dialog wymaga szybkiego filtra po aktorze/rasie/klasie/frakcji/komórce dla wszystkich odpowiedzi** przy każdym starcie rozmowy — to musi być w RAM i zwarte.
- **Animacje postaci to największy koszt „szablonów”:** ~2,8 MB na pierwsze NPC. 3dsOpenworld ma już własny, lżejszy format (chrbake).
- **Uważać na kopie list przy wczytywaniu i na fragmentację drobnych alokacji** (5,8 MB dziur): lepiej bloki/areny niż tysiące `new`.

## Pliki

- `logs/azahar-n3ds-run1.log` — pierwszy pełny przebieg
- `logs/t01..t05-*.log` — kolejne testy:
  - `t01` — limit O3DS, OOM w `setUp`,
  - `t02` — po poprawce INFO,
  - `t03` — bez limitu,
  - `t04`/`t05` — siatka 3×3 / 1×1.
- Stary log z konsoli: `D:\Programowanie\OpenMorrowind\3dsOpenworld\crash\openmw\`

## Test �szkieletu� (ma�y ESM, 17.11.2026, t06)

`tools-o3ds/make_mini_esm.py` wycina z Morrowind.esm sam� Seyda Neen: 22 kom�rki (9 zewn�trznych + 13 wn�trz),
30 NPC, bez dialog�w. Plik ma 3,1 MB zamiast 79,8 MB, a w pami�ci zajmuje 2,9 MB zamiast 23 MB.

Wynik z limitem O3DS: **brak pami�ci przy pierwszym NPC**, przy wczytywaniu animacji (`KeyframeManager`,
klucze kwaternion�w z `base_anim`). Ju� przed wej�ciem do �wiata sterta zajmuje ~36,5 MB z 38,5 MB.

Sam �szkielet� bez danych gry (stan przed newGame):

| Co | MB |
|---|---|
| W�tek g��wny: okna 8,4, indeks BSA 4,1, reszta startu | 15�21 |
| Pula robocza (w�tek t7) | 6,4 |
| Lua | 4,5 |
| Alokacje spoza C++ (czcionki, ICU, archiwa) | ~5�7 |
| **Razem** | **~35�37** |

M�j szacunek (18�21 MB) by� za niski, bo pomin��em pul� robocz�, Lua i alokacje C.
Wniosek: **sam silnik OpenMW zajmuje prawie ca�� stert� Old 3DS**. Zanim zmie�ci si� jakikolwiek �wiat, trzeba
odchudzi� szkielet o ~10�15 MB:

| Krok | MB |
|---|---|
| Okna interfejsu tworzone przy otwarciu | -5 |
| Indeks BSA | -2,5 |
| Pula robocza (co wczytuje na starcie?) | ? |
| Lua | -1,5 |
| Zwarte klucze animacji (ka�dy klucz to 3 kwaterniony, tak�e przy interpolacji liniowej) | -1,5 |
