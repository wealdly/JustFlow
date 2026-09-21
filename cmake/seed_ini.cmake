# Seed the run directory's ini files, but NEVER overwrite one that is already there.
#
# These files are live config, not build output: the Settings dialog, the quality presets and the
# tray all write to the copies next to the exe. Copying the repo versions over them on every build
# silently reverted whatever the user had just set - including, for a while, the settings they were
# reporting bugs against.
#
# Consequence worth knowing: editing a shipped profile in the repo no longer reaches an existing
# build directory. Delete the file there (or the whole profiles folder) to re-seed it.
file(GLOB _seed "${SRC}/*.ini")
foreach(_f ${_seed})
    get_filename_component(_n "${_f}" NAME)
    if(NOT EXISTS "${DEST}/${_n}")
        file(COPY "${_f}" DESTINATION "${DEST}")
        message(STATUS "seeded ${_n}")
    endif()
endforeach()
