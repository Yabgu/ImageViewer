add_library(media)

target_sources(
    media PUBLIC
    FILE_SET media_modules TYPE CXX_MODULES FILES
        Correspondence.ixx
        Buffer.ixx
        View.ixx
        Processor.ixx
        Scene.ixx
        ViewCorrespondence.ixx
        CorrespondenceInterpreter.ixx
        Dispatcher.ixx
)

target_include_directories(media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(media PUBLIC cxx_std_23)
