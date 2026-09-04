<a id="readme-top"></a>

[![Contributors][contributors-shield]][contributors-url]
[![Forks][forks-shield]][forks-url]
[![Stargazers][stars-shield]][stars-url]
[![Issues][issues-shield]][issues-url]
[![License][license-shield]][license-url]
[![Status][status-shield]][status-url]

<!-- PROJECT LOGO --> <br /> <div align="center"> <a href="https://github.com/GaudiestBaker74/Project-LUMA"> <img src="docs/images/luma-logo.png" alt="Project LUMA" width="650"> </a> <h3 align="center">Project LUMA</h3> <p align="center"> Super Mario Galaxy — Native PC Port <br /> A native PC port of Super Mario Galaxy for Windows and Linux. <br /> <br /> <a href="https://github.com/GaudiestBaker74/Project-LUMA/releases"><strong>Download »</strong></a> <br /> <br /> <a href="https://github.com/GaudiestBaker74/Project-LUMA">View Repository</a> &middot; <a href="docs/milestones.md">Roadmap</a> &middot; <a href="https://github.com/GaudiestBaker74/Project-LUMA/issues">Report Bug</a> &middot; <a href="https://github.com/GaudiestBaker74/Project-LUMA/issues">Request Feature</a> </p> </div> <!-- TABLE OF CONTENTS --> <details> <summary>Table of Contents</summary> <ol> <li> <a href="#about-the-project">About The Project</a> <ul> <li><a href="#development-progress">Development Progress</a></li> <li><a href="#features">Features</a></li> <li><a href="#architecture">Architecture</a></li> </ul> </li> <li> <a href="#getting-started">Getting Started</a> <ul> <li><a href="#prerequisites">Prerequisites</a></li> <li><a href="#building">Building</a></li> <li><a href="#game-assets">Game Assets</a></li> </ul> </li> <li><a href="#usage">Usage</a></li> <li><a href="#roadmap">Roadmap</a></li> <li><a href="#documentation">Documentation</a></li> <li><a href="#contributing">Contributing</a></li> <li><a href="#license">License</a></li> <li><a href="#legal">Legal</a></li> <li><a href="#acknowledgments">Acknowledgments</a></li> </ol> </details> <!-- ABOUT THE PROJECT -->
About The Project
<div align="center"> <img src="docs/images/screenshot-gameplay.png" alt="Project LUMA Gameplay" width="800"> </div>
Project LUMA is an independent effort to bring Super Mario Galaxy 1 to modern PCs as a native application rather than running the game through a Wii emulator.

Built around the SMGCommunity/Petari decompilation, LUMA replaces Wii-specific systems with native PC implementations while keeping the original game logic as faithful to the decompiled code as possible.

The project targets:

Windows
Linux
x86-64
AArch64
Vulkan
How it works
┌─────────────────────────────┐
│     Super Mario Galaxy      │
│        Game Code            │
└──────────────┬──────────────┘
               │
               ▼
┌─────────────────────────────┐
│      Wii Compatibility      │
│ OS • GX • DVD • VI • WPAD  │
│ KPAD • AX • NAND • ...      │
└──────────────┬──────────────┘
               │
               ▼
┌─────────────────────────────┐
│    Platform Abstraction     │
│                             │
│ Input • Audio • Video       │
│ Filesystem • Threads       │
└──────────────┬──────────────┘
               │
        ┌──────┴──────┐
        ▼             ▼
     Windows        Linux
        │             │
        └──────┬──────┘
               ▼
            Vulkan

The ultimate goal is a fully playable native PC version while preserving the original game's behavior wherever possible.

<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- DEVELOPMENT PROGRESS -->
Development Progress
<div align="center"> <img src="docs/images/progress-65.svg" alt="Project LUMA 65% progress" width="700">
65% Technical Progress
Current Milestone: M9 — First Real Boot 🚧

</div>
Note: The 65% figure represents the current state of the technical porting infrastructure. It does not mean that 65% of the original game is playable.

Current foundations include:

Native PC startup.
Platform abstraction.
Vulkan rendering.
Wii compatibility APIs.
Input and controller support.
Filesystem and asset loading.
RARC / Yaz0 / JKRArchive support.
Layout and resource systems.
Audio infrastructure.
GameSystem startup.
Scene system.
Logo scene.
The current objective is progressing further through the title and game initialization pipeline and reaching the first actual gameplay scene.

