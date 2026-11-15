# body_detail: the women's detailed bust / seat (prototype, Nov 15)

Offline prototypes for the Money > 100% bust and Cash > 100% seat (claude/female-models.md). Run with
`GODHAND_EXTRACT=<your extract>`; numpy + Pillow.

- `xfer.py`: `load_node` (a model's nodes as flat vertex records + triangles), `subdivide` (red-green, shared midpoints
  across vertex buffers), `transfer2` (the bust: subdivide the bone-29 region once and shrinkwrap it along smoothed
  normals onto the donor's bust, box-mapped to her size; border held, displacement smoothed). Donor: Olivia's bunny
  suit, el/elf4.dat (her bust is the generic women's size; the dress el0a compresses it).
- `transplant.py`: the seat (`build`): cut the window (any vertex in x +-0.15, y 0.88-1.08, z < -0.04), select the
  donor's triangles inside the hole's outline (back side), box-map them to the cut, stitch the two boundary loops with
  shortest diagonals, and give the new vertices the woman's UVs / colours / weights / vertex buffer by closest point on
  her cut surface (the cut lies in one UV island on em16 / em0f). `falloff=0.04` (use this, with `shrink=1.0`): the
  patch is blended onto her own surface over 4 cm from its edge, so the stitch has no step to bridge (the ledge between
  the lower back and the seat, K Nov 15). Donor: Shannon, el/el15.dat body_u1. Shrinkwrapping
  the seat failed (no edges where the crease and the under-curve go).
- `texr.py`: textured offline raster (per-pixel UV, Gouraud from vertex normals when the object has `N`).
