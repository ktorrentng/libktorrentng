add_subdirectory(tests)

set(datachecker_HDR
    datacheckerthread.h
    datachecker.h
    singledatachecker.h
    multidatachecker.h
    datacheckerjob.h
)

install(FILES ${datachecker_HDR} DESTINATION ${KDE_INSTALL_INCLUDEDIR_KF5}/libktorrent/datachecker COMPONENT Devel)
