# Build-time copy of one runtime file next to an executable, keeping the source timestamp:
# the debug overlay shows "libffplay.dll built <time>" from the file next to the exe.
# Usage: cmake -DSRC=<file> -DDST_DIR=<dir> -P copy-runtime-file.cmake
file(COPY "${SRC}" DESTINATION "${DST_DIR}")
