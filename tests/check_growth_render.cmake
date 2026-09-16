if(NOT DEFINED ARTMINER_EXE OR NOT DEFINED GROWTH_RECIPE OR NOT DEFINED OUTPUT_PNG)
    message(FATAL_ERROR "growth render check requires ARTMINER_EXE, GROWTH_RECIPE and OUTPUT_PNG")
endif()

file(REMOVE "${OUTPUT_PNG}" "${OUTPUT_PNG}.artminer.txt")

execute_process(
    COMMAND "${ARTMINER_EXE}" render "${GROWTH_RECIPE}" "${OUTPUT_PNG}" --tick 12
    RESULT_VARIABLE render_result
    OUTPUT_VARIABLE render_stdout
    ERROR_VARIABLE render_stderr
)
if(NOT render_result EQUAL 0)
    message(FATAL_ERROR "growth render failed (${render_result}): ${render_stdout}${render_stderr}")
endif()

string(FIND "${render_stdout}" "tick 12" tick_console_position)
if(tick_console_position EQUAL -1)
    message(FATAL_ERROR "growth render did not report explicit tick: ${render_stdout}")
endif()

if(NOT EXISTS "${OUTPUT_PNG}")
    message(FATAL_ERROR "growth render did not create PNG")
endif()
if(NOT EXISTS "${OUTPUT_PNG}.artminer.txt")
    message(FATAL_ERROR "growth render did not create provenance sidecar")
endif()

file(READ "${OUTPUT_PNG}.artminer.txt" provenance)
string(FIND "${provenance}" "simulation-tick 12\n" tick_provenance_position)
if(tick_provenance_position EQUAL -1)
    message(FATAL_ERROR "growth provenance does not identify requested simulation tick")
endif()
string(FIND "${provenance}" "recipe-begin\n" recipe_position)
if(recipe_position EQUAL -1)
    message(FATAL_ERROR "growth provenance lost canonical recipe payload")
endif()

file(REMOVE "${OUTPUT_PNG}" "${OUTPUT_PNG}.artminer.txt")
