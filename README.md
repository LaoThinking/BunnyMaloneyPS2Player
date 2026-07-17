# BunnyMaloneyPS2Player - Bunny Maloney Watch-On!
A PS2 application made with PS2SDK to play all the episodes from the show, Bunny Maloney!

An interactive homebrew video player and episode selector for the PlayStation 2 console, designed to play the animated series *Bunny Maloney*. Built using the native PS2SDK, gsKit, and hardware-accelerated video decoding.

---

## 🚀 Features
* **Custom Boot Timeline:** Implements smooth fading animations for loading bars and disclaimer screens.
* **Interactive Main Menu:** Fully functional UI handling options for playing sequential streams or entering specific menus.
* **52-Episode Browser:** A dynamic, memory-efficient selector submenu utilizing dynamic texture loading (`Delayed = 1`) to switch between 52 thumbnail slots without overflowing the PS2's 4MB VRAM.
* **Hardware Optimized:** Features a robust IOP reset sequence for absolute stability on real hardware and emulators like **PCSX2**.

---

## 🛠️ Compilation & Requirements

To compile this project from source, you need the official **PS2DEV SDK** environment configured.

### Dependencies
The project links against the following standard libraries:
* `gsKit` & `gsKit_toolkit` (Graphics processing)
* `dmaKit` (Direct Memory Access channeling)
* `libpad` (Controller input interface)
* `libpatches` (IOP kernel loading system patches)

### Building the Project
Navigate to your root directory containing the `Makefile` and execute:
```bash
# Clean previous build artifacts
rm -f *.o *.elf

# Compile into a bootable PlayStation 2 executable
make
