# DbxCli-Simulator

A lightweight, interactive command-line interface (CLI) written in C that simulates `dbxcli` functionality. It allows users to seamlessly manage local filesystem navigation alongside a simulated remote Dropbox root directory.

## Features
* **Dual-Environment Navigation:** Navigate both local filesystem directories and a simulated remote Dropbox sandbox environment (`~/.dropbox_sim`) independently.
* **Standard File Operations:** Execute core filesystem commands including directory listing (`ls`), path resolution (`pwd`), directory creation (`mkdir`), removal (`rm`), copying (`cp`), and moving/renaming (`mv`) for both local and simulated remote scopes.
* **Cloud Synchronization Simulation:** Transfer files between environments using dedicated `put` (upload to Dropbox) and `get` (download from Dropbox) commands.
* **Security & Path Sandboxing:** Implements robust path resolution and boundary checks to ensure simulated remote operations cannot escape the designated Dropbox root directory.
* **Interactive Shell Loop:** Custom command parser handling arguments, tokenization, error feedback, and colored terminal prompts.

## Tech Stack
* **Language:** C 
* **System Programming:** POSIX standard libraries (`unistd.h`, `sys/stat.h`, `dirent.h`, `ftw.h`)

