# Architektura silnika: uwagi do propozycji (2026-11-16)

Uwagi Claude'a do propozycji architektury RPG open-world („8 modułów", graf sceny, zapis jako
różnice, Lua, fizyka hybrydowa, decyzja A/B), skonfrontowane z tym, co silnik już ma i co
zmierzyliśmy na Old 3DS.

## Decyzja A czy B

**B: własny silnik RPG.** Dane Morrowinda służą jako pierwszy świat testowy (konwerter
`tools/mwbake.py`). Zgodność z Morrowindem (MWScript, pełne mechaniki) to osobny projekt, który
zjadłby wszystkie siły. OpenMW-3DS potwierdza ten wybór: na Old 3DS wyłożył się na samym
wczytaniu `Morrowind.esm`. Ma 32 MB kodu, do tego Boost, ICU, OSG i Bullet, a na całą resztę
zostało 19 MB sterty (log w `crash/openmw/`). Uniwersalność to tutaj luksus, na który ten sprzęt
nie stać.

## Gdzie się zgadzam

- **Stan świata oddzielony od tego, co jest załadowane.** To najważniejsza myśl w propozycji.
  NPC istnieje (żyje, ma przedmioty, harmonogram), nawet gdy jego komórka nie jest w pamięci.
- **Zapis jako różnice względem świata z płyty**, z wersją formatu, sumą kontrolną i zapisem
  do pliku tymczasowego z podmianą (rename). Geometrii nie zapisujemy.
- **Komunikacja przez identyfikatory i zdarzenia, nie przez wskaźniki.** Przy streamingu
  wskaźnik do obiektu z wyładowanej komórki to pewny crash.
- **Własna, prosta fizyka zamiast Bulleta.** Mamy ją już:
  - siatkę blokad 16×16 jednostek na komórkę,
  - podłogi (mosty, pomosty, schody),
  - grawitację, skok i pływanie.

  Brakuje tylko kolizji postać–postać (koła) i promieni do ataku i interakcji.
- **Lua z ograniczonym API i limitem wykonywania** (`lua_sethook` z licznikiem instrukcji).
  Skrypty widzą funkcje silnika, nie jego pamięć.
- **Kolejność budowy** w zasadzie tak. Poniżej poprawki do niej.

## Gdzie bym zrobił inaczej

### 1. Nie ogólny graf sceny, tylko płaskie listy

Graf sceny (drzewo węzłów z dziedziczonymi transformacjami) to model OpenSceneGraph. Na PC jest
wygodny, ale na Old 3DS kosztuje: każdy węzeł to wskaźniki, przechodzenie drzewa co klatkę i
osobne rysowanie każdego obiektu.

U nas statyczny świat jest już wypieczony na PC: obiekty scalone per materiał i ćwiartka
komórki, światło w kolorach wierzchołków. Wychodzi kilkadziesiąt rysowań na komórkę zamiast
tysięcy (test T1: CPU ok. 0,025 ms na rysowanie). Tego nie oddawajmy.

Proponuję trzy płaskie warstwy:

| Warstwa | Co to jest | Gdzie |
|---|---|---|
| Statyka | wypieczone partie, kolizja, podłogi | plik komórki (MWC3), bez zmian |
| Obiekty dynamiczne | przedmioty, drzwi, skrzynie, NPC: każdy osobno, z trwałym ID | nowa tablica w pliku komórki |
| Stan świata | różnice: zabrany, otwarty, zabity, przesunięty… | w RAM cały czas, do zapisu |

Hierarchia jest potrzebna tylko tam, gdzie naprawdę istnieje: szkielet postaci i przedmioty
przypięte do kości (broń, pochodnia). To już działa. „Wyłączenie całej komórki" to u nas po
prostu wyładowanie pliku.

### 2. Trwałe ID z danych, nie wymyślane

Morrowind ma w każdej referencji `FRMR`, numer obiektu w pliku. Trwałe ID to (plik, FRMR).
Baker musi przestać wtapiać w statykę rzeczy, które mogą się zmienić: przedmioty
(MISC/WEAP/ARMO/CLOT…), drzwi, skrzynie i NPC. Mają trafić do tablicy obiektów dynamicznych
z ID, pozycją i modelem. **To pierwszy konkretny krok.** Bez niego nie da się „zabrać miecza
ze stołu", bo dziś miecz jest częścią tej samej siatki co stół.

### 3. Baza świata bez SQLite

SQLite to 500 KB kodu i alokacje na stercie, która ma 12 MB. Wystarczy:
- **dane bazowe:** posortowane tablice rekordów wygenerowane przez bakera (bsearch po ID),
  czytane z romfs;
- **różnice:** mała mapa ID → zmiana (flagi, pozycja, zawartość pojemnika), kilkadziesiąt KB
  nawet po długiej grze;
