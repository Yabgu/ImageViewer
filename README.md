# ImageViewer

## Brief
ImageViewer is a C++ and OpenGL-powered image viewer pushing the boundaries of display technology. It aims to deliver virtual 9-bit (and higher) color perception on standard 8-bit monitors through rapid frame-sequential display, with exceptionally fast image loading and rendering (targeting <0.5s for large files like 10MB JPEGs). Currently in early alpha, it supports basic JPEG and PNG rendering. High-fidelity viewing and plugin support are in development to unlock unparalleled fidelity and extensibility on everyday hardware.

## Mission
Our mission is to provide an image viewing experience that transcends the limitations of conventional 8-bit displays and slow loading times. We aim to deliver perceptually higher bit-depth imagery and unparalleled speed to professionals and enthusiasts, making high-fidelity image review accessible on everyday hardware.

## Vision
ImageViewer is engineered from the ground up to redefine the experience of interacting with digital images. Our long-term vision, looking 3–5 years ahead, is to establish ImageViewer as the premier, high-performance, and infinitely extensible platform for image viewing and advanced manipulation—pushing the boundaries of what's possible on consumer hardware.

We aspire to:

- Democratize High-Fidelity Viewing: Provide a truly seamless and blazing-fast image loading and rendering experience, far surpassing conventional viewers. Our unique temporal dithering technique will make higher bit-depth images (e.g., 10-bit) visually accessible on standard 8-bit monitors, ensuring unparalleled color accuracy and detail for everyone.
- Empower Unprecedented Customization: Evolve into a dynamic, hybrid application featuring a lean, high-speed C++ viewer leveraging modern graphics APIs (initially OpenGL, with future plans for Vulkan, Metal, Direct2D, etc.) for core tasks, seamlessly integrated with a flexible, web-based (JavaScript/WebGPU) editor. This architecture will foster a vibrant, community-driven plugin ecosystem, where users can effortlessly create custom editing tools—even leveraging AI to generate their own JavaScript plugins for both CPU and GPU-accelerated effects.
- Unlock the Invisible Spectrum: Pioneer user-friendly access to spectral imaging, allowing curious science enthusiasts and professionals to define custom color spaces (e.g., IR, UV, arbitrary multi-band arrays) and interpret raw sensor data in novel ways. Through powerful WebGPU shaders, ImageViewer will enable real-time visualization and editing of spectral information, revealing hidden details and fostering new discoveries in fields like scientific photography and remote sensing.

Ultimately, ImageViewer aims to be more than just an image viewer; it seeks to be an open canvas for visual exploration, scientific insight, and creative expression—empowering users to see, understand, and interact with images in ways previously confined to specialized, expensive software.

## Key Features
- Virtual High Bit-Depth Viewing: In development to enable perceptually richer colors on 8-bit monitors using temporal dithering (e.g., simulating 9-bit or higher color depth).
- Optimized Performance (WIP): Aiming for sub-second loading and rendering of large images (e.g., 10MB JPEG in <0.5s) through chunked data processing (256x256 blocks or 256 lines) and asynchronous OpenGL texture uploads.
- Extensible Architecture: Designing a plugin system for seamless support of new formats and custom tools, with future integration of C++ and JavaScript/WebGPU plugins.
- OpenGL-Powered: Built on OpenGL for efficient rendering, with plans to support Vulkan, Metal, and Direct2D.
- Lightweight & Focused: Prioritizes core viewing functionality and performance over extraneous features.

## Supported Formats
- Current Support: JPEG and PNG, optimized for fast loading and high-fidelity viewing.
- Future Plans: Expand support to additional formats (e.g., TIFF, WEBP, HDR) through a flexible plugin system, enabling community-driven extensions for virtually any image format.

## Plugin System (Planned)
ImageViewer will support a flexible plugin architecture to enable:
- Community-driven support for new formats (e.g., TIFF, WEBP, RAW, HDR).
- Custom image processing tools (e.g., filters, spectral imaging) using C++ for performance and JavaScript/WebGPU for accessibility.
- A vibrant ecosystem where users can contribute plugins for scientific, creative, or professional use cases.

Stay tuned for updates as we design the plugin API!

## Why ImageViewer?
In a world of increasing image fidelity (HDR, 10-bit, 12-bit content), most monitors remain 8-bit, leading to compromises in visual quality. Existing viewers often fail to address this or are bogged down by slow loading. ImageViewer offers a unique solution:

- No Special Hardware Needed: Achieve a superior viewing experience on your current monitor.
- Instant Gratification (Aimed For): Say goodbye to waiting for large images to load.
- Precision & Perception: Get closer to the true visual data of your high bit-depth images.

## Current Status & Next Steps
ImageViewer is in early alpha, with basic JPEG and PNG support under active development. Our immediate priorities include:
- Completing high-fidelity rendering for JPEG and PNG (e.g., 10-bit color, HDR support).
- Refining the temporal dithering algorithm for broader display compatibility (60Hz+ monitors).
- Laying the foundation for a plugin system to support additional formats (e.g., TIFF, WEBP, RAW) and custom processing tools.

## Getting Started
Clone the repository:

```sh
git clone https://github.com/Yabgu/ImageViewer.git
cd ImageViewer
```

### Build
ImageViewer is designed with the C++23 standard in mind, requiring a fairly recent compiler (e.g., GCC 13+, Clang 16+, MSVC 19.38+).

**Dependencies (using Vcpkg – Recommended):**

- Windows: Run `prepare.bat` to automatically fetch and configure dependencies using Vcpkg.
- Linux: Run `prepare.sh` to automatically fetch and configure dependencies using Vcpkg.

After running the prepare script, navigate into the newly created build directory:

```sh
cd build
```

Then, compile the project using CMake:

```sh
cmake --build . -j 4 # Or use -j with a higher number for more cores, e.g., -j 8
```

**Dependencies (using System Libraries):** If you prefer not to use Vcpkg, you can configure your build system to use system-wide libraries for dependencies. (Further instructions may be added here for specific system library configurations.)

### Run
After building, simply execute the iview executable (or iview.exe on Windows) followed by the path to a supported image file.

```sh
./iview /path/to/your/image.jpg
```

or on Windows:

```sh
.\iview.exe C:\path\to\your\image.png
```

## Contributing
We welcome contributions of all kinds! Whether it's bug reports, feature suggestions, code contributions, documentation improvements, or just spreading the word, your help is greatly appreciated. Feel free to open an issue or pull request on our GitHub repository.

Want to shape the plugin system? Share your ideas for format support or custom tools in our [GitHub Discussions](https://github.com/Yabgu/ImageViewer/discussions)!

## License
This project is licensed under the Apache-2.0 License – see the LICENSE file for details.
