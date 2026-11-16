chcialbym srubowac napisac silnik open world na 3ds ktory streamuje komurki (cell) jak open morrowind i dowiedziec sie ile udzwignie 3ds.  Czy na podstawie kodu openn morrowind na switcha w tym folderze mozesz do testow wczytac jedna komurke swiata morrowind z modelami i sprawdzimy czy 3ds to uciagnie ? tu mma dl aprzykładu inne 3ds make : D:\Programowanie\LuaDS\HeroesDS\3ds


javis , robimy z clodem silnik open world ktory strimuj edane swiata na przykladzie open morrowind na 3ds. Doradz cos , konwerter clouda Konwerter działa i sparsował wszystkie 82 modele NIF bez błędów (287 obiektów, 68 tys. trójkątów obiektów, 186 batchy). Teraz zabieram się za silnik na 3DS (citro3d)., ale 3ds nie udzwignie tyle, ale streamowac umie, ? gothic 3ds pokazal
MANIEK! 😁 Javis melduje się na mostku. I powiem Ci coś: to jest jeden z ciekawszych kierunków dla naszego zwykłego 3DS-a.

Moja rada: nie próbujcie przenosić całego OpenMW na 3DS. Zbudujcie własny, mały silnik open world, który wykorzystuje najważniejszą sztuczkę dużych silników: świat może być ogromny, ale konsola nigdy nie musi mieć go całego w pamięci.

I tak, 3DS potrafi pracować w takim modelu. Streaming świata jest wręcz koniecznością, jeśli chcemy uzyskać coś więcej niż małą planszę.

Jest jednak ważne rozróżnienie:

Streaming geometrii: doczytywanie modeli, tekstur i obiektów.

Streaming świata: doczytywanie komórek terenu, NPC, przedmiotów, kolizji i informacji o otoczeniu.

Streaming zasobów: zarządzanie pamięcią tak, żeby nie zabrakło jej po kilkunastu minutach gry.

To trzy osobne problemy. I warto od początku zaprojektować je razem.

1. Co naprawdę ogranicza zwykłego 3DS-a?
Nintendo 3DS / 3DS XL

Old 3DS
268 MHz
ARM11, dwa rdzenie

128 MB
FCRAM fizycznie

6 MB
VRAM

268 MHz
GPU PICA200

Parametry sprzętu według dokumentacji 3dbrew. Pamięć dostępna dla gry jest mniejsza niż fizyczne 128 MB. 
3dbrew
+1

Największy problem nie musi być liczba trójkątów.

Moim zdaniem największe zagrożenia to:

Liczba draw calli i zmian materiałów.

Pamięć na tekstury i bufory.

Koszt CPU związany z animacją, AI i kolizjami.

Nagłe doczytywanie danych, powodujące przycięcia.

Fragmentacja pamięci po wielokrotnym ładowaniu i zwalnianiu modeli.

Wasze 68 tysięcy trójkątów nie brzmi jeszcze jak wyrok. Natomiast 186 batchy to liczba, którą warto mocno zoptymalizować.

2. Zbudowałbym streaming podobny do OpenMW
OpenMW dzieli świat zewnętrzny na komórki. Każda zawiera fragment świata i obiekty. Silnik ładuje sąsiednie komórki, zamiast przetwarzać całą mapę naraz. Ma też mechanizmy object paging, które łączą wybrane obiekty w większe partie. 

OpenMW
+1

Screenshot 2024-10-10 at 10.49.08 PM.jpg | Tamriel Rebuilt

Proponuję jednak pójść krok dalej i zrobić trzy poziomy świata:

System trzech pierścieni
1. ACTIVE

Pełna jakość
1 × 1 komórka
Pełne modele, kolizje, NPC, AI, animacje, interakcje.

2. NEAR

Uproszczone
Pierścień sąsiadów
Uproszczone modele, bez pełnego AI, podstawowe kolizje i geometria.

3. FAR

LOD
Dalszy horyzont
Tanie modele, sylwetki budynków, uproszczony teren, bez symulacji NPC.

