# gma500 patches not applied yet

CI and the debs apply only `drm/gma500/*.patch`; these wait here.

- `0006-drm-gma500-Don-t-lose-a-page-flip-s-completion-event.patch`: complete a
  page flip's event when no vblank interrupt will come (the pipe off at the flip,
  or DPMS off mid-flip). Compile- and checkpatch-clean, never run on Cedarview
  hardware, so it stays out of the testers' install. It goes back into
  `drm/gma500/` once a tester round has run it. It was made on a tree with the
  kit patches applied (`drm/ci/kit/`), so check that it applies on 0001-0005
  alone before moving it back.
- `0007-drm-gma500-Count-Cedarview-vblanks-in-FRAMEHIGH-alone.patch`: on Cedarview
  the vblank count is FRAMEHIGH alone (16 bits, `max_vblank_count` 0xffff); the
  FRAMEHIGH << 8 | frame-byte count of the other chips advanced 256 a vblank there,
  so OML_sync_control MSC pacing saw every frame 255 vblanks late. Reported with the
  register reads on an Atom N2600 (issue #5). Built and checkpatch-clean in CI, not
  yet run on Cedarview hardware.
