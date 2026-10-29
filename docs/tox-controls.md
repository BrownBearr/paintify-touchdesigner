# Using the Paintify TOXs

Connect an image or video TOP, view Paintify's output, and open its custom
parameters. **Paintify** is the starting page. **Quick start / Help** opens a
guide embedded in the component, so a source checkout or Internet connection
is unnecessary.

Every original control and parameter name is retained. Existing expressions
and parameter references remain valid. Specialist controls are grouped by
task instead of collected on a single advanced page. Built-in component
parameters remain available through TouchDesigner's Show Custom Only toggle.

## Everyday controls

- **Live paint:** start or stop the renderer. Windows holds the last painted
  image when stopped; Mac bypasses at the input's original size.
- **Look / Brush family:** choose the painting style.
- **Performance preset:** apply the settings below without changing your look,
  material controls or Painted FPS.
- **Painted FPS:** how often a new painting is generated. This is independent
  of TouchDesigner's project frame rate.
- **Status:** current renderer state. Use **Setup → Restart renderer** to
  recover, and **Renderer log** to find diagnostic output.

| Preset | Windows | Mac |
| --- | --- | --- |
| Fast preview | 640-pixel working cap; 4 painting passes; no refinement | 25% width and height |
| Balanced | 1280-pixel working cap; 4 painting passes; no refinement | 50% width and height |
| Full quality | Native size; 8 painting passes; 4 refinement passes | Native size |
| Custom | Keep individual settings | Keep individual settings |

Resolution and refinement affect detail as well as speed. These presets are
explicit choices; upgrading the interface preserves existing parameter values.
Editing a preset-owned setting changes the label to Custom automatically.

## Advanced controls

**Performance** contains individual working-resolution and painting settings.
**Strokes** groups geometry and placement. **Surface** and the Mac material
pages control paint appearance. **Motion** groups temporal behavior, while
**Refinement** isolates expensive stroke-improvement settings. **Setup** holds
restart, logs, and the optional Windows executable override.

Windows numeric `-1` values and blank brush radii preserve automatic look or
renderer defaults. Alt-hover for contextual help. Numeric slider maxima are
suggested ranges; larger typed values and imported looks remain available.

The native parameter interface adds no browser, per-frame UI polling or
separate installation. Both optimized TOXs include the helper code and guide.