Ważne: obiekt nie musi być ładowany w całości. Drzewo widoczne z daleka może być pojedynczym uproszczonym modelem, a dopiero po zbliżeniu zamieniać się w pełne drzewo z kolizją.

3. Co zrobiłbym z konwerterem NIF?
Tutaj jest bardzo ważna decyzja architektoniczna.

Nie pozwoliłbym silnikowi 3DS czytać oryginalnych NIF-ów.

Konwerter Clouda powinien przygotować własny format binarny, zoptymalizowany pod konsolę.

Przykładowa struktura:

WORLD/
  world.index
  cells/
    cell_000_000.bin
    cell_000_001.bin
    cell_001_000.bin
  meshes/
    tree_01.mesh
    house_01.mesh
    rock_01.mesh
  textures/
    atlas_00.tex
    atlas_01.tex
Ważne elementy:

Wspólne modele przechowywane tylko raz.

Instancje obiektów zawierają transformację i identyfikator modelu.

Tekstury łączone w atlasy.

Geometria statyczna grupowana według materiału.

Osobne dane kolizji, najlepiej uproszczone.

LOD-y generowane podczas konwersji.

Indeks świata pozwalający znaleźć plik komórki bez przeszukiwania całego katalogu.

Dzięki temu 50 drzew tego samego typu nie oznacza 50 kopii geometrii w RAM.

4. Najważniejszy mechanizm: asynchroniczne doczytywanie
Wyobraź sobie, że gracz idzie przez las.

Silnik powinien działać mniej więcej tak:

Gracz zbliża się do granicy komórki.

Silnik sprawdza kierunek ruchu i przewiduje następną komórkę.

Loader czyta dane z SD.

Wczytuje plik komórki do bufora pośredniego.

Dekompresja i przygotowanie geometrii.

CPU przygotowuje bufory w tle, nie blokując renderowania.

Upload do GPU.

Geometria i tekstury trafiają do odpowiednich zasobów graficznych.

Aktywacja komórki.

Stary obszar pozostaje aktywny, dopóki nowy nie będzie gotowy.

Sprzątanie.

Niepotrzebne bufory są zwalniane, ale współdzielone modele pozostają w cache.

Jedna ważna rzecz: nie należy mylić streamingu z nieograniczonym doczytywaniem. Karta SD jest magazynem, nie dodatkowym RAM-em. Trzeba pilnować przepustowości, kosztu dekompresji i budżetu pamięci.

5. Co z Waszymi 287 obiektami i 68 tys. trójkątów?
Ja bym zrobił następujące optymalizacje:

Element

Proponowane rozwiązanie

68 tys. trójkątów

LOD i frustum culling

186 batchy

Material batching

Powtarzalne modele

GPU instancing, jeśli możliwe

Drzewa i skały

Impostory lub uproszczone LOD

Kolizje

Oddzielne low-poly collision meshes

Tekstury

Atlasy i kompresja

NPC

Pełne AI tylko w pobliżu

Nie wprowadzałbym od razu skomplikowanego systemu wszystkiego. Najpierw trzeba zmierzyć wydajność.

6. Moja propozycja kolejności prac
Rekomendowana kolejność
Renderer: jeden model NIF po konwersji, tekstura, kamera, oświetlenie.

Model cache: wielokrotne instancje jednego modelu.

Cell Manager: ładowanie i zwalnianie komórek.

Streaming: odczyt SD, dekompresja, buforowanie.

LOD: trzy poziomy szczegółowości.

Collision: poruszanie się po świecie.

World simulation: NPC, AI, przedmioty, skrypty.

Najpierw sprawdziłbym, czy silnik potrafi utrzymać płynne 30 FPS przy 3–5 aktywnych komórkach. Dopiero później zwiększałbym horyzont.

7. Wiadomość dla Clouda
Przygotowałem Ci najważniejsze założenia, które przekazałbym programiście budującemu silnik.

Old 3DS Open World Engine – Architecture Requirements
Target
Original Nintendo 3DS / 3DS XL (Old 3DS), using libctru and Citro3D.

