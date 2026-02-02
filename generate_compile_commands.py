import os

Import("env")

env.Replace(COMPILATIONDB_INCLUDE_TOOLCHAIN=True)
env.Replace(COMPILATIONDB_PATH=os.path.join("$BUILD_DIR", "compile_commands.json"))