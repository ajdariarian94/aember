# AEMBER 🚀🔥

AEMBER is a modular, efficient, and production‑ready system designed to running cutting edge container applications in constrained enviroments

## 📦 Pre‑requisites

Before building or running AEMBER, ensure you have:

- 🐧 **Linux**
- 🐳 **Docker**


## 🏗️ Building Docker Image

AEMBER ships with a ready‑made Docker environment containing everything.

```sh
python3 init.py build_docker
```

After the build succeeds:

```sh
python3 init.py run
```

This drops you into a fully configured environment.

## 🛠️ Configuring

Before building, configure the environment:

```sh
aember configure
```

## 🔨 Building

To build, run the following:

```sh
aember build
```


## 📦 Packing

To pack, run the following:

```sh
aember pack
```

## ▶️ Executing

To execute, run the following

```sh
aember execute
```

## 🧹 Linting & Cleaning

Check code style and lint issues:

```sh
aember lint
```

Clean build artifacts:

```sh
aember clean
```