We are building a lightweight open-world engine inspired by OpenMW's world streaming architecture.

The engine must prioritize memory efficiency and stable frame pacing over maximum visual quality.

Core architecture
Cell-based world streaming.

Asynchronous SD loading and decompression.

Separate CPU-side staging buffers and GPU resources.

Shared mesh and texture cache.

Object instancing to avoid duplicated geometry.

Three-level visibility and detail system:

ACTIVE: full geometry, collision, nearby NPC simulation.

NEAR: simplified geometry, limited simulation.

FAR: low-detail terrain and static silhouettes.

Frustum culling and distance culling.

Independent collision meshes.

Predictive loading based on player movement.

Safe unloading and memory reclamation.

Asset pipeline
The existing NIF converter successfully processes:

82 NIF models

287 objects

Approximately 68,000 triangles

186 batches

Do not parse original NIF files at runtime.

Extend the converter to generate a compact, optimized binary asset format.

The format should support:

Shared mesh resources

Object transforms and instances

Material identifiers

Texture atlases

Multiple LOD levels

Collision geometry

Cell-based asset indexing

Critical constraints
Target the original 3DS, not New 3DS.

Do not assume all 128 MB of physical FCRAM is available to the application.

Treat the 6 MB VRAM as a very limited resource.

Avoid runtime allocation spikes and memory fragmentation.

Do not preload the entire world.

Development strategy
Build and benchmark incrementally.

First demonstrate:

Rendering one converted NIF model.

Rendering multiple instances of that model.

Loading and unloading neighboring cells.

Streaming without blocking the main rendering loop.

Maintaining stable frame pacing during continuous movement.

Measure:

FPS and frame time

CPU time

RAM usage

VRAM usage

SD read throughput

Number of draw calls

Do not implement the entire architecture at once. Build measurable milestones and preserve a working build after each milestone.

8. Jeszcze jedna rzecz, którą bym rozważył
Steam Community :: The Elder Scrolls III: Morrowind

Morrowind: Дневник прохиндея

Screenshot of The Elder Scrolls III: Morrowind (Windows, 2002) - MobyGames

Mgła jako sprzymierzeniec silnika.

Morrowind ma fantastyczny klimat, który można wykorzystać technicznie.

Nie musisz renderować obiektów oddalonych o kilometr. Możesz:

ograniczyć widoczność do rozsądnego dystansu;

zastosować mgłę stopniowo zasłaniającą dalszy teren;

wyświetlać dalekie góry jako uproszczone sylwetki;

doczytywać szczegóły dopiero podczas zbliżania.

Gracz widzi rozległy świat, a konsola nie musi go w całości liczyć.

Moja najważniejsza rada, Maniek
Nie zaczynajcie od pytania: ile polygonów udźwignie 3DS?

Zacznijcie od pytania: ile świata możemy mieć pod kontrolą, nie tracąc płynności?

68 tysięcy trójkątów w całym zestawie testowym to nie jest jeszcze powód do kapitulacji. Znacznie ciekawsze będzie sprawdzenie, ile z nich faktycznie musi być widocznych w jednej klatce.

I chciałbym zaproponować jeszcze jedną rzecz: niech konwerter Clouda będzie nie tylko konwerterem modeli, ale docelowo World Compilerem. Dostaje dane Morrowinda, analizuje modele, instancje, materiały, komórki i generuje gotowy świat pod architekturę 3DS.

To pozwoli Wam przenieść ciężką pracę z konsoli na komputer.

Mały 3DS, wielki świat. Taki mamy plan. 😁



Komórki Morrowinda działają na 3DS: Seyda Neen renderuje się w emulatorze Azahar poprawnie (domy, drzewa, pomost, woda, teren, mgła). W emulatorze działają też 9 i 25 komórek, ale na prawdziwym 3DS przy 25 komórkach byłoby za wolno. Czasy GPU w Azaharze są nieprawdziwe (zawsze 0,25 ms), więc ile naprawdę uciągnie konsola, pokaże dopiero test na sprzęcie.

