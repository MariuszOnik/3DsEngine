// Wszystkie pojemniki (beczki, skrzynie, urny...). Każdy ma własną kopię skryptu, więc "razy" liczy przeszukania TEGO pojemnika.
class Beczka : ObjectScript {
    int razy;

    public override string Dla() { return "#pojemnik"; }

    public override void OnOpen(int obj) {
        razy++;
        Game.Message(Game.Name(obj) + ": przeszukana " + razy + " raz(y) (skrypt)");
    }
}
