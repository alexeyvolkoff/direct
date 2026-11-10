# On My Disk Developer's Guide

Welcome to the On My Disk developer documentation. This guide will help you set up your development environment, build the project, and configure advanced features.

## 1. Environment Setup

### 1.1 Installation on Ubuntu 24.04 (Noble)

To install all necessary dependencies from the official Ubuntu repositories, follow these steps:

1. **Enable the Universe Repository**:
   Many Qt5 packages and Node.js are located in the universe repository.
   ```bash
   sudo add-apt-repository universe
   sudo apt update
   ```

2. **Install Build Tools and Backend Dependencies**:
   ```bash
   sudo apt install build-essential git qtbase5-dev qtbase5-dev-tools \
       libqt5webchannel5-dev libqt5websockets5-dev \
       libqt5sql5-sqlite libtag1-dev libssl-dev
   ```

3. **Install Frontend Build Dependencies**:
   The Web UI requires Node.js and npm.
   ```bash
   sudo apt install nodejs npm
   ```

### 1.2 Requirements
- **Qt 5.15**: The project is built using Qt 5.15. It is recommended to use the version provided by your distribution (e.g., `qtbase5-dev` on Ubuntu).
- **C++ Compiler**: GCC or Clang.
- **Git**: For version control.

### 1.3 Recommended IDEs
- **VSCode**:
    - Install the **C/C++** extension by Microsoft.
    - Install the **Qt Tools** extension for better integration.
    - Use the provided `.vscode` tasks for building and debugging.
- **Antigravity / Cursor**:
    - Full support for AI-assisted coding.
    - Integrated terminal for running build commands.
- **Qt Creator**:
    - The native IDE for Qt development. Open `Gateway.pro`, `Node.pro`, etc., directly.

---

## 2. Building the Project

The project consists of a C++ backend and an AngularJS frontend. The frontend is embedded into the backend binaries as Qt resources, so it **must be built first**.

### 2.1 Building the Frontend (svar)
The Web UI (PWA) is built from the `svar/` directory.

```bash
cd svar
npm install
npm run build   # vite build + compress-assets.mjs
cd ..
```

### 2.2 Building the Gateway
The Gateway (server) provides access to data shared by clients.
```bash
qmake Gateway.pro
make -j$(nproc)
```
The binary will be located in `run/bin_d/omd-server` (debug) or `run/bin/omd-server` (release).

### 2.3 Building the Node Client
The embedded node client (connector / router flavour) for devices like Raspberry Pi.
```bash
qmake Node.pro
# or, for the router/InnBox flavour:
qmake DEFINES+=ROUTER Node.pro
make -j$(nproc)
```
The binary will be located in `Node/target/usr/bin/onmydisk`.

---

## 3. Running and Debugging

### 3.1 Running the Server
To run the server in debug mode with a local configuration:
```bash
./run/bin_d/omd-server -c ../etc/config/local.conf
```
*Note: Ensure the configuration paths in `local.conf` are correct relative to your working directory.*

### 3.2 Running the Embedded Client
To run the embedded client:
```bash
LD_LIBRARY_PATH=$PWD/Node/target/usr/lib ./Node/target/usr/bin/onmydisk
```

### 3.3 Default Credentials
- **Username**: `tester`
- **Password**: `testerarts`
- **Server UI**: http://localhost:8080
- **Client UI**: http://localhost:8081

---

## 4. Advanced Client Configuration

The Embedded Client supports integration with several advanced services to enhance privacy and functionality.

### 4.1 PeARS (the People's Search Engine)

PeARS provides decentralized search capabilities, allowing you to index and search documents across your private cloud.

1.  **Clone the repository**:
    ```bash
    git clone https://github.com/PeARSearch/PeARS-OMD ../PeARS-OMD
    ```
2.  **Setup Virtual Environment**:
    ```bash
    cd ../PeARS-OMD
    python3 -m venv env
    source env/bin/activate
    pip install -r requirements.txt
    ```
3.  **Configuration**:
    - Copy `pears.ini.example` to `pears.ini`.
    - Set your local search token in `pears.ini` to match the one in your On My Disk `local.conf`.
4.  **Run PeARS**:
    ```bash
    python3 run.py
    ```
