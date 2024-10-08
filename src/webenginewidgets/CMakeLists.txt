if(TARGET Qt::Designer)
    add_subdirectory(plugins/qwebengineview)
else()
    message(WARNING "Qt tools Designer is required to compile designer plugin")
endif()
qt_internal_add_module(WebEngineWidgets
    SOURCES
        api/qtwebenginewidgetsglobal.h
        api/qwebenginenotificationpresenter.cpp api/qwebenginenotificationpresenter_p.h
        api/qwebengineview.cpp api/qwebengineview.h api/qwebengineview_p.h
        render_widget_host_view_qt_delegate_widget.cpp render_widget_host_view_qt_delegate_widget.h
    DEFINES
        QT_BUILD_WEBENGINEWIDGETS_LIB
    INCLUDE_DIRECTORIES
        ../core
        api
    LIBRARIES
        Qt::CorePrivate
        Qt::GuiPrivate
        Qt::QuickWidgets
        Qt::WidgetsPrivate
        Qt::WebEngineCorePrivate
        Qt::QuickPrivate
    PUBLIC_LIBRARIES
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::WebEngineCore
    OS2_SHORT_NAME Qt6WebEW
)

qt_internal_extend_target(WebEngineWidgets CONDITION QT_FEATURE_webengine_printing_and_pdf
    PUBLIC_LIBRARIES
        Qt::PrintSupport
)
