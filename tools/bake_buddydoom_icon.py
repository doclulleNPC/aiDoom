#!/usr/bin/env python3
import os
from PIL import Image

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    icon_png_path = os.path.join(root, 'icon.png')
    ico_path = os.path.join(root, 'buddydoom.ico')
    icns_path = os.path.join(root, 'buddydoom.icns')
    header_path = os.path.join(root, 'files', 'buddydoom_icon.h')

    print(f"Reading {icon_png_path}...")
    if not os.path.exists(icon_png_path):
        print(f"Error: {icon_png_path} does not exist!")
        return

    im = Image.open(icon_png_path).convert('RGBA')

    # 1. Save ICO
    print(f"Saving ICO to {ico_path}...")
    ico_sizes = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    im.save(ico_path, sizes=ico_sizes)

    # 2. Save ICNS
    print(f"Saving ICNS to {icns_path}...")
    icns_sizes = [(16, 16), (32, 32), (64, 64), (128, 128), (256, 256), (512, 512), (1024, 1024)]
    im.save(icns_path, sizes=icns_sizes)

    # 3. Generate buddydoom_icon.h (64x64 RGBA raw bytes)
    print(f"Generating C header at {header_path}...")
    im_64 = im.resize((64, 64), Image.Resampling.LANCZOS)
    pixels = list(im_64.getdata())

    bytes_list = []
    for r, g, b, a in pixels:
        bytes_list.extend([r, g, b, a])

    with open(header_path, 'w') as f:
        f.write("// Generated from icon.png -- embedded window icon.\n")
        f.write("#ifndef __BUDDYDOOM_ICON__\n")
        f.write("#define __BUDDYDOOM_ICON__\n")
        f.write("#define BUDDYDOOM_ICON_W 64\n")
        f.write("#define BUDDYDOOM_ICON_H 64\n")
        f.write(f"static const unsigned char buddydoom_icon_rgba[{len(bytes_list)}] = {{\n")
        
        # Write 16 bytes per line
        for i in range(0, len(bytes_list), 16):
            chunk = bytes_list[i:i+16]
            line = ",".join(str(b) for b in chunk)
            if i + 16 < len(bytes_list):
                line += ","
            f.write("  " + line + "\n")
            
        f.write("};\n")
        f.write("#endif\n")

    print("Done!")

if __name__ == '__main__':
    main()
