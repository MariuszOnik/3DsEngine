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

Następny krok: punkt 1 planu: obiekty z FRMR.
