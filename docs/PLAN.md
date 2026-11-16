# Plan: od renderera do silnika świata (2026-11-16)

Uzgodnione z Miśkiem (uwagi w ARCHITEKTURA.md). Cel pierwszego etapu: **gracz zabiera miecz ze
stołu, odchodzi kilometr, zapisuje, wyłącza konsolę, wraca i zastaje pusty stół.**

## Zasada

Stan obiektu jest oddzielony od jego obecności w pamięci:
- tabela obiektów dynamicznych (przedmioty, drzwi, skrzynie, NPC) jest **cały czas w RAM**,
  także dla niewczytanych komórek (ok. 3–4 tys. obiektów po ~32 B = ~120 KB);
- komórka dokłada tylko geometrię i modele do rysowania;
- zapis gry to tylko **różnice** względem danych z bakera.

## Kroki

1. **Obiekty z FRMR (baker + gra)**
   - Trwałe ID = (indeks pluginu u16, FRMR u32). Dwa identyczne miecze mają różne ID, a ID nie
     zmienia się po przeładowaniu komórki.
   - Baker przestaje wtapiać w statykę przedmioty (MISC / WEAP / ARMO / CLOT / BOOK / INGR /
     ALCH…), drzwi, skrzynie i NPC. Zapisuje je do `romfs/objects.bin`: ID, komórka, pozycja,
     obrót, skala, typ, flagi, indeks modelu.
   - Modele obiektów: wspólny cache siatek (jak tekstury), instancje rysowane z macierzą.
   - Rekord (szkic): `u32 frmr; u16 plugin; u16 type; u16 model; u16 flags; s16 cell[2]; f32 pos[3]; f32 rot[3]; f32 scale`.
2. **Interakcja**
   - Najbliższy obiekt przed postacią (promień ~120). Dolny ekran pokazuje „A: weź / otwórz /
     rozmawiaj”.
   - Zabranie przedmiotu: flaga TAKEN w stanie świata, przedmiot w ekwipunku.
   - Drzwi i skrzynie: otwarte / zamknięte.
   - Kolizja z NPC i obiektami dynamicznymi (koła / pudełka liczone w grze, nie w siatce bakera).
3. **Zapis i odczyt różnic**
   - Porcje: gracz (pozycja, stroje, ekwipunek), zmiany obiektów (ID → flagi / pozycja), zawartość
     skrzyń (po ID, dopiero gdy gracz coś zmienił; wcześniej obowiązuje lista z rekordu bazowego),
     zmienne, czas świata.
   - Każda porcja z wersją, całość z CRC32. Zapis do pliku tymczasowego, potem rename. Dwa sloty.
4. **NPC z danych** zamiast spawnów: pozycje z ESM, stroje z ich rekordów, idle, kolizja.
5. **Pluginy z OpenMW-CS (.omwaddon)**: scalanie jak w OpenMW: zmiana referencji mastera (ten sam
   FRMR), usunięcie (DELE), przeniesienie (MVRF), nowe obiekty. Test: postawić coś i coś usunąć
   w Seyda Neen, porównać na 3DS.
6. Dopiero potem: Lua (zdarzenia), dialogi, walka turowa, AI z harmonogramem.
   Szkic API skryptów, dialogów i questów: [SKRYPTY.md](SKRYPTY.md).

## Testy na konsoli po każdym kroku

- NPC: próg 30 fps (dziś ok. 15 widocznych; 1,3 ms CPU na widocznego).
- Przebieranie: wolna pamięć liniowa (`free` w play.log) bez zmian (siatki wgrane raz, przy
  starcie).
- Dwa identyczne przedmioty: zabrany jeden, drugi zostaje.
- Zapis: zabrany miecz i otwarta skrzynia po wyjściu z komórki, po powrocie i po restarcie konsoli.

## Stan na koniec sesji 2026-11-16 (build na konsoli = commit z walką)

