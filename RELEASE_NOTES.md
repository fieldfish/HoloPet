# HoloPet v0.9.0 Public Preview

This is the first repository-oriented public preview. It combines the current software line with the latest compact mechanical design and removes private delivery artifacts.

## Included

- C++20 device runtime and Python conversation service;
- nine-expression renderer and idle/thinking/speaking animation;
- square EC11 menu with clock, timer, alarm, note and model-mode functions;
- cloud and local model routing, tool confirmation and bounded memory;
- compact base V2 OpenSCAD source, print-ready STL files and assembly previews;
- build scripts, systemd templates, tests and public documentation.

## Not included

- API credentials or production environment files;
- recordings, transcripts, databases, logs or hardware evidence;
- model weights, third-party fonts or proprietary SDKs;
- historical task documents, delivery receipts and internal packaging scripts;
- the user's physical reference photograph.

The public snapshot also changes transcript logging to length plus a short SHA-256 digest; plaintext transcripts are no longer written to the default runtime log.

## Known limits

- the latest multi-step Agent scenarios still require a fresh Raspberry Pi verification;
- compact base V2 has passed CAD and mesh checks, but physical fit and optical performance remain waiting for first-article assembly;
- provider availability, model identifiers and commercial terms may change independently of this repository.

## Public snapshot verification

- clean Windows core build: 46/46 C++ tests passed;
- clean Windows AI/SDL build: 50/50 C++ tests passed;
- Python service: 312 tests collected, 290 passed, 22 platform-specific tests skipped, no failures or errors;
- all 10 supplied STL meshes are watertight with consistent winding;
- public-tree checks found no bundled credentials, private keys, raw audio, model weights, local paths, work logs or acceptance evidence.
