# Rename Sanitize (C++17) — Recursive Filename Cleaner

This project is a small **C++17 console tool** that recursively scans a folder and **renames files and folders** to produce clean, readable names.

It:
- Replaces `_`, `-`, and `.` with spaces (while keeping the final dot for file extensions).
- Removes common **release / download tags** (e.g., `BluRay`, `DVDRip`, `WEB-DL`, `MULTI`, `VFF`, etc.).
- Removes audio/channel patterns like `DDP2.0` (often split into `DDP2 0`) and `6CH`.
- Keeps apostrophes only when they are **inside words** (e.g., `D'Après`), removes them otherwise.
- Applies **sentence case**: first letter uppercase, the rest lowercase.
- Avoids Windows “(1)” suffix when only changing case (two-step rename).
- Never renames the currently running executable.

---

## Requirements

- **Visual Studio Community 2022**
- C++ standard: **C++17**
- Windows recommended (works on Linux/macOS too with the provided self-exe detection)

---

## Build (Visual Studio 2022)

1. Open the solution (`.sln`) in **Visual Studio 2022**.
2. Select configuration:
   - `Release` (recommended) or `Debug`
   - `x64` (recommended) or `x86`
3. Build:
   - `Build` → `Build Solution`

The executable will be generated under something like:
- `x64/Release/rename_sanitize.exe`
- `x64/Debug/rename_sanitize.exe`

---

## Usage

Run the tool from a terminal (PowerShell / CMD):

```bash
rename_sanitize.exe "C:\Path\To\Your\Folder"