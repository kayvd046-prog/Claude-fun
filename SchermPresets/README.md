# SchermPresets

Een klein Windows-programma (één `.exe`, geen installatie nodig) waarmee je je
beeldscherminstellingen opslaat als presets en er met één klik tussen wisselt.

## Wat het doet

- **Preset opslaan** — slaat je volledige huidige schermconfiguratie op:
  welke schermen aan of uit staan, uitbreiden of dupliceren, resolutie,
  positie en verversingssnelheid.
- **Preset toepassen** — selecteer een preset en klik *Toepassen*
  (of dubbelklik op de naam) om die indeling direct terug te zetten.
- **Snel schakelen** — knoppen voor *Uitbreiden*, *Dupliceren*,
  *Alleen scherm 1* en *Alleen scherm 2*, net als Win+P.

## Gebruik

1. Download `dist/SchermPresets.exe` en start het (geen installatie nodig).
2. Stel je schermen in zoals je ze wilt via Windows-instellingen
   (bijv. scherm 3 uitschakelen, scherm 1+2 uitbreiden).
3. Typ een naam (bijv. "Gamen" of "Werk") en klik **Huidige indeling opslaan**.
4. Herhaal dit voor elke indeling die je wilt bewaren.
5. Wissel daarna op elk moment van indeling door een preset te selecteren en
   op **Toepassen** te klikken.

Presets worden opgeslagen in `%APPDATA%\SchermPresets`.

> **Let op:** Windows SmartScreen kan bij de eerste start een waarschuwing
> tonen omdat de exe niet digitaal ondertekend is. Kies dan
> *Meer informatie → Toch uitvoeren*.

## Hoe het werkt

Het programma gebruikt de Windows CCD API (`QueryDisplayConfig` /
`SetDisplayConfig`) — dezelfde API die Windows zelf gebruikt voor
beeldscherminstellingen. Omdat adapter-ID's na een herstart kunnen
veranderen, slaat het programma ook de apparaatpaden van de videoadapters op
en vertaalt het oude ID's automatisch naar de huidige bij het toepassen.

## Zelf bouwen

Op Linux (cross-compile) of WSL:

```sh
sudo apt install mingw-w64
./build.sh
```

Het resultaat staat in `dist/SchermPresets.exe`.
