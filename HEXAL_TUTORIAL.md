# Hexal Language Tutorial

> **Status:** Draft documentation based on `hexal_compiler.c`.
>
> The examples below follow the compiler's parser and command handlers. They have not been compiled or run as part of preparing this document, so test them with the current compiler before treating this guide as a release-ready reference.

Hexal is a programming language that uses `.hxl` source files. The provided compiler translates Hexal into C and can invoke GCC to build a native Windows executable. The runtime provides a window, drawing commands, keyboard input, a render queue, and 2D/3D-oriented features.

## Contents

- [Requirements](#requirements)
- [Build the compiler](#build-the-compiler)
- [Compile a Hexal program](#compile-a-hexal-program)
- [Your first program](#your-first-program)
- [Variables and types](#variables-and-types)
- [Output and input](#output-and-input)
- [Conditions and loops](#conditions-and-loops)
- [Windows and drawing](#windows-and-drawing)
- [Keyboard input](#keyboard-input)
- [2D movement and physics](#2d-movement-and-physics)
- [3D worlds](#3d-worlds)
- [Including other files](#including-other-files)
- [Compiler command-line options](#compiler-command-line-options)
- [Notes and limitations](#notes-and-limitations)

## Requirements

The supplied compiler source documents this build environment:

- Windows
- MinGW-w64 GCC
- A C compiler capable of building the compiler source
- GCC available to the compiler when you compile a `.hxl` program

The generated runtime uses Win32 APIs and GDI, so this compiler/runtime is Windows-oriented.

## Build the compiler

Save the compiler source as `hexal_compiler.c`, then run:

```sh
gcc hexal_compiler.c -o hexal_compiler.exe
```

Run commands from a terminal in the directory where the compiler executable and your `.hxl` file are available, unless you provide paths.

## Compile a Hexal program

Create a file named `main.hxl`. Then run:

```sh
hexal_compiler main.hxl
```

By default, the compiler writes `main.c` and attempts to build `main.exe`.

To choose a different executable name:

```sh
hexal_compiler main.hxl -o game.exe
```

To generate the C source without invoking GCC:

```sh
hexal_compiler main.hxl --emit-only
```

To keep a console window in the generated program:

```sh
hexal_compiler main.hxl --console
```

The compiler also accepts `--help` or `-h` to print usage information.

## Your first program

A Hexal source file must contain a `Program main { ... }` block. A minimal console-style example is:

```hexal
Program main {
    println "Hello from Hexal!";
}
```

Compile it with:

```sh
hexal_compiler main.hxl --console
```

`println` writes its arguments and then adds a newline. `print` writes without automatically adding a newline.

## Variables and types

The compiler provides these declaration keywords:

| Keyword | Meaning | Example |
|---|---|---|
| `hex` | Integer | `hex score = 10;` |
| `hexf` | Floating-point number | `hexf speed = 1.5;` |
| `text` or `hext` | Text string | `text name = "Hexal";` |
| `letter` or `hexc` | Single character | `letter initial = 'H';` |

Examples:

```hexal
Program main {
    hex score = 10;
    hexf speed = 2.5;
    text title = "My project";
    letter initial = 'H';

    score = score + 5;
    speed = speed * 2;
    title = "Updated title";
    initial = 'X';

    println title;
    println score;
    println speed;
    println initial;
}
```

Variables declared without an initial value receive a default value: `0` for `hex`, `0.0` for `hexf`, an empty string for `text`/`hext`, and a null character for `letter`/`hexc`.

Variable names must be valid C-style identifiers and cannot be reserved words. Names beginning with `hx_` are reserved for the runtime.

## Output and input

Use `print` or `println` to display strings, character values, text variables, and numeric expressions. Multiple values can be separated by commas.

```hexal
Program main {
    text greeting = "Hello";
    hex count = 3;

    print greeting, " — count: ", count;
    println "";

    print "Enter a number: ";
    input count;
    println "You entered: ", count;
}
```

`input` reads into an already-declared variable. The variable's type determines how input is read:

- `hex`: integer input
- `hexf`: floating-point input
- `letter` / `hexc`: one character
- `text` / `hext`: a line of text

The compiler requires a semicolon after `input variable;`.

## Conditions and loops

Use `if`, `else if`, and `else` with braces:

```hexal
Program main {
    hex score = 12;

    if score >= 10 {
        println "You reached the target.";
    } else {
        println "Keep trying.";
    }
}
```

The compiler also accepts `otherwise` as an alternative to `else`.

A `while` statement accepts a condition followed by a block:

```hexal
Program main {
    hex count = 0;

    while count < 3 {
        println count;
        count = count + 1;
    }
}
```

For a loop controlled by a boolean-like variable, use `loop variable { ... }`. The compiler also accepts `loop variable inf { ... }`. The variable must be a non-text value that can be used as a condition.

```hexal
Program main {
    hex running = 1;
    hex ticks = 0;

    loop running {
        ticks = ticks + 1;
        if ticks >= 3 {
            running = 0;
        }
    }

    println "Loop finished.";
}
```

Use `when condition { ... }` for a conditional block. `when Key pressed X { ... }` is useful for one-shot key actions; see [Keyboard input](#keyboard-input).

Conditions support C-style comparisons and expressions. The compiler also translates `and`, `or`, and `not` as logical operators. The special key forms are described below.

## Windows and drawing

Create a window with:

```hexal
Window "My Hexal Window" 800 600;
```

A `Window` title can be a quoted string or a previously declared text variable.

### 2D rectangles

Use `draw_2d` with an object ID, position, dimensions, and RGB color components:

```hexal
Program main {
    Window "Hexal 2D" 640 480;

    draw_2d player 100 100 60 60 255 80 40;
}
```

General form:

```hexal
draw_2d ID X Y WIDTH HEIGHT RED GREEN BLUE;
```

RGB values are clamped to the range 0–255 by the runtime. An object's ID is a name, number, or quoted string; IDs identify objects in the runtime render queue.

### Text in the window

Use `draw_text` to draw text at an `(X, Y)` position with an RGB color:

```hexal
Program main {
    Window "Hexal Text" 640 480;

    draw_text "Hello, window!" 30 30 255 255 255;
}
```

The text argument may also be a text variable or a numeric variable.

### Flat projected 3D-style objects

`draw_3d` creates a simple projected square using an object ID, X/Y/Z values, and RGB color:

```hexal
Program main {
    Window "Hexal Projection" 640 480;

    draw_3d marker 0 0 100 40 180 255;
}
```

This command is distinct from the shaded-cube 3D world commands described later.

### A window that remains open

A windowed program without a `loop` block enters the runtime's idle/render loop when the `Program main` block closes. For interactive applications, use a loop and handle window messages within it.

## Keyboard input

Use `when Key pressed KEY { ... }` to run a block once when a key transitions from released to pressed:

```hexal
Program main {
    Window "Keyboard Example" 640 480;

    hex running = 1;
    loop running {
        when Key pressed Escape {
            running = 0;
        }

        when Key pressed Space {
            println "Space pressed!";
        }
    }
}
```

The compiler maps these named keys: `Space`, `Enter`/`Return`, `Escape`/`Esc`, arrow keys (`Left`, `Right`, `Up`, `Down`), `Shift`, `Ctrl`/`Control`, `Alt`, `Tab`, and `Backspace`. A single letter or digit can also be used as a key.

Mouse buttons can be referred to as `mouse_left` (also `click`) and `mouse_right`.

The condition forms `Key == KEY` and `Key != KEY` check whether a key is currently held or not held, respectively. `Key pressed KEY` is edge-triggered and is intended for one action per press.

## 2D movement and physics

### Move an object

Use `move ID DISTANCE DIRECTION;` to move an existing object. Supported directions are `up`, `down`, `left`, `right`, `forward`, and `backward`.

```hexal
Program main {
    Window "Move a Shape" 640 480;

    draw_2d player 100 100 40 40 60 180 255;
    move player 20 right;
}
```

For 2D objects, `up` decreases Y and `down` increases Y. For cube objects, world Y is upward. The meaning of forward/backward also differs between 2D-style objects and cubes.

### Physics commands

The compiler supports these commands:

```hexal
velocity player 2 0;
gravity player 0.2;
friction player 0.98;
physics player;
```

- `velocity ID VX VY;` sets the object's X and Y velocity.
- `gravity ID ACCEL;` sets the object's gravity acceleration.
- `friction ID FACTOR;` sets a friction factor, clamped to 0–1.
- `physics ID;` applies gravity, velocity, and friction to supported 2D rectangle/HUD objects.

These commands operate on objects that already exist in the render queue. In a game loop, call `physics ID;` repeatedly to update an object's motion.

### Collision and bounce

`collides("id1", "id2")` can be used in a condition. The IDs are strings in the condition expression.

```hexal
Program main {
    Window "Collision Example" 640 480;

    draw_2d player 100 100 40 40 255 80 80;
    draw_2d wall 160 100 40 100 100 100 100;

    if collides("player", "wall") {
        println "Collision!";
        bounce player wall;
    }
}
```

`collides` and `bounce` in the provided runtime support overlapping 2D rectangle objects. `bounce` adjusts their positions and velocities when they overlap.

## 3D worlds

Hexal includes a simple shaded-cube renderer and first-person-style camera controls.

### Camera and background

```hexal
Program main {
    Window "Hexal 3D" 800 600;

    sky 100 180 255;
    fog 30;

    camera 0 3 -8;
    look 0 0;

    cube block 0 2 8 2 220 80 60;
}
```

The `cube` command takes an ID, the cube's center `(X, Y, Z)`, its size, and RGB color:

```hexal
cube ID X Y Z SIZE RED GREEN BLUE;
```

The compiler's comments specify that world Y is up and that the camera looks along +Z when yaw is zero. `look YAW PITCH;` sets the camera angles in degrees. `turn DYAW DPITCH;` changes them relative to their current values. Positive yaw turns right; positive pitch looks up.

`sky R G B;` sets the background color. `fog DISTANCE;` fades distant cubes toward the sky color; a distance of `0` disables fog.

### Create a floor of cubes

Use `layer` to create a grid of cubes with generated IDs:

```hexal
layer floor 0 0 8 8 8 1 90 160 90;
```

General form:

```hexal
layer NAME X Y Z WIDTH DEPTH CUBE_SIZE RED GREEN BLUE;
```

The runtime limits the width and depth of a layer to 64 cubes each. Generated IDs use the layer name as a prefix.

### Camera movement and mouse look

Use `walk DISTANCE DIRECTION;` to move the camera. Directions are `forward`, `backward`, `left`, `right`, `up`, and `down`.

```hexal
walk 0.2 forward;
walk 0.2 right;
```

Enable mouse-look with:

```hexal
mouse_look;
```

You can specify sensitivity:

```hexal
mouse_look 0.15;
```

Disable it with:

```hexal
mouse_look off;
```

Mouse-look captures and hides the cursor while the window is active; disabling it releases the cursor.

### Picking, mining, placing, and removing

- `pick variable;` sets the variable to `1` if the crosshair is aimed at a cube within reach, otherwise `0`.
- `mine;` removes the cube currently selected by the picking state.
- `place RED GREEN BLUE;` places a cube against the face currently being targeted, if a valid cube is selected and the target space is available.
- `remove ID;` removes an object by ID.
- `hud_2d ID X Y WIDTH HEIGHT RED GREEN BLUE;` draws a flat rectangle over the 3D scene.
- `jump;` triggers the runtime's built-in camera jump impulse when the camera is on the ground.

Example input pattern:

```hexal
Program main {
    Window "Block World" 800 600;

    sky 120 190 255;
    camera 0 3 -8;
    look 0 0;
    layer floor -4 0 8 8 8 1 100 180 100;

    hex hasTarget = 0;
    hex running = 1;

    loop running {
        walk 0.1 forward;

        when Key pressed Escape {
            running = 0;
        }

        when Key pressed mouse_left {
            pick hasTarget;
            mine;
        }

        when Key pressed mouse_right {
            pick hasTarget;
            place 220 180 100;
        }
    }
}
```

This example illustrates the intended command pattern from the compiler, but should be tested and adjusted for the desired camera position and controls before publication.

## Including other files

Use `inject` to parse another `.hxl` file:

```hexal
inject "Player.hxl";
```

The filename must be quoted. Relative paths are resolved from the including file's directory first, with a fallback to the working directory. The compiler detects circular includes and limits nesting depth.

## Compiler command-line options

| Command | Behavior |
|---|---|
| `hexal_compiler` | Reads `main.hxl` by default |
| `hexal_compiler program.hxl` | Compiles the named source |
| `hexal_compiler program.hxl -o game.exe` | Names the executable `game.exe` and generated C file `game.c` |
| `hexal_compiler program.hxl --emit-only` | Writes the generated C file without invoking GCC |
| `hexal_compiler program.hxl --console` | Builds without the Windows GUI-subsystem flag, keeping a console window |
| `hexal_compiler --help` | Prints usage |

## Notes and limitations

- This guide is based on the supplied compiler source, not a separate formal language specification.
- The compiler translates expressions into C; this means some expression behavior follows the generated C implementation.
- The runtime is based on Win32/GDI APIs and is intended for Windows.
- The `physics`, `collides`, and `bounce` helpers described here target 2D rectangle/HUD objects, not the shaded 3D cubes.
- The compiler source has several aliases and implementation-specific rules. Verify examples against the exact compiler version used by the project.
- Generated C files are compiler output; edit the `.hxl` source and recompile rather than relying on manual edits to generated C.

## Contributing improvements

If you are contributing this tutorial to the Hexal repository, consider asking the maintainers to confirm:

1. The supported Windows/GCC versions.
2. Whether `TUTORIAL.md` is the preferred filename or whether the docs belong in a `docs/` directory.
3. Which example programs are considered canonical and tested.
4. Whether they want a tutorial only or a separate command/reference page as well.
