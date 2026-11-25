import numpy as np
import cv2
from rknnlite.api import RKNNLite

# Configuración de rutas
RKNN_MODEL = 'mobilenet.rknn'
IMG_PATH = 'test.jpg'  # Asegúrate de tener una imagen de prueba o ajústala

def main():
    # 1. Crear instancia de RKNNLite
    rknn = RKNNLite()

    # 2. Cargar el modelo RKNN
    print("--> Cargando el modelo RKNN...")
    ret = rknn.load_rk_model(RKNN_MODEL)
    if ret != 0:
        print(f"Error: No se pudo cargar el modelo {RKNN_MODEL}, código de error: {ret}")
        return

    # 3. Inicializar el entorno de ejecución en la NPU (Core 0)
    print("--> Inicializando runtime en la NPU...")
    ret = rknn.init_runtime(core_id=0)
    if ret != 0:
        print(f"Error: Falló la inicialización del runtime, código de error: {ret}")
        return

    # 4. Preparar datos de entrada (MobileNet típicamente usa 224x224x3)
    print("--> Procesando datos de entrada...")
    img = cv2.imread(IMG_PATH)
    if img is None:
        print(f"Aviso: No se encontró '{IMG_PATH}', usando datos aleatorios como prueba.")
        input_data = np.random.randn(1, 224, 224, 3).astype(np.uint8)
    else:
        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        img = cv2.resize(img, (224, 224))
        input_data = np.expand_dims(img, axis=0)

    # 5. Ejecutar la inferencia
    print("--> Ejecutando inferencia...")
    outputs = rknn.inference(inputs=[input_data])

    print("--> ¡Inferencia completada con éxito!")
    for i, out in enumerate(outputs):
        print(f"Salida [{i}] - Forma (Shape): {out.shape}")

    # 6. Liberar recursos de la NPU
    rknn.release()

if __name__ == '__main__':
    main()
