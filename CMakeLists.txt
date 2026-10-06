# Ultimate Headless — binário produto (mesmo núcleo validado do protótipo).
# Sem janela/renderer/áudio; rede apenas onde houver material legítimo.
# Game state, login e treino entram por etapas com testes, conforme o plano.

cmake_minimum_required(VERSION 3.16)
project(ultimate_headless LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OTC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/otcv8-dev/src)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/HeadlessCore.cmake)

add_headless_target(ultimate-headless ${CMAKE_CURRENT_SOURCE_DIR}/src/headless/main.cpp)
install(TARGETS ultimate-headless RUNTIME DESTINATION bin)
install(FILES scripts/runtime.lua scripts/powerdown.lua scripts/trainer.lua scripts/net_reconnect.lua scripts/training.lua scripts/live_training.lua
        DESTINATION share/ultimate-headless/scripts)
install(FILES resources/login-context.bin resources/metadata.json DESTINATION share/ultimate-headless/resources)
