# CycleOptima ESP32 Node.js API

## Skillsets & Technologies Used

- **Node.js** (Express.js framework)
- **RESTful API design**
- **ESP-IDF** (ESP32 development framework)
- **Python/ESP-IDF toolchain integration**
- **Windows shell integration**
- **File system operations (JSON config)**
- **Serial flashing and monitoring (via idf.py)**
- **ngrok** (secure public tunneling)
- **CORS, body-parser, morgan logging**

## Features

- Flash and monitor ESP32 from a web API
- Read and update device configuration (`spiffs/input.json`) via API
- Serve static files from the `spiffs` directory
- Health check and status endpoints
- Easily expose your local API to the internet with ngrok

## How to Run

### 1. Start the Node.js API (from ESP-IDF terminal)

Open the **ESP-IDF Command Prompt** (so `idf.py` is available), then:

```sh
cd C:\Users\ladcoop3\Desktop\cycleControl\cycleOptima\cycleOptima-esp
npm install   # Only needed the first time
npm start
```

- This will start the Express server on port 3000.
- Your ESP32 must be connected to your PC (e.g., COM9).

### 2. Expose the API with ngrok (from a normal terminal)

Open a new terminal (can be any shell) and run:

```sh
ngrok http 3000
```

- ngrok will give you a public HTTPS URL (e.g., `https://xxxx.ngrok-free.app`).
- You can now access your API from anywhere using this URL.

## Example API Endpoints

- **Flash and monitor ESP32:**
  ```sh
  curl -X POST https://xxxx.ngrok-free.app/api/flash
  ```
- **Read config:**
  ```sh
  curl https://xxxx.ngrok-free.app/api/input
  ```
- **Update config:**
  ```sh
  curl -X PUT https://xxxx.ngrok-free.app/api/input \
    -H "Content-Type: application/json" \
    -d "{\"app\": {\"name\": \"CycleOptima\",\"version\": \"2.0.0\"}}"
  ```

## Notes

- Flashing and monitoring only work if the ESP32 is physically connected to the server machine.
- The monitor output may not be interactive due to TTY limitations; use a serial terminal for live monitoring if needed.
- All API endpoints are available both locally and via ngrok.

---

**Developed with Node.js, Express, ESP-IDF, and ngrok for remote device management and configuration.**
