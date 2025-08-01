# CycleOptima Express Server

This Express.js server provides API endpoints for the CycleOptima ESP32 project.

## Features

- **RESTful API** for ESP32 communication
- **CORS enabled** for cross-origin requests
- **Static file serving** for SPIFFS content
- **Health monitoring** endpoints
- **Request logging** with Morgan
- **Error handling** middleware

## Installation

```bash
npm install
```

## Usage

### Development

```bash
npm run dev
```

### Production

```bash
npm start
```

The server will start on port 3000 by default (configurable via PORT environment variable).

## API Endpoints

### General

- `GET /` - Server status and information
- `GET /health` - Health check endpoint

### ESP32 Communication

- `GET /api/esp32/status` - Get ESP32 connection status
- `POST /api/esp32/update` - Receive updates from ESP32
- `GET /api/config` - Get configuration data
- `POST /api/data` - Receive data from ESP32

### File Serving

- `GET /spiffs/*` - Serve files from the spiffs directory
- `GET /api/input` - Get input.json configuration

## Project Structure

```
├── server.js          # Main Express server
├── package.json       # Node.js dependencies and scripts
├── spiffs/           # Static files (served at /spiffs)
│   └── input.json    # Configuration file
└── main/             # ESP32 source code
    └── main.c
```

## Environment Variables

- `PORT` - Server port (default: 3000)

## Development

The server includes:

- Hot reloading with nodemon
- Request logging
- Error handling
- CORS support for frontend development

## ESP32 Integration

This server is designed to work with the ESP32-C3 project in the `main/` directory. The ESP32 can:

- Send sensor data to `/api/data`
- Get configuration from `/api/config`
- Check server status via `/api/esp32/status`
- Access files from the SPIFFS partition via `/spiffs/*`
