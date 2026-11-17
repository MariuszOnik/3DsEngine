# Silnik 3DS z edytorem: projekt formatów i modułów

Stan: 17.11.2026, propozycja do dyskusji. Wizja w skrócie:
- **silnik gry RPG na 3DS** (Old 3DS jako minimum) z **edytorem świata na samej konsoli** (`mwedit`), jak OpenMW na PC;
- **repo publiczne kiedyś na GitHubie, bez żadnych danych Morrowinda**: gracz wypieka je z własnej kopii importerem na PC;
- **zgodność z danymi Morrowinda** (ESM/ESP, NIF, DDS), ale **własne formaty silnika**, żeby dało się używać innych modeli i treści.

## 1. Zasady (z pomiarów: OpenMW na Old 3DS, 3dsOpenworld)

1. **Pliki gotowe do użycia jednym odczytem.** Płaskie tablice, offsety zamiast wskaźników, zero rozbudowanych
   struktur do składania przy wczytywaniu. OpenMW tracił ~6 MB na drobnych, porozrzucanych alokacjach, a jego
   rekord dialogu ważył 400 B.
2. **Nazwy i ID jako indeksy** do jednej puli napisów (u16/u32), nie osobne napisy w każdym rekordzie.
3. **To, co rzadko czytane, zostaje na karcie:** teksty dialogów i książek, skrypty, dźwięki dłuższe niż kilka sekund.
4. **Format GPU od razu:** tekstury ETC1 w układzie 3DS, wierzchołki 16–20 B gotowe dla PICA200, pozycje s16
   względem komórki lub modelu.
5. **Każdy plik:** 4 znaki magii + wersja (u16) + rozmiar nagłówka. Starsze wersje czytamy albo odrzucamy z czytelnym
   komunikatem.
6. **Małe pliki pakujemy:** FTP i FAT na karcie źle znoszą tysiące drobnych plików (port OpenMW: 7 200 plików
   dźwięków = 12,8 s indeksowania). Wspólny kontener „pak” (rozdz. 2).
7. **Little-endian, wyrównanie do 4 (dane GPU do 16).**

## 2. Kontener `PAK` (wspólny dla tekstur, modeli, dźwięków, tekstów)

- **Nagłówek:** magia `PAK1`, wersja, liczba wpisów.
- **Indeks:** wpisy (hash nazwy u32, offset, rozmiar, typ), posortowane po hashu.
- **Pula nazw** (dla edytora i komunikatów).
- **Wyszukiwanie** binarne po hashu. Wczytanie wpisu to jeden `fseek` + `fread`. Indeks w RAM to ~12 B na wpis.

Dzisiejsze `textures.bin` (MWT1) i katalog `snd/` przejdą na PAK, gdy będziemy ruszać te części.

## 3. Świat

- **Komórka** `cells/<x>_<y>.bin` (dziś MWC3). Zawiera:
  - teren: 65×65 wysokości co 128 j. i obrazek 1024² ETC1 z mipmapami,
  - statyki scalone w paczki według materiału,
  - mapę zajętości 512² bitów (kolizja) i podłogi.

  Edytor buduje ją sam (bake na konsoli), więc format musi mieć wersję i zostać prosty.
- **Obiekty z identyfikatorem** (drzwi, skrzynie, przedmioty, NPC): tabela z trwałymi ID (jak FRMR w ESM). Zapis
  gry to **tylko różnice** względem świata (przesunięte, zabrane, otwarte, martwe).
- **Siatki ścieżek** (PGRD): punkty i połączenia na komórkę. Dziś `pathgrid.bin`.
- **Warstwa edycji:** zmiany z edytora zapisujemy jako **`.esp`** (LAND, CELL i referencje, później DIAL/INFO, NPC_).
  `.esp` otwiera się w OpenMW-CS i w Construction Secie. Pliki komórek to tylko wypieczona wersja do grania,
  zawsze odtwarzalna z ESM + ESP.

## 4. Modele statyczne `MDL1`

Biblioteka klocków do stawiania w edytorze (domy, drzewa, skały, meble).
- **Siatki według materiału:** tekstura (indeks w PAK tekstur), flagi (alpha test, dwustronny, mieszany, roślina
  „mała”).
