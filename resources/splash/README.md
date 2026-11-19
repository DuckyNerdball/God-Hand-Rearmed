# Splash text

The launcher shows one line of `splash.txt`, picked at random each time it starts, tilted over the wordmark's bottom
right corner (Minecraft's splash text). One splash per line; empty lines are skipped; any length (long lines are drawn
smaller). Edit the copy next to GHR.exe (`resources/splash/splash.txt`) to try lines out; the repo's copy is synced
from K's own list.

`splash.fnt` + `splash.png`: Dominican by Harold Lohner (haroldsfonts.com) baked as a glyph atlas with
`tools/splash/bake_font.py` (free for any use; the raw .ttf is not redistributed, so it is not in the repo). Glyphs:
ASCII, Latin-1 and common punctuation, as far as the font has them. The launcher's saloon writes with it too: the bill
of fare (raylib) and the Options book (ImGui), EB Garamond filling in what it lacks (# % < > { } |, the ellipsis, ™);
the splash text draws anything missing as `?`. Baked from K's DOMINICA.TTF (his Claude\Fonts folder) on Nov 19.
