# Claude-fun — projectinstructies

## GitHub-workflow
- Na elke wijziging: commit naar de feature branch **en** merge direct naar `main`.
- Geen aparte vraag nodig; dit mag altijd automatisch.
- Feature branch: `claude/screen-presets-exe-t6esgm`

## Build
```sh
cd SchermPresets && ./build.sh
```
Vereist `mingw-w64` (`sudo apt install mingw-w64`).

## Ondertekening
Na het bouwen opnieuw ondertekenen:
```sh
cd SchermPresets/signing
osslsigncode sign -certs cert.pem -key key.pem \
  -n "SchermPresets" -i "https://github.com/kayvd046-prog/claude-fun" \
  -in ../dist/SchermPresets.exe -out /tmp/sp.exe && mv /tmp/sp.exe ../dist/SchermPresets.exe
```
De privésleutel (`key.pem`, `*.pfx`) staat **niet** in de repo (`.gitignore`).
