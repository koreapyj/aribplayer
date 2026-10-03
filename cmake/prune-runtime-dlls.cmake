# Build-time removal of FFmpeg DLLs next to an executable that the ffmpeg dist no longer ships
# (a major version bump renames avcodec-NN.dll etc.). A DLL held by a running player cannot be
# deleted but can be renamed, so it is moved aside as <name>.locked-<timestamp>.
# Usage: cmake -DDIR=<dir> -DKEEP=<name|...> -P prune-runtime-dlls.cmake
cmake_minimum_required(VERSION 3.25)
string(REPLACE "|" ";" KEEP "${KEEP}")
file(GLOB candidates RELATIVE "${DIR}" "${DIR}/av*-*.dll" "${DIR}/sw*-*.dll" "${DIR}/postproc-*.dll")
string(TIMESTAMP stamp "%Y%m%d%H%M%S")
foreach(name IN LISTS candidates)
    if(name IN_LIST KEEP)
        continue()
    endif()
    file(RENAME "${DIR}/${name}" "${DIR}/${name}.locked-${stamp}" RESULT renamed)
    if(renamed EQUAL 0)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${DIR}/${name}.locked-${stamp}")
    endif()
endforeach()
