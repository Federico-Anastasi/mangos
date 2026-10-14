#!/usr/bin/env python3
"""Builds the Parakeet encoder used by mangOS: 8-bit, except the 24 "depthwise" convolutions,
which stay in floating point.

On the phone's Cortex-A55 onnxruntime runs those convolutions much slower in 8-bit than in
floating point, while everything else is faster in 8-bit: so only they stay in floating point.
Weights are unsigned 8-bit, as in the published model: with signed weights the other convolutions
slow down.

Run on the PC (needs the Python packages onnx and onnxruntime, and 6 GB of memory), on the 32-bit
model from https://huggingface.co/istupakov/parakeet-tdt-0.6b-v3-onnx (encoder-model.onnx and
encoder-model.onnx.data):

  parakeet_mixed.py <directory with the 32-bit model> <output directory>

The result (encoder-model.int8.onnx) goes in $W/asr/parakeet-v3-mixed (normally ~/m13), where
scripts/install_mangos.sh picks it up instead of the downloaded encoder.
"""
import os
import sys

import onnx
from onnxruntime.quantization import QuantType, quantize_dynamic

if len(sys.argv) != 3:
    sys.exit(__doc__)
source = os.path.join(sys.argv[1], "encoder-model.onnx")
output = os.path.join(sys.argv[2], "encoder-model.int8.onnx")
os.makedirs(sys.argv[2], exist_ok=True)
graph = onnx.load(source, load_external_data=False).graph
float_nodes = [n.name for n in graph.node if n.op_type == "Conv" and "depthwise" in n.name]
if len(float_nodes) != 24:
    sys.exit(f"expected 24 depthwise convolutions, found {len(float_nodes)}: not the expected model")
quantize_dynamic(source, output, weight_type=QuantType.QUInt8, nodes_to_exclude=float_nodes)
print(output, os.path.getsize(output), "bytes")
