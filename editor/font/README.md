# Czcionka edytora

`uifont.ttf`: **Noto Sans Regular** (The Noto Project Authors, https://github.com/notofonts/latin-greek-cyrillic),
licencja SIL Open Font License 1.1 (`OFL.txt`). Okrojona do znaków, których używa interfejs: łacina, w tym polskie
litery, plus kilka symboli (Basic Latin, Latin-1, Latin Extended-A, cudzysłowy, myślniki, strzałki, ▲▼):

    pyftsubset NotoSans-Regular.ttf --unicodes="U+0020-007E,U+00A0-00FF,U+0100-017F,U+2013-2014,U+2018-201E,U+2022,U+2026,U+2190-2193,U+25B2,U+25BC" --output-file=uifont.ttf --no-hinting

`Makefile.edit` robi z niej przy budowaniu czcionkę 3DS (`mkbcfnt -s 13`, ok. 0,5 MB) i wkłada ją do programu. Rozmiar 13 jest blisko tego, w jakim UI faktycznie rysuje
tekst; przy 22 glify były mocno pomniejszane i wychodziły cienkie i rozmyte. Dzięki temu
edytor nie potrzebuje czcionki systemowej konsoli (Azahar jej nie ma bez plików z prawdziwej konsoli) i ma polskie
znaki.
