# BunnyMaloneyPS2Player - Bunny Maloney Watch-On!
A PS2 application made with PS2SDK to play all the episodes from the show, Bunny Maloney!

An interactive homebrew video player and media selection interface designed for the PlayStation 2 console, centered around the animated series *Bunny Maloney*. Built from scratch in C using the native open-source **PS2SDK** toolchain, this project utilizes hardware-accelerated graphics via **gsKit** and is engineered for robust execution on physical hardware and modern emulators alike.

---

## 🚀 What This Is
This project is a custom-built PS2 homebrew application that acts as a digital dashboard and browser for the show. 
* **Custom Boot Timeline:** Features a smooth, cinematic intro with real-time loading bars and disclaimer screens.
* **Interactive Menu System:** Smooth navigation between the Title Screen, Main Options, and the Episode Selector.
* **52-Episode Browser:** A fully functional grid where you can scroll through all 52 custom episode thumbnails. It uses smart VRAM management so the PS2 can swap textures on-the-fly without running out of memory or crashing.
* **Max Compatibility:** Fully optimized with controller and hardware fixes to ensure it never freezes on a black screen, whether you are playing on a real console or an emulator.

---

## 🎮 How to Setup and Play

You do not need to compile anything. To play the application, just follow these simple steps to move the game files over:

### 📁 1. Match the Folder Structure
Ensure the compiled executable file (`bunny_burnout.elf`) and your `Images` folder are kept right next to each other. Your folder structure must look exactly like this:

```text
📦 Bunny Maloney Watchon PS2/
 ┣ 📜 bunny_maloney_watchon.elf
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
   ┗ 📜 ... (Sequentially named through thumb_ep52.bmp)

```

---

### 🕹️ 2. Choose Your Platform

#### Method A: Playing on a Real PS2 Console (Via USB)

1. Format a standard USB flash drive to **FAT32**.
2. Drop the `bunny_burnout.elf` file and the accompanying `Images` folder directly onto the root of your USB drive.
3. Plug the USB drive into your PlayStation 2.
4. Boot up your console and launch **uLaunchELF** (via FreeMcBoot, MechPawn, or a homebrew disc).
5. Navigate to `mass:/` (this is your USB drive).
6. Select and launch `bunny_burnout.elf` to start the app!

#### Method B: Playing Locally on PC (Via PCSX2 Emulator)

1. Open up your **PCSX2** emulator.
2. Before anything, go to Settings, Emulation and tick the box "Enable Host Filesystem" in order to get the textures working from the ELF, otherwise nothing will show or the application will be incomplete!
3. Now go to **System > Start File** (or File > Open ELF depending on your emulator version).
. Select the `bunny_burnout.elf` file on your computer.
. The emulator will automatically read the `Images/` folder from the same directory on your PC and boot the application instantly.

---

## 🕹️ Controls

The app uses standard PlayStation 2 DualShock 2 controller inputs:

| Button | Action |
| --- | --- |
| **D-Pad Up / Down** | Navigate menu options and browse up/down through the 52 episodes. |
| **Cross (❌)** | Confirm menu selection, open submenus, or play selections. |
| **Triangle (🔺) / Circle (⭕)** | Go back to the previous screen (e.g., exit Episode Menu back to Main Menu). |
| **Start** | Skip past the intro loading bar timeline or pass the Title Splash Screen. |

---

## 📜 Credits

* **Development Framework:** Built using the open-source **PS2SDK** ecosystem.
* **Media Assets:** *Bunny Maloney* and all characters/images are the intellectual property of their original animation studios and creators. This homebrew utility is built purely as a non-profit fan creation.

```

```
