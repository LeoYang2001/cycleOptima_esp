const express = require("express");
const cors = require("cors");
const bodyParser = require("body-parser");
const morgan = require("morgan");
const path = require("path");

// Import routes
const apiRoutes = require("./routes/api");

const app = express();
const PORT = process.env.PORT || 3000;

// Middleware
app.use(cors());
app.use(bodyParser.json());
app.use(bodyParser.urlencoded({ extended: true }));
app.use(morgan("combined"));

// Serve static files from spiffs directory

// Serve static files from spiffs directory
app.use("/spiffs", express.static(path.join(__dirname, "spiffs")));

// Directory listing for /spiffs/
app.get("/spiffs/", (req, res, next) => {
  const fs = require("fs");
  const spiffsPath = path.join(__dirname, "spiffs");
  fs.readdir(spiffsPath, (err, files) => {
    if (err) {
      return next(err);
    }
    res.json({
      files: files || [],
    });
  });
});

// Use API routes
app.use("/api", apiRoutes);

// Routes
app.get("/", (req, res) => {
  res.json({
    message: "CycleOptima Express Server",
    version: "1.0.0",
    status: "running",
    timestamp: new Date().toISOString(),
  });
});

// Health check endpoint
app.get("/health", (req, res) => {
  res.json({
    status: "healthy",
    uptime: process.uptime(),
    timestamp: new Date().toISOString(),
    memory: process.memoryUsage(),
    version: "1.0.0",
  });
});

// API documentation endpoint
app.get("/api", (req, res) => {
  res.json({
    name: "CycleOptima API",
    version: "1.0.0",
    endpoints: {
      general: [
        "GET / - Server status",
        "GET /health - Health check",
        "GET /api - This documentation",
      ],
      esp32: [
        "GET /api/esp32/status - ESP32 status",
        "POST /api/esp32/update - Send updates to ESP32",
        "POST /api/esp32/sensor-data - Receive sensor data",
      ],
      config: [
        "GET /api/config - Get configuration",
        "POST /api/config - Update configuration",
      ],
      files: [
        "GET /api/input - Get input.json",
        "GET /api/files/:filename - Get specific file",
        "GET /spiffs/* - Access SPIFFS files directly",
      ],
    },
  });
});

// Error handling middleware
app.use((err, req, res, next) => {
  console.error(err.stack);
  res.status(500).json({
    error: "Something went wrong!",
    message: err.message,
  });
});

// Handle 404
app.use("*", (req, res) => {
  res.status(404).json({
    error: "Route not found",
    path: req.originalUrl,
  });
});

// Start server
app.listen(PORT, () => {
  console.log(`🚀 CycleOptima Express Server running on port ${PORT}`);
  console.log(`📊 Health check: http://localhost:${PORT}/health`);
  console.log(`🔧 API docs: http://localhost:${PORT}/api`);
  console.log(`📁 SPIFFS files: http://localhost:${PORT}/spiffs`);
});

module.exports = app;
