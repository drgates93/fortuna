## `Fortuna`

A cross-platform CLI tool for creating and managing Fortran/C-based scientific projects. Inspired by build systems like Cargo, Fortuna offers a lightweight way to initialize, configure, build, and run modular Fortran/C projects using TOML config and automation.

---

### Features

* Initializes directory structure (`src`, `mod`, `obj`, etc.)
* Copies template executables and build config files
* Generates a `Fortuna.toml` for build configuration
* Supports incremental and parallel builds
* Cross-platform (Linux/Windows)
* Lightweight and Fast 
---

### Installation

#### From Source:

```bash
git clone https://github.com/drgates93/fortuna.git
cd fortuna
make install
```

> Ensure you have a C compiler and `make` installed. For Windows, use MinGW or WSL.

---

### Usage

```bash
fortean new <project-name>      # Initialize a new Fortran project
fortean build     # Build the project
fortean run      # Build and run the executable
```

#### Flags:

| Flag              | Description                        |
| ----------------- | ---------------------------------- |
| `-j`              | Enable parallel build              |
| `-r`, `--rebuild` | Disable incremental build          |
| `--bin`           | Skip build and run target bin given by name |
| `--lib`           | Force build of library only        |
| `clean`           | Clean the obj_dir and mod_dir      |
| `run`               | Re-builds as needed and runs the executable if successful |
| `new`               | Generates a new project dir with some name specified after new |

---

### Directory Structure

```
<project-name>/
├── src/           # Fortran/C source files
├── mod/           # Fortran modules (.mod)
├── obj/           # Object files (.o)
├── bin/           # Output binaries and config
├── data/          # Input or template files
├── lib/           # Libraries
└── .cache/        # Hidden cache directory
Fortean.toml
```
---

### `Fortean.toml` Example

```toml
[build]
target = "test"
compiler = "gfortran"

flags = [
  "-cpp", "-fno-align-commons", "-O3",
  "-ffpe-trap=zero,invalid,underflow,overflow",
  "-std=legacy", "-ffixed-line-length-none", "-fall-intrinsics",
  "-Wno-unused-variable", "-Wno-unused-function",
  "-Wno-conversion", "-fopenmp", "-Imod"
]

obj_dir = "obj"
mod_dir = "mod"

[search]
deep = ["src"]
#shallow = ["lib", "include"]

[library]
#source-libs = ["lib/test.lib"]

[exclude]
#Requires the relative path from the Fortean.toml file.
#files = ["src/some_file.f90"] 

[lib]
#Placed in the lib folder and only supports static linking with ar
#target = "test.lib"

[args]
#cmds = ["cmd_line_argument"] 
```

### Search Directories for files

```
[search]
deep = ["src"]
#shallow = ["lib", "include"]
```

| TOML             | Description                        |
| ----------------- | ---------------------------------- |
| `deep    = ["src"]` | Comma separated list of directories to **recursively** search for files and add to the depedency graph|
| `shallow = ["lib"]` | Comma separated list of directories to search for files and add to the depedency graph.|

### License
This project is licensed under the MIT License. See `LICENSE` for more information.
