# VBLIS-vlm-based-local-image-search

> [!WARNING]
> A large portion of the code for this software is AI-generated, guided and supervised by human input, and manually tested.

VBLIS is a powerful, privacy-focused local image search application that uses advanced Vision-Language Models (VLMs) like SigLIP-2 and CLIP to index and search your local image and video library semantically.

![VBLIS main UI screenshot](demo-pic-main-ui.png)

## About This Fork

This repository is a fork of [opoeci6/VBLIS-vlm-based-local-image-search](https://github.com/opoeci6/VBLIS-vlm-based-local-image-search). It is based on upstream v0.7.0 and keeps its search engine, models, server mode and replica mode, but replaces the Electron desktop host with a native shell on Linux and macOS:

- The desktop build targets Linux x86-64 and macOS (Apple Silicon and Intel). The Electron app, Windows installer and Electron-based packaging are not included; upstream releases are not used here.
- The React/Vite frontend lives in `vsite/` (upstream calls it `app/`) and is built into static files.
- A small static shell in `quickjs/` serves the UI on `127.0.0.1:11120` and starts the Python backend on `127.0.0.1:8000`. The shell passes the actual API address to the frontend at runtime.
- The desktop backend only accepts loopback requests and does not use the upstream shared API token. Server mode keeps upstream accounts, sessions, TLS and per-account permissions.
- The desktop app has its own folder picker (`GET /fs/list`), so folders can be added without Electron.
- Releases are built by the Linux workflow as a `VBLIS-linux-x86_64-<version>.tar.gz` bundle and a `VBLIS-web-<version>.zip`. macOS builds are made from source on the Mac (see [macOS Desktop Build](#macos-desktop-build)).

For building from source, see [Linux Desktop Build](#linux-desktop-build) or [macOS Desktop Build](#macos-desktop-build). For server and replica deployment, see [SERVER.md](SERVER.md).

## Features

- **Semantic Search**: Search your images and videos using natural language (e.g., "sunset at the beach" or "a cat sitting on a laptop").
- **Image-to-Image Search**: Find similar images by uploading or dragging an image into the search bar.
- **Video Search**: Videos are searched by their frames. Each video counts as one result and brings its best-matching frames; **Find more frames** shows the rest. Videos recorded in portrait on a phone are shown and searched upright.
- **Browse the Library**: Press Search with an empty box to see everything you've indexed, sorted by the date taken, newest or oldest first.
- **Search Filters**: Narrow any search, or the library view, to images or videos, chosen folders, a date range, a place, orientation, file format, a minimum resolution, or duplicates.
- **Place Names**: Photos and videos with GPS data show where they were taken (the city, its region, and the country or region), and the Place filter finds them by that. The lookup is built in and works offline.
- **Duplicate Detection**: Results show badges for exact copies and for images that look alike, counted across the whole library. The Duplicates filter lists only those.
- **Meaningful Scores**: With SigLIP models, a text search shows how sure the model is that a result fits your words; an image search shows how visually close each result is.
- **Local Indexing**: Rapidly index large folders of images and videos locally. All data stays on your machine.
- **Fast Re-sync**: Re-syncing a folder only processes new or changed files, so an unchanged folder is checked in a fraction of a second.
- **Index Caching**: The index is cached on disk and can be refreshed or removed in the settings. Thumbnails are also cached for faster loading.
- **Multiple Model Support**: Choose between SigLIP 2 and CLIP models (see [Models](#models)).
- **GPU Acceleration**: Built-in support for NVIDIA GPUs via CUDA and Apple GPUs via Metal (MPS) for fast inference. Without one, VBLIS runs on the CPU.
- **HEIC/HEIF and AVIF Support**: Full support for modern image formats commonly used on high-end cameras, iPhones and the web.
- **Two Interface Styles**: A modern style and a terminal style, each in dark and light mode.
- **English and Simplified Chinese**: The interface follows the system language, or the language you choose in the settings.
- **Server Mode**: Use your library from browsers on other devices, phones included, with an account for each person and per-account permissions. Turn on **Share on my network** in the desktop app's Settings, or run VBLIS as a server on Linux (also in Docker) or Windows. See [SERVER.md](SERVER.md).
- **Replica Mode**: Index on your PC, export the index into one file, and import it on a small machine (an Orange Pi, a NAS, a server) that then serves the library read-only without indexing anything itself. See [SERVER.md](SERVER.md).

## System Requirements

- The desktop shell runs on Linux x86-64, or on macOS 12.3 or newer (Apple Silicon and Intel). The upstream Windows installer is not part of it.
- Server mode also runs on a 64-bit Linux (x86-64 or ARM64) with Python 3.11 to 3.14, or in Docker. See [SERVER.md](SERVER.md).
- A GPU is optional. On Windows/Linux VBLIS uses an NVIDIA GPU through CUDA (a GeForce GTX 16 or RTX 20-series card or newer, with NVIDIA driver 580 or newer); on macOS it uses Apple's Metal backend (MPS) on any Mac that supports it. Without one, indexing runs on the CPU (slower), and Settings says why. See [NVIDIA GPUs on Linux](#nvidia-gpus-on-linux) and [Apple GPUs on macOS](#apple-gpus-on-macos).
- Disk space: about 3.7 GB for the app, plus 0.6–4.3 GB for each downloaded model, plus the index and the thumbnail cache.

## Linux Desktop Build

This branch replaces the Electron host with a small QuickJS shell: a static musl binary that serves the
built frontend on `127.0.0.1:11120` and starts the Python backend on `127.0.0.1:8000`. The frontend lives
in `vsite/` and is packaged into the shell.

Prerequisites: Node.js 20.19+ (22 recommended), Python 3.11–3.14, `cmake`, `gcc` and `musl-gcc`.

```bash
./build_linux.sh                 # frontend + shell + backend sources + smoke test
./build_linux.sh --binary        # also freeze the backend with PyInstaller
```

The result is `dist/vblis-linux-x86_64/`; start it with `./run.sh` inside that folder. The release
workflow builds the same bundle and publishes it with the web interface as a separate zip. On first run the
launcher creates a virtualenv under `~/.local/share/vblis` and installs `api/requirements.txt`
(PyTorch cu130 by default; NVIDIA driver 580+ is needed for GPU, CPU otherwise). Server mode still works
as described in [SERVER.md](SERVER.md).

## macOS Desktop Build

The same QuickJS shell builds on macOS: `quickjs/build.sh` detects Darwin and produces `app-macos`
instead of the static `app-linux`. The PyPI macOS wheels of PyTorch carry Metal (MPS), so no CUDA
toolchain is involved.

Prerequisites:

- macOS 12.3 or newer (Metal support in PyTorch; older systems build but run on the CPU)
- Xcode Command Line Tools: `xcode-select --install`
- `cmake`: `brew install cmake`
- Node.js 20.19+ (22 recommended): `brew install node`
- Python 3.11–3.14: `brew install python@3.12`, or python.org

```bash
./build_macos.sh                 # frontend + shell + backend sources + smoke test
./build_macos.sh --binary        # also freeze the backend with PyInstaller
```

The result is `dist/vblis-macos-<arch>/` (for example `dist/vblis-macos-arm64/`); start it with
`./run.sh` inside that folder. The first run creates a virtualenv in
`~/Library/Application Support/VBLIS/venv` and installs the backend dependencies from PyPI
(`torch` from PyPI, not the cu130 index). Nothing needs to be installed as root.

The shell is code-signed ad hoc (`codesign --force --sign -`) by the build, so Gatekeeper does not
treat it as a modified download. On a machine that only tests a local build, no notarization is
needed.

GPU notes:

- The backend picks the accelerator itself: CUDA on Windows/Linux, Metal on macOS. Settings →
  **Models and GPU** shows the detected chip (for example `Apple M2 Pro (Metal)`) and lets you turn
  acceleration off.
- The default precision on Metal is **FP32**; choosing **FP16** in the settings raises throughput and
  lowers memory use (the text tower is then kept half precision, like on CUDA). Auto batch sizing
  reads the recommended Metal memory budget, so 16 GB machines get smaller batches than NVIDIA cards.
- Operators that Metal does not implement run on the CPU instead of failing the batch
  (`PYTORCH_ENABLE_MPS_FALLBACK=1` is set by the backend). This is correct but can slow individual
  images; the log announces it.
- Intel Macs without Metal (or macOS older than 12.3) run on the CPU only; Settings then shows no
  accelerator, not an error.

## Apple GPUs on macOS

Check what PyTorch sees before the first launch (use the virtualenv from the build output):

```bash
"$HOME/Library/Application Support/VBLIS/venv/bin/python" -c "import torch; print(torch.backends.mps.is_built(), torch.backends.mps.is_available())"
```

Both should print `True`. Common problems:

- **`is_available()` is False.** macOS is older than 12.3, the machine has no Metal-capable GPU, or a
  CPU-only PyTorch wheel was installed (`TORCH_INDEX` or a pinned `constraints.txt` can cause that).
- **Indexing is slower than expected, or the log says an operator fell back to CPU.** Some vision
  operators are not implemented on Metal; they run on the CPU and the rest stays on the GPU. Smaller
  batches and the base model are faster than the 4.3 GB SO400M model on 8 GB machines.
- **The process is killed while loading a 4.3 GB model.** Metal shares system RAM; close other memory
  hungry apps, or use `siglip2-base-patch16-naflex` (1.5 GB). VBLIS falls back to the CPU when Metal
  refuses the model, and Settings reports `mps_failed`.
- **Data directory.** Everything (models, index, thumbnails, settings) lives in
  `~/Library/Application Support/VBLIS`; set `VBLIS_DATA_DIR` to move it.

## NVIDIA GPUs on Linux

The default environment installs PyTorch built for CUDA 13.0 (`cu130`), which needs NVIDIA driver **580 or newer**. Only the driver is required: the CUDA runtime libraries are included in the PyTorch wheels, so do not install a separate CUDA toolkit or cuDNN for VBLIS.

Check the driver and GPU before the first launch:

```bash
nvidia-smi
```

It should list the GPU and a driver version of 580 or newer. If it fails, the kernel driver is not loaded, the GPU is not visible, or Secure Boot rejected the module.

Common problems:

- **Driver older than 580.** VBLIS still runs, on the CPU; Settings → **Models and GPU** explains why the GPU is unavailable. Install a 580+ driver from your distribution (Arch: `nvidia` or `nvidia-open`; Ubuntu: `nvidia-driver-580` or `sudo ubuntu-drivers install`; Fedora: RPM Fusion), then reboot.
- **"Driver/library version mismatch" after a driver upgrade.** Reboot, or unload and reload the NVIDIA kernel modules, so that running processes release the old driver.
- **`torch.cuda.is_available()` is false, or PyTorch reports "CUDA driver version is insufficient for CUDA runtime version".** The driver is missing, too old, or not loaded. Check `nvidia-smi`, `lsmod | grep nvidia` and `dmesg`.
- **Secure Boot.** The NVIDIA kernel module must be signed (DKMS with MOK enrollment); otherwise it will not load even when the driver package is installed.
- **Laptops with hybrid graphics.** CUDA uses the discrete NVIDIA GPU when `nvidia-smi` lists it. If it is not listed, check the BIOS/UEFI settings and your distribution's PRIME configuration.
- **Unsupported GPU.** CUDA 13 supports GeForce GTX 16 and RTX 20 series or newer; older cards such as the GTX 10 series run on the CPU.
- **Docker or server mode.** A GPU inside a container needs the NVIDIA Container Toolkit and `--gpus all`; see [SERVER.md](SERVER.md).

## Supported Formats

- **Images:** JPEG, PNG, WebP, BMP, HEIC/HEIF, AVIF
- **Videos:** MP4, M4V, MOV, MKV, AVI, WebM

## Models

| Model | Size | Good for |
|---|---|---|
| `google/siglip2-so400m-patch16-naflex` | 4.3 GB | The best results; recommended with an NVIDIA GPU or a Mac with 16 GB+ unified memory |
| `google/siglip2-so400m-patch14-384` | 4.3 GB | The same size, with a fixed square input |
| `google/siglip2-base-patch16-naflex` | 1.5 GB | CPU, smaller GPUs, and 8 GB Macs |
| `google/siglip2-base-patch16-224` | 1.5 GB | CPU, smaller GPUs and 8 GB Macs, with a fixed square input |
| `openai/clip-vit-base-patch32` | 0.6 GB | The lightest option. Use English for best search results |

The `naflex` models look at each image in its own aspect ratio; the others resize it to a square first. Each model keeps its own index, so after switching to another model, index your folders again for it.

## How to Use

1. **Download & Run**: From the [Releases](https://github.com/YCL0609/VBLIS-vlm-based-local-image-search/releases) page, download the latest `VBLIS-linux-x86_64-<version>.tar.gz`, extract it into a folder you can write to, and start `./run.sh` (or the `vblis` binary). The first launch creates a Python environment in `~/.local/share/vblis/venv` and downloads PyTorch and the backend dependencies; it needs Python 3.11–3.14, network access and a few GB of disk. Later launches reuse it. On macOS the release bundle is not built yet: build it on the Mac with [macOS Desktop Build](#macos-desktop-build), then start `./run.sh` from `dist/vblis-macos-<arch>/`; the environment goes to `~/Library/Application Support/VBLIS/venv`.
For a server on Linux or in Docker, follow [SERVER.md](SERVER.md) instead. The release's `VBLIS-web-<version>.zip` holds the web interface, so the server needs no Node.js.
> [!NOTE]
> The Linux bundle isn't code-signed. To check a download, run `sha256sum -c SHA256SUMS.txt` in the folder with the downloaded files.
2. **Select a Model**: On first launch, choose a model and download it (a SigLIP 2 model is highly recommended; see [Models](#models)).
3. **Add Folders**: Click **Add Folder(s)** or drag and drop folders into the app to start indexing your library. VBLIS indexes all the subfolders of each folder.
4. **Search**: Once indexing is complete, type a query in the search bar or drag an image into the search area to find what you're looking for.
> [!TIP]
> VBLIS partially supports search while indexing. You can search while the app is indexing your folders, but the results will be limited to the images that have been indexed so far.
5. **GPU Settings**: If you have an NVIDIA GPU (CUDA) or an Apple GPU (Metal), ensure GPU acceleration is enabled in the settings (**Models and GPU**) for optimal performance.
6. **Result Filtering**: In the settings you can adjust how many results to display (a video counts as one result and brings its best frames; open it and use **Find more frames** to see all of its matching frames) and two minimums: a **match score** for text searches (with SigLIP models, how sure the model is that a result fits your words) and a **similarity** for image searches (how visually close a result is to the dropped image). VBLIS remembers these three settings; **Reset to defaults** restores them. The library view always shows everything.
7. **Search Filters**: The **Filters** button next to the search bar narrows any search to images or videos, chosen folders, a date range (the date a photo or video was taken, or the file's modified date when it has none), a place (a country or region, or a region or city inside it, from the GPS data of the file), orientation, file format, a minimum resolution, and duplicates (exact copies or images that look alike). Changing a filter updates the current results right away. Date, place, orientation and resolution come from media details that VBLIS reads while indexing; for folders indexed with an older version, use **Read details now** in the Filters panel. Places are matched to the nearest known town or city without boundary data, so a photo taken close to a border or in a suburb can be named after the neighbouring place.
8. **Model Management**: You can download, switch, and delete models in the settings.
9. **Index and Cache Management**: You can clear the index and cache in the settings (**Library**).
10. **Language**: Choose English or Simplified Chinese in the settings, or let VBLIS follow the system language. Searching works best with English queries; in Chinese, use common words.
11. **Share on Your Network**: Turn on **Share on my network** in the settings, add an account for each person with **Manage accounts**, and open one of the addresses it shows in a browser on a phone or another computer. Sharing lasts while the app is open. See [SERVER.md](SERVER.md) for HTTPS, permissions and running VBLIS as a server.
12. **Copy the Index to Another Device**: In the settings, **Library** → **Export for another device** writes the index into one file. Import it on the other machine with `import-index`, which maps your PC's folders to where the pictures are there; see [SERVER.md](SERVER.md).

## Upgrading

- Your index, downloaded models and settings are kept. On Linux they live in `~/.local/share/vblis` by default (set `VBLIS_DATA_DIR` to change it); the upstream Windows installer uses `%LOCALAPPDATA%\VBLIS`.
- After upgrading from v0.2.0 or older, re-sync your folders once (the **Re-sync** button of each folder in the Indexed Folders panel). That picks up AVIF files, and turns portrait videos and photos that older versions indexed sideways upright. Only those files are embedded again.
- If some HEIC photos showed up turned in v0.4.0 or older, re-sync their folder once and use **Clear Image Cache** in the settings. Only those photos are embedded again.
- Upgrading from v0.5.0 or newer needs no re-indexing: the new PyTorch in v0.7.0 gives the same search results. Sharing stays off until you turn on **Share on my network**.
- Since v0.7.0 the GPU needs NVIDIA driver 580 or newer. With an older driver VBLIS uses the CPU, and the settings (**Models and GPU**) say so; update the driver to use the GPU again.
- The date, place, orientation and resolution filters need media details that older versions didn't read. VBLIS reads them in the background the first time you open the Filters panel; no re-indexing is needed.

## TODO / Roadmap

- [x] Support for more Vision-Language Models.
- [x] Enhanced GPU acceleration settings (quantization, memory management).
- [ ] Support for non-NVIDIA GPUs (MPS for Mac, Vulkan/DirectML for AMD/Intel).
- [x] Interface translations (i18n): English and Simplified Chinese.
- [x] Browse the library without a search query.
- [x] Place names and a location filter, from the GPS data in photos and videos.
- [x] Advanced filtering (by date, folder, or image properties).
- [x] Image-to-Video searching.
- [x] Server mode: use the library from a browser on other devices, phones included, and run it on Linux or in Docker.
- [x] Replica mode: index on a PC and search a copy of the index on a small machine.
- [x] NVIDIA RTX 50-series GPUs.

## Credits

- Place names: [GeoNames](https://www.geonames.org/), licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The Chinese names were converted to Simplified Chinese with OpenCC.
