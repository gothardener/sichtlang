[![Latest Release](https://img.shields.io/github/v/release/Gothardener/sichtlang?label=latest%20release)](https://github.com/Gothardener/sichtlang/releases)
[![Downloads](https://img.shields.io/github/downloads/Gothardener/sichtlang/total?label=downloads)](https://github.com/Gothardener/sichtlang/releases)
[![License](https://img.shields.io/github/license/Gothardener/sichtlang)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20|%20Linux-blue)](https://github.com/Gothardener/sichtlang/releases)

# Sicht

Sicht is an English‑style programming language with a native compiler and a built‑in package manager (Annex).  
It’s designed to be readable, fast, and practical for real projects.

## Its features
- Simple English‑style syntax
- Interpreter + native compile (`sicht compile`)
- Annex package manager included
- Windows installer + Linux `.deb` releases
- Low‑level mode (`llvl`) when needed

## Install

**Windows**  
Download the installer from Releases and run it.

**Linux (Debian/Ubuntu/Mint)**  
Download the .deb package from releases, then
```bash
sudo dpkg -i sicht_1.0.0_amd64.deb
source /etc/profile.d/sicht.sh
```

## Quick Start
```sicht
start
  print "Hello Sicht"
end
```

Run:
```bash
sicht run hello.si
```

## Annex (Package Manager)
```bash
annex search math
annex install math
annex list
```

## Build / Compile
```bash
sicht compile --exe app.si
```
