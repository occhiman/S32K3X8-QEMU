- # QEMU S32K3X8 Emulation with FreeRTOS Demo

  ## Description
  This project provides an emulation environment. It uses QEMU. The project focuses on the NXP S32K3X8 series microcontroller. Specifically, it targets the S32K358EVB board. The emulation includes an LPUART peripheral. It also includes an LPSPI peripheral for SPI communication. A FreeRTOS operating system runs on this emulated hardware. The FreeRTOS application demonstrates multitasking. It also shows serial communication via the LPUART. It demonstrates SPI data transfer via the LPSPI. This project helps developers. They can develop and test embedded software. They do not need physical hardware for this.

  **Key Features:**
  * The project includes a QEMU board model. This model is for the S32K358EVB.
  * It features LPUART/LPSPI peripheral emulation.
  * A FreeRTOS port for the Cortex-M7 core is available.
  * The project contains an example FreeRTOS application. This application shows multiple tasks and UART/SPI communication.
  * It provides startup code for the S32K3 microcontroller. System initialization is also included.

  ## Installation and Setup
  These steps will guide you through setting up the project. You will also build QEMU.

  1.  **Prepare Project Directory and Clone Repository**
      * First, choose or create a new directory. This directory will hold your cloned project. Let's call it `your_project_path`.
          ```bash
          mkdir your_project_path
          cd your_project_path
          ```
      * Next, clone the Git repository. Use the specified branch. Include its submodules.
          ```bash
          git clone --recurse-submodules https://baltig.polito.it/eos2024/group1.git
          ```

  2.  **Install Dependencies**
      * You need to install various build tools and libraries. Open your terminal. Run the following command:
          ```bash
          sudo apt update
          sudo apt install build-essential zlib1g-dev libglib2.0-dev \
              libfdt-dev libpixman-1-dev ninja-build python3-sphinx \
              python3-sphinx-rtd-theme pkg-config libgtk-3-dev \
              libvte-2.91-dev libaio-dev libbluetooth-dev \
              libbrlapi-dev libbz2-dev libcap-dev libcap-ng-dev \
              libcurl4-gnutls-dev python3-venv gcc-arm-none-eabi cmake git
          ```

  3.  **Build and Install QEMU**
      
      * Navigate into the QEMU directory. This directory is inside your cloned project.
          ```bash
          cd group1/qemu
          ```
          *(The `group1` directory is created by the `git clone` command above.)*
      * Configure the build. Target the ARM softmmu.
          ```bash
          ./configure --target-list=arm-softmmu
          ```
      * Compile QEMU. Then, install QEMU.
          ```bash
          make
          sudo make install
          ```
      
  4.  **Verify QEMU Installation (Optional)**
      * You can check the available ARM machine types. This helps confirm your board is listed.
          ```bash
          qemu-system-arm -M ?
          ```
          *(You should see `s32k3x8evb` or a similar name in the output list.)*

  ## MSYS2 (Windows) Findings and Workarounds
  These are the known-good findings/workarounds validated for this repository on Windows:
  * Source tree: `C:/Users/ochiman/git-repos/S32K3X8-QEMU/qemu`
  * Target: `arm-softmmu`
  * Shell: MSYS2 `MINGW64`

  1. **Install prerequisites in MINGW64**
      ```bash
      pacman -Syu
      pacman -S --needed \
        base-devel git \
        mingw-w64-x86_64-toolchain \
        mingw-w64-x86_64-python \
        mingw-w64-x86_64-python-pip \
        mingw-w64-x86_64-meson \
        mingw-w64-x86_64-ninja
      ```

  2. **Workaround A: `pycotap==1.3.1` + mkvenv wheel URI**
      * If configure fails in offline mkvenv mode (`mkvenv was configured to operate offline...`), pre-download the wheel:
      ```bash
      mkdir -p python/wheels
      PIP_NO_INDEX=0 /mingw64/bin/python3 -m pip download \
        --dest python/wheels \
        --index-url https://pypi.org/simple \
        pycotap==1.3.1
      ```
      * In `qemu/python/scripts/mkvenv.py`, ensure `--find-links` uses a proper Windows file URI:
      ```python
      full_args += ["--find-links", str(Path(wheels_dir).resolve().as_uri())]
      ```

  3. **Workaround B: Windows symlink privilege (`WinError 1314`)**
      * Preferred: enable Windows Developer Mode and rerun configure.
      * Alternative: use the fallback behavior in `qemu/scripts/symlink-install-tree.py`:
      * fall back to copy when symlink privilege is missing
      * skip missing generated-later sources (for example `trace-events-all`)

  4. **Configure with Windows-style compiler path**
      * For this tree, avoid passing POSIX compiler path (`/mingw64/bin/gcc`) to `configure`.
      ```bash
      GCC_WIN="$(cygpath -m "$(command -v gcc)")"
      rm -rf build pyvenv
      ./configure --target-list=arm-softmmu --enable-download --cc="$GCC_WIN"
      ```

  5. **If configure reports `ERROR: missing subprojects`**
      ```bash
      /mingw64/bin/meson subprojects download
      ./configure --target-list=arm-softmmu --enable-download --cc="$GCC_WIN"
      ```
      * If you need an offline build later, first make sure subprojects are downloaded.

  6. **Workaround C: `qemu_ftruncate64` undefined reference in tests**
      * Root fix (already applied in this repository):
      * remove `qemu_ftruncate64()` from `qemu/block/file-win32.c`
      * define `qemu_ftruncate64()` in `qemu/util/oslib-win32.c` so it is linked from `libqemuutil`
      * Then reconfigure and rebuild:
      ```bash
      rm -rf build
      ./configure --target-list=arm-softmmu --enable-download --cc="$GCC_WIN"
      ```

  7. **Workaround D: install fails with `dst_dir must be absolute`**
      * If install resolves to POSIX paths like `/qemu/share/doc`, switch to a Windows absolute prefix:
      ```bash
      PREFIX_WIN="$(cygpath -m /mingw64)"

      build/pyvenv/bin/meson configure build \
        -Dprefix="$PREFIX_WIN" \
        -Dbindir=bin -Dlibdir=lib -Ddatadir=share \
        -Ddocdir=share/doc -Dqemu_suffix=qemu \
        -Dmandir=share/man -Dsysconfdir=etc -Dlocalstatedir=var

      ninja -C build install
      ```

  8. **Build and install sequence**
      ```bash
      ninja -C build qemu-system-arm
      ninja -C build
      ninja -C build install
      ```

  9. **Quick troubleshooting map**
      * `Could not find pycotap==1.3.1`: pre-download wheel + use `as_uri()` in `mkvenv.py`.
      * `ERROR: missing subprojects`: run `meson subprojects download` and configure with `--enable-download`.
      * `Unknown compiler(s): [['/mingw64/bin/gcc', '-m64']]`: pass `--cc="$(cygpath -m "$(command -v gcc)")"`.
      * `WinError 1314`: enable Developer Mode or use symlink fallback behavior.
      * `error copying /qemu/share/trace-events-all`: ensure fallback script skips missing sources.
      * `undefined reference to qemu_ftruncate64`: keep function in `util/oslib-win32.c` (not `block/file-win32.c`), then clean reconfigure.
      * `dst_dir must be absolute`: set Windows absolute `prefix` and rerun install.

  ## Usage
  Follow these steps to build and run the firmware on the emulated board.

  1.  **Build and Run Firmware**
      * Navigate to the firmware directory. This is likely `Demo/Firmware` within your cloned `group1` repository.
          ```bash
          cd ../Demo/Firmware 
          ```
          *(If you are in `group1/qemu`, use `cd ../../Demo/Firmware`. If in `group1`, use `cd Demo/Firmware`.)*
      * Compile the firmware.
          ```bash
          make
          ```
      * Run the compiled firmware using QEMU.
          ```bash
          qemu-system-arm -M s32k3x8evb -nographic -kernel firmware.elf
          ```

  **Expected Output Example:**
  The console should display messages similar to these:

