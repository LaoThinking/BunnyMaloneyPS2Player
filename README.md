# BunnyMaloneyPS2Player - Bunny Maloney Watch-On!
A PS2 application made with PS2SDK to play all the episodes from the show, Bunny Maloney!

An interactive homebrew video player and media selection interface designed for the PlayStation 2 console, centered around the animated series *Bunny Maloney*. Built from scratch in C using the native open-source **PS2SDK** toolchain, this project utilizes hardware-accelerated graphics via **gsKit** and is engineered for robust execution on physical hardware and modern emulators alike.

---

## 🚀 Features

* **Cinematic Boot Timeline:** Implements a frame-counted, smooth alpha-blended fading sequence for the initial splash graphics, a real-time tracking loading bar, and safety/legal disclaimers.
* **Interactive Menu State Machine:** Features a responsive transition matrix handling inputs between the Title Screen, Main Option layout, multi-item Submenus, and dedicated Credits backdrop.
* **52-Episode Dynamic Browser:** Utilizes a memory-efficient asset-swapping structure. By setting `Delayed = 1` on texture initializations and executing on-demand VRAM binding via `gsKit_TexManager_bind`, the engine successfully displays 52 custom episode thumbnails without overflowing the PS2's strict 4MB embedded Video RAM (VRAM) limitation.
* **Hardware Interoperability & Stability:** Incorporates an explicit IOP (Input/Output Processor) hardware reset sequence alongside manual ROM module injection (`rom0:SIO2MAN` and `rom0:PADMAN`). This guarantees that pad controller states align perfectly on startup, eradicating the common black-screen freeze bug found in older homebrew frameworks.
* **Adaptive Path Resolution:** Automatically parses the execution arguments buffer (`argv[0]`) at entry point to dynamically shift asset-seeking roots based on whether the binary is loaded via USB (`mass:`), network emulation (`host:`), or optical media disc (`cdrom0:`).

---

## 🎮 Controls

The application maps standard Sony DualShock 2 controller inputs via the native `libpad` framework:

| Input Button | Action Mapping |
| :--- | :--- |
| **D-Pad Up / Down** | Navigate between menu choices and browse up or down through the 52 episode rows. |
| **Cross (❌)** | Confirm your current menu selection, open submenus, or execute stream play actions. |
| **Triangle (🔺) / Circle (⭕)** | Back out of sub-menus (e.g., escape from the Episode Selection grid back to the Main Menu). |
| **Start** | Skip past the active loading meter timeline or instantly dismiss the Title Splash Screen. |

---

## 📁 Repository Directory Structure

For data to register successfully at runtime, ensure your target media deployment directory (whether configured via a local PC shared path, an external FAT32 USB drive, or packed inside an optical image) precisely mirrors this file layout:

```text
📦 Bunny Maloney Watchon PS2/
 ┣ 📜 bunny_burnout.elf
 ┗ 📂 Images/
   ┣ 📜 logo.bmp
   ┣ 📜 test_bar.bmp
   ┣ 📜 loading_text.bmp
   ┣ 📜 disclaimer_text.bmp
   ┣ 📜 play_normal.bmp
   ┣ 📜 play_selected.bmp
   ┣ 📜 episodes_normal.bmp
   ┣ 📜 episodes_selected.bmp
   ┣ 📜 credits_normal.bmp
   ┣ 📜 credits_selected.bmp
   ┣ 📜 credits_bg.bmp
   ┣ 📜 button_normal.bmp
   ┣ 📜 button_selected.bmp
   ┣ 📜 thumb_ep1.bmp
   ┣ 📜 thumb_ep2.bmp
   ┣ 📜 thumb_ep3.bmp
   ┗ 📜 ... (Sequentially named through thumb_ep52.bmp)

```

> **Asset Format Requirements:** All graphic assets must be formatted as uncompressed, standard Windows Bitmap (`.bmp`) images matching the software execution color-depth targets (e.g., 32-bit or 24-bit uncompressed raster grids).

---

## 🛠️ Compilation & Environment Setup

Building this project from its source files requires an active, modern installation of the official MIPS **PS2DEV SDK** environment variables configured on your operating system.

### Linker Requirements

To resolve object references cleanly across the toolchain compiler graph, your system's target Makefile must append the specific library dependency tags before pointing to the hardware core. Ensure your compilation strings include these targets in the exact following order:

* `gsKit_toolkit` & `gsKit` (High-level GS frame-buffering pipeline wrappers)
* `dmaKit` (Direct Memory Access controller channeling paths)
* `pad` (DualShock hardware controller IO device interfaces)
* `patches` (Sub-CPU IOP kernel system patch definitions)
* `mpeg` (IPU hardware macroblock decompression bindings)
* `kernel` (Core PlayStation 2 Operating System low-level system calls)

### Build Commands

Open your terminal window within the root directory containing your source code and execute the following automated directives:

```bash
# Erase old compilation binaries and object cache maps
make clean

# Process the source tree and generate a bootable MIPS ELF binary
make

```

Upon successful linkage, the toolchain will generate a compiled executable file named `bunny_burnout.elf`.

---

## 💻 Running the App on PCSX2

To test, execute, or debug your compiled project file inside the **PCSX2 Emulator** without running into black asset screens or controller timeout loops, choose one of these two standard homebrew workflows:

### Method A: Host Virtual Filesystem (Recommended for Development)

1. Open up the PCSX2 main program interface.
2. Go to **Settings > Folder Settings** (or access the individual profile settings for your homebrew ELF path).
3. Set up a virtual host directory redirection pointing your host emulator directly to the local folder containing your compiled binary and `/Images` directory.
4. Boot the `bunny_burnout.elf` execution path directly. The dynamic file-path system logic will bind to the virtual `host:` line and instantly locate your file folders.

### Method B: ISO Image Master Deployment

1. Use a standard PlayStation 2 CD/DVD master creation program (such as `cdgenPS2`) to compile your workspace files.
2. Place the `bunny_burnout.elf` file and its accompanying `/Images` subfolder directly into the **root sector** of the new file image structure.
3. Export the file architecture into a standard `.iso` format file.
4. Mount the generated image inside PCSX2 via **System > Start File / ISO**. The virtual console system will initialize and interpret the workspace files cleanly out of the optical disc driver mount (`cdrom0:`).

---

## 🧠 VRAM & Performance Design Details

The physical Graphics Synthesizer (GS) processor inside a PlayStation 2 features exactly 4MB of embedded video memory. Because loading fifty-two bitmap images into video buffers all at once would result in an immediate out-of-memory crash, this engine handles graphics dynamically:

1. **System Memory Buffering:** All bitmaps are initialized with the `.Delayed = 1` flag parameter set, locking them safely inside the main system EE RAM on boot.
2. **Dynamic VRAM Cache Swapping:** During the active rendering loops, the texture engine reads only the texture elements that are visible within the active rendering screen view.
3. **On-The-Fly Binding:** Calling `gsKit_TexManager_bind` dynamically uploads the requested bitmap blocks into VRAM right before drawing the primitives, dropping unused cache lines automatically when new texture pointers require the space.

---

## 📜 License & Credits

* **Project Architecture:** Developed using the open-source **PS2DEV SDK** ecosystem libraries.
* **Media Assets:** *Bunny Maloney* and its visual elements remain the intellectual property of their original animation studios, authors, and production networks. This software engine is built strictly for non-profit, homebrew fan project testing.

```

```
