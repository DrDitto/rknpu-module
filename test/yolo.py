import ctypes
import sys
import os
import time
import numpy as np
from PIL import Image, ImageDraw

try:
    rknn_rt = ctypes.CDLL("./librknnrt.so")
except Exception as e:
    print(f"[ERROR] Failed to load librknnrt.so: {e}")
    sys.exit(1)

RKNN_SUCC = 0

rknn_rt.rknn_init.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
rknn_rt.rknn_init.restype = ctypes.c_int

rknn_rt.rknn_inputs_set.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
rknn_rt.rknn_inputs_set.restype = ctypes.c_int

rknn_rt.rknn_run.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
rknn_rt.rknn_run.restype = ctypes.c_int

rknn_rt.rknn_outputs_get.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p]
rknn_rt.rknn_outputs_get.restype = ctypes.c_int

rknn_rt.rknn_outputs_release.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
rknn_rt.rknn_outputs_release.restype = ctypes.c_int

rknn_rt.rknn_destroy.argtypes = [ctypes.c_void_p]
rknn_rt.rknn_destroy.restype = ctypes.c_int

class rknn_input(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("buf", ctypes.c_void_p),
        ("size", ctypes.c_uint32),
        ("pass_through", ctypes.c_ubyte),
        ("type", ctypes.c_uint32),
        ("fmt", ctypes.c_uint32)
    ]

class rknn_output(ctypes.Structure):
    _fields_ = [
        ("want_float", ctypes.c_ubyte),
        ("is_prealloc", ctypes.c_ubyte),
        ("index", ctypes.c_uint32),
        ("buf", ctypes.c_void_p),
        ("size", ctypes.c_uint32),
        ("size_with_stride", ctypes.c_uint32),
        ("np_shape", ctypes.c_uint32 * 16)
    ]

def run_and_draw():
    model_path = "./yolov5s-640-640.rknn"
    image_path = "test.jpg"
    output_path = "output_result.jpg"

    if not os.path.exists(model_path) or not os.path.exists(image_path):
        print("[ERROR] Model or test image not found.")
        return

    with open(model_path, "rb") as f:
        model_data = f.read()

    ctx = ctypes.c_void_p(None)
    print(f"[INFO] Initializing YOLOv5 model on NPU...")
    ret = rknn_rt.rknn_init(ctypes.byref(ctx), model_data, len(model_data), 0, None)
    if ret != RKNN_SUCC:
        print(f"[ERROR] rknn_init failed: {ret}")
        return

    orig_img = Image.open(image_path).convert('RGB')
    orig_w, orig_h = orig_img.size

    img_resized = orig_img.resize((640, 640))
    img_data = np.array(img_resized, dtype=np.float32)

    inp = rknn_input()
    inp.index = 0
    inp.buf = img_data.ctypes.data_as(ctypes.c_void_p)
    inp.size = img_data.nbytes
    inp.pass_through = 0
    inp.type = 0
    inp.fmt = 1   # NHWC

    ret = rknn_rt.rknn_inputs_set(ctx, 1, ctypes.byref(inp))
    if ret != RKNN_SUCC:
        print(f"[ERROR] rknn_inputs_set failed: {ret}")
        rknn_rt.rknn_destroy(ctx)
        return

    print("[INFO] Running NPU inference...")
    t_start = time.perf_counter()
    ret = rknn_rt.rknn_run(ctx, None)
    infer_time = (time.perf_counter() - t_start) * 1000

    if ret != RKNN_SUCC:
        print(f"[ERROR] rknn_run failed: {ret}")
        rknn_rt.rknn_destroy(ctx)
        return

    outputs_array = (rknn_output * 1)()
    outputs_array[0].want_float = 1
    outputs_array[0].index = 0

    ret = rknn_rt.rknn_outputs_get(ctx, 1, outputs_array, None)
    if ret != RKNN_SUCC:
        print(f"[ERROR] rknn_outputs_get failed: {ret}")
        rknn_rt.rknn_destroy(ctx)
        return

    buffer_size = outputs_array[0].size
    num_elements = buffer_size // 4
    output_data = np.ctypeslib.as_array(
        ctypes.cast(outputs_array[0].buf, ctypes.POINTER(ctypes.c_float)),
        shape=(num_elements,)
    )

    print(f"[INFO] Inference completed in {infer_time:.2f} ms")
    
    draw = ImageDraw.Draw(orig_img)
    threshold = 0.4
    indices = np.where(output_data > threshold)[0]
    print(f"[INFO] Activations > {threshold}: {len(indices)} found in tensor.")

    if len(indices) > 0:
        draw.rectangle([20, 20, orig_w - 20, orig_h - 20], outline="lime", width=5)
        draw.text((30, 30), f"NPU RK3566 - Detections OK ({len(indices)})", fill="lime")

    orig_img.save(output_path)
    print(f"[SUCCESS] Result image saved to: {output_path}")

    rknn_rt.rknn_outputs_release(ctx, 1, outputs_array)
    rknn_rt.rknn_destroy(ctx)

if __name__ == "__main__":
    run_and_draw()
