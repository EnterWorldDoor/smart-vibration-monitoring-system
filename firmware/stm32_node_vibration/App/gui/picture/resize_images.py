#!/usr/bin/env python3
"""
STM32F407 Image Resizer: Downsample AI-generated character images
from full resolution to 80x120 (main) and 32x32 (avatar) in RGB565.

Input format: LVGL RGB565A8 C arrays (3 bytes/pixel: R5G6B5 + A8)
Output format: Standard RGB565 C arrays (2 bytes/pixel)
"""

import re
import os

def parse_rgb565a8_c_array(filepath):
    """
    Parse a LVGL RGB565A8 C array file.
    Returns (pixel_data, width, height) where pixel_data is list of (r,g,b) tuples.
    """
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    w_match = re.search(r'\.header\.w\s*=\s*(\d+)', content)
    h_match = re.search(r'\.header\.h\s*=\s*(\d+)', content)
    if not w_match or not h_match:
        print(f"ERROR: Cannot find dimensions in {filepath}")
        return None, 0, 0

    w = int(w_match.group(1))
    h = int(h_match.group(1))

    start = content.find('_map[] = {')
    if start == -1:
        start = content.find('_map[]= {')
    if start == -1:
        start = content.find('_map[] ={')
    if start == -1:
        start = content.find('_map[]={')
    if start == -1:
        print(f"ERROR: Cannot find _map[] in {filepath}")
        return None, w, h

    start = content.find('{', start)

    # Find the closing brace by counting braces
    depth = 0
    for i in range(start, len(content)):
        if content[i] == '{':
            depth += 1
        elif content[i] == '}':
            depth -= 1
            if depth == 0:
                end = i
                break

    array_text = content[start:end+1]
    hex_vals = re.findall(r'0x([0-9a-fA-F]{2})', array_text)
    raw_bytes = bytes([int(v, 16) for v in hex_vals])

    expected_size = w * h * 3
    actual_size = len(raw_bytes)

    if actual_size < expected_size:
        print(f"WARNING: {filepath}: expected {expected_size} bytes, got {actual_size}")

    pixels = []
    max_pixels = min(w * h, actual_size // 3)
    for i in range(max_pixels):
        offset = i * 3
        if offset + 2 < actual_size:
            color16 = (raw_bytes[offset] << 8) | raw_bytes[offset + 1]
            alpha = raw_bytes[offset + 2] if offset + 2 < actual_size else 0xFF
            r = ((color16 >> 11) & 0x1F) * 255 // 31
            g = ((color16 >> 5) & 0x3F) * 255 // 63
            b = (color16 & 0x1F) * 255 // 31
            pixels.append((r, g, b, alpha))

    return pixels, w, h


def downsample_bilinear(pixels, src_w, src_h, dst_w, dst_h):
    """Downsample using bilinear interpolation. Returns list of (r,g,b) tuples."""
    result = []
    for dst_y in range(dst_h):
        for dst_x in range(dst_w):
            src_x = dst_x * src_w / dst_w
            src_y = dst_y * src_h / dst_h

            x0 = int(src_x)
            y0 = int(src_y)
            x1 = min(x0 + 1, src_w - 1)
            y1 = min(y0 + 1, src_h - 1)

            fx = src_x - x0
            fy = src_y - y0

            def get_pixel(x, y):
                idx = y * src_w + x
                if idx < len(pixels):
                    return pixels[idx]
                return (0, 0, 0, 0)

            p00 = get_pixel(x0, y0)
            p10 = get_pixel(x1, y0)
            p01 = get_pixel(x0, y1)
            p11 = get_pixel(x1, y1)

            r = int((1-fx)*(1-fy)*p00[0] + fx*(1-fy)*p10[0] + (1-fx)*fy*p01[0] + fx*fy*p11[0])
            g = int((1-fx)*(1-fy)*p00[1] + fx*(1-fy)*p10[1] + (1-fx)*fy*p01[1] + fx*fy*p11[1])
            b = int((1-fx)*(1-fy)*p00[2] + fx*(1-fy)*p10[2] + (1-fx)*fy*p01[2] + fx*fy*p11[2])

            r = max(0, min(255, r))
            g = max(0, min(255, g))
            b = max(0, min(255, b))

            result.append((r, g, b))
    return result


def rgb_to_rgb565(r, g, b):
    """Convert 8-bit RGB to RGB565 uint16."""
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5


def generate_c_array(pixels, name, width, height):
    """Generate C source with RGB565 pixel data and lv_img_dsc_t."""
    lines = []
    lines.append('')
    lines.append('#ifdef __has_include')
    lines.append('    #if __has_include("lvgl.h")')
    lines.append('        #ifndef LV_LVGL_H_INCLUDE_SIMPLE')
    lines.append('            #define LV_LVGL_H_INCLUDE_SIMPLE')
    lines.append('        #endif')
    lines.append('    #endif')
    lines.append('#endif')
    lines.append('')
    lines.append('#if defined(LV_LVGL_H_INCLUDE_SIMPLE)')
    lines.append('    #include "lvgl.h"')
    lines.append('#else')
    lines.append('    #include "lvgl/lvgl.h"')
    lines.append('#endif')
    lines.append('')
    lines.append('#ifndef LV_ATTRIBUTE_MEM_ALIGN')
    lines.append('#define LV_ATTRIBUTE_MEM_ALIGN')
    lines.append('#endif')
    lines.append('')
    lines.append(f'#ifndef LV_ATTRIBUTE_IMG_{name.upper()}')
    lines.append(f'#define LV_ATTRIBUTE_IMG_{name.upper()}')
    lines.append('#endif')
    lines.append('')

    map_name = f'{name}_map'
    lines.append(f'const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST '
                 f'LV_ATTRIBUTE_IMG_{name.upper()} uint8_t {map_name}[] = {{')

    bytes_per_line = 16
    total = len(pixels) * 2
    for i in range(0, len(pixels), bytes_per_line // 2):
        line_vals = []
        for j in range(i, min(i + bytes_per_line // 2, len(pixels))):
            c16 = rgb_to_rgb565(pixels[j][0], pixels[j][1], pixels[j][2])
            line_vals.append(f'0x{c16 & 0xFF:02X}, 0x{(c16 >> 8) & 0xFF:02X}')
        lines.append('  ' + ', '.join(line_vals) + ',')

    lines.append('};')
    lines.append('')

    lines.append(f'const lv_img_dsc_t {name} = {{')
    lines.append(f'  .header.cf = LV_IMG_CF_TRUE_COLOR,')
    lines.append(f'  .header.always_zero = 0,')
    lines.append(f'  .header.reserved = 0,')
    lines.append(f'  .header.w = {width},')
    lines.append(f'  .header.h = {height},')
    lines.append(f'  .data_size = {total},')
    lines.append(f'  .data = {map_name},')
    lines.append('};')
    lines.append('')

    return '\n'.join(lines)


def main():
    base_dir = os.path.dirname(os.path.abspath(__file__))

    images = {
        'anime_sakoji_main': {
            'file': os.path.join(base_dir, 'xiangzi.c'),
            'dst_w': 80, 'dst_h': 120,
        },
        'anime_taki_main': {
            'file': os.path.join(base_dir, 'lixi.c'),
            'dst_w': 80, 'dst_h': 120,
        },
        'anime_rin_main': {
            'file': os.path.join(base_dir, 'bi.c'),
            'dst_w': 80, 'dst_h': 120,
        },
    }

    for out_name, info in images.items():
        print(f"\nProcessing {info['file']} -> {out_name} ({info['dst_w']}x{info['dst_h']})")

        pixels, w, h = parse_rgb565a8_c_array(info['file'])
        if pixels is None:
            print(f"  SKIP: Could not parse {info['file']}")
            continue

        print(f"  Source: {w}x{h}, {len(pixels)} pixels")
        print(f"  Target: {info['dst_w']}x{info['dst_h']}")

        downsampled = downsample_bilinear(pixels, w, h, info['dst_w'], info['dst_h'])
        print(f"  Downsampled: {len(downsampled)} pixels")

        c_code = generate_c_array(downsampled, out_name, info['dst_w'], info['dst_h'])

        out_file = os.path.join(base_dir, f'{out_name}.c')
        with open(out_file, 'w', encoding='utf-8') as f:
            f.write(c_code)
        print(f"  Output: {out_file} ({os.path.getsize(out_file)} bytes)")

    # Also generate avatars (32x32) from the same source images
    avatars = {
        'anime_sakoji_avatar': {
            'file': os.path.join(base_dir, 'xiangzi.c'),
            'dst_w': 32, 'dst_h': 32,
        },
        'anime_taki_avatar': {
            'file': os.path.join(base_dir, 'lixi.c'),
            'dst_w': 32, 'dst_h': 32,
        },
        'anime_rin_avatar': {
            'file': os.path.join(base_dir, 'bi.c'),
            'dst_w': 32, 'dst_h': 32,
        },
    }

    for out_name, info in avatars.items():
        print(f"\nProcessing {info['file']} -> {out_name} ({info['dst_w']}x{info['dst_h']})")

        pixels, w, h = parse_rgb565a8_c_array(info['file'])
        if pixels is None:
            print(f"  SKIP: Could not parse {info['file']}")
            continue

        downsampled = downsample_bilinear(pixels, w, h, info['dst_w'], info['dst_h'])
        c_code = generate_c_array(downsampled, out_name, info['dst_w'], info['dst_h'])

        out_file = os.path.join(base_dir, f'{out_name}.c')
        with open(out_file, 'w', encoding='utf-8') as f:
            f.write(c_code)
        print(f"  Output: {out_file} ({os.path.getsize(out_file)} bytes)")

    print("\nDone! All images resized.")


if __name__ == '__main__':
    main()
