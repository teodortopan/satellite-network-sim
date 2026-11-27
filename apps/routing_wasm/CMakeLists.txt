# satsim-routing: the routing program as a WebAssembly ES module for the dashboard (built only
# with Emscripten; scripts/ci.sh build_wasm_engine). main.cpp is a thin entry over the same
# satsim_routing_app library as the native program. Output in <build>/bin: satsim-routing.js
# and satsim-routing.wasm.
#
#  - MODULARIZE + EXPORT_ES6: a factory, one fresh instance per run, in Node, a browser page
#    or a Web Worker (no NODERAWFS: inputs and outputs go through the in-memory FS).
#  - INVOKE_RUN=0: the caller writes the inputs, then calls callMain(arguments).
#  - wasm exceptions (the root CMakeLists.txt sets them for every target); memory grows up
#    to 4 GB; an 8 MB stack.
#  - Release, exact IEEE arithmetic (never -ffast-math), our warnings.
add_executable(satsim-routing main.cpp)
target_link_libraries(satsim-routing PRIVATE satsim::routing_app)
set_target_properties(satsim-routing PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
  SUFFIX ".js")
satsim_set_warnings(satsim-routing)
target_link_options(satsim-routing PRIVATE
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createSatsimRouting
  -sENVIRONMENT=web,worker,node
  -sINVOKE_RUN=0
  -sEXIT_RUNTIME=0
  -sEXPORTED_RUNTIME_METHODS=callMain,FS,HEAPU8
  -sALLOW_MEMORY_GROWTH=1
  -sMAXIMUM_MEMORY=4GB
  -sSTACK_SIZE=8MB)