Co zrobiłem
Konwerter tools/mwbake.py: czyta Morrowind.esm, Morrowind.bsa i modele NIF, a zapisuje własny format do romfs/. Wszystkie 284 modele z obszaru 5×5 wczytały się bez błędów. Architekturę wziąłem z Gothic3DS: światło wypalone w kolorach wierzchołków, statyka scalona po materiale, jeden draw call na teksturę na ćwiartkę komórki.
Silnik source/main.c i world.v.pica: citro3d, wczytuje i zwalnia komórki wokół kamery, odrzuca obiekty poza polem widzenia, ma mgłę i statystyki na dolnym ekranie.
Budowanie: .\build.ps1 (z opcją -bake "--radius 0"). Wynik: mwcell3d.3dsx.
Tryb benchmarku: jeśli na karcie jest plik sdmc:/3ds/mwcell3d/bench.txt, aplikacja sama przechodzi przez promienie 0/1/2, patrzy w 8 kierunkach, zapisuje bench.log i zrzuty BMP, po czym się zamyka.
Wyniki (Azahar)
komórki	widoczne trójkąty	draw calle	CPU na klatkę	dane
promień 0	1	44–62 tys.	47–118	0,8–1,7 ms	2 MB
promień 1	9	140–163 tys.	218–273	3,3–4,0 ms	10,6 MB
promień 2	25	225–312 tys.	377–572	6–8,4 ms	25 MB
Wczytanie jednej komórki trwa 50–210 ms. Na razie blokuje to klatkę, więc przy przejściu między komórkami będzie przycięcie. Wg pomiaru z Gothic3DS GPU New 3DS potrzebuje około 5 ms + 0,25 ms na każde 1000 trójkątów. Szacunkowo wychodzi:

1 komórka: ok. 19 ms, czyli 30–50 fps.
3×3: ok. 40 ms, czyli ok. 25 fps.
5×5: około 12 fps, bez uproszczonych modeli (LOD) się nie da.
Prośba: wrzuć mwcell3d.3dsx na 3DS z plikiem bench.txt i odeślij mi bench.log. Dopiero wtedy będą prawdziwe czasy GPU. Bez bench.txt aplikacja startuje w trybie ręcznym. W Azaharze ten plik nadal leży w %APPDATA%\Azahar\sdmc\3ds\mwcell3d\, więc żeby sterować ręcznie, trzeba go usunąć.

Przepraszam za jedną wpadkę: pierwszy zrzut ekranu złapał Twoją przeglądarkę zamiast emulatora (od razu go usunąłem), a Azahar przejął część Twoich klawiszy. Od tamtej pory obraz zapisuje sama aplikacja, a benchmark ignoruje klawiaturę.

Co myślę o radach ChatGPT
Zgadzam się z:

Własny format zamiast NIF-ów na konsoli, z konwerterem jako „kompilatorem świata” na PC. Tak to już działa.
Mgła jako sprzymierzeniec, pierścienie szczegółowości (blisko / średnio / daleko), osobne uproszczone kolizje.
Mierzenie przed optymalizacją.
Asynchroniczne wczytywanie. To realnie najważniejszy brak: dziś wczytanie komórki blokuje klatkę na 50–210 ms.
Nie zgadzam się z:

„68 tys. trójkątów to nie problem, 186 batchy to problem”. Pomiary mówią odwrotnie. Rysuje się tylko 50–120 batchy, a CPU zajmuje to 1–2 ms. Ograniczeniem są trójkąty: 60 tys. to już około 15 ms samego GPU.
GPU instancing. PICA200 nie ma instancingu. Każda instancja to osobny draw call z własną macierzą, czyli dokładnie to, czego scalanie unika. Gothic3DS świadomie scala geometrię, a pamięci na to starcza (3×3 komórki to 10,6 MB).
Atlasy tekstur. Morrowind mocno powtarza tekstury na powierzchniach, a atlas to psuje. Lepsza jest kompresja ETC1 (jak w Gothic3DS), która zmniejszy tekstury 4 razy.
„6 MB VRAM to twardy limit”. Tekstury i geometria leżą w zwykłej pamięci (FCRAM), którą GPU czyta bezpośrednio. W VRAM są tylko bufory ekranu.
Cel: Old 3DS. Gothic3DS świadomie wspiera tylko New 3DS. Silnik sam dzieli pamięć na obu modelach, ale Old 3DS ma wolniejszy procesor i mniej pamięci, więc „ile świata” wyjdzie tam dużo mniej. Warto zdecydować, który model jest celem.
Proponowane następne kroki
Pomiar na prawdziwym 3DS.
Wczytywanie komórek w osobnym wątku.
Uproszczenie siatek przy wypiekaniu (jak --simplify w Gothic3DS) oraz pomijanie małych obiektów z daleka.
Kompresja ETC1.
Uproszczony teren dalekich komórek jako horyzont.

