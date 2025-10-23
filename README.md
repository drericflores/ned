**NED - A Simple Terminal Text Editor
**

**Author**: Dr. Eric Oliver Flores
**Version**: 1.0

NED is a minimal, simple terminal text editor written in pure C99. It is designed to be lightweight and has no external dependencies like ncurses.

b

Based on the source code implementation:

Minimal and Dependency-Free: Written in pure C99 with no external libraries. Raw Terminal Handling: Directly manipulates the terminal using termios.h for raw input mode. 

**Clean UI**: Uses the terminal's "alternate screen" for a clean start and exit, leaving no editor text in your shell history.
**Resize-Safe**: Correctly handles window resize events (SIGWINCH) in the main loop to prevent visual glitches.
**Robust**: Safely handles editing new or empty files without crashing.
**Static Binary**: The included Makefile builds a fully static executable by default, making it highly portable.

**Building**

A Makefile is provided for easy compilation on a system with gcc and make.

**Build the editor**:

make

This will compile the source and create a static executable file named ned in the current directory.

Clean up build files:

make clean

Usage

To run the editor (for a new file):

./ned

To open an existing file:

./ned path/to/your/file.txt

**Controls**

The keybindings are simple and hardcoded in main.c:

Ctrl+S: Save the current file.

Ctrl+Q: Quit the editor.

Arrow Keys: Move the cursor (Up, Down, Left, Right).

Backspace: Delete the character to the left of the cursor.

Enter: Insert a new line.

**Configuration**

Basic editor constants are defined in config.h. You can modify this file before building to change default values for:

NED_VERSION

SCREEN_ROWS / SCREEN_COLS (fallbacks)

TAB_WIDTH

MAX_FILENAME

BUFFER_SIZE
