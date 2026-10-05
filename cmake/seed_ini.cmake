# Seed the run directory's ini files, but NEVER overwrite one that is already there: they are live
# config (the Settings dialog, the quality presets and the tray write to the copies next to the exe),
# and copying the repo versions over them would revert whatever the user had just set.
#
# So editing a shipped profile in the repo does not reach an existing build directory: delete the file
# there (or the whole profiles folder) to re-seed it.
file(GLOB _seed "${SRC}/*.ini")
foreach(_f ${_seed})
    get_filename_component(_n "${_f}" NAME)
    if(NOT EXISTS "${DEST}/${_n}")
        file(COPY "${_f}" DESTINATION "${DEST}")
        message(STATUS "seeded ${_n}")
    endif()
endforeach()
