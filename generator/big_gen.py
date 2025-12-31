import networkx as nx
import random
import os


NUM_VERTICES = 705
NUM_EDGES = 705
OUTPUT_FILE = "/home/oleg/client-sever/graph_input.txt"
MIN_WEIGHT = 1
MAX_WEIGHT = 20

def generate_graph_file():
    # 1. Генерируем случайный граф (gnm_random_graph создает граф с V узлами и E ребрами)
    # Используем seed для повторяемости (опционально)
    # random.seed(42) 
    
    # Генерируем пока не получим связный граф (опционально, но удобно для тестов)
    # Если не нужно обязательно связный, цикл while можно убрать
    # while True:
    #     G = nx.gnm_random_graph(NUM_VERTICES, NUM_EDGES)
    #     if NUM_EDGES < NUM_VERTICES - 1: break # Невозможно сделать связным
    #     if nx.is_connected(G):
    #         break
    G = nx.gnm_random_graph(NUM_VERTICES, NUM_EDGES)
    # 2. Назначаем веса ребрам
    for (u, v) in G.edges():
        G.edges[u, v]['weight'] = random.randint(MIN_WEIGHT, MAX_WEIGHT)

    # Получаем список ребер для фиксации порядка (важно для матрицы и весов)
    edges_list = list(G.edges(data=True))
    
    # Если генератор создал меньше ребер (дубликаты удаляются), дополним или оставим как есть
    actual_edges_count = len(edges_list)

    print(f"Сгенерирован граф: {NUM_VERTICES} вершин, {actual_edges_count} ребер.")

    # 3. Формируем матрицу инцидентности
    # Размер V строк x E столбцов
    # Инициализируем нулями
    incidence_matrix = [[0] * actual_edges_count for _ in range(NUM_VERTICES)]

    weights = []

    for edge_idx, (u, v, data) in enumerate(edges_list):
        # В неориентированном графе для ребра (u, v) ставим 1 в строке u и строке v
        incidence_matrix[u][edge_idx] = 1
        incidence_matrix[v][edge_idx] = 1
        weights.append(str(data['weight']))

    # 4. Запись в файл
    try:
        with open(OUTPUT_FILE, 'w') as f:
            # Первая строка: <вершины> <ребра>
            f.write(f"{NUM_VERTICES} {actual_edges_count}\n")

            # Матрица инцидентности (построчно)
            for row in incidence_matrix:
                f.write(" ".join(map(str, row)) + "\n")

            # Список весов
            f.write(" ".join(weights) + "\n")
        
        print(f"Файл '{OUTPUT_FILE}' успешно создан.")
        print(f"Путь к файлу: {os.path.abspath(OUTPUT_FILE)}")

    except IOError as e:
        print(f"Ошибка записи файла: {e}")

if __name__ == "__main__":
    generate_graph_file()