- **Wierzchołek 16 B** jak dziś: pozycja s16×3 (+ skala modelu w nagłówku), UV s16×2 (/1024), kolor RGBA8. Światło w
  kolorze wypiekane przy bake'u komórki. Normalna niepotrzebna, bo statyki nie są oświetlane dynamicznie (pochodnia
  liczy odległość, nie kąt).
- **LOD:** 1–2 uproszczone wersje (`tools/simplify.py`) i odległości przełączenia.
- **Kolizja:** obrys w paśmie 30–140 j. nad podstawą (jak dziś mapa zajętości) i trójkąty „podłóg” (schody, mosty,
  dachy).
- **Kula i prostopadłościan otaczający, punkty zaczepienia** (np. światło lampy, płomień pochodni, miejsce
  siedzenia).
- **Bake komórki** (na PC lub konsoli) przekształca modele według pozycji i scala je w paczki komórki.

## 5. Postacie `CHR2` (rozwinięcie dzisiejszego `char.bin` MWCK)

Cel: mało pamięci, szybkie rysowanie, animacje, zmiana ekwipunku i broni.
- **Szkielet:** kości (hash nazwy, rodzic, poza spoczynkowa), wspólny dla rasy lub rodzaju (ludzie, bestie).
- **Części ciała i ekwipunku** podpinane pod sloty jak w Morrowindzie (głowa, tors, ręce, nogi…, broń, tarcza):
  - siatka ze skinningiem na GPU: wierzchołek z indeksami i wagami 4×u8, paleta ≤ 20 kości na wywołanie
    (ograniczenie stałych w shaderze);
  - części sztywne (broń, hełm) zawieszone na kości, bez wag.
- **Animacje (największy koszt w OpenMW):**
  - stała częstotliwość próbek (np. 15/s) zamiast kluczy z dowolnym czasem i stycznymi,
  - obroty jako skwantowane kwaterniony (smallest-three, 6 B na kość na próbkę),
  - przesunięcie tylko dla korzenia,
  - zdarzenia (uderzenie, krok, dźwięk) w osobnej tabeli,
  - grupy animacji wspólne dla szkieletu, wczytywane raz.
- **Sklejanie NPC:** przy pojawieniu się lub zmianie stroju części NPC łączą się w jedną siatkę (według tekstury i
  zestawu kości). NPC w tym samym stroju dzielą sklejoną siatkę. Gracz zostaje rysowany po kawałku.
- **Importery:** NIF (Morrowind) teraz, glTF z Blendera dla własnych modeli później.

## 6. Tekstury

- **ETC1** (nieprzezroczyste), **ETC1A4** (alfa), **RGB565 / RGBA4** (UI, małe), **A8** (maski). Wszystkie mipmapy, układ
  3DS (`source/etc1.c` umie kodować na konsoli).
- **Teren:** obrazek komórki 1024² ETC1 plus tekstura detalu. Do malowania w edytorze: paleta tekstur terenu
  (LTEX), małe kafle 64–128 px w RAM edytora.

## 7. Dźwięk i muzyka

- **Efekty:** **DSP-ADPCM** (format GameCube). DSP 3DS dekoduje go sam (`NDSP_ENCODING_ADPCM`), więc procesor nic nie
  liczy, a pamięć to ~3,5× mniej niż dzisiejsze PCM16. Mono, 16–22 kHz. Krótkie w RAM (linear), dłuższe
  strumieniowane.
- **Muzyka i głosy:** strumieniowane z karty. Też DSP-ADPCM (stereo 32 kHz to ~1,9 MB na minutę na karcie), zero CPU
  na Old 3DS. Ogg Vorbis (tremor jest w portlibs) to opcja, gdy liczy się miejsce na karcie, ale kosztuje ~10–15%
  CPU Old 3DS.
- **PAK dźwięków** z metadanymi: głośność, zasięg 3D, zapętlenie, punkty pętli.
- Importer: WAV, MP3 (Morrowind) → ADPCM, na PC.

## 8. Skrypty: MiniSharp2

- **MiniSharp2** (`E:\MiniSharp2`, osobne repo autora) jest już w grze. Kompiluje się na konsoli (6 ms na 3 pliki),
  obiekty mają klasy `ObjectScript` i zdarzenia.
- **Model:** zdarzenia, nie skrypty wołane co klatkę. Przykłady: `OnActivate`, `OnTalk`, `OnEnterCell`, `OnHit`,
  `OnDeath`, `OnTimer`, `OnItemAdded`. Plus API silnika (świat, gracz, przedmioty, dziennik, dialog, NPC/AI, dźwięk,
  wiadomości).
