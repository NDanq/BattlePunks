# The smallest example project, for NimbleEditorProjectTests (UnpackExample.cmake).
cmake_minimum_required(VERSION 3.16)
project(Tiny LANGUAGES CXX)
set(GAME_NAME   "Tiny")
set(GAME_MODULE "TinyGame")
set(GAME_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine" CACHE PATH "The Engine tree ${GAME_NAME} builds against")
