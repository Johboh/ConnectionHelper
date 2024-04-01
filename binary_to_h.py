#/usr/bin/env python

import os
import sys

def convert(input_file, output_file):
    file_name, file_extension = os.path.splitext(os.path.basename(output_file))
    file_extension = file_extension.replace(".", "")

    with open(input_file, 'rb') as f:
        content = f.read()
        content_length = len(content)

    define = f"__{file_name.upper()}_{file_extension.upper()}__"
    with open(output_file, 'w') as f:
        f.write(f"#ifndef {define}\n")
        f.write(f"#define {define}\n\n")
        f.write("#include <cstdint>\n\n")
        f.write(f"const char {file_name.lower()}[{content_length+1}] = {{")
        for byte in content:
            f.write(str(byte) + ', ')
        f.write("0};\n")
        f.write(f"#endif // {define}\n")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python convert_to_h.py <input_file> <output_file>")
    else:
        convert(sys.argv[1], sys.argv[2])