![image-20250525202512245](./img/UartTest.png)



![](./img/spitest.png)

## How to Add a New Device Model (S32K3X8EVB)
Use this checklist when you want to add a new emulated peripheral to this machine.

1. **Choose the subsystem and create the device files**
   - Pick the closest QEMU subsystem folder:
     - `qemu/hw/char/` for UART-like devices
     - `qemu/hw/ssi/` for SPI-like devices
     - `qemu/hw/net/can/` for CAN-like devices
     - `qemu/hw/dma/` for DMA-like devices
     - `qemu/hw/misc/` for simple MMIO support peripherals
   - Add the source file (for example `qemu/hw/misc/s32k358_mydev.c`).
   - Add a header:
     - Preferred style in this tree: `qemu/include/hw/<subsystem>/...`
     - Existing legacy models also use local headers in `qemu/hw/<subsystem>/...`

2. **Implement the QOM + SysBus skeleton**
   - Define a type macro (for example `TYPE_S32K358_MYDEV`).
   - Define your state struct (`SysBusDevice parent_obj`, `MemoryRegion`, `qemu_irq`, registers/state).
   - Implement:
     - MMIO callbacks (`read`/`write`) and `MemoryRegionOps`
     - `reset`
     - `instance_init` with `memory_region_init_io()` + `sysbus_init_mmio()` (+ `sysbus_init_irq()` if needed)
     - `class_init` with `realize`, properties, reset hook
     - `TypeInfo` + `type_init(...)`

3. **Register the model in build configuration**
   - Add a config symbol in the subsystem `Kconfig`, for example:
     - `qemu/hw/misc/Kconfig`
   - Add the source file in the subsystem `meson.build`, for example:
     - `qemu/hw/misc/meson.build`
   - If the device is in CAN subfolder, source is added in:
     - `qemu/hw/net/can/meson.build`

4. **Enable the symbol for this machine**
   - In `qemu/hw/arm/Kconfig`, make `S32K3X8_MCU` (or `S32K3X8EVB`) `select` your new symbol.