- **zapis:** porcje (gracz, różnice, zmienne, zadania, czas), każda z wersją, plus CRC32 całości.

### 4. Skrypty: tylko zdarzenia, nie skrypty działające co klatkę

Skrypty MWScript Morrowinda działają co klatkę. Na Old 3DS to za drogie. U nas skrypt
reaguje na zdarzenia:
- `on_enter_cell`, `on_activate`, `on_item_taken`, `on_dialog`, `on_timer`, `on_death`.

Wywołania skryptów pozwalamy tylko z kolejki zdarzeń, z limitem czasu na klatkę.

### 5. NPC poza ekranem: harmonogram, nie symulacja

Na ekranie NPC ma animację i ruch. Poza ekranem tylko „gdzie powinien być o tej godzinie",
liczone raz na minutę gry. Walka turowa (twój pomysł) bardzo pomaga: AI walki to decyzja raz
na turę, nie co klatkę, i nie ma fizyki ciosów.

## Budżet pamięci Old 3DS (dziś, zmierzone)

| Co | Ile |
|---|---|
| Sterta zwykła | 12 MB (kod, postać 1,6 MB, logi) |
| Sterta liniowa (GPU) | ok. 52 MB |
| – tekstury obiektów (ETC1) | 4,3 MB |
| – komórki w RTS (6 sztuk) | ok. 11 MB |
| – komórki w TPP (do 9–12) | ok. 20–26 MB |
| – siatki postaci (GPU skinning) | ok. 0,7 MB, wspólne dla wszystkich NPC |
| Wolne na stan świata, Lua, dialogi | ok. 5–8 MB sterty |

Lua 5.1 z limitem 2–3 MB i własnym alokatorem się zmieści. Teksty dialogów trzymamy na karcie
i czytamy na żądanie.

## Moja kolejność (od tego, co już jest)

1. **Obiekty dynamiczne w bakerze** (osobno, z ID z FRMR) i ich rysowanie: instancje modeli,
   wspólny cache siatek.
2. **Interakcja:** najbliższy obiekt przed postacią, dolny ekran pokazuje „A: weź / otwórz /
   rozmawiaj". Zabieranie przedmiotów do ekwipunku (ekwipunek już jest).
3. **Zapis i odczyt różnic.** Test z propozycji: zabrać miecz, wyjść, zapisać, wyłączyć
   konsolę, wrócić, miecza nie ma.
4. **NPC z danych** zamiast spawnów L+A: pozycje z ESM, kolizja postać–postać, prosty
   harmonogram.
5. **Lua i zdarzenia**, zmienne globalne, zadania (etapy).
6. **Dialogi** na dolnym ekranie (tematy jak w Morrowindzie).
7. **Walka turowa:** atak już animuje, dojdą trafienie, obrażenia i tury.

Między krokami pomiar na konsoli, jak dotąd (benchlogs, docs/benchmarks.html).

## Edytor świata na PC (twój pomysł)

**Na start: OpenMW-CS zamiast własnego edytora.** OpenMW-CS już umie rzeźbić teren, malować go
teksturami, stawiać i obracać modele, tworzyć NPC, przedmioty, dialogi i zadania. Zapisuje
`.omwaddon`, czyli ten sam format TES3 co `Morrowind.esm`. Nasz `mwbake.py` czyta już rekordy
LAND, CELL, STAT i resztę, więc wystarczy, że przyjmie dodatkowo plik dodatku. Zmiany w edytorze
od razu przejdą przez bakera na konsolę. Koszt: mały, kilkadziesiąt linii w bakerze.

**Własny edytor ma sens później**, gdy będą rzeczy, których OpenMW-CS nie zna, np.:
- strefy walki turowej,
- nasze skrypty Lua i ich podpięcie do obiektów,
- podgląd budżetu 3DS na żywo (trójkąty i pamięć komórki),
- własne typy obiektów.

Wtedy proponuję prosty program w Pythonie z oknem 3D (np. moderngl, pyglet lub Dear PyGui).
Dzieliłby kod z bakerem: wczytuje komórki tak, jak widzi je 3DS, i od razu pokazuje koszt. To
większa praca (tygodnie). Lepiej zacząć ją, gdy gra będzie wiedziała, czego jej brakuje.

## Co zmierzyć teraz na konsoli (build npc1)

- **NPC:** L+A dodaje po 5. Patrzymy na fps i „NPCs … pose … draw … ms" na dolnym ekranie przy
  10, 20, 30 postaciach, w RTS i TPP. W emulatorze rysowanie kosztuje ok. 0,6–0,8 ms CPU na
  NPC, ale emulator nie oddaje prawdziwych czasów.
- **GPU skinning:** czy GPU mieści się w budżecie przy 20+ NPC.
- **Kolizja z roślinami:** czy coś jeszcze blokuje.
