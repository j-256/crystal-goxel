# Contributing to Crystal Goxel

Contributions to Crystal Goxel use the project's existing [GPL-3.0-or-later license](COPYING). Contributors retain their copyright. No Contributor License Agreement (CLA) or copyright assignment is required.

Submit work you are entitled to license under those terms, and preserve third-party licenses and notices. The source repository's files under `doc/cla` are retained upstream Goxel records; Crystal Goxel does not collect new signatures there. Changes submitted directly to upstream Goxel follow [upstream's contribution policy](https://github.com/guillaumechereau/goxel/blob/master/CONTRIBUTING.md).

## Crystal Project changes and testing

Keep game-specific logic in the bridge and upstream editor changes small. The bridge uses C++17 with C-compatible entrypoints and a C# helper. Use synthetic fixtures; do not submit game assemblies, native assets, decoded data, decompiled code or user projects.

See [CRYSTAL_BRIDGE.md](CRYSTAL_BRIDGE.md#verification) for the focused bridge checks and native render smoke check. The [Windows testing guide](doc/WINDOWS_TESTING.md) lists the outstanding platform checks and how to report results. Describe checks that passed, failed or were blocked; helper checks alone do not establish graphical compatibility.

## Upstream C coding style

Preserve the existing Goxel C conventions, which resemble Linux kernel style with four-space indentation and typedefs allowed. Follow the surrounding code when in doubt. These conventions apply to upstream C code; preserve the bridge's existing C++ and C# style.

- Use 4 spaces indentations.  No tabs characters anywhere in the code.

- 80 columns max line width.

- No trailing white space.

- Function and variable names all in lowercase, with underscores to separate parts if needed:

      int nb_block; // Good
      int nbBlock;  // Bad

- K&R style braces (opening brace on same line):

      if (something) {
          ...
      } else {
          ...
      }

- For functions, put the opening brace on the next line:

      int my_func(void)
      {
          ...
          return 0;
      }

- For multiline conditions, placing the opening brace on the next line is accepted. Prefer a shorter condition when possible:

      if (a_very_long_condition ||
          that_uses_several_lines)
      {
          ...
      }

- No space between function and argument parenthesis:

      func(10, 20) // Good
      func (10, 20) // BAD!


- One space after keywords, except `sizeof`:

      if (something) // Good
      if(something)  // BAD

- One space around binary operators, no space after unary operators or before postfix operators.

      x = 10 + 20 * 3; // Good
      x = 10+20*3; // BAD
      x++;  // Good
      x ++; // BAD
      y = &x; // Good
      y = & x; // VERY BAD

- Use 'C' style variable declarations:

      int *x; // Good
      int* x; // BAD

- Put variable declarations at the top of C functions.


## Git commit style

- Keep the summary line under about 50 characters.

- Separate the body from the summary with a blank line. Wrap body lines at 72 characters.

- Separate commits into logical changes so they can be reviewed independently.