MANIEK... 😁

Przeczytałem całość i powiem Ci: Cloud zrobił już coś znacznie poważniejszego niż eksperyment z wyświetlaniem modeli. Macie działający prototyp silnika strumieniującego świat Morrowinda.

I co ważne, Cloud bardzo sensownie skorygował kilka moich wcześniejszych założeń. Szczególnie w kwestii PICA200, instancingu i liczby trójkątów.

Najbardziej podoba mi się jednak coś innego: macie już konkretne liczby, a nie dyskusję w stylu „wydaje mi się, że 3DS powinien dać radę”.
1. Najważniejszy wniosek z wyników
Działa w emulatorze
Promień	Komórki	Trójkąty	Dane
0	1	44–62 tys.	2 MB
1	9	140–163 tys.	10,6 MB
2	25	225–312 tys.	25 MB

Wzrost liczby trójkątów
Przybliżone wartości środkowe z pomiarów Azahar.


0 tys.70 tys.140 tys.210 tys.280 tys.1 komórka9 komórek25 komórek














Widzimy bardzo ważną rzecz:
- Jedna komórka to już sporo geometrii.
- Dziewięć komórek zwiększa obciążenie niemal trzykrotnie.
- Dwadzieścia pięć komórek to już zupełnie inna liga.
Moim zdaniem nie powinniście teraz próbować osiągnąć 5×5 pełnej geometrii. To byłoby walczenie z ograniczeniami sprzętu zamiast projektowania pod niego.
2. Jedna rzecz, którą szczególnie popieram: scalanie geometrii
Cloud ma rację z instancingiem.

Jeżeli 40 drzew ma ten sam model, ale różne położenie, zamiast wysyłać 40 osobnych poleceń rysowania, lepiej przygotować jedną większą siatkę.

I właśnie to robi Gothic3DS.

Warto jednak zachować rozróżnienie:
- statyczne obiekty można scalać;
- obiekty ruchome muszą pozostać osobne;
- drzwi, NPC, przedmioty interaktywne i animowane elementy wymagają osobnej obsługi.
To bardzo dobry fundament.
3. Co bym zmienił w następnej wersji silnika?
Mam pięć pomysłów.

A. Streaming etapowy

Obecnie wczytanie komórki blokuje klatkę na 50–210 ms.

Zrobiłbym tak:
- osobny wątek czyta plik z SD;
- osobny bufor przechowuje dane;
- dekompresja odbywa się porcjami;
- przygotowanie zasobów GPU odbywa się w głównej pętli, z ograniczonym budżetem na klatkę;
- dopiero gotowa geometria zostaje aktywowana.
Uwaga: osobny wątek nie gwarantuje dodatkowej mocy obliczeniowej na Old 3DS. Najważniejsza korzyść to rozłożenie pracy w czasie i uniknięcie zatrzymania renderowania.

B. Inteligentny streaming kierunkowy

Nie ładowałbym wszystkich sąsiadów jednakowo.








mobygames.com


Jeśli gracz biegnie na północ:
- północ: priorytet ładowania;
- północny zachód i północny wschód: wysoki priorytet;
- południe: niższy priorytet;
- obszary za plecami: można szybciej zwalniać.
To pozwala utrzymywać większy dystans widoczności bez ładowania całego pierścienia w jednakowej jakości.

