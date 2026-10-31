idf_component_register(
    SRCS "face_engine.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__human_face_detect
             espressif__human_face_recognition
             fatfs
             wear_levelling
)