- **Na konsoli:** źródła w PAK skryptów, kompilacja przy starcie, z pamięcią podręczną kodu na karcie, jeśli start
  okaże się wolny. Edytor może pokazać i zmienić prosty skrypt.
- **Zgodność z MWScriptem** (skrypty Morrowinda, wyniki dialogów): **tłumacz MWScript → MiniSharp2 w importerze na PC**
  dla używanej części języka. Funkcje API nazwane tak jak w MWScripcie (`AddItem`, `Journal`, `SetDisposition`…).
  Interpreter MWScriptu na konsoli to opcja zapasowa.
- **Decyzja do podjęcia:** czy MiniSharp2 dostaje własny kod bajtowy na dysku (szybszy start, mniej RAM), czy zawsze
  kompilujemy ze źródeł.

## 9. Dialogi i dziennik `DLG1`

Z pomiarów: pełne dialogi Morrowinda to ~1,5 MB RAM w tym układzie, wobec 9 MB w OpenMW.
- **Tematy:** typ (temat, powitanie, perswazja, głos, dziennik), nazwa (pula napisów), zakres odpowiedzi.
- **Nagłówek odpowiedzi, ~24 B, zawsze w RAM:** aktor, rasa, klasa, frakcja, ranga, komórka, płeć, dyspozycja,
  frakcja i ranga gracza, liczba warunków. To wystarcza, by przy każdej rozmowie szybko odrzucić prawie wszystko
  (rozmowa przegląda odpowiedzi **wszystkich** tematów, jak w OpenMW `updateActorKnownTopics`).
- **Warunki, ~8 B:** funkcja, operator, indeks zmiennej lub przedmiotu, wartość.
- **Na karcie, w PAK tekstów:** tekst odpowiedzi, skrypt wyniku (MiniSharp2), plik głosu. Czytane przy wyświetleniu.
- **Dziennik:** te same tabele (typ „dziennik”, indeks etapu).
- **Edytor:** tabele na dolnym ekranie. Eksport do `.esp` (DIAL/INFO) dla zgodności z CS.
- **Lokalizacja:** teksty w osobnym PAK na język. Czcionka z polskimi znakami w atlasie UI.

## 10. Rekordy gry (NPC, przedmioty, czary, frakcje…)

- **Zwarte tablice według typu,** pola o stałym rozmiarze, nazwy i ID przez pulę napisów. Teksty opisów i książek na
  karcie. Rozmiary z pomiarów OpenMW:
  - NPC 1 552 B → cel ~100–150 B,
  - przedmioty ~400 B → ~40–60 B.
- **Importer** czyta ESM i ESP i pisze te tablice. Edytor zmienia je i eksportuje do `.esp`.

## 11. Moduły kodu (docelowo)

| Katalog | Co |
|---|---|
| `core/` | formaty (PAK, komórka, MDL, CHR, DLG, ESP odczyt i zapis), ETC1, pula napisów, matematyka |
| `render/` | shadery, teren, statyki, obiekty, postacie (skinning, sklejanie), woda, UI 3D |
| `audio/` | DSP: efekty, strumienie, 3D |
| `script/` | MiniSharp2 i API silnika |
| `game/` | logika gry: gracz, NPC/AI, walka, dialogi, dziennik, zapis gry |
| `editor/` | edytor (dziś: teren) |
| `tools/` | importery na PC (Morrowind → formaty silnika), później eksport z Blendera |

Przenoszenie po kawałku: dany kod trafia do `core/` czy `render/`, gdy potrzebują go i gra, i edytor.
Commit po każdej większej zmianie.

## 12. Kolejność (propozycja)

1. Edytor, teren: rzeźbienie (jest), malowanie, zapis do `.esp` (LAND), wczytanie w grze.
2. Edytor, obiekty statyczne: biblioteka `MDL1`, stawianie, bake komórki.
3. Format `CHR2` i sklejanie NPC, NPC z danych.
4. Dialogi `DLG1` i dziennik w grze, potem w edytorze.
5. Skrypty: API silnika w MiniSharp2, tłumacz MWScript.
6. Dźwięk: DSP-ADPCM i PAK.
7. Porządki przed publikacją: licencja, nazwa, README, importer dla graczy.
