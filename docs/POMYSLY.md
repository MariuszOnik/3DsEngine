# Pomysły i wnioski: silnik open world na 3DS (Morrowind)

Stan na 2026-11-16. Zbiór tego, co wiemy z pomiarów, co robią inni (Gothic3DS, openmw-3ds) i co proponowali
ChatGPT i Claude. Przy każdym pomyśle: skąd jest, czy jest zrobiony i co o nim myślimy.

## 1. Co wiemy z pomiarów (Old 3DS, test sprzętowy 1)

Log: [benchlogs/hw1-old3ds.log](../benchlogs/hw1-old3ds.log).

| | komórki | FPS | GPU | CPU | widoczne trójkąty | pamięć komórek |
|---|---|---|---|---|---|---|
| promień 0 | 1 | 60 (vsync) | 7–11 ms | 1,3–2,8 ms | 44–62 tys. | 2 MB |
| promień 1 | 3×3 | 36–43 | 17–20 ms | 6–7 ms | 140–163 tys. | 10,6 MB |
| promień 2 | 5×5 | 19–27 | 26–37 ms | 10–14 ms | 225–312 tys. | 25 MB |

- GPU ≈ 1 ms + **0,13 ms na 1000 trójkątów**. Wąskim gardłem są trójkąty, nie draw calle.
- CPU ≈ **0,025 ms na draw call** (Old 3DS).
- Pamięć: Old 3DS daje 53 MB linear (12 MB zostawiamy na zwykłą stertę). 3×3 komórki to ~11 MB.
- Wczytanie komórki z karty: 40–190 ms (w teście 1 jeszcze w głównej pętli).

Skąd są trójkąty (7×7 komórek wokół Seyda Neen, 1,67 mln trójkątów obiektów):

| rodzaj | udział |
|---|---|
| roślinność (drzewa, trawa, wodorosty, grzyby) | **60 %** |
| skały terenu | 19 % |
| budynki, architektura | 14,5 % |
| reszta | 6 % |

Sama trawa (`flora_grass_01`, `flora_bc_grass_01`) i wodorosty pod wodą (`flora_kelp_0x`) to ~15 %, każdy egzemplarz
ma 170–320 trójkątów i jest ich setki. Pojedyncze drzewa mają 400–1700 trójkątów.

## 2. Co robią inni

### Gothic3DS (`E:\Gothic3DS\GothicN3DS`, gra działa na New 3DS)
- Pieczenie na PC do własnych paczek, świat w chunkach 40 m, statyka scalona, światło w kolorach wierzchołków.
- Pozycje jako int16 względem chunka, szybki shader statyki (19 instrukcji), mgła w alfie wierzchołka.
- ETC1, tekstury max 128, `--simplify` (upraszczanie siatek), LOD-y chunków, warstwa daleka (`--far-lod`),
  tabela widoczności (`--vis`), pacer szczegółów/dystansu, odczyt z wyprzedzeniem (readahead), linear 42 MB.
- **Przejęte przez nas:** scalanie, wypiekane światło, int16, mgła w alfie, podział pamięci, pacer.

### openmw-3ds (`referencja/openmw-3ds`, pełny OpenMW na New 3DS)
- Cały OpenMW (skrypty, AI, fizyka, dialogi, MyGUI). OpenGL zastąpiony atrapą, własny backend citro3d chodzi po
  wynikach cullingu OSG i rysuje każdy liść osobno (bez scalania, object paging wyłączony).
- Tekstury na PC przez `tex3ds`: **ETC1 / ETC1A4** z mipmapami, max 256, przezroczyste teksele wypełnione kolorem
  sąsiadów (bez ciemnych obwódek).
- Światło liczone na PICA per wierzchołek: słońce + do 4 świateł punktowych, warianty shadera 0/1/2/4 świateł.
- **Skinning na GPU**: siatka postaci dzielona na partie po 14 kości, kości jako uniformy (`skin1.v.pica`).
- Pozy dalekich postaci aktualizowane co 2/3/4 klatkę zależnie od odległości.
- Small feature culling: obiekty mniejsze niż 6–10 pikseli nie są rysowane.
- Ceny: **tylko New 3DS**, zasięg widzenia **2560 jednostek** (~36 m), limit 30 FPS, wczytywanie „kilka sekund”,
  przycięcia 1–2 s, fizyka 17–26 ms na klatkę.
