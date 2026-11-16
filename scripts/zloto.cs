// Złoto: po podniesieniu mówi, ile rzeczy jest w torbie.
class Zloto : ObjectScript {
    public override string Dla() { return "gold_*"; }

    public override void OnTake(int obj) {
        Game.Message("Złoto! W torbie: " + Game.Bag() + " (skrypt)");
    }
}
