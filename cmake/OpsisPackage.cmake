# Direct package: after the listed targets are built, run an OPSIS script that
# packs those binaries into a .oaa. Include this file, then call
# opsis_direct_package. The target package-<NAME> is part of the default build.
#
# opsis_direct_package(
#   NAME okrapm
#   NAMESPACE Okra
#   VERSION 0.1.0
#   DESCRIPTION "Okra package manager"
#   SCRIPT ${CMAKE_SOURCE_DIR}/package/okrapm.opsis
#   OUTPUT ${CMAKE_BINARY_DIR}/Okra.okrapm.oaa
#   TARGETS lunar opsis
# )
#
# The script can read OPSIS_PKG_*, OPSIS_BUILD_DIR, and OPSIS_BIN_<TARGET>.

function(opsis_direct_package)
	cmake_parse_arguments(ARG "" "NAME;NAMESPACE;VERSION;DESCRIPTION;SCRIPT;OUTPUT" "TARGETS" ${ARGN})
	if(NOT ARG_NAME OR NOT ARG_VERSION OR NOT ARG_SCRIPT)
		message(FATAL_ERROR "opsis_direct_package needs NAME, VERSION and SCRIPT")
	endif()
	if(NOT TARGET opsis)
		message(FATAL_ERROR "opsis_direct_package needs the opsis executable")
	endif()
	if(NOT ARG_NAMESPACE)
		set(ARG_NAMESPACE "Okra")
	endif()
	if(NOT ARG_DESCRIPTION)
		set(ARG_DESCRIPTION "${ARG_NAME} direct package")
	endif()
	if(NOT ARG_OUTPUT)
		set(ARG_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${ARG_NAMESPACE}.${ARG_NAME}.oaa")
	endif()

	set(env_args
		"OPSIS_PKG_NAMESPACE=${ARG_NAMESPACE}"
		"OPSIS_PKG_NAME=${ARG_NAME}"
		"OPSIS_PKG_VERSION=${ARG_VERSION}"
		"OPSIS_PKG_DESCRIPTION=${ARG_DESCRIPTION}"
		"OPSIS_PKG_OUTPUT=${ARG_OUTPUT}"
		"OPSIS_BUILD_DIR=${CMAKE_BINARY_DIR}"
		"OPSIS_ALLOW_NONROOT=1"
	)
	set(deps "${ARG_SCRIPT}" "$<TARGET_FILE:opsis>")
	foreach(tgt IN LISTS ARG_TARGETS)
		if(NOT TARGET ${tgt})
			message(FATAL_ERROR "opsis_direct_package missing target ${tgt}")
		endif()
		string(TOUPPER "${tgt}" upper)
		list(APPEND env_args "OPSIS_BIN_${upper}=$<TARGET_FILE:${tgt}>")
		list(APPEND deps "$<TARGET_FILE:${tgt}>")
	endforeach()

	add_custom_command(
		OUTPUT "${ARG_OUTPUT}"
		COMMAND ${CMAKE_COMMAND} -E env ${env_args}
			$<TARGET_FILE:opsis> pack --allow-nonroot "${ARG_SCRIPT}"
		DEPENDS ${deps}
		VERBATIM
		COMMENT "Packing ${ARG_NAMESPACE}.${ARG_NAME} with OPSIS"
	)
	add_custom_target(package-${ARG_NAME} ALL DEPENDS "${ARG_OUTPUT}")
endfunction()
