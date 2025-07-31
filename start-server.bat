@echo off
echo Starting CycleOptima Express Server...
echo.
echo Server will be available at:
echo - Main: http://localhost:3000
echo - Health: http://localhost:3000/health
echo - API Docs: http://localhost:3000/api
echo - SPIFFS Files: http://localhost:3000/spiffs
echo.
echo Press Ctrl+C to stop the server
echo.
node server.js