<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- FEATURES -->
Features
🖥️ Native PC Runtime
Native x86-64 support.
Native AArch64 support.
Windows support.
Linux support.
C++20.
CMake build system.
Cross-platform platform abstraction.
Native PC execution.
No Wii emulator required.
🎮 Input System
LUMA uses SDL3 for its input layer.

Supported functionality includes:

Keyboard.
Mouse.
Gamepads.
Xbox controllers.
DualSense / DualShock controllers.
Controller hot-plugging.
Rumble.
Configurable controls.
Wii Remote-style input mapping.
Default Keyboard Controls
Keyboard	Action
W A S D	Nunchuk Stick
SPACE	A
LEFT CLICK	B
MOUSE	Pointer
ARROW KEYS	D-Pad
+ / -	Wii Plus / Minus
HOME / ESC	Exit

More information can be found in the [Input Documentation][input-doc].

🎨 Vulkan Renderer
Project LUMA uses Vulkan 1.3 as its primary graphics backend.

The renderer currently provides:

Swapchain management.
Dynamic rendering.
VSync.
Fullscreen support.
Resizable windows.
GPU timestamps.
VRAM reporting.
Dynamic vertex buffers.
Texture uploads.
Render targets.
Depth buffers.
Pipeline caching.
Push constants.
Uniform buffers.
Offscreen rendering.
Renderer tests.
The original Wii GX API is translated through a dedicated compatibility layer:

Nintendo GX
     │
     ▼
GX Compatibility
     │
     ▼
Renderer Abstraction
     │
     ▼
Vulkan 1.3

See the [Renderer Documentation][renderer-doc] and [GX Documentation][gx-doc].

🧩 Wii Compatibility Layer
LUMA recreates the Wii APIs expected by the decompiled game.

Current compatibility work includes:

OS
DVD
VI
GX
WPAD
KPAD
PAD
AX
NAND
NWC24
Memory systems
Math systems
JGeometry
JMath
nw4r
JSystem
📦 Asset & Resource System
LUMA loads assets extracted from the user's own legitimate copy of Super Mario Galaxy.

The repository does not distribute Nintendo's proprietary game data.

The resource pipeline includes:

Virtual filesystem.
Wii-style DVD API.
Directory scanning.
FST generation.
Synchronous DVD reads.
Asynchronous DVD reads.
DVD worker thread.
Asset validation.
RARC archives.
Yaz0 decompression.
JKRArchive mounting.
Host-memory resource loading.
See the [Asset Documentation][assets-doc].

🔊 Audio
LUMA contains a native audio compatibility layer based around the original JAudio2 architecture.

Current work includes:

CPU-side DSP / JDSP emulation.
compat/ai.
SDL3 audio backend.
Headless audio mode.
Native JAudio2 compilation.
See the [Audio Documentation][audio-doc].

🧪 Testing
The project is designed around incremental development and automated testing.

Current testing infrastructure includes:

doctest.
CTest.
Renderer tests.
Vulkan headless tests.
Pixel-accurate rendering tests.
Compatibility-layer tests.
Asset tests.
Input tests.
Archive tests.
Layout tests.
       CODE
         │
         ▼
      BUILD
         │
         ▼
       LINK
         │
         ▼
      CTEST
         │
         ▼
    ┌─────────┐
    │    ✓    │
    │  GREEN  │
    └─────────┘

<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- ARCHITECTURE -->
Architecture
Project LUMA is divided into several major layers:

Project-LUMA/
│
├── src/
│   ├── main.cpp
│   │
│   ├── platform/
│   │   ├── windows/
│   │   ├── linux/
│   │   ├── renderer/
│   │   ├── input/
│   │   ├── audio/
│   │   ├── filesystem/
│   │   └── ...
│   │
│   ├── compat/
│   │   ├── os/
│   │   ├── dvd/
│   │   ├── gx/
│   │   ├── wpad/
│   │   ├── kpad/
│   │   ├── jsystem/
│   │   ├── nw4r/
│   │   └── ...
│   │
│   ├── tools/
│   └── tests/
│
├── third_party/
│   └── petari/
│
├── tools/
├── docs/
├── assets/
└── CMakeLists.txt

The repository separates platform code, Wii compatibility code, tools, tests, third-party dependencies, documentation and user-provided assets.

This allows Wii-specific implementations to be replaced without unnecessarily modifying the original game logic.

