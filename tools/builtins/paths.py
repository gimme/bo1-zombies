"""Where things are: the engine's folder, the retail exes in its game/t5/ (not in the repo) or the folder BO1_GAME_DIR
names, the work files in the engine's build/builtins/."""
import os

ENGINE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace("\\", "/")
GAME = os.environ.get("BO1_GAME_DIR", ENGINE + "/game/t5").replace("\\", "/")
SRC = ENGINE + "/src"
OUT = ENGINE + "/build/builtins"
HERE = ENGINE + "/tools/builtins"
MP_EXE = GAME + "/BlackOpsMP.exe"
SP_EXE = GAME + "/BlackOps.exe"
STATUS = HERE + "/status.tsv"
