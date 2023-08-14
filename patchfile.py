# Fix the pitch bend bug in the MIDI Library

import fileinput
from os.path import join, isfile

def replace_in_file(file_path, search_text, new_text):
    with fileinput.input(file_path, inplace=True) as file:
        for line in file:
            new_line = line.replace(search_text, new_text)
            print(new_line, end='')

def _touch(path):
    with open(path, "w") as fp:
        fp.write("")

Import("env")

#print(env.Dump())

LIBDEPS_DIR = env['PROJECT_LIBDEPS_DIR']

LIB_DIR = LIBDEPS_DIR + "/" + env['PIOENV']  + "/MIDI Library/src"

FILE2PATCH = LIB_DIR + "/MIDI.hpp"

if not isfile(join(LIB_DIR, ".patching-done")):
    print("File to patch: " + FILE2PATCH)

    replace_in_file(FILE2PATCH, "const int value = int(inPitchValue * double(scale));", "const int value = int(fabs(inPitchValue) * double(scale));")

    _touch(join(LIB_DIR, ".patching-done"))
else:
    print("MIDI.hpp already patched")