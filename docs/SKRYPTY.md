# Skrypty, zdarzenia i dialogi — szkic (2026-11-16)

Dotyczy kroku 6 z PLAN.md. Najpierw muszą być gotowe obiekty z FRMR, interakcja i zapis różnic,
bo skrypty i dialogi odwołują się do obiektów po trwałym ID i zmieniają stan świata.

## Podział odpowiedzialności

| Warstwa | Co | Gdzie |
|---|---|---|
| Silnik (C) | render, streaming, kolizja, animacje, AI ogólne (idź / wędruj / podążaj / walcz / uciekaj), reguły walki, ekwipunek, zapis, interpreter Lua, system zdarzeń, wybór kwestii dialogu | `source/` |
| Dane (OpenMW-CS → baker) | NPC, przedmioty, pakiety AI, harmonogramy, frakcje, dialogi z warunkami, questy (etapy dziennika), zmienne globalne | `romfs/*.bin` |
| Skrypty (Lua) | logika fabularna i jednorazowa: „gdy X, zrób Y” | `romfs/scripts/*.lua` |

Zasada: silnik daje **czasowniki**, dane i skrypty decydują, **kiedy i komu** ich użyć. Całą grę
da się zbudować bez kompilowania silnika.

## Stan na 2026-11-16: MiniSharp2 zamiast Lua (działa)