C. Uproszczony horyzont

To mój ulubiony pomysł.

Dalsze komórki nie muszą mieć pełnej geometrii. Konwerter może generować:
- uproszczony teren;
- sylwetki budynków;
- uproszczone drzewa;
- odległe góry.
Dzięki temu można zobaczyć Balmorę z daleka, nie renderując każdego domu.

D. Budżet GPU

Wprowadziłbym limit trójkątów na klatkę, ale nie jako sztywną wartość na zawsze.

Silnik powinien mierzyć rzeczywisty czas GPU i dostosowywać dystans widoczności.

Przykładowo:
- GPU ma zapas: zwiększamy szczegółowość;
- GPU jest przeciążone: ograniczamy dalsze LOD-y;
- GPU nie wyrabia: chowamy drobne obiekty.
E. Kompresja tekstur

ETC1 brzmi sensownie, ale sprawdziłbym też koszt dekodowania i obsługę przez PICA200. Trzeba uważać na tekstury z kanałem alfa, ponieważ ETC1 go standardowo nie przechowuje.
4. Najważniejsza decyzja: Old 3DS czy New 3DS?










Old 3DS
Nasz sprzęt docelowy
Najtrudniejszy wariant, ale prawdziwy test możliwości projektu.











New 3DS
Wariant rozszerzony
Więcej możliwości CPU i pamięci, potencjalnie większy świat aktywny.


Ja bym zdecydował:

Silnik ma działać na Old 3DS, ale architektura ma pozwalać na wykorzystanie New 3DS.

Nie twórzcie dwóch silników. Zróbcie jeden z profilami sprzętowymi:
Parametr	Old 3DS	New 3DS
Aktywne komórki	mniejszy budżet	większy budżet
LOD	agresywniejszy	łagodniejszy
AI	ograniczone	więcej NPC
Dystans widoczności	krótszy	dłuższy
Streaming	ten sam system	ten sam system

5. Co teraz zrobiłbym na Waszym miejscu?
Nie kazałbym Cloudowi jeszcze optymalizować wszystkiego.

Najpierw potrzebujemy prawdziwego benchmarku na Twoim zwykłym 3DS-ie.

Interesują mnie szczególnie cztery rzeczy:
1. FPS przy promieniu 0.
2. FPS przy promieniu 1.
3. Rzeczywisty czas GPU.
4. Zużycie pamięci podczas przemieszczania się i doczytywania komórek.
I jeszcze jedna ważna uwaga: niech benchmark sprawdzi nie tylko statyczne patrzenie w osiem kierunków, ale również ciągły ruch przez granice komórek. To właśnie tam może wyjść najwięcej problemów.

Moja wiadomość do Clouda
Gdybym teraz prowadził ten projekt, wysłałbym mu takie polecenie:
Next Milestone: Old 3DS Hardware Validation
The current prototype is already rendering Morrowind exterior cells successfully in Azahar.

Do not expand the engine yet.

Prioritize validation on the original Old Nintendo 3DS.
1. Hardware profiling
Measure actual:
- GPU frame time
- CPU frame time
- FPS
- FCRAM usage
- Geometry upload time
- SD loading time
2. Streaming improvements
Design incremental cell loading:
- Background SD file reading
- Separate staging buffers
- Incremental decompression
- Bounded GPU resource upload per frame
- Predictive directional preloading
- Safe cell activation and unloading
Do not assume background threads automatically provide additional CPU performance.
3. Geometry optimization
Extend the existing world compiler:
- Static mesh simplification
- Distance-based object filtering
- Multiple LOD levels
- Simplified distant terrain
- Distant building silhouettes
Preserve the current material-based geometry batching.
4. Hardware profiles
Implement a shared engine with separate Old 3DS and New 3DS resource budgets.

Old 3DS must remain the primary compatibility target.
5. Acceptance criteria
Maintain stable frame pacing while continuously walking through multiple exterior cells.

Do not sacrifice nearby geometry quality to render distant objects unnecessarily.

Keep a working build after each milestone.




