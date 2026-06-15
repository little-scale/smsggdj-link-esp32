idf_component_register(
  SRCS "main.cpp"
  INCLUDE_DIRS "."
  REQUIRES driver esp_driver_usb_serial_jtag esp_wifi nvs_flash esp_netif esp_event esp_timer
)

# Ableton Link (header-only C++) lives in the lib/link git submodule.
# Override with: idf.py -DLINK_DIR=/path/to/link build
if(NOT DEFINED LINK_DIR)
  set(LINK_DIR "${CMAKE_SOURCE_DIR}/lib/link")
endif()
if(NOT EXISTS "${LINK_DIR}/include/ableton/Link.hpp")
  message(FATAL_ERROR
    "Ableton Link not found at LINK_DIR=${LINK_DIR}. "
    "Run: git submodule update --init lib/link")
endif()

# Our own include dir is added first (INCLUDE_DIRS "."), so any patched Link
# platform headers we drop under main/ableton/ override the upstream ones.
target_include_directories(${COMPONENT_LIB} PRIVATE "${LINK_DIR}/include")
target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions)

# Pin the Link service task to a core (single-core C3: tskNO_AFFINITY).
if(NOT DEFINED LINK_ESP_TASK_CORE_ID)
  set(LINK_ESP_TASK_CORE_ID tskNO_AFFINITY)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
  LINK_ESP_TASK_CORE_ID=${LINK_ESP_TASK_CORE_ID})