- Ocena: jako gra o klasę dalej niż my; w rysowaniu świata my jesteśmy wydajniejsi (3–5× dalszy zasięg na
  słabszej konsoli). **Do przejęcia:** ETC1 przez tex3ds, bleed przezroczystości, skinning w partiach po 14 kości,
  rzadsze pozy, small feature culling, metodologia benchmarku (1 % najwolniejszych klatek).

## 3. Lista pomysłów

Status: ✅ zrobione · 🔜 następne · 💡 do rozważenia · ❌ odrzucone

| | pomysł | skąd | ocena |
|---|---|---|---|
| ✅ | Własny format na konsoli, nie NIF; konwerter jako „kompilator świata” | ChatGPT, Gothic3DS | podstawa |
| ✅ | Scalanie statyki per materiał per ćwiartka komórki | Gothic3DS | mało draw calli, CPU nie jest problemem |
| ✅ | Światło słońca wypieczone w kolorach wierzchołków | Gothic3DS | tani shader |
| ✅ | Teren: jedna tekstura złożona na komórkę (256²) | Claude | 1 draw call; z bliska rozmyty |
| ✅ | Mgła jako zasłona granicy widoczności | ChatGPT | działa |
| ✅ | 3×3 komórki w pamięci, zasięg ustawiany przez pacer z uśrednionego GPU (histereza) | ChatGPT, Claude | wersja stream1, czeka na test |
| ✅ | Doczytywanie w wątku, przewidywanie kierunku ruchu, stara komórka znika 2 pierścienie dalej | ChatGPT, Claude | wersja stream1, czeka na test |
| ✅ | Benchmark na konsoli: trasa Seyda Neen → Pelagiad, zawracanie, sprint, pominięte klatki | ChatGPT, Claude | wersja stream1 |
| 🔜 | **Odcinanie małych obiektów po odległości** (trawa, wodorosty, kamyki osobną partią z krótszym zasięgiem) | openmw-3ds (small feature culling), Claude | najtańszy zysk: ~15–20 % trójkątów, bez nowej grafiki |
| 🔜 | **Wodorosty tylko gdy kamera blisko wody** | Claude | kelp to ~11 % trójkątów i spod wody prawie go nie widać z lądu |
| 🔜 | ETC1/ETC1A4 przez tex3ds + bleed przezroczystości | openmw-3ds, Gothic3DS | 4× mniej pamięci na tekstury, ładniejsze krawędzie liści |
| 🔜 | Upraszczanie siatek przy wypiekaniu (meshoptimizer / quadric) | Gothic3DS (`--simplify`) | drzewa 1000+ trójkątów to dobry cel |
| 🔜 | Uproszczony teren dalekich komórek (np. 9×9 wierzchołków = 128 trójkątów na komórkę) poza 3×3 | Gothic3DS (`--far-lod`), ChatGPT | horyzont za grosze: 25 dalekich komórek ≈ 3 tys. trójkątów |
| 💡 | **Impostory drzew** (billboardy w średniej odległości) | Maniek / ChatGPT | patrz sekcja 4: tak, ale dla drzew i w jednym draw callu na komórkę |
| 💡 | Panorama gór na horyzoncie | Maniek / ChatGPT | zbędna, jeśli będzie daleki teren low-poly; Morrowind i tak ma gęstą mgłę |
| 💡 | Octahedral impostors | Maniek / ChatGPT | za drogie w pamięci tekstur na 3DS; ewentualnie dla kilku punktów orientacyjnych |
| 💡 | Hybryda: logika gry z openmw-3ds + nasze wypieczone komórki zamiast rysowania obiekt po obiekcie | Claude | duże, ale łączy mocne strony obu |
| 💡 | Profile sprzętowe Old/New 3DS w jednym silniku | ChatGPT | tak: te same dane, inne budżety (zasięg, budżet GPU) |
| 💡 | NPC: skinning na GPU (partie 14 kości), rzadsze pozy z daleka, pełne AI tylko blisko | openmw-3ds, ChatGPT | po ustabilizowaniu streamingu; budżet ~15–17 ms GPU przy 30 FPS |
| 💡 | Światła punktowe (latarnie, pochodnie) w shaderze, 0–4 na draw | openmw-3ds | dopiero gdy dojdzie noc |
| 💡 | **Postacie ze zmienną zbroją / ubraniem**: szkielet + części ciała i ekwipunku podpinane do kości, wszystkie skinowane tym samym szkieletem | Maniek | wymaganie silnika (nie Morrowinda); u nas ludzie Morrowinda są właśnie tak złożeni, więc są dobrym testem |
| 💡 | Osobne, uproszczone siatki kolizji | ChatGPT | gdy dojdzie chodzenie po świecie |
| ❌ | GPU instancing | ChatGPT | PICA200 nie ma instancingu; każda instancja = draw call |
| ❌ | Atlasy tekstur dla świata | ChatGPT | Morrowind powtarza tekstury (wrap), atlas to psuje; ETC1 daje więcej |
| ❌ | „6 MB VRAM to limit” | ChatGPT | tekstury i geometria są w FCRAM (linear), VRAM tylko na bufory ekranu |

