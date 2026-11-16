# body_detail: the women's detailed bust / seat (prototype, Nov 15)

The game builds these at boot with a C++ port of `transfer3` + `build2` (src/kamite/body_detail_build.cpp, checked
triangle for triangle against this code); this folder stays the place to try changes offline first.

Offline prototypes for the Money > 100% bust and Cash > 100% seat (claude/female-models.md). Run with
`GODHAND_EXTRACT=<your extract>`; numpy + Pillow.

- `xfer.py`: `load_node` (a model's nodes as flat vertex records + triangles), `subdivide` (red-green, shared midpoints
  across vertex buffers), `transfer2` (the bust: subdivide the bone-29 region once and shrinkwrap it along smoothed
  normals onto the donor's bust, box-mapped to her size; border held, displacement smoothed). Donor: Olivia's bunny
  suit, el/elf4.dat (her bust is the generic women's size; the dress el0a compresses it).
- `transplant.py`: the seat (`build`): cut the window (any vertex in x +-0.15, y 0.88-1.08, z < -0.04), select the
  donor's triangles inside the hole's outline (back side), box-map them to the cut, join the two boundary loops (first
  a stitch of shortest diagonals; since round 4 one shared edge, below), and give the new vertices the woman's UVs / colours / weights / vertex buffer by closest point on
  her cut surface (the cut lies in one UV island on em16 / em0f). `falloff=0.04` (use this, with `shrink=1.0`): the
  patch is blended onto her own surface over 4 cm from its edge, so the join has no step to bridge (the ledge between
  the lower back and the seat, K Nov 15). Donor: Shannon, el/el15.dat body_u1. Shrinkwrapping
  the seat failed (no edges where the crease and the under-curve go).
- `cash.py`: Cash's growth above 101% on the detailed seat (`growth(P, T, N)` -> the move at 130% and the normals
  there, `at(...)` for a Cash value): a function of position only, along K's side-profile reference (Nov 15); the C++
  (`cashGrowth` in body_detail_build.cpp) matches it to 0.001 mm. Not on em07 (her dress is too coarse). Shannon and
  Olivia (em52 / ema0 and their el/ cutscene models) keep their models: `growOnly` grows every node the field reaches;
  `grow_layers` (= GrowRules in C++) for clothing: a skirt held from the fullest point, the body under it kept inside it,
  a belt plate pushed out over it (Olivia; matched to 0.001 mm).
- `texr.py`: textured offline raster (per-pixel UV, Gouraud from vertex normals when the object has `N`).

**Bust recipe:** `transfer3(woman, bunny, levels=1)` with its defaults `back=(0.02, 0.05), anchor=0.8` (Nov 15 fix: pushed
along the tilted normals the cleavage's centre vertices met in a ledge; the 0.5 smoothing anchor left the centre 1.5 cm
proud of the donor).

**Nov 15, second in-game round:** transfer3 subdivides the triangles touching the centre line once more (`band=0.004`,
not at the region's border), holds each centre-line vertex at least as deep as its neighbours' mean, and ends with
`crease()` (the centre records split into left / right copies with their own wall normals). build2 deepens Shannon's
patch per height to her own seat's depth plus 3 mm (`depth_match`; em0d's seat is 1-2 cm deeper than Shannon's) and
blends the new vertices' normals from hers at the patch edge to the patch's over the falloff (the ring keeps hers).

**Current seat recipe (K's round 3, Nov 15): `build2(m, shannon, (-0.15, 0.88, -1), (0.15, 1.16, -0.04),
falloff=0.05, mapping='own')` with `CUT_MIN = 1`.** Shannon's seat at her own size (only her hip width matched): the
box-mapped version squeezed her depth into the generic seat's boxy plateau and came out up to 1.1 cm smaller in the
upper half (K: "too small"); at her own size the new seat is 0 to +0.9 cm fuller across the cheek (Cash at 101% never
smaller than the original). The cut stays inside the costume piece holding the seat (`one_vb`), so it ends on the
piece's own seam (crossing it made a sawtooth waistband). `build2` handles cuts across several vertex buffers (each new
triangle takes the buffer under it). `prof.py`-style side profiles (the rearmost surface per height, x 0.03-0.10) are
how the size was checked.

**Round 4 (Nov 15): one shared edge, no stitch.** The stitch's slivers showed as dots along the waistband from low
angles (culled where they folded), and its crotch triangles took a hole vertex's first record, which sits across the
leg's UV seam (the texture squeezed across the inner thigh). Now: the patch loses its ears (`no_ears`), its edge
vertices go to their closest points on the hole's polyline in loop order (`join_edge`: least-squares monotonic, runs
within 1 mm merged, every hole vertex onto its nearest patch vertex where that keeps the order), and each side's edge
triangles are split at the other side's remaining vertices (`fan`; her side's new records lerped along the edge). The
patch keeps Shannon's own winding (consistent with hers across the edge; no re-winding), a fan folded by a hole corner
gets its inner edge flipped, and a hole vertex's record comes from the cut triangles. Checks: no new open edges, no
T-junctions, no edge wound the same way by both its triangles, no new seat triangle with a UV span over 1200 of 4096.
