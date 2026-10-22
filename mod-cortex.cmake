# Included by the core's modules/CMakeLists.txt after the `modules` target exists.
#
# Cortex's body was ported from mod-playerbots (src/Body/README.md). The core turns on MOD_PLAYERBOTS only for a module
# named mod-playerbots; bots need what it gates (PathGenerator.h: 148-poly paths instead of 74), so Cortex sets it.
target_compile_options(game-interface INTERFACE -DMOD_PLAYERBOTS)
target_compile_options(database PRIVATE -DMOD_PLAYERBOTS)
target_compile_options(modules PRIVATE -DMOD_PLAYERBOTS)

# The core links the MySQL client library privately into its `database` target,
# so its include directories do not reach module code. PlayerbotsDatabase.cpp
# needs the complete MySQLPreparedStatement type (which pulls in mysql.h) and
# Playerbots.cpp uses ER_BAD_DB_ERROR from mysqld_error.h. Linking the imported
# `mysql` target here propagates those include directories to the module build.
target_link_libraries(modules
  PRIVATE
    mysql)