## 4. Impostory (pomysł Mańka) — ocena

**Werdykt: dobry pomysł, ale nie jako pierwszy krok i nie dla wszystkiego.**

Za:
- Nasze wąskie gardło to trójkąty (0,13 ms / 1000), a 60 % trójkątów to roślinność. Drzewo 600–1700 trójkątów
  zastąpione dwoma trójkątami w średniej odległości to ogromna oszczędność.
- Da się to zrobić tanio na PICA: wszystkie impostory komórki w **jednym** draw callu. Wierzchołek niesie środek
  drzewa i narożnik (−1/+1); shader rozsuwa go o wektory „prawo” i „góra” kamery z uniformów, więc obrót do
  kamery kosztuje kilka instrukcji, zero pracy CPU na klatkę.
- Tekstury impostorów wypieka konwerter na PC (render modelu z boku do atlasu, np. 64×64 na gatunek drzewa,
  ~20 gatunków w okolicy = jeden atlas 512×256).

Przeciw / uwagi:
- Najpierw tańsze rzeczy: odcinanie trawy i wodorostów po odległości daje ~15–20 % bez żadnej nowej grafiki.
- Fill-rate (overdraw): Gothic3DS nie był ograniczony fill-rate; billboardy z alpha testem są w porządku.
- „Pica200 i makroinstrukcje” z opisu ChatGPT to nieprawda; problemem jest liczba wierzchołków — to akurat się
  zgadza z naszymi pomiarami.
- Panorama gór jest niepotrzebna, jeśli zrobimy daleki teren low-poly (prawdziwa geometria, poprawna paralaksa,
  mało trójkątów).
- Octahedral impostors: wiele widoków na obiekt = dużo pamięci tekstur; na 3DS tylko dla kilku dużych punktów
  orientacyjnych, jeśli w ogóle.
- Ryzyko „przeskoku” przy zamianie 3D ↔ impostor: łagodzi go mgła i przejście (dithering alfą) na granicy.

Proponowana kolejność (po teście stream1):
1. Małe obiekty (trawa, kelp, kamyki) osobną partią z krótkim zasięgiem.
2. ETC1 + bleed.
3. Daleki teren low-poly poza 3×3.
4. Impostory drzew w średniej odległości (np. 3000–8000 jednostek), pełne 3D bliżej.
5. Upraszczanie siatek drzew i skał.

## 5. Kamera z góry pod kątem, sterowanie jak w RTS (pomysł Mańka, 2026-11-16)

**Werdykt: bardzo dobry pomysł dla 3DS, prawdopodobnie największy pojedynczy zysk ze wszystkich.**

- Kamera pochylona w dół nie widzi horyzontu: kadr na ziemi to prostokąt rzędu kilku tysięcy jednostek. Zasięg
  rysowania spada z ~10 000 do ~3000–4000, więc trójkątów w kadrze jest kilka razy mniej (szacunek: 15–30 tys.,
  GPU ~3–5 ms zamiast ~15 ms przy 0,13 ms / 1000).
- Znikają całe pozycje planu: daleki teren, impostory, część LOD. Mgła przestaje być potrzebna jako zasłona.
- W pamięci wystarczy 2×2 najbliższe komórki zamiast 3×3 plus pierścień: mniej pamięci, mniej wczytywania.
- Zwolniony budżet idzie na to, czego naprawdę chcemy: wiele animowanych postaci ze zmienną zbroją.
- Ekran dotykowy 3DS pasuje do RTS: dotknij, żeby iść, zaznacz postać, menu na dolnym ekranie.
- Uwagi: wysokie obiekty zasłaniają postać (trzeba przezroczystości / zdejmowania dachów i koron przy postaci);
  traci się widoki na horyzont; kąt i wysokość kamery najlepiej ograniczyć, bo od nich zależy budżet.
