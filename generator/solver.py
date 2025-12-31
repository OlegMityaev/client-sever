import networkx as nx

INPUT_FILE = "/home/oleg/client-sever/graph_input.txt"
START_NODE = 0
END_NODE = 704

def solve_graph():
    try:
        with open(INPUT_FILE, 'r') as f:
            lines = [line.strip() for line in f if line.strip()]
    except FileNotFoundError:
        print(f"Файл {INPUT_FILE} не найден. Сначала запустите генератор.")
        return

    # 1. Парсинг заголовка
    v_count, e_count = map(int, lines[0].split())
    
    # 2. Парсинг матрицы и весов
    # Матрица занимает строки с 1 по v_count включительно
    matrix_lines = lines[1:1 + v_count]
    weights_line = lines[1 + v_count]

    # Преобразуем строки матрицы в список списков
    incidence_matrix = [list(map(int, line.split())) for line in matrix_lines]
    weights = list(map(int, weights_line.split()))

    if len(weights) != e_count:
        print("Ошибка: количество весов не совпадает с количеством ребер в заголовке.")
        return

    # 3. Восстановление графа NetworkX
    G = nx.Graph()
    G.add_nodes_from(range(v_count))

    # Проходим по столбцам (ребрам) матрицы
    for edge_idx in range(e_count):
        # Ищем, какие две вершины соединены этим ребром (где стоят единицы)
        connected_nodes = []
        for node_idx in range(v_count):
            if incidence_matrix[node_idx][edge_idx] == 1:
                connected_nodes.append(node_idx)
        
        if len(connected_nodes) == 2:
            u, v = connected_nodes
            w = weights[edge_idx]
            G.add_edge(u, v, weight=w)
        else:
            print(f"Предупреждение: Ребро {edge_idx} соединяет {len(connected_nodes)} вершин (ожидалось 2).")

    # 4. Поиск кратчайшего пути (Эталонная проверка)
    print(f"\n--- Результаты для маршрута {START_NODE} -> {END_NODE} ---")
    
    if START_NODE >= v_count or END_NODE >= v_count:
        print("Ошибка: Указанные вершины выходят за пределы графа.")
        return

    try:
        # Используем алгоритм Дейкстры, так как веса неотрицательные.
        # Это "золотой стандарт" проверки.
        length = nx.shortest_path_length(G, source=START_NODE, target=END_NODE, weight='weight')
        path = nx.shortest_path(G, source=START_NODE, target=END_NODE, weight='weight')
        
        print(f"Длина пути (Reference): {length}")
        print(f"Путь: {' -> '.join(map(str, path))}")
        
    except nx.NetworkXNoPath:
        print("Путь между вершинами не существует.")

if __name__ == "__main__":
    solve_graph()