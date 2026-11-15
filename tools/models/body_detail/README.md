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

**Current seat recipe (K's round 3, Nov 15): `build2(m, shannon, (-0.15, 0.88, -1), (0.15, 1.16, -0.04),
falloff=0.05, mapping='own')` with `CUT_MIN = 1`.** Shannon's seat at her own size (only her hip width matched): the
box-mapped version squeezed her depth into the generic seat's boxy plateau and came out up to 1.1 cm smaller in the
upper half (K: "too small"); at her own size the new seat is 0 to +0.9 cm fuller across the cheek (Cash at 101% never
smaller than the original). The cut stays inside the costume piece holding the seat (`one_vb`), so it ends on the
piece's own seam (crossing it made a sawtooth waistband). The patch's edge is pinned 85% onto the hole's boundary and
the stitch is left out of the shading normals (two edge-on slivers at the top corners darkened the shading). `build2`
handles cuts across several vertex buffers (each new triangle takes the buffer under it). `prof.py`-style side
profiles (the rearmost surface per height, x 0.03-0.10) are how the size was checked.
