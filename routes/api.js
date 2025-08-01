const express = require("express");
const path = require("path");
const router = express.Router();
const { SerialPort } = require("serialport");
const { ReadlineParser } = require("@serialport/parser-readline");

// ESP32 specific routes
router.get("/esp32/status", (req, res) => {
  res.json({
    status: "connected",
    device: "ESP32-C3",
    project: "CycleOptima",
    firmware_version: "1.0.0",
    timestamp: new Date().toISOString(),
  });
});

router.post("/flash", (req, res) => {
  const { spawn } = require("child_process");
  const path = require("path");
  const flash = spawn("cmd", ["/c", "idf.py -p COM7 flash monitor"], {
    cwd: path.join(__dirname, ".."),
    shell: false,
  });

  let output = "Flashing and monitoring ESP32...\n\n";
  console.log("Flashing and monitoring ESP32...");

  flash.stdout.on("data", (data) => {
    const str = data.toString();
    output += str;
    process.stdout.write(str); // Also log to server console
  });
  flash.stderr.on("data", (data) => {
    const str = data.toString();
    output += str;
    process.stderr.write(str); // Also log to server console
  });
  flash.on("close", (code) => {
    output += `\nProcess exited with code ${code}\n`;
    res.json({
      success: true,
      message: "Flashing and monitoring complete.",
      output,
      exitCode: code,
    });
  });
  flash.on("error", (err) => {
    output += `\nFailed to start flash process: ${err.message}\n`;
    res.status(500).json({
      success: false,
      message: "Failed to start flash process.",
      error: err.message,
      output,
    });
  });
});

router.post("/esp32/update", (req, res) => {
  console.log("ESP32 update request:", req.body);
  res.json({
    status: "update received",
    data: req.body,
    timestamp: new Date().toISOString(),
  });
});

router.post("/esp32/sensor-data", (req, res) => {
  const { temperature, humidity, pressure, timestamp } = req.body;
  console.log("Sensor data received:", {
    temperature,
    humidity,
    pressure,
    timestamp,
  });

  res.json({
    status: "success",
    message: "Sensor data received and processed",
    received_at: new Date().toISOString(),
  });
});

// Configuration routes
router.get("/config", (req, res) => {
  res.json({
    wifi: {
      ssid: "CycleOptima-Network",
      reconnect_interval: 5000,
    },
    sensors: {
      reading_interval: 1000,
      temperature_threshold: 50,
    },
    server: {
      endpoint: "http://localhost:3000/api",
      upload_interval: 30000,
    },
  });
});

router.post("/config", (req, res) => {
  console.log("Configuration update:", req.body);
  res.json({
    status: "configuration updated",
    data: req.body,
    timestamp: new Date().toISOString(),
  });
});

// Data routes
router.post("/data", (req, res) => {
  console.log("General data received:", req.body);
  res.json({
    status: "success",
    message: "Data received and processed",
    timestamp: new Date().toISOString(),
  });
});

// File routes

// Update input.json
router.put("/input", (req, res) => {
  const fs = require("fs");
  const inputPath = path.join(__dirname, "..", "spiffs", "input.json");
  
  // Try to get data from req.body.phases first, then fallback to req.body
  const newJson = req.body.phases || req.body;
  
  console.log("Full request body:", JSON.stringify(req.body, null, 2));
  console.log("Updating input.json with:", JSON.stringify(newJson, null, 2));
  console.log("Target file path:", inputPath);
  
  if (!newJson || !Array.isArray(newJson)) {
    return res.status(400).json({ 
      error: "Invalid JSON body - expected array of phases",
      received: typeof newJson,
      body: req.body
    });
  }
  
  fs.writeFile(inputPath, JSON.stringify(newJson, null, 2), (err) => {
    if (err) {
      console.error("File write error:", err);
      return res
        .status(500)
        .json({ error: "Failed to write file", details: err.message });
    }
    
    // Force file system sync and verify write
    fs.fsync(fs.openSync(inputPath, 'r'), (syncErr) => {
      if (syncErr) {
        console.warn("File sync warning:", syncErr);
      }
    });
    
    // Verify the file was written by reading it back
    fs.readFile(inputPath, 'utf8', (readErr, data) => {
      if (readErr) {
        console.error("Verification read error:", readErr);
      } else {
        console.log("Successfully wrote to input.json - Verified file size:", data.length);
      }
    });
    
    console.log("Successfully wrote to input.json");
    res.json({
      status: "success",
      message: "input.json updated",
      data: newJson,
      file_path: inputPath,
      timestamp: new Date().toISOString()
    });
  });
});

router.get("/files/:filename", (req, res) => {
  const filename = req.params.filename;
  const filePath = path.join(__dirname, "..", "spiffs", filename);

  res.sendFile(filePath, (err) => {
    if (err) {
      res.status(404).json({
        error: "File not found",
        filename: filename,
      });
    }
  });
});

router.get("/monitor", (req, res) => {
  const port = new SerialPort({
    path: "COM7",
    baudRate: 115200,
    autoOpen: false,
  });

  const parser = port.pipe(new ReadlineParser({ delimiter: "\r\n" }));

  let output = "";
  let closed = false;
  res.setHeader("Content-Type", "text/plain; charset=utf-8");

  // Handle port errors gracefully
  port.on("error", (err) => {
    if (!closed) {
      closed = true;
      res.status(500).send("Serial port error: " + err.message);
    }
  });

  port.open((err) => {
    if (err) {
      closed = true;
      return res.status(500).send("Failed to open serial port: " + err.message);
    }
    parser.on("data", (line) => {
      output += line + "\n";
      // Optionally, stream to client: res.write(line + "\n");
    });

    // Stop after 10 seconds or when client closes connection
    const closePort = () => {
      if (!closed) {
        closed = true;
        if (port.isOpen) port.close();
        res.end(output);
      }
    };

    setTimeout(closePort, 10000);
    req.on("close", closePort);
  });
});

module.exports = router;
