# Motion

How Rondo 2's screens move. The Y1's panel refreshes at **42.14 Hz** (a frame every 23.7 ms), so
motion is designed for that rate: every frame is computed for the moment it appears (its vsync), which
keeps steps even whatever a frame costs to draw. The engine draws a frame in about 1 ms, so every
refresh gets one.

Rondo 2 keeps every screen exactly as Rondo 1.0 lays it out: the same sizes, colors, text and
behavior. Motion is what changes: everything that moves, moves at the panel's full rate, and screen
changes slide.

## Principles

- **Answer at once.** A wheel pulse moves the selection in the very next frame; the bar and the list
  then glide there. Nothing waits for an animation to finish, and every motion can be retargeted.
- **Fast start, slow end.** Ease-out curves and springs: the eye looks where motion settles.
- **Fades and color carry most changes.** They look smooth at any rate. Big moves are kept for screen
  changes, where they explain where you are.
- **Sub-pixel while moving, whole pixels at rest.** Moving text and pictures glide; when they stop they
  land on exact pixels and are sharp.
- **Content is ready before it moves.** Covers come from the thumbnail cache; a picture that arrives
  while on screen fades in.

## The motions

| Motion | Duration | Curve | Notes |
| --- | --- | --- | --- |
| Selection bar and list scroll | ~110 ms to 95 % | critically damped spring (ω = 43) | Retargeted on every pulse; the list scrolls only when the selection would leave the view, as on the iPod |
| Row text under the bar | with the bar | follows how much of the row the bar covers | Text turns white as the bar passes, not all at once |
| Screen push (Center) | 250 ms | ease-out cubic | The new screen comes from the right and pushes the old one out to the left; both move together. Positions snap to quarter pixels |
| Screen pop (Menu) | 250 ms | ease-out cubic | The reverse |
| Status bar title | 250 ms | with the push or pop | The bar stays; the old title fades out as the new one fades in. Light and dark bars blend when moving between the shell and the YouTube sections |
| Now Playing song change | 180 ms | ease-in-out cubic | Cover, blurred backdrop and text cross-fade |
| A picture arriving | 150 ms | linear | Fades in over its placeholder |
| Keyboard highlight | ~110 ms | the same spring as the bar | Glides between keys; the key's label changes color under it |
| Lyrics | 450 ms slide, 200 ms fade | as in 1.0.x | |

## Wheel acceleration

As in 1.0.x (Settings › Wheel): pulses less than 154 ms apart in one direction are a spin. *Normal*
moves two rows a pulse after 12 pulses, *Fast* three after 6, *Off* always one. Volume and the
keyboard always take one step.
