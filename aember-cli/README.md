# AEMBER-CLI

`aember-cli` is the command-line interface for **Aember**, a lightweight build and execution system for embedded Linux environments.

It provides a single, consistent CLI to configure, build, package, run, lint, document, and release Aember-based systems.

The CLI is written in **Python**, built with **Typer**, and uses structured logging with **logging + rich**.

## Features

- 🛠️ CMake-based configure & build workflow  
- 📦 Initramfs and root filesystem packaging  
- 🐧 QEMU execution (x86_64, ARM, AArch64)  
- 🧪 Optional test binaries in initramfs  
- 🧹 Clean build artifacts  
- 📚 Generate and serve documentation (Doxygen)  
- 🧹 Lint Python, C++, and CMake code  
- 📦 Create versioned release archives  


## Usage

Run the following to learn how the cli tool works:

```bash
aember --help
```
