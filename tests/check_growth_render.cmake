if(NOT DEFINED ARTMINER_EXE OR NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "growth render check requires ARTMINER_EXE, SOURCE_DIR and OUTPUT_DIR")
endif()

set(growth_cases
    "am006-reaction-diffusion.amr|80"
    "am006-cellular-automaton.amr|28"
    "am006-walkers.amr|60"
    "am006-branching.amr|90"
)
set(image_hashes)

foreach(growth_case IN LISTS growth_cases)
    string(REPLACE "|" ";" fields "${growth_case}")
    list(GET fields 0 recipe_name)
    list(GET fields 1 tick)
    set(recipe "${SOURCE_DIR}/examples/${recipe_name}")
    set(output_png "${OUTPUT_DIR}/${recipe_name}.png")
    file(REMOVE "${output_png}" "${output_png}.artminer.txt")

    execute_process(
        COMMAND "${ARTMINER_EXE}" render "${recipe}" "${output_png}" --tick "${tick}"
        RESULT_VARIABLE render_result
        OUTPUT_VARIABLE render_stdout
        ERROR_VARIABLE render_stderr
    )
    if(NOT render_result EQUAL 0)
        message(FATAL_ERROR "growth render ${recipe_name} failed (${render_result}): ${render_stdout}${render_stderr}")
    endif()

    string(FIND "${render_stdout}" "tick ${tick}" tick_console_position)
    if(tick_console_position EQUAL -1)
        message(FATAL_ERROR "${recipe_name} did not report explicit tick ${tick}: ${render_stdout}")
    endif()
    string(REGEX MATCH "image-hash ([0-9a-f]+)" hash_match "${render_stdout}")
    if(NOT hash_match)
        message(FATAL_ERROR "${recipe_name} did not report an image hash: ${render_stdout}")
    endif()
    list(APPEND image_hashes "${CMAKE_MATCH_1}")

    if(NOT EXISTS "${output_png}")
        message(FATAL_ERROR "${recipe_name} did not create PNG")
    endif()
    if(NOT EXISTS "${output_png}.artminer.txt")
        message(FATAL_ERROR "${recipe_name} did not create provenance sidecar")
    endif()

    file(READ "${output_png}.artminer.txt" provenance)
    string(FIND "${provenance}" "simulation-tick ${tick}\n" tick_provenance_position)
    if(tick_provenance_position EQUAL -1)
        message(FATAL_ERROR "${recipe_name} provenance does not identify simulation tick ${tick}")
    endif()
    string(FIND "${provenance}" "recipe-begin\n" recipe_position)
    if(recipe_position EQUAL -1)
        message(FATAL_ERROR "${recipe_name} provenance lost canonical recipe payload")
    endif()

    file(REMOVE "${output_png}" "${output_png}.artminer.txt")
endforeach()

set(unique_hashes ${image_hashes})
list(REMOVE_DUPLICATES unique_hashes)
list(LENGTH unique_hashes unique_count)
if(NOT unique_count EQUAL 4)
    message(FATAL_ERROR "committed AM-006 examples did not produce four distinct image hashes: ${image_hashes}")
endif()
