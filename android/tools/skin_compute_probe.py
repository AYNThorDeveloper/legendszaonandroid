#!/usr/bin/env python3
"""Wrap the existing Z-A skinning shader for an offline compute comparison.

The original arithmetic stays intact. Only the vertex-stage input/output and
builtin declarations are replaced with per-invocation fixture buffers. This
does not exercise vertex fetch, transform feedback, or a captured gameplay frame.
"""
import argparse
from pathlib import Path
import re
import subprocess


def wrap(source, stage='compute'):
    ins, outs = {}, {}
    pattern = re.compile(r'^layout\(location = (\d+)[^)]*\) (in|out) (\w+) (\w+);$', re.M)

    def declaration(m):
        location, direction, ty, name = m.groups()
        (ins if direction == 'in' else outs)[int(location)] = (ty, name)
        return f'layout(location = {location}) {direction} {ty} {name};' if stage == 'raster' else f'{ty} {name};'

    source = pattern.sub(declaration, source)
    expected = {0: 'vec4', 1: 'vec4', 3: 'vec4', 12: 'uvec4', 13: 'vec4'}
    if {loc: ty for loc, (ty, _) in ins.items()} != expected:
        raise ValueError(f'Unexpected skinning inputs: {ins}')
    if set(outs) != {0, 1, 2} or outs[0][0] != 'vec3' or outs[1][0] != 'float' or outs[2][0] != 'float':
        raise ValueError(f'Expected the final XFB-patched skinning outputs: {outs}')
    if source.count('void main()') != 1:
        raise ValueError('Expected one entry point')
    if stage == 'raster':
        # Preserve vertex fetching and the original skinning arithmetic. Lay out
        # 256 points across a 256x1 float attachment, with skinned position as RGB.
        source = source.replace('void main()', 'void skinning_main()')
        return source + '''
void main() {
    skinning_main();
    gl_Position = vec4((float(gl_VertexIndex) + 0.5) / 128.0 - 1.0, 0.0, 0.0, 1.0);
    gl_PointSize = 1.0;
}
'''
    source = source.replace('invariant gl_Position;', '')
    source = source.replace('gl_Position', 'lab_position').replace('gl_PointSize', 'lab_point_size')
    source = source.replace('void main()', 'void skinning_main()')
    declarations = '''
layout(local_size_x = 64) in;
vec4 lab_position;
float lab_point_size;
struct InputVertex { uvec4 joints; vec4 weights; vec4 normal; vec4 tangent; vec4 position; };
struct OutputVertex { vec4 position; uvec4 packed; };
layout(std430, set = 0, binding = 3) readonly buffer InputVertices { InputVertex input_vertices[]; };
layout(std430, set = 0, binding = 4) buffer OutputVertices { OutputVertex output_vertices[]; };
'''
    source = source.replace('#version 450', '#version 450\n' + declarations, 1)
    fields = {0: 'position', 1: 'normal', 3: 'tangent', 12: 'joints', 13: 'weights'}
    source += '\nvoid main() {\n    uint i = gl_GlobalInvocationID.x;\n'
    for loc, field in fields.items():
        source += f'    {ins[loc][1]} = input_vertices[i].{field};\n'
    source += '    skinning_main();\n'
    source += f'    output_vertices[i].position = vec4({outs[0][1]}, 1.0);\n'
    source += f'    output_vertices[i].packed = uvec4(floatBitsToUint({outs[1][1]}), floatBitsToUint({outs[2][1]}), 0u, 0u);\n'
    return source + '}\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shader', type=Path, help='Existing final .vks or .spv skinning shader')
    parser.add_argument('output', type=Path)
    parser.add_argument('--stage', choices=('compute', 'raster'), default='compute')
    args = parser.parse_args()
    data = args.shader.read_bytes()
    if data.startswith(b'OK '):
        header = data.splitlines()[0].split()
        lines = 1 + sum(int(x) for x in header[2:5])
        offset = 0
        for _ in range(lines):
            offset = data.index(b'\n', offset) + 1
        data = data[offset:]
        if len(data) != int(header[1]):
            raise ValueError('Invalid VKS byte count')
    if not data.startswith(b'\x03\x02\x23\x07'):
        raise ValueError('Not SPIR-V')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    original = args.output.with_suffix('.original.spv')
    original.write_bytes(data)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(original)], check=True)
    result = subprocess.run(['spirv-cross', str(original), '--version', '450', '--vulkan-semantics'],
                            check=True, capture_output=True, text=True)
    args.output.write_text(wrap(result.stdout, args.stage))


if __name__ == '__main__':
    main()