Zrobione i sprawdzone na Old 3DS:
- kamery RTS (zoom, kąt: X + D-pad) / TPP / FPP, pływanie, nurkowanie, efekt podwodny, pochodnia;
- kolizja v2 (siatka 16 j., podłogi mostów i pomostów, rośliny bez kolizji), skok i grawitacja;
- GPU skinning: ok. 15 widocznych NPC przy 30 fps (1,3 ms CPU na widocznego), dalecy NPC bez animacji;
- dźwięk (po DSP1): kroki zależne od zbroi, pływanie, ciosy, pętle pochodni i wody, muzyka;
- dolny ekran: okno Morrowinda, ekwipunek z pojedynczych części (3 strony ikon), przyciski;
- walka turowa 1:1 (cięcie / pchnięcie / rąbanie, trafienia, zbroja, śmierć).

Do sprawdzenia na konsoli: kolory dolnego okna (w emulatorze paski HP wychodzą jasnołososiowe),
odczucia z walki, most po wygładzeniu podłóg.

## Punkt 1 (obiekty z FRMR) + pierwsze skrypty: zrobione 2026-11-16, sprawdzone w Azaharze

- `tools/mwbake.py` → `romfs/objects.bin` (MWO1): 444 obiekty w 88 komórkach (36 przedmiotów, 199 drzwi,
  209 pojemników), 64 wspólne modele, 318 KB. Rośliny do zbierania (CONT z flagą organic) zostają
  w scalonej statyce. Drzwi i pojemniki dalej blokują w siatce kolizji, przedmioty nie.
- Uwaga: na zewnątrz prawie nie ma luźnych przedmiotów (Seyda Neen: 0). Leżą we wnętrzach.
  Najbliższe: -2,-8 (sztylet Firebite, złoto) i -2,-10 (topór, złoto, kubek).
- Silnik: `source/objects.c` (stan zawsze w RAM, flagi TAKEN/OPEN/LOCKED/CHANGED), `obj.v.pica`
  (macierz na obiekt), A przy obiekcie = weź / otwórz / przeszukaj (podpowiedź na dolnym ekranie).
- Drzwi: 196 ze 199 na zewnątrz to drzwi ładujące (DODT/DNAM w FRMR: prowadzą do wnętrz). Nie obracają
  się; A pokazuje „Drzwi do: <wnętrze> (wnętrz jeszcze nie ma)”. Na zawiasach chodzą tylko drzwi bez celu.
  objects.bin = MWO2 (rekord 44 B z celem drzwi).
- Skrypty MiniSharp2: docs/SKRYPTY.md. Test „views 18” (bench.txt): drzwi (pukanie, potem cel),
  beczka (licznik skryptu).
- Do sprawdzenia na konsoli: koszt rysowania obiektów (`obj draws` w play.log), czy podpowiedź „A: ...”
  łapie właściwy obiekt.

## Kolejne kroki (2026-11-16)

1. **Kursor w świecie** (pomysł użytkownika). Old 3DS nie ma C-sticka, a dotyk działa tylko na dolnym ekranie.
   Propozycja: tryb celowania (przycisk na dolnym ekranie albo L+R), kursor na ziemi na górnym ekranie
   sterowany circle padem (promień z kamery na siatkę wysokości terenu), przyciąganie do obiektu lub NPC
   pod kursorem. A = potwierdź: idź tutaj / użyj / atakuj ten cel. Linia trasy i **koszt w punktach
   akcji za metr** (walka turowa i eksploracja). Zaznaczanie celu ataku zastępuje dzisiejsze „najbliższy NPC”.
   Alternatywa / dodatek: dotyk na mapce z góry na dolnym ekranie.
2. Punkt 3 planu: zapis różnic (flagi CHANGED + pola skryptów).
3. Edytor w grze (pomysł z rozmowy): dolny ekran = edytor MiniSharp2 (app/editor.c), L+X przeładowuje;
   stawianie obiektów kursorem (z puli wypieczonych modeli), zapis jako dodatek na karcie SD.
4. Wnętrza (drzwi ładujące już znają cel).
