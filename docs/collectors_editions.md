# Collector's Editions (in progress)

Notes on the Collector's Edition discs, before they get their own builds.

| Disc | `default.xex` | Media ID | SHA-1 | Entry point | Code |
|---|---|---|---|---|---|
| `Need for Speed - Carbon - Collector's Edition (USA) (En,Es)` | 0.0.0.7 | `1F8BEB88` | `0f685c38fc972972be36f3ce63e85e2e77f84a59` | `0x82944510` | moved against USA and Europe: needs its own manifest |
| `Need for Speed - Carbon - Collector's Edition (Europe)` | 0.0.0.11 | `5E74E60D` | `2020cfe3f0df44ded6cdd7c164542ab18d886ee2` | `0x82943460` | moved against all the others: needs its own manifest |

Same Title ID as the other discs (`454107EC`). The code is the same game with functions moved
(`tools/compare_images.py`: differs even with immediates and branch displacements ignored), so it
is made like France, Italy, Germany and Spain: `tools/make_edition_manifest.py --sources ...`, then
`tools/add_thunks.py`, then the repeated-code groups checked by hand.

Each Collector's Edition matches its region's normal disc up to about 0x821DF800 (CE USA against
USA, CE Europe against Europe, France and Germany) and differs after that: the same build with the
Collector's Edition content added, which moves the rest of the code. So CE USA is best made from USA
first, and CE Europe from Europe (pal2) and the other European discs.
