Checks for mot_edit's motion edits (punish_fit.cpp rewrite(): Gene's pose pre-rotations, the left forearm twist).
apply_fit.cpp applies them exactly as the runtime does (same matrices and order) to a motion blob, per-frame values from
a file. Build: g++ -std=c++20 -O1 -I../../../../src apply_fit.cpp ../../../../src/kamite/mot_edit.cpp -o apply_fit
Run from a scratch directory (they write src.bin / out.bin / vals.txt there), GODHAND_EXTRACT set:
- chk2.py <body hash> <punish>: per motion and bone, the worst error at any half frame (60 FPS) against the intended
  rotation (pre * own * post). Nov 8: <= ~1.4 deg after the half-frame slopes; a branch flip had made 45 deg (Spanking's
  grab, the twitch K saw).
- reg.py: every fit's edited motions, the half-frame wobble beyond the original's own (the originals wobble up to
  ~30 deg at half frames themselves). Nov 8: <= 0.4 deg.
- chest_check.py <body hash> <scale> <money> [victim motion] (Nov 15, the women's chest grab Suplex): the fit's pose
  lines (Money blended) and the fit's own pose applied as rewrite() does (apply_pose.cpp: per-frame pre-rotations in the
  file's order; build like apply_fit), each bone against the intended rotation at every frame and half frame. Nov 15:
  <= 0.9 deg at frames, <= 4 deg beyond the original's own half-frame drift (a free forearm sat near Euler gimbal lock
  and wobbled 13 deg; a flipping elbow hinge 180).
- hold_check.py <body file> <fit line (1-based, the fit's header in punish_fit.txt)> <money> [channels] [-v] (Nov 15, K: the
  chest grab's hands still wobbled): his hand points in her chest frame (bone 2) through the hold, as the runtime plays
  the pair (pose lines in float, Gene's / her other edits from copies baked by bake.cpp = rewrite()'s addCurve /
  preRotate every frame) against everything in float; `channels` puts Gene's hips (g0) / torso (g1), her hips (v0) /
  bend (v1) in float too. Before 1cad407: up to 1 cm at frames, 2 cm at half frames; `all` (what 1cad407 plays): 0.
  Build bake like apply_fit.
