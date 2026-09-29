cmake_minimum_required(VERSION 3.16)

# PROJECT_VER is copied into esp_app_desc and is what tools/ota_update.py
# waits for after a reboot. Bump it for every image you ship.
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wt32-ota-boilerplate)
