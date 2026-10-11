# Hexal Language and 3D Voxel Engine (v1.1)

Hexal (.hxl) is a high-performance, minimalist Domain-Specific Language (DSL) and lightweight 3D/2D voxel game engine built on top of the native Windows API (Win32/GDI32) and C. 

The Hexal compiler acts as an optimized source-to-source transpiler (blind translator) that converts plain Hexal scripts directly into high-efficiency C code, leveraging MinGW-w64 GCC to spit out a lightning-fast native Windows executable capable of running at a smooth, locked 60+ FPS with hardware-accelerated feel and zero engine bloat.

---

## Key Features in v1.1

- **Advanced 3D Camera Controls**: Seamless first-person Mouse-Look mode that locks the cursor, camera pitch limits preventing screen flipping, and relative walk systems driven by low-level trigonometry.
- **2D Physics Engine Backend**: Real-time multi-object rendering supporting velocity accumulation, customizable gravity scales, surface friction coefficients, and automated rigid axis-aligned bounding box (AABB) collision detection and elastic bounce calculations.
- **Interactive Console I/O**: Direct typed console streaming through robust string buffer utilities (print, println, and dynamic variable-typed user input).
- **GDI32 UI Text Rendering**: Native pixel-perfect string and integer conversion drawing text and variables directly on top of the active 3D window.
- **One-Click Auto-Execution**: Automated workflow chains that cleanly compile, call the backend optimization flags, and immediately launch the compiled binary without external tool macros.

---

## Build and Compilation Guide

### 1. Rebuilding the Compiler Spec
To compile or upgrade the standalone transpiler executable via Windows Terminal (CMD):
```sh
gcc hexal_compiler.c -o hexal_compiler.exe
```

### 2. Translating and Running a Hexal Script
To compile a Hexal file (.hxl), invoke the binary. The transpiler will generate the intermediate clean C file, bridge all active libraries, run GCC optimizations, and automatically run the resulting game window:
```sh
hexal_compiler.exe program.hxl -o game.exe --console
```
Note: The --console flag is mandatory when building programs that require persistent input or print buffers to stay visible.

---

## Installation for End-Users

To distribute a game built with Hexal, users only need to download the compiled executable output (e.g., game.exe) along with any asset scripts inside the same directory folder. No external frameworks, heavy runtime environments, or large installation wizard steps are required by the client system.

---

## Notes and Core Architecture Limitations

Hexal is constructed strictly as an ultra-fast, single-pass pipeline transpiler. It prioritizes direct execution speeds and hardware proximity above heavy safety syntax compilers. 

1. Scope Boundaries: Variables exist inside a single compiler-wide flattened symbol configuration layout map. Nested lexical block-scoping rules and custom user-defined function hooks within the high-level .hxl language specification are currently omitted.
2. Win32 Hard Dependencies: The compiled target file outputs are completely Win32/GDI32 platform-dependent. While compilation and text parser verification runs successfully on other platforms, the compiled target outputs require a Windows runtime environment to successfully execute windows and graphical updates.
