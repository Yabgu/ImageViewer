# TODOs and Improvement Suggestions for ImageViewer

## media Module
- [ ] Refactor Scene/Processor/View coupling for more flexible pipelines (support multiple processors/views per scene).
- [ ] Enhance event system: add asynchronous/event queue support, stronger type safety, and more granular event types/metadata.
- [ ] Use smart pointers for all resource ownership (replace raw pointers in Buffer/ViewCorrespondence).
- [ ] Add more documentation and usage examples for each module.

## imageviewer Module
- [ ] Improve error handling and exception safety in image loading and OpenGL resource management.
- [ ] Use smart pointers consistently for images, textures, and windows.
- [ ] Split Window class into smaller, focused components (e.g., InputHandler, Renderer).
- [ ] Ensure thread safety for shared resources (textures, images) when using OpenMP.
- [ ] Prepare for plugin support by defining clear extension points (formats, filters).

## ui Module
- [ ] Ensure only nodes needing cameras/resources have them (make camera optional in node).
- [ ] Define clear interfaces for rendering, updating, and event handling at each node.
- [ ] Add support for arranging child nodes (grid, overlay, etc.).
- [ ] Integrate input events (mouse, keyboard) into the node system for interactive UI.

## General/Build System
- [ ] Add CI for Linux, Windows, and macOS.
- [ ] Begin designing the plugin API and extension points.
- [ ] Expand README with architecture diagrams, usage examples, and developer guides.

## Code Smells & Refactoring Opportunities
- [ ] Replace raw pointers with smart pointers everywhere for safety.
- [ ] Split large classes (e.g., Window) into smaller, focused components.
- [ ] Automate cleanup with RAII and smart pointers.
- [ ] Make event system more robust and extensible.

## Next Steps
- [ ] Refactor scene/view/node logic for maximum flexibility and extensibility.
- [ ] Improve error handling and resource management.
- [ ] Design and document the plugin system.
- [ ] Add more tests and CI coverage.
- [ ] Expand documentation and developer guides.
