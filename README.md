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
annex install sicht-math
annex list
```

## Build / Compile
```bash
sicht compile --exe app.si
```
