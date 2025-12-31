import sys
import networkx as nx
import random

def generate_graph(filename, v_count, e_count):
    G = nx.gnm_random_graph(v_count, e_count, seed=42) # Seed для воспроизводимости структуры, но можно убрать
    # Веса
    for (u, v) in G.edges():
        G.edges[u, v]['weight'] = random.randint(1, 10)

    edges_list = list(G.edges(data=True))
    
    with open(filename, 'w') as f:
        f.write(f"{v_count} {len(edges_list)}\n")
        
        # Матрица инцидентности
        incidence_matrix = [[0] * len(edges_list) for _ in range(v_count)]
        weights = []
        for idx, (u, v, data) in enumerate(edges_list):
            incidence_matrix[u][idx] = 1
            incidence_matrix[v][idx] = 1
            weights.append(str(data['weight']))
            
        for row in incidence_matrix:
            f.write(" ".join(map(str, row)) + "\n")
        
        f.write(" ".join(weights) + "\n")

if __name__ == "__main__":
    generate_graph(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