Zamiast Lua wybraliśmy własny język użytkownika, MiniSharp2 (`E:\MiniSharp2`, podobny do C#). Jego `lang/`
i niezależna od platformy część `lib/` są budowane prosto z tamtego repo (`MS2` w Makefile).
Kod: `source/script.c`, skrypty: `scripts/*.cs` (build.ps1 kopiuje je do romfs).

- **Skrypt = klasa pochodna `ObjectScript`.** Sama mówi, do czego należy (`Dla()`: `"barrel_*, crate_*"`,
  dokładne id albo `#drzwi` / `#przedmiot` / `#pojemnik`). Silnik znajduje takie klasy przez refleksję
  MiniSharp2, więc nie ma `Main` ani listy skryptów.
- **Każdy obiekt ma własną kopię skryptu**, tworzoną przy pierwszym zdarzeniu. Pola klasy to stan tych
  konkretnych drzwi (jak zmienne lokalne w Morrowindzie). `Obj` = numer obiektu.
- **Tylko zdarzenia**: `OnActivate` (`false` = silnik nie wykona domyślnej akcji), `OnTake`, `OnOpen`, `OnClose`.
  Co klatkę nic się nie dzieje, chyba że jakaś klasa ma `static void Update()`.
- `Game.*`: `Message`, `Log`, `Name`, `Id`, `Type`, `Find`, `FindNext`, `IsOpen/SetOpen`, `IsTaken/SetTaken`,
  `IsLocked/SetLocked`, `Bag`, `Time`, `Distance`; stałe `Game.Item/Door/Container`.
- **Bez budowania**: gra czyta najpierw `sdmc:/3ds/mwcell3d/scripts/*.cs` (wrzucone przez FTP), dopiero potem
  romfs. **L+X** w grze kompiluje skrypty od nowa (ok. 3 ms). Pola skryptów się zerują, stan świata zostaje.
- Błędy kompilacji i wykonania pokazują `plik.cs:linia` na dolnym ekranie i w play.log. Pętla dłuższa niż
  100 ms jest przerywana.
- Do zrobienia w MiniSharp2: atrybuty w refleksji (`[Dla("barrel_*")]` zamiast metody `Dla()`).
  Docelowo nazwa skryptu przyjdzie z pola „Script” obiektu w OpenMW-CS (rekord SCRI), przez bakera.

Poniżej pierwotny szkic (Lua). Zdarzenia i czasowniki zostają te same, zmienił się język.

## Skrypty: Lua oparte na zdarzeniach (szkic pierwotny)

Na Old 3DS (268 MHz) nie ma skryptów wykonywanych co klatkę jak w MWScript. Skrypt śpi, a budzi go
zdarzenie albo timer.

- Interpreter: Lua 5.4 (ok. 200 KB kodu), jedna maszyna (`lua_State`) na całą grę, własny alokator
  z limitem pamięci (np. 1 MB), limit instrukcji na wywołanie (hook `lua_sethook`), żeby zapętlony
  skrypt nie zawiesił gry.
- Skrypty przypięte do **rekordu bazowego** (wszystkie kopie obiektu) albo do **konkretnego ID**.
- Budżet: skrypty łącznie ≤ 1 ms CPU na klatkę, mierzone w play.log.
- Stan skryptu (`self.state`) to tabela prostych wartości zapisywana w porcji zapisu gry. Nie
  zapisujemy funkcji ani referencji do obiektów C, tylko ID.

### Zdarzenia (silnik → skrypt)

```
onActivate(self, actor)        -- gracz nacisnął A na obiekcie; return false = zablokuj domyślne
onPickup(self, actor) / onDrop
onOpen(self, actor) / onClose  -- drzwi, skrzynie
onLockpick(self, actor, ok)
onEnterCell(cell) / onLeaveCell(cell)     -- skrypty globalne
onTrigger(self, actor, enter)  -- strefa (pudełko z edytora)
onHit(self, attacker, dmg) / onDeath(self, killer)
onCombatStart(self, enemy) / onCombatEnd(self, won)
onDialogue(self, topic, infoId)           -- po wypowiedzeniu kwestii
onChoice(self, n)                         -- gracz wybrał odpowiedź
onJournal(quest, stage)
onTimer(self, name)
onHour(hour)                              -- zegar świata, skrypty globalne
onLoad(self)                              -- po wczytaniu zapisu (odtwórz efekty)
```

### Czasowniki (skrypt → silnik)

```
-- świat i obiekty
obj = world.get("id_obiektu")             -- po ID z edytora; działa też dla niewczytanych komórek
obj:enable() / obj:disable()              -- pokaż / ukryj (np. przedmiot po scenie)
obj:moveTo(cell, x, y, z, rot)
obj:setLocked(level) / obj:isLocked()
obj:addItem(item, n) / obj:removeItem(item, n) / obj:itemCount(item)
world.spawn(base, cell, x, y, z) -> obj   -- nowy obiekt z trwałym ID w zapisie
world.time() / world.setTime(h)
world.global("zmienna") / world.setGlobal("zmienna", v)

-- NPC i AI (zachowania są w C, skrypt tylko je przełącza)
npc:aiWander(radius) / npc:aiTravel(x, y, z) / npc:aiFollow(target) / npc:aiStay()
npc:startCombat(target) / npc:stopCombat()
npc:say(soundOrText) / npc:playAnim(name)
npc:disposition() / npc:modDisposition(d)
npc:setSchedule(name)                     -- harmonogram dnia z danych

-- gracz, questy, UI
player:addGold(n) / player:gold()
journal.set(quest, stage) / journal.stage(quest)
ui.message("tekst") / ui.choice({"Tak", "Nie"})   -- wynik przychodzi w onChoice
timer.after(self, seconds, name)
sound.play(name, obj)
```

Przykład:

```lua
-- skrzynia_kapitana.lua
function onOpen(self, actor)
  if journal.stage("q_kapitan") < 20 then
    ui.message("Skrzynia jest zamknięta na klucz kapitana.")
    return false
  end
end

function onPickup(self, actor)               -- przypięte do miecza kapitana
  local cap = world.get("kapitan_hrundi")
  cap:startCombat(actor)
  journal.set("q_kapitan", 30)
end
```

## Dialogi

### Model z Morrowinda / OpenMW-CS (bierzemy go w całości)

Edytor już to obsługuje, więc nie trzeba pisać własnego edytora dialogów.

- **Temat** (DIAL): słowo kluczowe, np. „praca”, „Seyda Neen”, „plotki”. Specjalne tematy:
  Greeting (powitanie), Journal (wpisy questów), Persuasion, Voice (okrzyki w walce).
- **Kwestia** (INFO): odpowiedź na temat z listą **warunków**. Silnik bierze **pierwszą kwestię
  od góry**, której wszystkie warunki są spełnione.
- Warunki: ID mówiącego, rasa, klasa, frakcja i ranga, płeć, komórka, nastawienie (disposition),
  etap questa, zmienna globalna lub lokalna, przedmiot u gracza, statystyki gracza, porę dnia.
- Wynik (result): w Morrowindzie mały MWScript. U nas **wywołanie Lua** (`onDialogue`) albo krótki
  fragment Lua zapisany w polu wyniku.
- Słowa w tekście, które są nazwami innych tematów, stają się linkami i dopisują temat do listy
  gracza.
- `Choice "Tak" 1 "Nie" 2` w wyniku daje odpowiedzi do wyboru. Następna kwestia ma warunek „wybór = 1”.
  Tak buduje się rozgałęzione rozmowy w systemie tematów.

### Baker

- `romfs/dialog/index.bin`: tematy i dla każdego lista kwestii: warunki w zwartej postaci
  (typ u8, operator u8, argument u16/u32, wartość f32), offset tekstu, ID skryptu wyniku.
- `romfs/dialog/text.bin`: teksty w UTF-8, czytane z SD **na żądanie** (jedna kwestia naraz), nie
  trzymane w RAM. W RAM zostaje tylko indeks.
- Kolejność kwestii z edytora (PNAM/NNAM) zachowana, bo decyduje o tym, która wygrywa.
- Ile pamięci zajmie indeks dla pełnego Morrowinda: do zmierzenia w bakerze. Dla własnej gry
  będzie dużo mniej.

### Gra

- A przy NPC → wybór powitania (Greeting) → okno dialogu.
- **Górny ekran**: twarz / postać NPC i jego wypowiedź (duży czytelny tekst).
- **Dolny ekran (dotyk)**: lista tematów, przewijana palcem; odpowiedzi z Choice jako duże
  przyciski; „Do widzenia”. Pasuje do istniejącego okna w stylu Morrowinda (`source/ui.c`).
- Ewaluacja warunków w C (szybka, po kilkadziesiąt kwestii na temat), bez Lua.
- Polskie znaki: czcionka z pełnym zestawem Latin Extended (sprawdzić systemową czcionkę 3DS
  w citro2d; jeśli brakuje, własny atlas z uibake).
- Nastawienie (disposition) i perswazja: później, jako zwykłe warunki i czasowniki.

### Questy

- Quest = temat typu Journal z etapami (10, 20, 30…), każdy z tekstem do dziennika i flagą
  „zakończony”.
- Etap zmieniają: wynik kwestii, skrypt (`journal.set`), zdarzenie (śmierć, podniesienie).
- Dziennik na dolnym ekranie: aktywne / zakończone, wpisy z datą gry.
- W zapisie: tylko `quest → etap` (mała porcja).

## Kolejność wdrażania

1. Baker: czytanie DIAL/INFO/warunków do `dialog/*.bin`, test na Morrowind.esm (liczby, rozmiar).
2. Okno dialogu: powitanie + tematy + teksty, warunki bez skryptów (ID, frakcja, quest, zmienne).
3. Dziennik i questy.
4. Lua: interpreter, limity, zdarzenia onActivate / onDialogue / onDeath, czasowniki z listy.
5. Choice i rozgałęzione rozmowy.
6. AI z harmonogramem dnia (pakiety z danych + `onHour`).
