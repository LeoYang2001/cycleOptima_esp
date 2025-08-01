const express = require("express");

// Simple test to verify Express is working
const app = express();
const PORT = 3001;

app.get("/", (req, res) => {
  res.json({ message: "Express server test successful!" });
});

app.listen(PORT, () => {
  console.log(`Test server running on http://localhost:${PORT}`);
  setTimeout(() => {
    console.log("Test completed - stopping server");
    process.exit(0);
  }, 2000);
});