I jeszcze moje osobiste zdanie, Maniek: nie skreślałbym Old 3DS-a, zanim nie zobaczymy wyników na konsoli. Nie obiecywałbym też 30 FPS w 3×3.

Chciałbym zobaczyć Morrowinda na Twoim sprzęcie nawet przy 20–25 FPS, jeśli świat będzie płynnie doczytywany, a ruch nie będzie powodował zatrzymań.

Bo wyobraź sobie: odpalasz zwykłego 3DS-a, wychodzisz z Seyda Neen, idziesz w stronę Balmory, a za kolejnym wzgórzem pojawia się nowy fragment Vvardenfell.

To już nie jest demonstracja renderera. To jest zalążek przenośnego Morrowinda. 😁

Jak tylko będziesz miał bench.log z prawdziwej konsoli, przeanalizujemy wyniki i ustalimy, gdzie naprawdę jest wąskie gardło.
Przeanalizuj benchmark Old 3DS











Jasne! 🎮 Kiedy mam przypomnieć o analizie benchmarku Old 3DS? Możesz podać konkretny dzień i godzinę.


No i mamy to. Zwykły Old 3DS renderuje Morrowinda w 60 FPS. To już jest naprawdę konkretny sukces.

Co więcej, wyniki pokazują, że Cloud dobrze rozpoznał charakterystykę sprzętu. Nie trzeba uciekać od geometrii tak szybko, jak początkowo zakładaliśmy.

Moja odpowiedź dla Clouda: tak, zaczynajcie, ale zmieniłbym trochę kolejność i dodał jeden ważny warunek.
1. Najpierw profil 3×3 i dynamiczny zasięg
To jest moim zdaniem najlepszy kierunek.
Cel: stabilne 30 FPS
Niech silnik:
- trzyma 9 komórek w pamięci;
- renderuje pełną jakość blisko gracza;
- ogranicza odległe obiekty;
- wykorzystuje mgłę do maskowania granicy widoczności;
- dynamicznie reguluje zasięg renderowania.
Ale nie ustawiałbym sztywno 7500 jednostek. Zrobiłbym automatyczny regulator.

Przykładowo:
- 30 FPS stabilne: zwiększaj zasięg;
- poniżej 30 FPS: zmniejszaj;
- poniżej 25 FPS: mocniej ograniczaj szczegóły.
Ważne, żeby regulator nie zmieniał zasięgu co klatkę. Powinien reagować na uśredniony czas GPU, z histerezą.
2. Streaming w tle
Tutaj nie odpuszczałbym.

40–190 ms zatrzymania obrazu to coś, co zepsuje wrażenie otwartego świata.

Chciałbym, żeby:
- komórki były przewidywane na podstawie kierunku ruchu;
- odczyt odbywał się poza główną pętlą;
- nowa komórka pojawiała się dopiero po przygotowaniu;
- stara nie znikała przedwcześnie.
3. Większy obszar testowy
To bardzo ważne. Obecne 5×5 nie pozwala sprawdzić rzeczywistego streamingu.

Zrobiłbym trasę Seyda Neen → Pelagiad i testował:
- marsz;
- sprint;
- obracanie kamerą;
- zawracanie na granicy komórki;
- szybkie przemieszczanie się.
4. Jedna dodatkowa rzecz: niech Cloud nie optymalizuje tylko FPS
Poproś go o mierzenie również:
- liczby widocznych obiektów;
- liczby trójkątów;
- czasu GPU;
- czasu CPU;
- pamięci;
- czasu doczytywania;
- liczby klatek z przycięciem.
30 FPS ze sporadycznymi zatrzymaniami obrazu to nie to samo co 30 FPS z płynnym streamingiem.

Gotowa odpowiedź dla Clouda
Tak, zaczynajmy od tych trzech kroków.

