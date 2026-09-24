# macOS TouchDesigner feasibility

Status: researched, not implemented or tested on macOS. The current `Paintify.tox` is a Windows package.

## Conclusion

A macOS component with the same painting controls and algorithms is technically feasible on a supported Mac, especially Apple Silicon. It requires a native GPU renderer and a Syphon video bridge. The present Windows `.exe`, DLLs, OpenGL 4.6 shaders, and Spout bridge cannot be relabeled as a Mac `.tox`. Visual and performance parity remain unproven until the port is built and compared on Mac hardware.

TouchDesigner itself supports macOS and provides Syphon Spout In/Out TOPs: Spout is the Windows route; Syphon is the macOS route. Apple documents Metal compute and GPU-generated indirect draw commands, which cover the core classes of work used by Paintify. These APIs make a port plausible; they do not automatically translate this project's shaders or guarantee identical output.

## Current code versus a Mac port

| Feature | Current Windows component | macOS work needed |
| --- | --- | --- |
| TOP input and output | Spout sender/receiver in `src/live_spout.cpp` | SyphonMetalClient and SyphonMetalServer, with unique sender names per component |
| Painting presets and Look dropdown | Python parameters in `touchdesigner/install_paintify.py` plus shared renderer parameters | Preserve the same menu and parameter names; feed the Mac renderer the same values |
| Advanced controls and live changes | Settings file consumed by the running Windows renderer | Reuse the control protocol and implement its application in the Mac renderer without process restarts |
| Stroke placement, tracing, relaxation, optical flow, texture, impasto | OpenGL 4.6 compute shaders in `shaders/` and `src/pipeline.cpp` | Port shaders and resource/synchronization logic to Metal; compare output and timing for every look |
| GPU-generated stroke draw | OpenGL indirect draw in `src/pipeline.cpp` | Metal indirect draw/command buffer equivalent; check device feature support at runtime |
| Single drag-and-drop TOX | Embedded Windows `.exe`, DLLs, GLSL shaders | Embed a signed/notarized Mac executable, libraries/frameworks, and Metal shader assets; use a Mac cache path and extraction permissions |

The existing renderer requests an OpenGL 4.6 context in `src/main.cpp`, uses GLSL 4.60 compute shaders, and routes live I/O through a Windows-only Spout implementation. Apple supports OpenGL only through 4.1 on macOS and recommends Metal. TouchDesigner's own Vulkan/MoltenVK renderer does not make the separate `gpu-sbr` executable's OpenGL 4.6 context available on Mac.

Syphon's Metal API can receive and publish `MTLTexture` frames. TouchDesigner's Syphon Spout Out TOP documents an 8-bit RGBA Syphon limit, so a high-precision TouchDesigner input would be quantized at the interchange boundary. Paintify's internal Metal pipeline could still use float/half-float textures. Confirm color space, alpha, vertical orientation, resize/reconnect behavior, and frame timing in a Mac prototype.

## Recommended implementation sequence

1. Target an Apple Silicon Mac supported by TouchDesigner 2025 (macOS 13 or newer). Keep Intel/AMD as a later validation target. Derivative recommends Apple Silicon and requires a discrete AMD GPU for supported Intel Macs.
2. Build a minimal native Metal process that receives one Syphon frame and publishes it unchanged. Verify `Video Device In TOP -> Syphon Spout Out TOP -> process -> Syphon Spout In TOP -> out1`, including reconnect and resize.
3. Port the 13 compute shaders and stroke draw shaders to Metal, mapping image/buffer formats, atomics, workgroup barriers, pass ordering, and indirect draw. Keep the existing parameter schema and presets shared where practical.
4. Add temporal painting, optical flow, relaxation, impasto, and the full Advanced page. Feed live settings to the running process using the current settings-file protocol.
5. Compare still frames against Windows for all five Looks and important Advanced settings; compare videos for temporal stability and test live input at multiple resolutions/FPS. Record acceptable visual tolerance and performance on actual Mac hardware.
6. Package a separate macOS `Paintify.tox` with its native dependencies. Test it by dragging into a blank TouchDesigner project on a clean Mac. Verify code signing/notarization and first-run extraction/launch before calling it a one-file install.

A separate Mac TOX is the lowest-risk release shape because a binary-only Windows TOX cannot run on macOS. The TouchDesigner UI can stay effectively the same. The main uncertainty is engineering and validation effort, especially shader equivalence and real-time performance; there is no tested Mac build in this repository today.

## Primary sources

- [TouchDesigner system requirements](https://docs.derivative.ca/System_Requirements) and [macOS support](https://docs.derivative.ca/MacOS)
- [TouchDesigner Syphon Spout In TOP](https://docs.derivative.ca/Syphon_Spout_In_TOP) and [Syphon Spout Out TOP](https://docs.derivative.ca/Syphon_Spout_Out_TOP)
- [TouchDesigner Vulkan/MoltenVK architecture](https://docs.derivative.ca/Release_Notes/2022.20000)
- [Apple on macOS OpenGL 4.1 and Metal](https://developer.apple.com/videos/play/wwdc2020/10631/)
- [Apple Metal GPU-generated indirect commands](https://developer.apple.com/documentation/metal/encoding-indirect-command-buffers-on-the-gpu)
- [SyphonMetalClient](https://github.com/Syphon/Syphon-Framework/blob/main/SyphonMetalClient.h) and [SyphonMetalServer](https://github.com/Syphon/Syphon-Framework/blob/main/SyphonMetalServer.h)