5. **Wire the device into the S32K3X8 board**
   - Update `qemu/hw/arm/s32k3_board.c`:
     - Include your header
     - Add base address and IRQ constants
     - Instantiate with `qdev_new(TYPE_...)`
     - Set required properties (`qdev_prop_set_*` / `object_property_set_link`)
     - Realize and map (`sysbus_realize_and_unref`, `sysbus_mmio_map`, `sysbus_connect_irq`)
   - If needed, store a pointer in board state (`qemu/hw/arm/s32k3x8evb.h`).
   - If you expose machine links/properties, add them in `s32k3x8evb_instance_init()`.

6. **Build and run**
   ```bash
   cd qemu
   ./configure --target-list=arm-softmmu
   make -j"$(nproc)"
   ./qemu-system-arm -M s32k3x8evb -nographic -kernel ../Demo/Firmware/firmware.elf
   ```

7. **Validate guest behavior**
   - Confirm guest register accesses hit your MMIO handlers.
   - Confirm interrupt path to NVIC is correct.
   - If your device uses links (for example CAN bus or memory), verify the property is set before realize.

**Good references in this repository:**
- `qemu/hw/net/can/s32k3x8_flexcan.c`
- `qemu/hw/dma/s32k358_dmamux.c`
- `qemu/hw/misc/s32k358_mscm.c`
- `qemu/hw/arm/s32k3_board.c`

## Project Structure
This is the main directory structure of the cloned repository (`group1/`):

```text

├── Demo/                   # Contains demo code and FreeRTOS files
│   ├── Firmware/           # Firmware source code and build files
│   ├── FreeRTOS/           # FreeRTOS kernel source (or submodule)
│   └── Headers/            # Shared project header files
├── img/                    # Contains images for documentation (e.g., README)
├── materials/              # Contains related reference materials and tools
│   ├── fmstr_uart_s32k358.zip  # FreeMASTER UART S32K358 related files
│   └── split_rm.zip        # Other material archive
├── qemu/                   # QEMU source code (includes board/peripheral modifications)
│   ├── hw/                 # QEMU hardware models
│   ├── build/              # QEMU build output directory (if built here)
│   ├── configure           # QEMU configuration script
│   ├── ...                 # Other QEMU core source, tools, and docs
│   └── README.rst          # QEMU's own README
├── LICENSE                 # Project's software license file
└── README.md               # This project description file
```

**Key Directory Explanations:**

* **`Demo/`**: This directory holds the demonstration application code. It runs on the emulated hardware.
    * `Demo/Firmware/`: Contains core firmware logic. This includes `main.c`, startup code, linker scripts, and the `Makefile` for firmware compilation.
    * `Demo/FreeRTOS/`: May contain FreeRTOS kernel source files.
    * `Demo/Headers/`: Stores common header files for the firmware project.
* **`materials/`**: This directory includes supplementary materials.
* **`qemu/`**: This is the QEMU source code directory. This QEMU version is already modified. It includes emulation code for the S32K3X8EVB board, LPUART and LPSPI peripherals.
* **`README.md`**: This file. It provides an overview, setup guide, and usage instructions for the project.

*(The `qemu/` directory is very large. The list above highlights parts relevant to this project's context. Refer to the "Installation and Setup" section for build and integration details.)*

## Roadmap
If you plan future releases, list them here. This gives people an idea of where the project is going.
* Example:
    * Emulate more S32K3X8 peripherals (I2C, CAN).
    * Integrate more complex demo applications.

## Contributing
Contributions are welcome. Clearly state your requirements for accepting contributions.
1.  Fork this repository.
2.  Create a new branch for your feature or bugfix (e.g., `git checkout -b feature/YourFeature`).
3.  Make your changes. Commit them with a clear message (e.g., `git commit -m 'Add some feature'`).
4.  Push your branch to your forked repository (e.g., `git push origin feature/YourFeature`).
5.  Create a new Merge Request.

**Development Setup:**
Document how contributors can set up their local development environment. Mention any scripts to run or environment variables to set. These instructions are helpful for others and your future self. You can also include commands for linting code or running tests.

## Authors and Acknowledgment
Authors:

* s338247 - Rebecca Burico(s338247@studenti.polito.it)
* s336721 - Yuqi Li(s336721@studenti.polito.it)

Acknowledgment:

* Stefano Di Carlo(stefano.dicarlo@polito.it): Professor 
* Carpegna Alessio(alessio.carpegna@polito.it): Co-lectures
* Eftekhari Moghadam Vahid(vahid.eftekhari@polito.it): Co-lectures
* Magliano Enrico(enrico.magliano@polito.it): Co-lectures

## License

This project, including the source code and any accompanying documents (such as presentations, reports, etc.), is distributed under the **Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0) License**.

* **Attribution (BY):** You must give appropriate credit, provide a link to the license, and indicate if changes were made. You may do so in any reasonable manner, but not in any way that suggests the licensor endorses you or your use.
* **NonCommercial (NC):** You may not use the material for commercial purposes.
