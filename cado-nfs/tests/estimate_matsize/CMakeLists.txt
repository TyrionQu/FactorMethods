

cado_define_test(estimate_matsize
    SCRIPT
    ${CADO_NFS_SOURCE_DIR}/scripts/estimate_matsize.sh
    ENVIRONMENT
    CADO_BUILD=${PROJECT_BINARY_DIR}
    NO_DEFAULT_RUN
    TARGET_DEPENDENCIES
        las
        makefb
        fake_rels
        purge
        merge-dl
        freerel
)

cado_divert_test(estimate_matsize
    p30
    APPEND_ARGUMENTS
    -params ${CMAKE_CURRENT_SOURCE_DIR}/p30.ems.params
    ${CMAKE_CURRENT_SOURCE_DIR}/p30.poly
    )

cado_divert_test(estimate_matsize
    p80_compsq
    -params ${CMAKE_CURRENT_SOURCE_DIR}/p80_compsq.ems.params
    ${CMAKE_CURRENT_SOURCE_DIR}/p80.poly
    )
