# AEMBER 🚀🔥

AEMBER is a modular, efficient, and production‑ready system designed to running cutting edge container applications in constrained enviroments.

Building embedded systems wastes time on infrastructure instead of features. Before running application logic, developers lose hours bootstrapping init systems, wiring services, debugging startup failures, and fighting tooling never designed for constrained or early-boot environments. AEMBER is a developer-first PID1 (init system) that eliminates this overhead by providing a modern C++ runtime for process supervision, container orchestration, and service management - letting you focus on your application, not your plumbing.

# Quick Start

## 📦 Pre‑requisites

Before building or running AEMBER, ensure you have:

- 🐧 **Linux**
- 🐳 **Docker**
- 🖥️ **Visual Studio Code** with the **Dev Containers** extension installed

Run the following and follow the instructions:

```sh
./tools/scripts/prerequisites.sh 
```

## 🐳 Inside DevContainer

You can use Visual Studio Code bash terminal to execute commands.

All commands start with aember and auto completion should work out of box.

If not, type the following:

```sh
aember --install-completion
```

After this step, restart bash and it should work.

### 🔨 Building

To build, run the following (Artifacts and resources will be downloaded, takes around 5min for first time):

```sh
aember qemu build
```

### ▶️ Launching

To execute, run the following

```sh
aember qemu launch
```

### 📚 Documentation

Generate and view Documentation

```sh
aember generate-docs
aember view-docs
```