5.  **Enable Integration**:
    - In the Embedded Client UI, go to **Settings** -> **Integrations**.
    - Enable **Local PeARS server**.
    - Set **Base URL** to `http://localhost:9090`.
    - Set the **API token** to the one you defined in your `pears.ini`.

### 4.2 On My Chat (AI Assistant)

On My Chat is the AI assistant backend that integrates with Ollama and On My Disk.

1.  **Clone the repository**:
    ```bash
    git clone git@forge.disk.direct:onmydisk/onmychat.git ../onmychat
    ```
2.  **Setup Virtual Environment**:
    ```bash
    cd ../onmychat
    python3 -m venv venv
    source venv/bin/activate
    pip install -r requirements.txt
    # Optional: Install playwright for Web Search/URL Scraping support
    # playwright install
    ```
3.  **Configuration**:
    - Copy `config.example.ini` to `config.ini`.
    - Edit `config.ini`:
      - Set `OLLAMA_URL` (default: `http://localhost:11434`).
      - Set `APP_ROOT_DIR` to the absolute path of the `onmychat` directory.
4.  **Ollama Setup**:
    - Ensure Ollama is installed and running.
    - Pull the required models:
      ```bash
      ollama pull gemma3:12b
      ollama pull functiongemma
      ```
5.  **Run On My Chat**:
    ```bash
    python3 api.py
    ```

#### 4.2.1 Image Generation (Creations Mode)

To enable image generation in the **Creations** tab:

1.  **Install ComfyUI**: Clone and install from [comfyanonymous/ComfyUI](https://github.com/comfyanonymous/ComfyUI).
2.  **Workflow**: Use the `flow.json` provided in the `onmychat` repository.
3.  **Config (`config.ini`)**:
    *   `COMFY_API_URL`: `http://localhost:8188` (default).
    *   `COMFY_INPUT_DIR` / `COMFY_OUTPUT_DIR`: Absolute paths to your ComfyUI input/output directories.
    *   **Style Models**: Set `REALISTIC_MODEL`, `FANTASY_MODEL`, etc., to the exact filenames of your `.safetensors` models.

#### 4.2.2 Open Code (Code Mode)

Code mode provides an interactive coding sandbox.

1.  **Install Open Code**: Clone the Open Code repository and ensure the service is running.
2.  **Config (`config.ini`)**:
    *   `CODE_BASE_URL`: `http://localhost:4096` (default).
    *   `CODE_MODEL`: The Open Code backend uses your main assistant model. Ensure it is set to a model with tool-calling capabilities (e.g., `gemma4-32k:latest` or `qwen3:8b-32k`).

6.  **Enable Integration**:
    - In the Embedded Client UI, go to **Settings** -> **Integrations**.
    - Enable **On My Chat**.
    - Set **Base URL** to `http://localhost:8000`.

> [!NOTE]
> If this configuration is missing, the default Bineon´s public AI node will serve your AI requests, with some restrictions applied for guest users.

### 4.3 Jekyll (Static Site Hosting)
Jekyll allows users to host static websites directly from their On My Disk storage.

#### Setup Jekyll:
1. **Install Jekyll**:
   ```bash
   sudo apt install jekyll
   ```
2. **Site Service**:
   The "Sites" service manages multiple Jekyll instances. The management script is located in:
   `Node/service/lib/onmydisk/sites/serve-all.sh`
   
   This script reads the `sitesRoot` path from `/etc/onmydisk/onmydisk.conf.user` and starts a Jekyll server for each directory containing a `_config.yml` file.

3.  **Running Locally**:
    For development, you can manually run Jekyll for a specific site:
    ```bash
    cd /path/to/your/site
    bundle exec jekyll serve --port <port>
    ```
    The pre-configured templates for new sites are located in `filemanager/templates/jekyll`.

4.  **Enable Integration**:
    - In the Embedded Client UI, go to **Settings** -> **Integrations**.
    - Enable **Host your sites on this device**.
    - Set the **Sites folder** to the absolute path where your sites are located.

---

## 5. Project Structure Overview
- `Node/`: Core logic for server and clients.
- `OnMyDiskLink/`: Network communication layer (TCP/SSL and WebSockets).
- `filemanager/`: Frontend Web UI (AngularJS).
- `Docs/`: Documentation including `ARCHITECTURE.md` and `USER_GUIDE.md`.
