// Wszystkie drzwi: za pierwszym razem gracz puka i nikt nie otwiera (OnActivate zwraca false = silnik nie
// otwiera), za drugim drzwi się otwierają. "zapukano" to stan tych jednych drzwi.
class Drzwi : ObjectScript {
    bool zapukano;

    public override string Dla() { return "#drzwi"; }

    public override bool OnActivate(int obj) {
        if (Game.IsOpen(obj) || zapukano) return true;
        zapukano = true;
        Game.Message("Pukasz... nikt nie otwiera. Spróbuj jeszcze raz (skrypt)");
        return false;
    }
}