<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- GETTING STARTED -->
Getting Started
To build Project LUMA locally, follow the instructions below.

Prerequisites
You will need:

Git
CMake
A C++20-compatible compiler
Vulkan 1.3-capable hardware
SDL3
A legitimate copy of Super Mario Galaxy for the required game assets
For complete dependency and environment information, see the [Build Guide][build-doc].

Building
Linux
cmake -B build -DPLATFORM=PC
cmake --build build
ctest --test-dir build

Windows
Using Visual Studio / MSVC:

cmake -B build -DPLATFORM=PC
cmake --build build --config Release
ctest --test-dir build -C Release

<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- GAME ASSETS -->
Game Assets
Project LUMA does not distribute copyrighted Nintendo game assets.

The repository does not contain or redistribute:

ROMs.
ISOs.
WAD files.
Nintendo models.
Textures.
Music.
Sound effects.
Other proprietary game data.
Users must extract the required data from their own legitimate copy of Super Mario Galaxy.

Extracted data is stored locally in:

assets/

The directory is ignored by Git.

See the [Asset Documentation][assets-doc] for the complete process.

<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- USAGE -->
Usage
Once built and configured, Project LUMA will initialize the native PC runtime and load the required game resources from the local assets/ directory.

The current development target is M9 — First Real Boot.

As development progresses, the launcher/runtime will move through the game initialization pipeline toward the first playable Galaxy.

Current status: LUMA is an active development project and is not yet a complete playable replacement for the original game.

<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- ROADMAP -->
Roadmap
Milestone	Description	Status
M0	Analysis & Architecture	✅
M1	PC Build + Platform Core	✅
M2	Windows Support	✅
M3	Window + Vulkan + Game Loop	✅
M4	Renderer API	✅
M5	GX Compatibility	✅
M6	Complete Input System	✅
M6.5	JMath / JGeometry	✅
M7	VFS + Assets	✅
M8	Audio Core	✅
M9	First Real Boot	🚧
M10	First Playable Galaxy	⏳
M11	Full Game	⏳

See the [complete roadmap][roadmap].

<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- DOCUMENTATION -->
Documentation
All technical documentation is available in the docs/ directory.

[Architecture][architecture-doc]
[Build Guide][build-doc]
[Input][input-doc]
[Renderer][renderer-doc]
[GX][gx-doc]
[Assets][assets-doc]
[Audio][audio-doc]
[Boot][boot-doc]
[Milestones][roadmap]
[Porting][porting-doc]
Development Notes
[M9.5.3 Plan][m953-doc]
<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- CONTRIBUTING -->
Contributing
Contributions, testing, documentation improvements and technical discussion are welcome.

Before contributing:

Read the [Architecture Documentation][architecture-doc].
Read the [Porting Documentation][porting-doc].
Review the [Milestones][roadmap].
Keep compatibility behavior as close to the original implementation as possible.
Add tests when introducing new compatibility functionality.
Pull Requests
Fork the Project.
Create your Feature Branch:
git checkout -b feature/AmazingFeature

Commit your Changes:
git commit -m "Add AmazingFeature"

Push to the Branch:
git push origin feature/AmazingFeature

Open a Pull Request.
Contributors
<a href="https://github.com/GaudiestBaker74/Project-LUMA/graphs/contributors"> <img src="https://contrib.rocks/image?repo=GaudiestBaker74/Project-LUMA" alt="Project LUMA contributors" /> </a> <p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- LICENSE -->
License
Project LUMA is distributed under the Unlicense.

See the LICENSE file for more information.

<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- LEGAL -->
Legal
Project LUMA is an independent fan-made technical project.

It is not affiliated with, endorsed by, or sponsored by Nintendo.

Nintendo and Super Mario Galaxy are trademarks and/or properties of their respective owners.

Project LUMA does not provide copyrighted game assets and does not facilitate obtaining them.

<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- ACKNOWLEDGMENTS -->
Acknowledgments
Project LUMA would not be possible without the work of the reverse-engineering and game-decompilation communities.

Special thanks to:

[SMGCommunity / Petari][petari]
The contributors to the Super Mario Galaxy decompilation effort.
The open-source community.
The developers and maintainers of the libraries used by the project.
<p align="right">(<a href="#readme-top">back to top</a>)</p> <!-- MARKDOWN LINKS & IMAGES -->