Priorytet:
1. Old 3DS profile: 3×3 komórki w pamięci, dynamiczny zasięg renderowania, cel 30 FPS.
2. Asynchroniczny streaming komórek bez zatrzymywania głównej pętli.
3. Większy obszar testowy i rzeczywisty przejazd przez granice komórek.
Dodatkowe wymagania:
- Dynamiczny zasięg powinien mieć histerezę i bazować na uśrednionych pomiarach GPU, a nie reagować na każdą pojedynczą klatkę.
- Nie usuwaj starej komórki, dopóki nowa nie jest gotowa.
- Mierz osobno CPU, GPU, FPS, pamięć, liczbę trójkątów i czas doczytywania.
- Zachowaj możliwość ręcznego ustawiania zasięgu do benchmarków.
- Nie wprowadzaj jeszcze animacji szkieletowej ani rozbudowanego AI. Najpierw ustabilizuj streaming i renderer.
Zachowaj działającą wersję po każdym etapie.





A teraz moja wizja na przyszłość...






















9











Jeżeli uda się utrzymać 3×3 komórki, płynne doczytywanie i sensowny horyzont, możemy zacząć myśleć o czymś znacznie większym:
- NPC z własnymi harmonogramami;
- cykl dnia i nocy;
- podróżowanie między miastami;
- wnętrza budynków;
- zapis pozycji i stanu świata.





Tak, jak najbardziej! Technika, o której piszesz, nazywa się impostors (często też w szerszym kontekście określa się ją jako billboarding lub dynamic impostors).

Dla projektu silnika open-world na Nintendo 3DS jest to wręcz święty Graal optymalizacji, zwłaszcza biorąc pod uwagę specyfikę architektury tej konsoli.

Jak działają Impostory?
Zamiast renderować odległy teren, lasy czy budynki przy użyciu pełnej geometrii 3D (co drastycznie obciążałoby procesor i przepustowość wierzchołków), technika ta polega na:

Zastąpieniu 3D płaszczyzną (quadem): Odległy obiekt lub fragment terenu jest renderowany na prostym czworokącie złożonym z zaledwie 2 trójkątów.

Utrzymaniu orientacji: Czworokąt ten (sprite) jest stale obrócony w stronę kamery (gracza) – jest to tzw. billboarding.

Teksturze z widokiem: Tekstura naklejona na ten sprite przedstawia dany obiekt lub pasmo górskie widziane pod odpowiednim kątem.

Dlaczego to kluczowe dla Nintendo 3DS (Pica200)?
Układ graficzny 3DS-a (Pica200) ma swoje mocne strony (np. unikalny potok renderingu z obsługą makroinstrukcji), ale cierpi na tradycyjne ograniczenia konsol przenośnych z tamtej epoki:

Niski budżet wierzchołków (Vertex Count): Przetwarzanie tysięcy oddalonych drzew, skał czy pagórków w otwartym świecie szybko doprowadziłoby do spadku klatek. Impostory redukują liczbę wierzchołków do absolutnego minimum.

Wypełnienie ekranu (Fill-rate): Zbytnie nadpisywanie pikseli (overdraw) przy gęstej roślinności potrafi "zabić" wydajność. Odpowiednio zaprojektowany atlas impostorów pozwala to kontrolować.

Jak możesz to ugryźć w swoim silniku na 3DS-a?
Statyczne Impostory / Horyzonty: Dla bardzo odległych pasm górskich czy horyzontu możesz przygotować statyczną teksturę panoramy i owinąć ją wokół sfery horyzontu lub renderować jako prosty billboard w tle.

LOD (Level of Detail) dla obiektów i terenu:

Blisko gracza: Pełne 3D (siatka terenu, modele drzew).

Średnia odległość: Uproszczona siatka (Low-poly LOD).

Daleka odległość: Impostor (sprite 2D).

Octahedron Impostors (Wersja zaawansowana): Jeśli chcesz, aby sprite wyglądał trójwymiarowo i zmieniał oświetlenie/wygląd podczas kręcenia się kamery wokół obiektu, możesz wyrenderować obiekt z kilku perspektyw i zapisać je w jednej teksturze atlasowej, mapując je zależnie od kąta patrzenia kamery.

Projektowanie otwartego świata na tę platformę to fascynujące wyzwanie niskopoziomowe!