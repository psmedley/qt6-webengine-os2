include(../../util/util.cmake)
qt_internal_add_test(tst_qquickwebengineviewgraphics
    SOURCES
        tst_qquickwebengineviewgraphics.cpp
    LIBRARIES
        Qt::CorePrivate
        Qt::WebEngineQuickPrivate
        Qt::Test
        Test::Util
)
