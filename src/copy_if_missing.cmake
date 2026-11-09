# cmake -DSOURCE=<file> -DDEST_DIR=<dir> -P copy_if_missing.cmake: copies the file into the directory unless a file of
# its name is there already (one put there by hand is kept); a symlink (a SONAME) is copied as the file it points to
get_filename_component(name "${SOURCE}" NAME)
if(EXISTS "${DEST_DIR}/${name}")
    message(STATUS "${name} is already in ${DEST_DIR}: kept")
else()
    file(REAL_PATH "${SOURCE}" real)
    file(COPY_FILE "${real}" "${DEST_DIR}/${name}")
endif()
