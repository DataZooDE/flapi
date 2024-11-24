---
sidebar_position: 2
---

# Installation

There are several ways to install and run flAPI:

## Using Docker (Recommended)

The easiest way to get started is using our official Docker image:

```bash
docker pull ghcr.io/datazoode/flapi:latest
```

## Building from Source

### Prerequisites

- C++ compiler with C++17 support
- CMake 3.15 or higher
- Ninja build system (optional but recommended)

### Build Steps

1. Clone the repository:
```bash
git clone --recurse-submodules https://github.com/datazoode/flapi.git
```

2. Build the project:
```bash
make release
```

The binary will be available in `build/release/flapi`. 